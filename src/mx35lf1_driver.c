#include "mx35lf1_driver.h"
#include "mx35lf1_cmd.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h" /* esp_rom_delay_us()     */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define ECC_STATUS_NO_ERR        0b00
#define ECC_STATUS_1_4_CORRECTED 0b01
#define ECC_STATUS_NOT_CORRECTED 0b10

#define BAD_BLOCK_MARK 0

#define NAND_TIMEOUT_RESET_MS 10U
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

#define NAND_CHECK_HANDLE(p)                  \
    do                                        \
    {                                         \
        if (!((p)->initialized))              \
        {                                     \
            ESP_LOGE(TAG, "Not initialized"); \
            return NAND_RET_E_INVALID_ARG;    \
        }                                     \
    } while (0)

#define NAND_LOCK(h)   xSemaphoreTakeRecursive((h)->lock, portMAX_DELAY)
#define NAND_UNLOCK(h) xSemaphoreGiveRecursive((h)->lock)

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

/* ------------------ SPI functions section ------------------ */

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

/* ------------------ Private section ------------------ */

static bool validate_row_address(row_address_t row)
{
    uint32_t block = row >> 6;
    uint32_t page = row & 0x3F;
    if ((block > NAND_MAX_BLOCK_ADDRESS) || (page > NAND_MAX_PAGE_ADDRESS))
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

static bool validate_length(column_address_t column, size_t len)
{
    return (len > 0U) && (len <= (size_t) (NAND_PAGE_FULL_SIZE - column));
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
    const int64_t start_us = esp_timer_get_time();
    const int64_t deadline_us = start_us + (int64_t) timeout_ms * 1000LL;

    for (;;)
    {
        int ret = get_feature(h, NAND_FEAT_STATUS, &status_out->whole);
        if (ret != NAND_RET_OK)
        {
            return ret;
        }

        if (status_out->OIP == 0)
        {
            ESP_LOGD(TAG, "status register=%02X", status_out->whole);
            return NAND_RET_OK;
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us > deadline_us)
        {
            ESP_LOGE(TAG, "wait_ready timeout (%u ms, SR=0x%02X)", (unsigned) timeout_ms, status_out->whole);
            return NAND_RET_TIMEOUT;
        }

        if ((now_us - start_us) < 2000)
        {
            esp_rom_delay_us(50U); /* reads (tRD) and programs finish within this window */
        }

        else
        {
            vTaskDelay(1); /* erase takes milliseconds: do not hog the CPU */
        }
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

static int nand_write_protect(nand_handle_t *h)
{
    int ret = write_disable(h);
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

    return NAND_RET_OK;
}

#endif /* CONFIG_NAND_MX35_PROTECTED_MODE */

static int write_begin(nand_handle_t *h)
{
#ifdef CONFIG_NAND_MX35_PROTECTED_MODE
    int ret = unlock_all_blocks(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Failed to write unprotected");
    }
    return ret;
#else
    (void) h;
    return NAND_RET_OK;
#endif
}

static int write_end(nand_handle_t *h, int op_ret)
{
    int ret = NAND_RET_OK;

#ifdef CONFIG_NAND_MX35_PROTECTED_MODE
    ret = nand_write_protect(h); /* also clears WEL */
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "failed to set the write protection");
    }
#else
    if (op_ret != NAND_RET_OK)
    {
        (void) write_disable(h); /* best effort: do not leave WEL set after a failed operation */
    }
#endif

    return (op_ret != NAND_RET_OK) ? op_ret : ret;
}

static int configure_ecc(nand_handle_t *h, bool enable)
{
    uint8_t value = 0;
    int ret = get_feature(h, NAND_FEAT_SECURE_OTP, &value);
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    feature_reg_configuration_t reg = {.whole = value};
    reg.ECC_EN = enable ? 1U : 0U;

    ret = set_feature(h, NAND_FEAT_SECURE_OTP, reg.whole);
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    feature_reg_status_t st;
    ret = wait_ready(h, &st, NAND_OP_TIMEOUT);
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    ret = get_feature(h, NAND_FEAT_SECURE_OTP, &value);
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    feature_reg_configuration_t readback = {.whole = value};
    if (readback.ECC_EN != reg.ECC_EN)
    {
        ESP_LOGE(TAG, "ECC configuration verify failed (wanted %d, got %d)", (int) reg.ECC_EN, (int) readback.ECC_EN);
        return NAND_RET_VERIFY_FAIL;
    }

    return NAND_RET_OK;
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

    if ((rx_data[2] != NAND_MANUFACTURER_ID) || (rx_data[3] != NAND_DEVICE_ID))
    {
        ESP_LOGE(TAG, "Unexpected ID: manufacturer=0x%02X device=0x%02X", rx_data[2], rx_data[3]);
        return NAND_RET_DEVICE_ID;
    }

    *manufacturer_out = rx_data[2];
    *device_out = rx_data[3];
    return NAND_RET_OK;
}

static int program_load(nand_handle_t *h, column_address_t column, const uint8_t *data_in, size_t write_len)
{
    size_t total_len = 3 + write_len;

    h->tx_buf[0] = CMD_PROG_LOAD;
    h->tx_buf[1] = column >> 8;
    h->tx_buf[2] = column;
    memcpy(&h->tx_buf[3], data_in, write_len);

    int ret = spi_write(h, h->tx_buf, total_len);
    return (ret == NAND_RET_OK) ? NAND_RET_OK : NAND_RET_BAD_SPI;
}

static int program_execute(nand_handle_t *h, row_address_t row)
{
    uint8_t tx_data[4] = {0};
    tx_data[0] = CMD_PROG_EXECUTE;
    tx_data[1] = row >> 16;
    tx_data[2] = row >> 8;
    tx_data[3] = row;

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
    tx_data[1] = row >> 16;
    tx_data[2] = row >> 8;
    tx_data[3] = row;

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

static int read_from_cache_raw(nand_handle_t *h, column_address_t column, uint8_t *data_out, size_t read_len)
{
    size_t total_len = 4 + read_len;  // cmd (1B) + column (2B) + dummy (1B) + data

    h->tx_buf[0] = CMD_READ_CACHE_X1;
    h->tx_buf[1] = column >> 8;
    h->tx_buf[2] = column;
    h->tx_buf[3] = 0x00;  // Dummy byte
    /* Remaining TX bytes are don't-care while the device is shifting data out. */

    return spi_xfer(h, h->tx_buf, h->rx_buf, total_len);
}

static int read_from_cache(nand_handle_t *h, column_address_t column, uint8_t *data_out, size_t read_len)
{
    int ret = read_from_cache_raw(h, column, data_out, read_len);
    if (ret == NAND_RET_OK)
    {
        memcpy(data_out, &h->rx_buf[4], read_len);
    }
    return ret;
}

static int block_erase(nand_handle_t *h, row_address_t row)
{
    uint8_t tx_data[4] = {0};
    tx_data[0] = CMD_BLOCK_ERASE;
    tx_data[1] = row >> 16;
    tx_data[2] = row >> 8;
    tx_data[3] = row;

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

/* Lock-free internal helpers (the caller holds h->lock). */

static int do_page_read(nand_handle_t *h, row_address_t row, column_address_t column, uint8_t *data_out, size_t read_len)
{
    int ret = page_read(h, row);
    if (ret != NAND_RET_OK)
    {
        return ret;
    }
    return read_from_cache(h, column, data_out, read_len);
}

static int read_bad_marker(nand_handle_t *h, uint32_t block, bool *is_bad)
{
    uint8_t mark[2];
    int ret = do_page_read(h, NAND_ROW_ADDR(block, 0), NAND_PAGE_SIZE, mark, sizeof(mark));
    if (ret != NAND_RET_OK)
    {
        return ret;
    }

    *is_bad = (mark[0] == BAD_BLOCK_MARK || mark[1] == BAD_BLOCK_MARK);
    return NAND_RET_OK;
}

static void release_resources(nand_handle_t *h)
{
    if (h->spi)
    {
        spi_bus_remove_device(h->spi);
    }
    heap_caps_free(h->tx_buf);
    heap_caps_free(h->rx_buf);
    if (h->lock)
    {
        vSemaphoreDelete(h->lock);
    }
    memset(h, 0, sizeof(*h));
}

/* ------------------ Public section ------------------ */

int nand_mx35lf1_init(nand_handle_t *h, const nand_config_t *cfg)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_ARG(cfg);

    if (h->initialized)
    {
        ESP_LOGE(TAG, "Already initialized");
        return NAND_RET_E_INVALID_ARG;
    }

    memset(h, 0, sizeof(*h));

    h->lock = xSemaphoreCreateRecursiveMutex();
    h->tx_buf = (uint8_t *) heap_caps_malloc(NAND_SPI_MAX_TRANSFER, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    h->rx_buf = (uint8_t *) heap_caps_malloc(NAND_SPI_MAX_TRANSFER, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!h->lock || !h->tx_buf || !h->rx_buf)
    {
        ESP_LOGE(TAG, "Failed to allocate driver resources");
        release_resources(h);
        return NAND_RET_NO_MEM;
    }
    memset(h->tx_buf, 0xFF, NAND_SPI_MAX_TRANSFER);

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

    esp_err_t esp_ret = spi_bus_add_device(cfg->spi_host, &devcfg, &h->spi);
    if (esp_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(esp_ret));
        h->spi = NULL;
        release_resources(h);
        return NAND_RET_BAD_SPI;
    }

    int ret;
    uint8_t manufacturer = 0;
    uint8_t device = 0;

    vTaskDelay(pdMS_TO_TICKS(NAND_TIMEOUT_RESET_MS));
    ret = nand_reset(h);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to reset the flash");
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(NAND_TIMEOUT_RESET_MS));

    ret = nand_read_id(h, &manufacturer, &device);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to get the ID");
        goto fail;
    }

#ifdef CONFIG_NAND_MX35_PROTECTED_MODE
    ret = nand_write_protect(h);
#else
    ret = unlock_all_blocks(h);
#endif
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Error to set the block protection data");
        goto fail;
    }

    /* Always write the ECC bit explicitly so that disable_ecc really disables it. */
    ret = configure_ecc(h, !cfg->disable_ecc);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Failed to configure ecc");
        goto fail;
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

    ESP_LOGD(TAG, "Manufacturer=0x%02X Device=0x%02X", manufacturer, device);
    return NAND_RET_OK;

