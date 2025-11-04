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
static nand_mx35_handle_t mx35_ctx;

#define MX35_SELECT   (gpio_set_level(mx35_ctx->cfg.spi_pins.cs_io, 0))
#define MX35_UNSELECT (gpio_set_level(mx35_ctx->cfg.spi_pins.cs_io, 1))

#define ENABLE_WP  (gpio_set_level(mx35_ctx->cfg.spi_pins.wp_io, 0))
#define DISABLE_WP (gpio_set_level(mx35_ctx->cfg.spi_pins.wp_io, 1))

#define ENABLE_HOLD  (gpio_set_level(mx35_ctx->cfg.spi_pins.hd_io, 0))
#define DISABLE_HOLD (gpio_set_level(mx35_ctx->cfg.spi_pins.hd_io, 1))

//--- Private ----------------------------------------------------------

static esp_err_t spi_write_read(const uint8_t *cmd, const uint8_t len, uint8_t *rx)
{
    if (len == 0 || cmd == NULL)
        return ESP_ERR_INVALID_ARG;  // no need to send anything

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = cmd,
        .flags = SPI_TRANS_USE_RXDATA,
    };

    MX35_SELECT;
    esp_err_t ret = spi_device_polling_transmit(mx35_ctx->spi, &t);  // Transmit!
    MX35_UNSELECT;

    if (rx != NULL)
    {
        ESP_LOGD(TAG, "RECEIVED: [0x%02X, 0x%02X, 0x%02X, 0x%02X]", t.rx_data[0], t.rx_data[1], t.rx_data[2], t.rx_data[3]);
        memcpy(rx, t.rx_data, 4);
    }

    return ret;
}


