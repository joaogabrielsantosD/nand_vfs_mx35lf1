#include "MX35LF1.h"

#include "esp_timer.h"
#include <string.h>

#define MX35_CLOCK_FREQUENCY (50 * 1000 * 1000)  // 50 MHz

#if CONFIG_NAND_MX35_SPI2_BUS
#    define SPI_BUS_HOST SPI2_HOST
#elif CONFIG_NAND_MX35_SPI3_BUS
#    define SPI_BUS_HOST SPI3_HOST
#endif

static const char *TAG = "NAND MX35";
static nand_mx35_context_t mx35_ctx;

static uint8_t bad_blocks_map[20] = {0};
static uint8_t bad_blocks_count = 0;

#define MX35_SELECT   (gpio_set_level(mx35_ctx.cfg.spi_pins.cs_io, 0))
#define MX35_UNSELECT (gpio_set_level(mx35_ctx.cfg.spi_pins.cs_io, 1))

#define ENABLE_WP  (gpio_set_level(mx35_ctx.cfg.spi_pins.wp_io, 0))
#define DISABLE_WP (gpio_set_level(mx35_ctx.cfg.spi_pins.wp_io, 1))

#define ENABLE_HOLD  (gpio_set_level(mx35_ctx.cfg.spi_pins.hd_io, 0))
#define DISABLE_HOLD (gpio_set_level(mx35_ctx.cfg.spi_pins.hd_io, 1))

//--- Private ----------------------------------------------------------

static bool spi_write_read(const uint8_t *cmd, const uint8_t len, uint8_t *rx)
{
    if (len == 0 || cmd == NULL)
        return ESP_ERR_INVALID_ARG;  // no need to send anything

    spi_transaction_t t = {
        .length = len * 8,  // Length in bits
        .tx_buffer = cmd,
        .flags = SPI_TRANS_USE_RXDATA,
    };

    MX35_SELECT;
    esp_err_t ret = spi_device_polling_transmit(mx35_ctx.spi, &t);  // Transmit!
    MX35_UNSELECT;

    if (rx != NULL)
    {
        ESP_LOGD(TAG, "RECEIVED: [0x%02X, 0x%02X, 0x%02X, 0x%02X]", t.rx_data[0], t.rx_data[1], t.rx_data[2], t.rx_data[3]);
        memcpy(rx, t.rx_data, 4);
    }

    return ret == ESP_OK ? true : false;
}


static bool nand_mx35lf_SET_Features(uint8_t address, uint8_t value)
{
    uint8_t cmd[] = {CMD_SET_FEATURES, address, value};
    return spi_write_read(cmd, sizeof(cmd), NULL);
}

static uint8_t nand_mx35lf_GET_Features(uint8_t address)
{
    uint8_t cmd[] = {CMD_GET_FEATURES, address, 0x00};
    uint8_t _rx[4];
    spi_write_read(cmd, sizeof(cmd), _rx);
    return _rx[2];
}

static mx35_err_t WaitOperationDone()
{
    uint8_t status = 0;
    uint32_t timeout = 1000;

    do
    {
        status = nand_mx35lf_GET_Features(REG_STATUS);
        if ((status & 0x01) == STATUS_READY)
            return MX35_OK;

        vTaskDelay(pdMS_TO_TICKS(1));
    } while (timeout--);

    return MX35_FAIL;
}


static bool nand_mx35_write_enable()
{
    uint8_t cmd[] = {CMD_WRITE_ENABLE};
    spi_write_read(cmd, sizeof(cmd), NULL);
    vTaskDelay(pdMS_TO_TICKS(1));
    uint8_t reg = nand_mx35lf_GET_Features(REG_STATUS) & WEL_BIT;

    return reg == WEL_BIT;
}

static bool nand_mx35_write_disable()
{
    uint8_t cmd[] = {CMD_WRITE_DISABLE};
    spi_write_read(cmd, sizeof(cmd), NULL);
    vTaskDelay(pdMS_TO_TICKS(1));
    uint8_t reg = nand_mx35lf_GET_Features(REG_STATUS) & WEL_BIT;

    return reg == 0x00;
}