fail:
    release_resources(h);
    return ret;
}

int nand_mx35lf1_deinit(nand_handle_t *h)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_HANDLE(h);

    NAND_LOCK(h); /* wait for any operation in progress */
    h->initialized = false;
    NAND_UNLOCK(h);

    release_resources(h);
    return NAND_RET_OK;
}

int nand_mx35lf1_page_program(nand_handle_t *h, row_address_t row, column_address_t column, const uint8_t *data_in, size_t write_len)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_HANDLE(h);
    NAND_CHECK_ARG(data_in);

    if (!validate_row_address(row) || !validate_column_address(column))
    {
        ESP_LOGE(TAG, "Invalid row or column address");
        return NAND_RET_BAD_ADDRESS;
    }

    if (!validate_length(column, write_len))
    {
        ESP_LOGE(TAG, "invalid write length (%u) for column %u", (unsigned) write_len, (unsigned) column);
        return NAND_RET_INVALID_LEN;
    }

    NAND_LOCK(h);

    int ret = write_begin(h);
    if (ret != NAND_RET_OK)
    {
        NAND_UNLOCK(h);
        return ret;
    }

    ret = write_enable(h);
    if (ret == NAND_RET_OK)
    {
        ret = program_load(h, column, data_in, write_len);
    }

    if (ret == NAND_RET_OK)
    {
        ret = program_execute(h, row);
    }
    ret = write_end(h, ret);

    NAND_UNLOCK(h);

    if (ret == NAND_RET_OK)
    {
        ESP_LOGD(TAG, "Page program passed");
    }
    return ret;
}