static void nand_mx35lf_SET_Features(uint8_t address, uint8_t value)
{
    uint8_t cmd[] = {CMD_SET_FEATURES, address, value};
    spi_write_read(cmd, sizeof(cmd), NULL);
    return;
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
    uint32_t time_reference = esp_timer_get_time() / 1000;
    const unsigned long timeout = 500;
    while (((uint32_t) (esp_timer_get_time() / 1000) - time_reference) < timeout)
        if ((nand_mx35lf_GET_Features(REG_STATUS) & 0x01) == STATUS_READY)
            return MX35_OK;
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


static void nand_mx35_program_load(uint8_t *data, size_t len)
{
    uint8_t wrap_bit = FINAL_PAGE_ADDRESS_2048 << 6;  // default in 2048 bytes
    uint8_t cmd[3] = {CMD_PROGRAM_LOAD_X1, wrap_bit, 0x00};
    spi_write_read(cmd, sizeof(cmd), NULL);
    spi_write_read(data, len, NULL);
}

static void nand_mx35_program_execute(uint16_t page_address)
{
    uint8_t cmd[4] = {
        CMD_PROGRAM_EXECUTE,
        (uint8_t) ((page_address >> 16) & 0xFF),
        (uint8_t) ((page_address >> 8) & 0xFF),
        (uint8_t) ((page_address >> 0) & 0xFF),
    };
    spi_write_read(cmd, sizeof(cmd), NULL);
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
    spi_device_polling_transmit(mx35_ctx->spi, &trans_desc);
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

//--- Public ----------------------------------------------------------

mx35_err_t nand_mx35_init(const nand_mx35_config_t *cfg)
{
    if (cfg == NULL)
        return MX35_INVALID_ARGUMENT;

    // esp_log_level_set(TAG, ESP_LOG_DEBUG);

    mx35_ctx = (nand_mx35_context_t *) malloc(sizeof(nand_mx35_context_t));
    if (!mx35_ctx)
        return MX35_NO_MEM;

    *mx35_ctx = (nand_mx35_context_t) {
        .cfg = *cfg,
        .spi_host = SPI_BUS_HOST,
    };

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

    esp_err_t ret = spi_bus_initialize(SPI_BUS_HOST, &buscfg, SPI_DMA_CH_AUTO);

    if (ret)
    {
        ESP_LOGE(TAG, "Error to initialize the SPI bus");
        goto cleanup;
    }

    ESP_LOGI(TAG, "SPI Initialize");
    ret = spi_bus_add_device(SPI_BUS_HOST, &devcfg, &mx35_ctx->spi);

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
    if (mx35_ctx->spi)
    {
        spi_bus_remove_device(mx35_ctx->spi);
        mx35_ctx->spi = NULL;
    }

    free(mx35_ctx);
    return MX35_FAIL;
}


mx35_err_t nand_mx35_deinit()
{
    if (mx35_ctx->spi)
        spi_bus_remove_device(mx35_ctx->spi);

    if (mx35_ctx)
        free(mx35_ctx);

    ESP_LOGW(TAG, "Deinit nand_mx35");
    return MX35_OK;
}


mx35_err_t nand_mx35_erase_block(uint16_t page_address)
{
    DISABLE_WP;
    nand_mx35_write_enable();

    uint8_t cmd[] = {
        CMD_BLOCK_ERASE,
        (uint8_t) ((page_address >> 16) & 0xFF),
        (uint8_t) ((page_address >> 8) & 0xFF),
        (uint8_t) ((page_address >> 0) & 0xFF),
    };
    spi_write_read(cmd, sizeof(cmd), NULL);
    // vTaskDelay(pdMS_TO_TICKS(1));

    if (WaitOperationDone())
    {
        ESP_LOGE(TAG, "error to erase block (page=0x%06X)", page_address);
        return MX35_FAIL;
    }

    // nand_mx35_write_disable();
    // ENABLE_WP;

    return MX35_OK;
}


mx35_err_t nand_mx35_bulk_erase()
{
    for (uint16_t i = 0; i < (MAX_PAGE_SIZE - NUM_PAGES_PER_BLOCK); i += NUM_PAGES_PER_BLOCK)
        if (nand_mx35_erase_block(i))
            return MX35_FAIL;
    return MX35_OK;
}


mx35_err_t nand_mx35_write_page(uint16_t start_page, uint8_t *buffer, size_t len, uint16_t *page_address)
{
    uint16_t num_pages = (len + PAGE_SIZE_WITHOUT_ECC - 1) / PAGE_SIZE_WITHOUT_ECC;

    DISABLE_WP;
    nand_mx35_write_enable();

    for (uint16_t i = 0; i < num_pages; i++)
    {
        uint32_t offset = i * PAGE_SIZE_WITHOUT_ECC;
        size_t chunk = (len - offset > PAGE_SIZE_WITHOUT_ECC) ? PAGE_SIZE_WITHOUT_ECC : (len - offset);

        nand_mx35_program_load(&buffer[offset], chunk);
        nand_mx35_program_execute(start_page + i);

        if (WaitOperationDone())
        {
            ESP_LOGE(TAG, "Error to programming the buffer");
            return MX35_FAIL;
        }

        uint8_t status = nand_mx35lf_GET_Features(REG_STATUS);
        // ESP_LOGW(TAG, "status: %d", status);
        if (status & 0x04)
        {
            ESP_LOGE(TAG, "Programming error in the block %d, page %d", (start_page + i) / 64, start_page + i);
            return MX35_FAIL;
        }
    }

    nand_mx35_write_disable();
    ENABLE_WP;
    uint16_t final_address = start_page + num_pages;

    if (page_address != NULL)
        *page_address = final_address;

    ESP_LOGI(TAG, "Recording completed: %u bytes, final position in block %u and page %d", len, Page_To_Block(final_address), final_address);
    return MX35_OK;
}


mx35_err_t nand_mx35_read_page(uint16_t start_page, uint16_t final_page, uint8_t *buffer, size_t len)
{
    if (!buffer)
        return MX35_INVALID_ARGUMENT;

    uint32_t current_row = start_page;

    uint8_t cmd_page_read[4] = {
        CMD_PAGE_READ,
        (uint8_t) ((current_row >> 16) & 0xFF),
        (uint8_t) ((current_row >> 8) & 0xFF),
        (uint8_t) (current_row & 0xFF),
    };

    MX35_SELECT;
    spi_device_polling_transmit(
        mx35_ctx->spi,
        &(spi_transaction_t) {
            .length = 32,
            .tx_buffer = cmd_page_read,
        });
    MX35_UNSELECT;

    vTaskDelay(pdMS_TO_TICKS(1));

    if (WaitOperationDone() != MX35_OK)
        return MX35_FAIL;

    if (current_row == (final_page - 1))
    {
        uint8_t cmd_read_cache[3] = {CMD_READ_FROM_CACHE, 0x00, 0x00};
        MX35_SELECT;
        spi_device_polling_transmit(
            mx35_ctx->spi,
            &(spi_transaction_t) {
                .length = sizeof(cmd_read_cache) * 8,
                .tx_buffer = cmd_read_cache,
            });

        spi_device_polling_transmit(
            mx35_ctx->spi,
            &(spi_transaction_t) {
                .length = PAGE_SIZE_WITHOUT_ECC * 8,
                .rxlength = len * 8,
                .rx_buffer = buffer,
            });
        MX35_UNSELECT;

        goto end_read;
    }

    for (uint8_t i = start_page; i < final_page; i++)
    {
        uint8_t cmd_read_cache[3] = {CMD_READ_FROM_CACHE, 0x00, 0x00};
        MX35_SELECT;
        spi_device_polling_transmit(
            mx35_ctx->spi,
            &(spi_transaction_t) {
                .length = sizeof(cmd_read_cache) * 8,
                .tx_buffer = cmd_read_cache,
            });

        spi_device_polling_transmit(
            mx35_ctx->spi,
            &(spi_transaction_t) {
                .length = PAGE_SIZE_WITHOUT_ECC * 8,
                .rxlength = PAGE_SIZE_WITHOUT_ECC * 8,
                .rx_buffer = buffer + (i * PAGE_SIZE_WITHOUT_ECC),
            });
        MX35_UNSELECT;

        if (i < final_page - 1)
        {
            current_row++;
            uint8_t cmd_next_page[4] = {
                CMD_PAGE_READ_CACHE_SEQUENTIAL,
                (uint8_t) ((current_row >> 16) & 0xFF),
                (uint8_t) ((current_row >> 8) & 0xFF),
                (uint8_t) (current_row & 0xFF),
            };

            MX35_SELECT;
            spi_device_polling_transmit(
                mx35_ctx->spi,
                &(spi_transaction_t) {
                    .length = 32,
                    .tx_buffer = cmd_next_page,
                });
            MX35_UNSELECT;

            if (WaitOperationDone() != MX35_OK)
                return MX35_FAIL;
        }

        else
        {
            uint8_t cmd_end[4] = {
                CMD_PAGE_READ_CACHE_END,
                (uint8_t) ((current_row >> 16) & 0xFF),
                (uint8_t) ((current_row >> 8) & 0xFF),
                (uint8_t) (current_row & 0xFF),
            };

            MX35_SELECT;
            spi_device_polling_transmit(
                mx35_ctx->spi,
                &(spi_transaction_t) {
                    .length = 32,
                    .tx_buffer = cmd_end,
                });
            MX35_UNSELECT;

            WaitOperationDone();
        }
    }

end_read:
    ESP_LOGI(TAG, "Complete sequential reading (%d pages of %d bytes)", final_page, PAGE_SIZE_WITHOUT_ECC);
    return MX35_OK;
}
