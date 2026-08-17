#include "mx35lf1_driver.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h" /* esp_rom_delay_us()     */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "string.h"

#define ECC_STATUS_NO_ERR        0b00
#define ECC_STATUS_1_4_CORRECTED 0b01
#define ECC_STATUS_NOT_CORRECTED 0b10

#define BAD_BLOCK_MARK 0

#define NAND_TIMEOUT_RESET_MS 10U
#define NAND_TIMEOUT_READ_MS  10U
#define NAND_TIMEOUT_PROG_MS  50U
#define NAND_TIMEOUT_ERASE_MS 100U
#define NAND_OP_TIMEOUT       3000  // ms

#define NAND_CHECK_ARG(p)                   \
    do                                      \
    {                                       \
        if (!(p))                           \
        {                                   \
            ESP_LOGE(TAG, "NULL argument"); \
            return NAND_RET_E_INVALID_ARG;  \
        }                                   \
    } while (0)

typedef union
{
    uint8_t whole;
    struct
    {
        uint8_t SP : 1;
        uint8_t COMPLEMENTARY : 1;
        uint8_t INVERT : 1;
        uint8_t BP0 : 1;
        uint8_t BP1 : 1;
        uint8_t BP2 : 1;
        uint8_t : 1;
        uint8_t BPRWD : 1;
    };
} feature_reg_block_lock_t;

typedef union
{
    uint8_t whole;
    struct
    {
        uint8_t QE : 1;
        uint8_t : 3;
        uint8_t ECC_EN : 1;
        uint8_t : 1;
        uint8_t SECURE_OTP_ENABLE : 1;
        uint8_t SECURE_OTP_PROTECT : 1;
    };
} feature_reg_configuration_t;

typedef union
{
    uint8_t whole;
    struct
    {
        uint8_t OIP : 1;
        uint8_t WEL : 1;
        uint8_t E_FAIL : 1;
        uint8_t P_FAIL : 1;
        uint8_t ECCS0_2 : 2;
        uint8_t : 2;
    };
} feature_reg_status_t;

static const char TAG[] = "mx35lf1";

/* ------------------ Private section ------------------ */

static int spi_xfer(nand_handle_t *h, const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8U,
        .tx_buffer = tx,
        .rx_buffer = rx ? rx : NULL,
    };

    // The SPI function handles the CS pin automatically.
    esp_err_t ret = (len <= 64U) ? spi_device_polling_transmit(h->spi, &t) : spi_device_transmit(h->spi, &t);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI transfer failed: %s", esp_err_to_name(ret));
        return NAND_RET_BAD_SPI;
    }

    return NAND_RET_OK;
}

static inline int spi_write(nand_handle_t *h, const uint8_t *tx, size_t len)
{
    return spi_xfer(h, tx, NULL, len);
}

static bool validate_row_address(row_address_t row)
{
    if ((row.block > NAND_MAX_BLOCK_ADDRESS) || (row.page > NAND_MAX_PAGE_ADDRESS))
    {
        return false;
    }
    return true;
}

static bool validate_column_address(column_address_t address)
{
    if (address >= (NAND_PAGE_SIZE + NAND_SPARE_SIZE))
    {
        return false;
    }
    return true;
}

static int get_ret_from_ecc_status(feature_reg_status_t status)
{
    int ret;
    switch (status.ECCS0_2)
    {
        case ECC_STATUS_NO_ERR:
        case ECC_STATUS_1_4_CORRECTED: ret = NAND_RET_OK; break;

        case ECC_STATUS_NOT_CORRECTED:
        default: ret = NAND_RET_ECC_ERR; break;
    }

    return ret;
}

static int set_feature(nand_handle_t *h, uint8_t reg, uint8_t data)
{
    uint8_t tx_data[3] = {0};
    tx_data[0] = CMD_SET_FEATURE;
    tx_data[1] = reg;
    tx_data[2] = data;

    int ret = spi_write(h, tx_data, sizeof(tx_data));
    return (ret == NAND_RET_OK) ? NAND_RET_OK : NAND_RET_BAD_SPI;
}

static int get_feature(nand_handle_t *h, uint8_t reg, uint8_t *data_out)
{
    uint8_t tx_data[3] = {0};
    uint8_t rx_data[3] = {0};
    tx_data[0] = CMD_GET_FEATURE;
    tx_data[1] = reg;

    int ret = spi_xfer(h, tx_data, rx_data, sizeof(tx_data));
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    *data_out = rx_data[2];
    return NAND_RET_OK;
}