static mx35_err_t nand_mx35_program_load(uint8_t *data, size_t len)
{
    if (!data || len == 0)
        return MX35_INVALID_ARGUMENT;

    if (len > PAGE_SIZE_WITHOUT_ECC)
        return MX35_INVALID_ARGUMENT;

    uint8_t cmd_header[3] = {CMD_PROGRAM_LOAD_X1, 0x00, 0x00};  // start from column 0 (byte 0 of the page)

    spi_transaction_t t = {
        .tx_buffer = cmd_header,
        .length = sizeof(cmd_header) * 8,
    };

    MX35_SELECT;
    esp_err_t ret = spi_device_polling_transmit(mx35_ctx.spi, &t);
    if (ret == ESP_OK)
    {
        t.tx_buffer = data;
        t.length = len * 8;
        ret = spi_device_polling_transmit(mx35_ctx.spi, &t);
    }
    MX35_UNSELECT;

    return ret == ESP_OK ? MX35_OK : MX35_WRITE_FAIL;
}

static bool nand_mx35_program_execute(uint16_t page_address)
{
    uint8_t cmd[4] = {
        CMD_PROGRAM_EXECUTE,
        0x00,
        (uint8_t) ((page_address >> 8) & 0xFF),
        (uint8_t) (page_address & 0xFF),
    };

    return spi_write_read(cmd, sizeof(cmd), NULL);
}


static void nand_mx35_reset()
{
    /* reset the module */
    spi_transaction_t trans_desc = {
        .flags = SPI_TRANS_USE_TXDATA,
        .tx_data = {CMD_RESET},
        .length = 8,
    };

    MX35_SELECT;
    spi_device_polling_transmit(mx35_ctx.spi, &trans_desc);
    MX35_UNSELECT;

    ENABLE_WP;
    vTaskDelay(pdMS_TO_TICKS(2));
    DISABLE_WP;
}

static bool nand_mx35_get_id()
{
    uint8_t cmd[] = {CMD_READ_MANUFACTURER_ID, 0xFF, 0xFF, 0xFF};
    uint8_t _rx[4];
    spi_write_read(cmd, sizeof(cmd), _rx);
    return (_rx[2] == MANUFACTURER_ID && _rx[3] == DEVICE_ID);
}

static void nand_mx35_verify_bad_blocks()
{
    uint8_t b[2];
    for (uint16_t i = 0; i < BLOCK_SIZE; i++)
    {
        nand_mx35_read_page(i, 0, b, 2, NULL);
        if (b[1] == 0x00 || b[2] == 0x00)
        {
            bad_blocks_map[bad_blocks_count++] = i;
            ESP_LOGW(TAG, "Bad block found at index %d", i);
        }
    }
}

static mx35_err_t nand_mx35_check_block(uint16_t block)
{
    for (uint8_t i = 0; i < bad_blocks_count; i++)
        if (bad_blocks_map[i] == block)
            return MX35_INVALID_BLOCK;
    return MX35_OK;
}


#if CONFIG_NAND_MX35_DEBUG_GET_REG

static void Test_GET_Registers_BlockProtection()
{
    uint8_t reg = nand_mx35lf_GET_Features(REG_BLOCK_PROTECTION);
    printf("RESULT: 0x%02X\r\n", reg);

    printf("bit[0] - [SP]            - %d\r\n", (reg >> 0) & 0x01);
    printf("bit[1] - [Complementary] - %d\r\n", (reg >> 1) & 0x01);
    printf("bit[2] - [Invert]        - %d\r\n", (reg >> 2) & 0x01);
    printf("bit[3] - [BP0]           - %d\r\n", (reg >> 3) & 0x01);
    printf("bit[4] - [BP1]           - %d\r\n", (reg >> 4) & 0x01);
    printf("bit[5] - [BP2]           - %d\r\n", (reg >> 5) & 0x01);
    printf("bit[6] - [Reserved]      - %d\r\n", (reg >> 6) & 0x01);
    printf("bit[7] - [BPRWD]         - %d\r\n", (reg >> 7) & 0x01);
    printf("\r\n");
}

