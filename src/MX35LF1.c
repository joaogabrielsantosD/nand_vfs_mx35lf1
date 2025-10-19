#include "MX35LF1.h"

#include <string.h>

#define MX35_CLOCK_FREQUENCY (10 * 1000 * 1000)  // 10 MHz

#define SELECT_WP(pin)   (gpio_set_level(pin, 0))
#define UNSELECT_WP(pin) (gpio_set_level(pin, 1))

#if CONFIG_NAND_MX35_SPI2_BUS
#    define SPI_BUS_HOST SPI2_HOST
#elif CONFIG_NAND_MX35_SPI3_BUS
#    define SPI_BUS_HOST SPI3_HOST
#endif

static const char *TAG = "NAND MX35";
static nand_mx35_handle_t mx35_ctx;

static esp_err_t _spi_write(uint8_t td, const uint8_t *cmd, const uint8_t len)
{
    esp_err_t ret;
    if (len == 0)
        return ESP_ERR_INVALID_ARG;  //no need to send anything

    // spi_device_acquire_bus(mx35_dev, portMAX_DELAY);

    uint8_t rx[4];

    spi_transaction_t t = {
        // .cmd = *(cmd + 0),
        .length = len * 8,
        .tx_buffer = cmd,
        .rx_buffer = rx,
    };

    ret = spi_device_polling_transmit(mx35_ctx->spi, &t);  // Transmit!
    // ret = spi_device_transmit(mx35_dev, &t);
    assert(ret == ESP_OK);

    // spi_device_release_bus(mx35_dev);

    ESP_LOGD(TAG, "RECEIVED: [0x%02X, 0x%02X, 0x%02X, 0x%02X]", rx[0], rx[1], rx[2], rx[3]);

    return ret;
}

// static esp_err_t spi_write(nand_mx35_handle_t ctx, const uint8_t *cmd, const uint8_t len)
// {
//     esp_err_t ret;
//     if (len == 0)
//         return ESP_ERR_INVALID_ARG;  //no need to send anything

//     // spi_device_acquire_bus(mx35_dev, portMAX_DELAY);

//     uint8_t rx[4];

//     spi_transaction_t t = {
//         // .cmd = *(cmd + 0),
//         .length = len * 8,
//         .tx_buffer = cmd,
//         .rx_buffer = rx,
//     };

//     ret = spi_device_polling_transmit(mx35_dev, &t);  // Transmit!
//     // ret = spi_device_transmit(mx35_dev, &t);
//     assert(ret == ESP_OK);

//     // spi_device_release_bus(mx35_dev);

//     ESP_LOGD(TAG, "RECEIVED: [0x%02X, 0x%02X, 0x%02X, 0x%02X]", rx[0], rx[1], rx[2], rx[3]);

//     return ret;
// }

// static uint8_t spi_read()
// {
//     spi_transaction_t t;
//     memset(&t, 0, sizeof(t));
//     t.length = 8;
//     t.tx_buffer = (void *) 0;
//     t.flags = SPI_TRANS_USE_RXDATA;

//     esp_err_t ret = spi_device_polling_transmit(mx35_dev, &t);
//     assert(ret == ESP_OK);

//     ESP_LOGD(TAG, "RECEIVED: [0x%02X, 0x%02X, 0x%02X, 0x%02X]", t.rx_data[0], t.rx_data[1], t.rx_data[2], t.rx_data[3]);

//     return *(uint8_t *) t.rx_data;
// }

// static uint8_t nand_mx35lf_GET_Features(uint8_t address)
// {
//     ESP_LOGV(TAG, "uint8_t nand_mx35lf_GET_Features(...)");

//     // uint8_t cmd[] = {CMD_GET_FEATURES, address};
//     // spi_write(cmd, sizeof(cmd));
//     // uint8_t ret = spi_read();

//     return 1;
// }

// static void Test_GET_Registers_BlockProtection()
// {
//     ESP_LOGV(TAG, "void Test_GET_Registers_BlockProtection()");

//     uint8_t reg = nand_mx35lf_GET_Features(REG_BLOCK_PROTECTION);
//     printf("RESULT: 0x%02X\r\n", reg);

//     printf("bit[0] - [SP]            - %d\r\n", (reg >> 0) & 0x01);
//     printf("bit[1] - [Complementary] - %d\r\n", (reg >> 1) & 0x01);
//     printf("bit[2] - [Invert]        - %d\r\n", (reg >> 2) & 0x01);
//     printf("bit[3] - [BP0]           - %d\r\n", (reg >> 3) & 0x01);
//     printf("bit[4] - [BP1]           - %d\r\n", (reg >> 4) & 0x01);
//     printf("bit[5] - [BP2]           - %d\r\n", (reg >> 5) & 0x01);
//     printf("bit[6] - [Reserved]      - %d\r\n", (reg >> 6) & 0x01);
//     printf("bit[7] - [BPRWD]         - %d\r\n", (reg >> 7) & 0x01);
//     printf("\r\n");
// }

// static void Test_GET_Registers_SecureOTP()
// {
//     ESP_LOGV(TAG, "void Test_GET_Registers_SecureOTP()");

//     uint8_t reg = nand_mx35lf_GET_Features(REG_SECURE_OTP);
//     printf("RESULT: 0x%02X\r\n", reg);