static int wait_ready(nand_handle_t *h, feature_reg_status_t *status_out, uint32_t timeout_ms)
{
    int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;

    for (;;)
    {
        int ret = get_feature(h, NAND_FEAT_STATUS, &status_out->whole);
        ESP_LOGD(TAG, "status register=%02X", status_out->whole);
        if (ret != NAND_RET_OK)
        {
            return ret;
        }

        if (status_out->OIP == 0)
        {
            return NAND_RET_OK;
        }

        if (status_out->E_FAIL == 0x01)
        {
            ESP_LOGE(TAG, "Error in E_FAIL bit in wait function");
        }

        if (status_out->P_FAIL == 0x01)
        {
            ESP_LOGE(TAG, "Error in P_FAIL bit in wait function");
        }

        if (status_out->ECCS0_2 == 0x02)
        {
            ESP_LOGE(TAG, "Error in ECCSx bit in wait function");
        }

        if (esp_timer_get_time() > deadline_us)
        {
            ESP_LOGE(TAG, "wait_ready timeout (%u ms, SR=0x%02X)", (unsigned) timeout_ms, status_out->whole);
            return NAND_RET_TIMEOUT;
        }

        /* Poll every 200 µs — fine-grained enough for tRD=25 µs */
        esp_rom_delay_us(200U);
    }
}

static int write_enable(nand_handle_t *h)
{
    uint8_t cmd = CMD_WRITE_ENABLE;
    int ret = spi_write(h, &cmd, sizeof(cmd));
    if (ret == NAND_RET_OK)
    {
        ESP_LOGD(TAG, "Enable WEL bit");
    }
    return ret;
}

static int unlock_all_blocks(nand_handle_t *h)
{
    feature_reg_block_lock_t unlock_all = {.whole = 0};
    int ret = set_feature(h, NAND_FEAT_BLOCK_PROT, unlock_all.whole);
    if (ret == NAND_RET_OK)
    {
        ESP_LOGD(TAG, "unlock all blocks");
    }
    return ret;
}

#ifdef CONFIG_NAND_MX35_PROTECTED_MODE

static int write_disable(nand_handle_t *h)
{
    uint8_t cmd = CMD_WRITE_DISABLE;
    int ret = spi_write(h, &cmd, sizeof(cmd));
    if (ret == NAND_RET_OK)
    {
        ESP_LOGD(TAG, "Disable WEL bit");
    }
    return ret;
}

static int lock_all_blocks(nand_handle_t *h)
{
    feature_reg_block_lock_t lock_all = {.whole = 0};
    lock_all.BP0 = 0x01;
    lock_all.BP1 = 0x01;
    lock_all.BP2 = 0x01;
    int ret = set_feature(h, NAND_FEAT_BLOCK_PROT, lock_all.whole);
    if (ret == NAND_RET_OK)
    {
        ESP_LOGD(TAG, "Enable protection in all blocks");
    }
    return ret;
}

#endif

static int enable_ecc(nand_handle_t *h)
{
    feature_reg_configuration_t ecc_enable = {.whole = 0};
    ecc_enable.ECC_EN = 1;
    return set_feature(h, NAND_FEAT_SECURE_OTP, ecc_enable.whole);
}

static int nand_reset(nand_handle_t *h)
{
    uint8_t tx_data = CMD_RESET;
    int ret = spi_write(h, &tx_data, sizeof(tx_data));
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to send reset frame");
        return ret;
    }

    feature_reg_status_t st;
    return wait_ready(h, &st, NAND_OP_TIMEOUT);
}

static int nand_read_id(nand_handle_t *h, uint8_t *manufacturer_out, uint8_t *device_out)
{
    uint8_t tx_data[4] = {0};
    uint8_t rx_data[4] = {0};
    tx_data[0] = CMD_READ_ID;

    int ret = spi_xfer(h, tx_data, rx_data, sizeof(tx_data));
    if (ret != NAND_RET_OK)
    {
        return NAND_RET_BAD_SPI;
    }

    if ((rx_data[2] != NAND_MANUFACTURER_ID) && (rx_data[3] != NAND_DEVICE_ID))
    {
        return NAND_RET_DEVICE_ID;
    }

    *manufacturer_out = rx_data[2];
    *device_out = rx_data[3];
    return NAND_RET_OK;
}