static void Test_GET_Registers_SecureOTP()
{
    uint8_t reg = nand_mx35lf_GET_Features(REG_SECURE_OTP);
    printf("RESULT: 0x%02X\r\n", reg);

    printf("bit[0] - [QE]                 - %d\r\n", (reg >> 0) & 0x01);
    printf("bit[1] - [Reserved]           - %d\r\n", (reg >> 1) & 0x01);
    printf("bit[2] - [Reserved]           - %d\r\n", (reg >> 2) & 0x01);
    printf("bit[3] - [Reserved]           - %d\r\n", (reg >> 3) & 0x01);
    printf("bit[4] - [ECC enabled]        - %d\r\n", (reg >> 4) & 0x01);
    printf("bit[5] - [Reserved]           - %d\r\n", (reg >> 5) & 0x01);
    printf("bit[6] - [Secure OTP Enable]  - %d\r\n", (reg >> 6) & 0x01);
    printf("bit[7] - [Secure OTP Protect] - %d\r\n", (reg >> 7) & 0x01);
    printf("\r\n");
}

static void Test_GET_Registers_Status()
{
    uint8_t reg = nand_mx35lf_GET_Features(REG_STATUS);
    printf("RESULT: 0x%02X\r\n", reg);

    printf("bit[0] - [OIP]      - %d\r\n", (reg >> 0) & 0x01);
    printf("bit[1] - [WEL]      - %d\r\n", (reg >> 1) & 0x01);
    printf("bit[2] - [E_Fail]   - %d\r\n", (reg >> 2) & 0x01);
    printf("bit[3] - [P-Fail]   - %d\r\n", (reg >> 3) & 0x01);
    printf("bit[4] - [ECC_S0]   - %d\r\n", (reg >> 4) & 0x01);
    printf("bit[5] - [ECC_S1]   - %d\r\n", (reg >> 5) & 0x01);
    printf("bit[6] - [CRBSY]    - %d\r\n", (reg >> 6) & 0x01);
    printf("bit[7] - [Reserved] - %d\r\n", (reg >> 7) & 0x01);
    printf("\r\n");
}

static void Test_GET_Registers_InternalECC()
{
    uint8_t reg = nand_mx35lf_GET_Features(REG_INTERNAL_ECC_STATUS);
    printf("RESULT: 0x%02X\r\n", reg);

    printf("bit[0] - [ECCSR[0]] - %d\r\n", (reg >> 0) & 0x01);
    printf("bit[1] - [ECCSR[1]] - %d\r\n", (reg >> 1) & 0x01);
    printf("bit[2] - [ECCSR[2]] - %d\r\n", (reg >> 2) & 0x01);
    printf("bit[3] - [ECCSR[3]] - %d\r\n", (reg >> 3) & 0x01);
    printf("bit[4] - [Reserved] - %d\r\n", (reg >> 4) & 0x01);
    printf("bit[5] - [Reserved] - %d\r\n", (reg >> 5) & 0x01);
    printf("bit[6] - [Reserved] - %d\r\n", (reg >> 6) & 0x01);
    printf("bit[7] - [Reserved] - %d\r\n", (reg >> 7) & 0x01);
    printf("\r\n");
}

#endif


static void nand_mx35_start_program_mode(void)
{
    DISABLE_WP;
    nand_mx35lf_SET_Features(REG_BLOCK_PROTECTION, 0x00);  // Unprotect all blocks
    nand_mx35_write_enable();
}

static void nand_mx35_stop_program_mode(void)
{
    nand_mx35_write_disable();
    nand_mx35lf_SET_Features(REG_BLOCK_PROTECTION, BP0_BIT | BP1_BIT | BP2_BIT);  // Protect all blocks
    ENABLE_WP;
}

#define START_PROGRAM_MODE() nand_mx35_start_program_mode()
#define STOP_PROGRAM_MODE()  nand_mx35_stop_program_mode()

//--- Public ----------------------------------------------------------