int nand_mx35lf1_page_read(nand_handle_t *h, row_address_t row, column_address_t column, uint8_t *data_out, size_t read_len)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_HANDLE(h);
    NAND_CHECK_ARG(data_out);

    if (!validate_row_address(row) || !validate_column_address(column))
    {
        ESP_LOGE(TAG, "Invalid row or column address");
        return NAND_RET_BAD_ADDRESS;
    }

    if (!validate_length(column, read_len))
    {
        ESP_LOGE(TAG, "invalid read length (%u) for column %u", (unsigned) read_len, (unsigned) column);
        return NAND_RET_INVALID_LEN;
    }

    NAND_LOCK(h);
    int ret = do_page_read(h, row, column, data_out, read_len);
    NAND_UNLOCK(h);

    if (ret == NAND_RET_OK)
    {
        ESP_LOGD(TAG, "Page read passed");
    }
    return ret;
}

int nand_mx35lf1_page_copy(nand_handle_t *h, row_address_t src, row_address_t dst)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_HANDLE(h);

    if (!validate_row_address(src) || !validate_row_address(dst))
    {
        return NAND_RET_BAD_ADDRESS;
    }

    NAND_LOCK(h);

    int ret = page_read(h, src);
    if (ret != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Failed to read page in page_copy()");
        NAND_UNLOCK(h);
        return ret;
    }

    ret = write_begin(h);
    if (ret != NAND_RET_OK)
    {
        NAND_UNLOCK(h);
        return ret;
    }

    ret = write_enable(h);
    if (ret == NAND_RET_OK)
    {
        ret = program_execute(h, dst);
    }
    ret = write_end(h, ret);

    NAND_UNLOCK(h);

    if (ret == NAND_RET_OK)
    {
        ESP_LOGD(TAG, "Page copy passed");
    }
    return ret;
}