static int program_load(nand_handle_t *h, column_address_t column, const uint8_t *data_in, size_t write_len)
{
    size_t total_len = 3 + write_len;
    uint8_t *tx_buf = (uint8_t *) malloc(total_len);
    if (!tx_buf)
    {
        return NAND_RET_E_INVALID_ARG;
    }

    tx_buf[0] = CMD_PROG_LOAD;
    tx_buf[1] = column >> 8;
    tx_buf[2] = column;
    memcpy(&tx_buf[3], data_in, write_len);

    int ret = spi_write(h, tx_buf, total_len);
    free(tx_buf);
    return (ret == NAND_RET_OK) ? NAND_RET_OK : NAND_RET_BAD_SPI;
}

static int program_execute(nand_handle_t *h, row_address_t row)
{
    uint8_t tx_data[4] = {0};
    tx_data[0] = CMD_PROG_EXECUTE;
    tx_data[1] = row.whole >> 16;
    tx_data[2] = row.whole >> 8;
    tx_data[3] = row.whole;

    int ret = spi_write(h, tx_data, sizeof(tx_data));
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to send the spi frame command in program execute");
        return ret;
    }

    feature_reg_status_t status;
    ret = wait_ready(h, &status, NAND_OP_TIMEOUT);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to wait the OIP bit in program_execute");
        return ret;
    }

    if (status.P_FAIL)
    {
        ESP_LOGE(TAG, "P_FAIL set in the program execute");
        return NAND_RET_P_FAIL;
    }

    return NAND_RET_OK;
}

static int page_read(nand_handle_t *h, row_address_t row)
{
    uint8_t tx_data[4] = {0};
    tx_data[0] = CMD_PAGE_READ;
    tx_data[1] = row.whole >> 16;
    tx_data[2] = row.whole >> 8;
    tx_data[3] = row.whole;

    int ret = spi_write(h, tx_data, sizeof(tx_data));
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to send the spi frame command in page_read()");
        return ret;
    }

    feature_reg_status_t status;
    ret = wait_ready(h, &status, NAND_OP_TIMEOUT);
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    return get_ret_from_ecc_status(status);
}

static int read_from_cache(nand_handle_t *h, column_address_t column, uint8_t *data_out, size_t read_len)
{
    size_t total_len = 4 + read_len;  // cmd (1B) + column (2B) + dummy (1B) + data
    uint8_t *tx_buf = (uint8_t *) malloc(total_len);
    uint8_t *rx_buf = (uint8_t *) malloc(total_len);

    if (!tx_buf || !rx_buf)
    {
        free(tx_buf);
        free(rx_buf);
        return NAND_RET_E_INVALID_ARG;
    }

    memset(tx_buf, 0xFF, total_len);
    tx_buf[0] = CMD_READ_CACHE_X1;
    tx_buf[1] = column >> 8;
    tx_buf[2] = column;
    tx_buf[3] = 0x00;  // Dummy byte

    int ret = spi_xfer(h, tx_buf, rx_buf, total_len);
    if (ret == NAND_RET_OK)
    {
        memcpy(data_out, &rx_buf[4], read_len);
    }

    free(tx_buf);
    free(rx_buf);
    return ret;
}

static int block_erase(nand_handle_t *h, row_address_t row)
{
    uint8_t tx_data[4] = {0};
    tx_data[0] = CMD_BLOCK_ERASE;
    tx_data[1] = row.whole >> 16;
    tx_data[2] = row.whole >> 8;
    tx_data[3] = row.whole;

    int ret = spi_write(h, tx_data, sizeof(tx_data));
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    feature_reg_status_t status;
    ret = wait_ready(h, &status, NAND_OP_TIMEOUT);
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    if (status.E_FAIL)
    {
        ESP_LOGE(TAG, "E_FAIL is set in block erase operation");
        return NAND_RET_E_FAIL;
    }

    return NAND_RET_OK;
}

/* ------------------ Public section ------------------ */

int nand_mx35lf1_init(nand_handle_t *h, const nand_config_t *cfg)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_ARG(cfg);

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = cfg->clock_speed_hz,
        .mode = 0, /* CPOL=0, CPHA=0 — Serial Mode 0 */
        .spics_io_num = cfg->pin_cs,
        .queue_size = 4,
        .pre_cb = NULL,
        .post_cb = NULL,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
    };

    int ret = spi_bus_add_device(cfg->spi_host, &devcfg, &h->spi) != ESP_OK ? NAND_RET_BAD_SPI : NAND_RET_OK;
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %d", ret);
        return ret;
    }

    vTaskDelay(NAND_TIMEOUT_RESET_MS);
    ret = nand_reset(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to reset the flash");
        return ret;
    }
    vTaskDelay(NAND_TIMEOUT_RESET_MS);

    uint8_t manufacturer, device;
    ret = nand_read_id(h, &manufacturer, &device);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to get the ID");
        return ret;
    }