mx35_err_t nand_mx35_init(const nand_mx35_config_t *cfg)
{
    if (cfg == NULL)
        return MX35_INVALID_ARGUMENT;

    // esp_log_level_set(TAG, ESP_LOG_DEBUG);

    mx35_ctx.cfg = *cfg;
    mx35_ctx.spi_host = SPI_BUS_HOST;

    spi_bus_config_t buscfg = {
        .mosi_io_num = (int) cfg->spi_pins.mosi_io,
        .miso_io_num = (int) cfg->spi_pins.miso_io,
        .sclk_io_num = (int) cfg->spi_pins.sclk_io,
        .quadhd_io_num = (int) cfg->spi_pins.hd_io,
        .quadwp_io_num = (int) cfg->spi_pins.wp_io,
        .max_transfer_sz = 4095,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = MX35_CLOCK_FREQUENCY,
        .mode = 0,
        .spics_io_num = cfg->spi_pins.cs_io,
        .queue_size = 10,
    };

    esp_err_t ret = spi_bus_initialize(mx35_ctx.spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret)
    {
        ESP_LOGE(TAG, "Error to initialize the SPI bus");
        goto cleanup;
    }

    ESP_LOGI(TAG, "SPI Initialize");
    ret = spi_bus_add_device(mx35_ctx.spi_host, &devcfg, &mx35_ctx.spi);

    gpio_config_t out_cfg = {
        .pin_bit_mask = BIT64(cfg->spi_pins.cs_io) | BIT64(cfg->spi_pins.hd_io) | BIT64(cfg->spi_pins.wp_io),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);

    MX35_UNSELECT;
    DISABLE_WP;
    DISABLE_HOLD;

    nand_mx35_reset();
    if (!nand_mx35_get_id())
    {
        ESP_LOGE(TAG, "Dont communicate with the MX35 module");
        goto cleanup;
    }
    nand_mx35_verify_bad_blocks();

    nand_mx35_write_disable();

#if CONFIG_NAND_MX35_DEBUG_GET_REG
    Test_GET_Registers_BlockProtection();
    Test_GET_Registers_SecureOTP();
    Test_GET_Registers_Status();
    Test_GET_Registers_InternalECC();
#endif

    MX35_UNSELECT;
    DISABLE_HOLD;
    ENABLE_WP;
    return MX35_OK;

cleanup:
    if (mx35_ctx.spi)
    {
        spi_bus_remove_device(mx35_ctx.spi);
        mx35_ctx.spi = NULL;
    }
    return MX35_FAIL;
}


mx35_err_t nand_mx35_deinit()
{
    if (mx35_ctx.spi)
        spi_bus_remove_device(mx35_ctx.spi);

    ESP_LOGW(TAG, "Deinit nand_mx35");
    return MX35_OK;
}


mx35_err_t nand_mx35_erase_block(uint16_t block)
{
    if (block >= BLOCK_SIZE || block == 0)
        return MX35_INVALID_ARGUMENT;

    if (nand_mx35_check_block(block) != MX35_OK)
    {
        ESP_LOGW(TAG, "Block %d is a bad block. Erase operation skipped.", block);
        return MX35_INVALID_BLOCK;
    }

    uint16_t page_address = block << 6;  // Block address[15:6] + Page address[5:0] = 0
    START_PROGRAM_MODE();

    uint8_t cmd[] = {
        CMD_BLOCK_ERASE,
        0x00,
        (uint8_t) ((page_address >> 8) & 0xFF),
        (uint8_t) (page_address & 0xFF),
    };
    spi_write_read(cmd, sizeof(cmd), NULL);
    // vTaskDelay(pdMS_TO_TICKS(1));

    mx35_err_t ret = WaitOperationDone();
    if (ret != MX35_OK)
    {
        ESP_LOGE(TAG, "Timeout in erase block operation");
    }

    else
    {
        uint8_t status = nand_mx35lf_GET_Features(REG_STATUS);
        // ESP_LOGW(TAG, "status: %d", status);
        if (status & ERS_FAIL_BIT)
        {
            ESP_LOGE(TAG, "Erase error in the block %d", block);
            ret = MX35_FAIL;
        }

        else
        {
            ESP_LOGD(TAG, "Block %d erased successfully", block);
            ret = MX35_OK;
        }
    }

    STOP_PROGRAM_MODE();
    return ret;
}