int nand_mx35lf1_block_erase(nand_handle_t *h, row_address_t row)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_HANDLE(h);
    row &= ~0x3F;  // make sure page address is zero

    if (!validate_row_address(row))
    {
        return NAND_RET_BAD_ADDRESS;
    }

    NAND_LOCK(h);

    int ret = write_begin(h);
    if (ret != NAND_RET_OK)
    {
        NAND_UNLOCK(h);
        return ret;
    }

    ret = write_enable(h);
    if (ret == NAND_RET_OK)
    {
        ret = block_erase(h, row);
    }
    ret = write_end(h, ret);

    NAND_UNLOCK(h);

    if (ret == NAND_RET_OK)
    {
        ESP_LOGD(TAG, "Block erase passed");
    }
    return ret;
}

int nand_mx35lf1_block_is_bad(nand_handle_t *h, row_address_t row, bool *is_bad)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_HANDLE(h);
    NAND_CHECK_ARG(is_bad);

    if (!validate_row_address(row))
    {
        return NAND_RET_BAD_ADDRESS;
    }

    const uint32_t block = row >> 6;

    NAND_LOCK(h);
    int ret = read_bad_marker(h, block, is_bad);
    NAND_UNLOCK(h);

    return ret;
}

int nand_mx35lf1_block_mark_bad(nand_handle_t *h, row_address_t row)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_HANDLE(h);

    if (!validate_row_address(row))
    {
        return NAND_RET_BAD_ADDRESS;
    }

    uint8_t bad_block_mark[2] = {BAD_BLOCK_MARK, BAD_BLOCK_MARK};
    return nand_mx35lf1_page_program(h, row & ~0x3FU, NAND_PAGE_SIZE, bad_block_mark, sizeof(bad_block_mark));
}