//     printf("bit[0] - [QE]                 - %d\r\n", (reg >> 0) & 0x01);
//     printf("bit[1] - [Reserved]           - %d\r\n", (reg >> 1) & 0x01);
//     printf("bit[2] - [Reserved]           - %d\r\n", (reg >> 2) & 0x01);
//     printf("bit[3] - [Reserved]           - %d\r\n", (reg >> 3) & 0x01);
//     printf("bit[4] - [ECC enabled]        - %d\r\n", (reg >> 4) & 0x01);
//     printf("bit[5] - [Reserved]           - %d\r\n", (reg >> 5) & 0x01);
//     printf("bit[6] - [Secure OTP Enable]  - %d\r\n", (reg >> 6) & 0x01);
//     printf("bit[7] - [Secure OTP Protect] - %d\r\n", (reg >> 7) & 0x01);
//     printf("\r\n");
// }

// static void Test_GET_Registers_Status()
// {
//     ESP_LOGV(TAG, "void Test_GET_Registers_Status()");

//     uint8_t reg = nand_mx35lf_GET_Features(REG_STATUS);
//     printf("RESULT: 0x%02X\r\n", reg);

//     printf("bit[0] - [OIP]      - %d\r\n", (reg >> 0) & 0x01);
//     printf("bit[1] - [WEL]      - %d\r\n", (reg >> 1) & 0x01);
//     printf("bit[2] - [E_Fail]   - %d\r\n", (reg >> 2) & 0x01);
//     printf("bit[3] - [P-Fail]   - %d\r\n", (reg >> 3) & 0x01);
//     printf("bit[4] - [ECC_S0]   - %d\r\n", (reg >> 4) & 0x01);
//     printf("bit[5] - [ECC_S1]   - %d\r\n", (reg >> 5) & 0x01);
//     printf("bit[6] - [CRBSY]    - %d\r\n", (reg >> 6) & 0x01);
//     printf("bit[7] - [Reserved] - %d\r\n", (reg >> 7) & 0x01);
//     printf("\r\n");
// }

static void mx35_reset(nand_mx35_handle_t ctx)
{
    // Internal ECC status going to zero
    SELECT_WP(ctx->cfg.spi_pins.wp_io);

    uint8_t cmd[] = {CMD_RESET, 0xFF, 0xFF, 0xFF, 0xFF};
    spi_transaction_t t = {.length = 8, .tx_buffer = cmd};

    spi_device_polling_transmit(ctx->spi, &t);  // Transmit!

    vTaskDelay(pdMS_TO_TICKS(2));
    UNSELECT_WP(ctx->cfg.spi_pins.wp_io);

    vTaskDelay(pdMS_TO_TICKS(250));
}

mx35_err_t nand_mx35_init(const nand_mx35_config_t *cfg)
{
    if (cfg == NULL)
        return MX35_INVALID_ARGUMENT;

    esp_log_level_set(TAG, ESP_LOG_VERBOSE);
    ESP_LOGV(TAG, "mx35_err_t nand_mx35_config(...)");

    nand_mx35_context_t *ctx = (nand_mx35_context_t *) malloc(sizeof(nand_mx35_context_t));
    if (!ctx)
        return MX35_NO_MEM;

    *ctx = (nand_mx35_context_t) {
        .cfg = *cfg,
        .spi_host = SPI_BUS_HOST,
    };

    spi_bus_config_t buscfg = {
        .mosi_io_num = (int) cfg->spi_pins.mosi_io,
        .miso_io_num = (int) cfg->spi_pins.miso_io,
        .sclk_io_num = (int) cfg->spi_pins.sclk_io,
        .quadhd_io_num = (int) cfg->spi_pins.hd_io,
        .quadwp_io_num = (int) cfg->spi_pins.wp_io,
        .max_transfer_sz = 0,  // driver decides
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = MX35_CLOCK_FREQUENCY,
        .mode = 0,
        .spics_io_num = cfg->spi_pins.cs_io,
        .queue_size = 128,
    };

    esp_err_t ret = spi_bus_initialize(SPI_BUS_HOST, &buscfg, SPI_DMA_CH_AUTO);

    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGI(TAG, "SPI Initialize");
        ret = spi_bus_add_device(SPI_BUS_HOST, &devcfg, &ctx->spi);
    }

    if (ret)
    {
        ESP_LOGE(TAG, "Error to initialize the SPI bus");
        goto cleanup;
    }

    mx35_ctx = ctx;

    gpio_config_t out_cfg = {
        .pin_bit_mask = BIT64(cfg->spi_pins.cs_io) | BIT64(cfg->spi_pins.hd_io) | BIT64(cfg->spi_pins.wp_io),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out_cfg);

    gpio_set_level(cfg->spi_pins.cs_io, 0);
    gpio_set_level(cfg->spi_pins.hd_io, 1);
    UNSELECT_WP(cfg->spi_pins.wp_io);

    // mx35_reset(ctx);

    // função de get_id
    UNSELECT_WP(ctx->cfg.spi_pins.wp_io);
    uint8_t cmd[] = {CMD_READ_MANUFACTURER_ID, 0xFF, 0xFF, 0xFF};
    _spi_write(1, cmd, sizeof(cmd));

    // Test_GET_Registers_BlockProtection();
    // Test_GET_Registers_SecureOTP();
    // Test_GET_Registers_Status();

    SELECT_WP(ctx->cfg.spi_pins.wp_io);
    return MX35_OK;

cleanup:
    if (ctx->spi)
    {
        spi_bus_remove_device(ctx->spi);
        ctx->spi = NULL;
    }

    free(ctx);
    return MX35_FAIL;
}

mx35_err_t nand_mx35_deinit()
{
    if (mx35_ctx->spi)
        spi_bus_remove_device(mx35_ctx->spi);

    free(mx35_ctx);
    return MX35_OK;
}