mx35_err_t nand_mx35_bulk_erase()
{
    for (int16_t i = 1; i < 1024; i++)
    {
        if (nand_mx35_erase_block(i) != MX35_OK)
        {
            // return MX35_FAIL;
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return MX35_OK;
}


mx35_err_t nand_mx35_write_page(uint16_t block, uint8_t page, uint8_t *buffer, size_t len, uint16_t *page_address)
{
    if (!buffer || len == 0 || block >= BLOCK_SIZE || page >= NUM_PAGES_PER_BLOCK)
        return MX35_INVALID_ARGUMENT;

    mx35_err_t ret = MX35_OK;
    size_t bytes_written = 0;
    uint16_t current_block = block;
    uint8_t current_page = page;
    uint16_t address = 0;

    while (bytes_written < len)
    {
        ret = nand_mx35_check_block(block);
        if (ret != MX35_OK)
        {
            ESP_LOGW(TAG, "Skipping bad block %d during write operation", current_block);
            current_block++;
            current_page = 0;
            continue;
        }

        size_t chunk = (len - bytes_written > 2048) ? 2048 : (len - bytes_written);
        address = (current_block << 6) | current_page;

        START_PROGRAM_MODE();
        ret = nand_mx35_program_load(buffer + bytes_written, chunk);
        if (ret != MX35_OK)
        {
            ESP_LOGE(TAG, "Error in program load no bloco %d, pag %d", current_block, current_page);
            goto end;
        }

        nand_mx35_program_execute(address);

        ret = WaitOperationDone();
        if (ret != MX35_OK)
        {
            ESP_LOGE(TAG, "Timeout tPROG no bloco %d", current_block);
            goto end;
        }

        uint8_t status = nand_mx35lf_GET_Features(REG_STATUS);
        if (status & PGM_FAIL_BIT)
        {
            ESP_LOGE(TAG, "Program fail in the block %d, page %d", current_block, current_page);
            ret = MX35_WRITE_FAIL;
            goto end;
        }

        bytes_written += chunk;
        current_page++;
        if (current_page >= NUM_PAGES_PER_BLOCK)
        {
            current_page = 0;
            current_block++;
        }
    }

end:
    STOP_PROGRAM_MODE();

    if (page_address != NULL)
        *page_address = address;

    if (ret == MX35_OK)
    {
        ESP_LOGD(TAG, "Complete sequential writing (B:%u P:%u of %u bytes)", address >> 6, address & 0x3F, len);
    }

    return ret;
}


mx35_err_t nand_mx35_read_page(uint16_t block, uint8_t page, uint8_t *buffer, size_t len, uint16_t *page_address)
{
    if (!buffer || len == 0 || block >= BLOCK_SIZE || block == 0 || page > NUM_PAGES_PER_BLOCK)
        return MX35_INVALID_ARGUMENT;

    mx35_err_t ret = MX35_OK;
    size_t bytes_read = 0;
    uint16_t current_block = block;
    uint8_t current_page = page;
    uint16_t address = 0x00;

    while (bytes_read < len)
    {
        ret = nand_mx35_check_block(block);
        if (ret != MX35_OK)
        {
            ESP_LOGW(TAG, "Skipping bad block %d during read operation", current_block);
            current_block++;
            current_page = 0;
            continue;
        }

        size_t chunk = (len - bytes_read > 2048) ? 2048 : (len - bytes_read);
        address = (current_block << 6) | current_page;

        uint8_t cmd_page_read[4] = {
            CMD_PAGE_READ,
            0x00,
            (uint8_t) ((address >> 8) & 0xFF),
            (uint8_t) (address & 0xFF),
        };
        spi_write_read(cmd_page_read, sizeof(cmd_page_read), NULL);

        vTaskDelay(pdMS_TO_TICKS(1));

        if (WaitOperationDone() != MX35_OK)
        {
            ESP_LOGE(TAG, "Timeout in page read operation");
            return MX35_READ_FAIL;
        }

        uint8_t cmd_read_cache[4] = {CMD_READ_FROM_CACHE, FINAL_PAGE_ADDRESS_2048 << 6, 0x00, 0x00};
        spi_transaction_t t = {
            .length = sizeof(cmd_read_cache) * 8,
            .tx_buffer = cmd_read_cache,
        };

        MX35_SELECT;
        spi_device_polling_transmit(mx35_ctx.spi, &t);
        t.length = chunk * 8;
        t.rxlength = chunk * 8;
        t.tx_buffer = NULL;
        t.rx_buffer = buffer + bytes_read;
        spi_device_polling_transmit(mx35_ctx.spi, &t);
        MX35_UNSELECT;

        bytes_read += chunk;
        current_page++;
        if (current_page >= NUM_PAGES_PER_BLOCK)
        {
            current_page = 0;
            current_block++;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (page_address != NULL)
        *page_address = address;

    ESP_LOGD(TAG, "Complete sequential reading (B:%u P:%u of %u bytes)", address >> 6, address & 0x3F, len);
    return MX35_OK;
}