#ifdef CONFIG_NAND_MX35_PROTECTED_MODE
    ret = write_disable(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to disable the WEL bit");
        return ret;
    }

    ret = lock_all_blocks(h);
#else
    ret = unlock_all_blocks(h);
#endif
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to set the block protection data");
        return ret;
    }

    if (!cfg->disable_ecc)
    {
        ret = enable_ecc(h);
        if (ret != NAND_RET_OK)
        {
            ESP_LOGE(TAG, "Failed to enable ecc");
            return ret;
        }
    }

    /* Populate device info */
    h->info.device_id = device;
    h->info.manufacturer_id = manufacturer;
    h->info.ecc_enabled = !cfg->disable_ecc;
    h->info.page_size = NAND_PAGE_SIZE;
    h->info.pages_per_block = NAND_PAGES_PER_BLOCK;
    h->info.spare_size = NAND_SPARE_SIZE;
    h->info.total_blocks = NAND_BLOCKS_PER_LUN;
    h->initialized = true;

    ESP_LOGD(TAG, "Manufacturer=%" PRIx8 " Device=%" PRIx8, manufacturer, device);
    return ret;
}

int nand_mx35lf1_page_program(nand_handle_t *h, row_address_t row, column_address_t column, const uint8_t *data_in, size_t write_len)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_ARG(data_in);

    if (!validate_row_address(row) || !validate_column_address(column))
    {
        ESP_LOGE(TAG, "Invalide row or column address");
        return NAND_RET_BAD_ADDRESS;
    }

    uint16_t max_write_len = (NAND_PAGE_SIZE + NAND_SPARE_SIZE) - column;
    if (write_len > max_write_len)
    {
        ESP_LOGE(TAG, "write length is bigger than the page");
        return NAND_RET_INVALID_LEN;
    }

    int ret = NAND_RET_OK;

#ifdef CONFIG_NAND_MX35_PROTECTED_MODE
    ret = unlock_all_blocks(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to unlock all blocks");
        return ret;
    }
#endif

    ret = write_enable(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to set the WEL bit");
        return ret;
    }

    ret = program_load(h, column, data_in, write_len);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error on program_load() function");
        return ret;
    }

    ret = program_execute(h, row);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error on program_execute() function");
        return ret;
    }

#ifdef CONFIG_NAND_MX35_PROTECTED_MODE
    ret = write_disable(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to disable the WEL bit");
        return ret;
    }

    ret = lock_all_blocks(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to set the block protection data");
        return ret;
    }
#endif

    ESP_LOGD(TAG, "Page program passed");
    return ret;
}

int nand_mx35lf1_page_read(nand_handle_t *h, row_address_t row, column_address_t column, uint8_t *data_out, size_t read_len)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_ARG(data_out);

    if (!validate_row_address(row) || !validate_column_address(column))
    {
        ESP_LOGE(TAG, "Invalide row or column address");
        return NAND_RET_BAD_ADDRESS;
    }

    int ret = page_read(h, row);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "error on page_read() function");
        return ret;
    }

    ret = read_from_cache(h, column, data_out, read_len);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "error on read_from_cache() function");
        return ret;
    }

    ESP_LOGD(TAG, "Page read passed");
    return ret;
}

int nand_mx35lf1_page_copy(nand_handle_t *h, row_address_t src, row_address_t dst)
{
    NAND_CHECK_ARG(h);

    if (!validate_row_address(src) || !validate_row_address(dst))
    {
        return NAND_RET_BAD_ADDRESS;
    }

    int ret = page_read(h, src);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Failed to read page in page_copy()");
        return ret;
    }

#ifdef CONFIG_NAND_MX35_PROTECTED_MODE
    ret = unlock_all_blocks(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to unlock all blocks");
        return ret;
    }
#endif

    ret = write_enable(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to set the WEL bit");
        return ret;
    }

    ret = program_execute(h, dst);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error on program_execute() function");
        return ret;
    }

#ifdef CONFIG_NAND_MX35_PROTECTED_MODE
    ret = write_disable(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to disable the WEL bit");
        return ret;
    }

    ret = lock_all_blocks(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to set the block protection data");
        return ret;
    }