int nand_mx35lf1_page_is_free(nand_handle_t *h, row_address_t row, bool *is_free)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_HANDLE(h);
    NAND_CHECK_ARG(is_free);

    if (!validate_row_address(row))
    {
        return NAND_RET_BAD_ADDRESS;
    }

    size_t alloc_len = NAND_PAGE_FULL_SIZE;
    uint8_t *page_main_and_oob_buffer = (uint8_t *) malloc(alloc_len);
    if (page_main_and_oob_buffer == NULL)
    {
        ESP_LOGE(TAG, "failed to allocate buffer");
        return NAND_RET_E_INVALID_ARG;
    }

    NAND_LOCK(h);

    int ret = nand_mx35lf1_page_read(h, row, 0, page_main_and_oob_buffer, alloc_len);
    if (ret != NAND_RET_OK)
    {
        free(page_main_and_oob_buffer);
        NAND_UNLOCK(h);
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

    free(page_main_and_oob_buffer);
    ESP_LOGD(TAG, "Nand is free passed");
    NAND_UNLOCK(h);
    return NAND_RET_OK;
}

int nand_mx35lf1_clear(nand_handle_t *h)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_HANDLE(h);

    int result = NAND_RET_OK;
    uint32_t failed = 0;

    for (uint32_t i = 0; i < NAND_BLOCKS_PER_LUN; i++)
    {
        ESP_LOGD(TAG, "Erase block : %" PRIu32, i);
        row_address_t row = NAND_ROW_ADDR(i, 0);

        bool is_bad = false;
        int ret = nand_mx35lf1_block_is_bad(h, row, &is_bad);
        if (ret != NAND_RET_OK)
        {
            failed++;
            result = ret;
            continue;
        }

        if (is_bad)
        {
            continue;
        }

        ret = nand_mx35lf1_block_erase(h, row);
        if (ret != NAND_RET_OK)
        {
            ESP_LOGW(TAG, "Erase of block %" PRIu32 " failed: %s", i, nand_err_to_str(ret));
            failed++;
            result = ret;
        }
    }

    if (failed > 0)
    {
        ESP_LOGW(TAG, "%" PRIu32 " block(s) could not be erased", failed);
    }
    return result;
}

bool nand_mx35lf1_mounted(const nand_handle_t *h)
{
    return (h != NULL) && h->initialized;
}

int nand_mx35lf1_get_info(const nand_handle_t *h, nand_info_t *info_out)
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

const char *nand_err_to_str(const int code)
{
    if (code > NAND_RET_OK || code < NAND_RET_VERIFY_FAIL)
    {
        return "unknown";
    }

    nand_err_t err = (nand_err_t) code;
    switch (err)
    {
        case NAND_RET_OK: return "NAND_RET_OK";
        case NAND_RET_BAD_SPI: return "NAND_RET_BAD_SPI";
        case NAND_RET_TIMEOUT: return "NAND_RET_TIMEOUT";
        case NAND_RET_DEVICE_ID: return "NAND_RET_DEVICE_ID";
        case NAND_RET_BAD_ADDRESS: return "NAND_RET_BAD_ADDRESS";
        case NAND_RET_INVALID_LEN: return "NAND_RET_INVALID_LEN";
        case NAND_RET_ECC_REFRESH: return "NAND_RET_ECC_REFRESH";
        case NAND_RET_ECC_ERR: return "NAND_RET_ECC_ERR";
        case NAND_RET_P_FAIL: return "NAND_RET_P_FAIL";
        case NAND_RET_E_FAIL: return "NAND_RET_E_FAIL";
        case NAND_RET_E_INVALID_ARG: return "NAND_RET_E_INVALID_ARG";
        case NAND_RET_NO_MEM: return "NAND_RET_NO_MEM";
        case NAND_RET_VERIFY_FAIL: return "NAND_RET_VERIFY_FAIL";
        default: return "unknown";
    }
}