#endif

    ESP_LOGD(TAG, "Page copy passed");
    return ret;
}

int nand_mx35lf1_block_erase(nand_handle_t *h, row_address_t row)
{
    NAND_CHECK_ARG(h);
    row.page = 0;  // make sure page address is zero

    if (!validate_row_address(row))
    {
        return NAND_RET_BAD_ADDRESS;
    }

    int ret = NAND_RET_OK;

#ifdef CONFIG_NAND_MX35_PROTECTED_MODE
    ret = unlock_all_blocks(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to unlock all blocks");
        return ret;
    }
#endif

    ret = write_enable(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to set the WEL bit");
        return ret;
    }

    ret = block_erase(h, row);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to erase block");
        return ret;
    }

#ifdef CONFIG_NAND_MX35_PROTECTED_MODE
    ret = write_disable(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to disable the WEL bit");
        return ret;
    }

    ret = lock_all_blocks(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to set the block protection data");
        return ret;
    }
#endif

    ESP_LOGD(TAG, "Block erase passed");
    return ret;
}

int nand_mx35lf1_block_is_bad(nand_handle_t *h, row_address_t row, bool *is_bad)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_ARG(is_bad);
    uint8_t bad_block_mark[2];

    int ret = nand_mx35lf1_page_read(h, row, NAND_PAGE_SIZE, bad_block_mark, sizeof(bad_block_mark));
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    ESP_LOGD(TAG, "Block is bad passed");
    *is_bad = (bad_block_mark[0] == BAD_BLOCK_MARK || bad_block_mark[1] == BAD_BLOCK_MARK);
    return NAND_RET_OK;
}

int nand_mx35lf1_block_mark_bad(nand_handle_t *h, row_address_t row)
{
    NAND_CHECK_ARG(h);
    uint8_t bad_block_mark[2] = {BAD_BLOCK_MARK, BAD_BLOCK_MARK};
    return nand_mx35lf1_page_program(h, row, NAND_PAGE_SIZE, bad_block_mark, sizeof(bad_block_mark));
}

int nand_mx35lf1_page_is_free(nand_handle_t *h, row_address_t row, bool *is_free)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_ARG(is_free);

    size_t alloc_len = NAND_PAGE_FULL_SIZE;
    uint8_t *page_main_and_oob_buffer = (uint8_t *) malloc(alloc_len);
    if (page_main_and_oob_buffer == NULL)
    {
        ESP_LOGE(TAG, "failed to allocate buffer");
        return NAND_RET_E_INVALID_ARG;
    }

    int ret = nand_mx35lf1_page_read(h, row, 0, page_main_and_oob_buffer, alloc_len);
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    *is_free = true;
    uint32_t comp_word = 0xffffffff;
    for (int i = 0; i < alloc_len; i += sizeof(comp_word))
    {
        if (memcmp(&comp_word, &page_main_and_oob_buffer[i], sizeof(comp_word)) != 0)
        {
            *is_free = false;
            break;
        }
    }

    ESP_LOGD(TAG, "Nand is free passed");
    return NAND_RET_OK;
}

int nand_mx35lf1_clear(nand_handle_t *h)
{
    NAND_CHECK_ARG(h);

    bool is_bad;
    for (int i = 0; i < NAND_BLOCKS_PER_LUN; i++)
    {
        ESP_LOGI(TAG, "Erase block : %d", i);
        row_address_t row = {.block = i, .page = 0};
        int ret = nand_mx35lf1_block_is_bad(h, row, &is_bad);
        if (ret != NAND_RET_OK)
        {
            continue;
        }

        if (!is_bad)
        {
            int ret_ = nand_mx35lf1_block_erase(h, row);
            if (ret_ != NAND_RET_OK)
            {
                continue;
            }
        }

        vTaskDelay(100);
    }

    return NAND_RET_OK;
}

bool nand_mx35lf1_mounted(nand_handle_t *h)
{
    NAND_CHECK_ARG(h);
    return h->initialized;
}

int nand_mx35lf1_get_info(nand_handle_t *h, nand_info_t *info_out)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_ARG(info_out);
    info_out->device_id = h->info.device_id;
    info_out->ecc_enabled = h->info.ecc_enabled;
    info_out->manufacturer_id = h->info.manufacturer_id;
    info_out->page_size = h->info.page_size;
    info_out->pages_per_block = h->info.pages_per_block;
    info_out->spare_size = h->info.spare_size;
    info_out->total_blocks = h->info.total_blocks;
    return NAND_RET_OK;
}
