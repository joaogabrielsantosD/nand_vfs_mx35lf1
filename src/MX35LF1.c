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

static esp_err_t spi_write_read(const uint8_t *cmd, const uint8_t len, uint8_t *rx)
{
    if (len == 0)
        return ESP_ERR_INVALID_ARG;  //no need to send anything

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = cmd,
        .flags = SPI_TRANS_USE_RXDATA,
    };

    MX35_SELECT;
    esp_err_t ret = spi_device_polling_transmit(mx35_ctx->spi, &t);  // Transmit!
    MX35_UNSELECT;

    ESP_LOGD(TAG, "RECEIVED: [0x%02X, 0x%02X, 0x%02X, 0x%02X]", t.rx_data[0], t.rx_data[1], t.rx_data[2], t.rx_data[3]);

    if (rx != NULL)
        memcpy(rx, t.rx_data, 4);

    return ret;
}

static void nand_mx35lf_SET_Features(uint8_t address, uint8_t value)
{
    ESP_LOGV(TAG, "void mx35lf_SET_features()");

    uint8_t cmd[] = {CMD_SET_FEATURES, address, value};
    spi_write_read(cmd, sizeof(cmd), NULL);
    return;
}

static uint8_t nand_mx35lf_GET_Features(uint8_t address)
{
    ESP_LOGV(TAG, "uint8_t nand_mx35lf_GET_Features(...)");

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
    uint8_t _rx[4];
    spi_write_read(cmd, sizeof(cmd), _rx);

    return _rx[2] & WEL_BIT;
}

static bool nand_mx35_write_disable()
{
    uint8_t cmd[] = {CMD_WRITE_DISABLE};
    uint8_t _rx[4];
    spi_write_read(cmd, sizeof(cmd), _rx);

    return (_rx[2] & WEL_BIT) == 0x00;
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
    ESP_LOGV(TAG, "void Test_GET_Registers_BlockProtection()");

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
    ESP_LOGV(TAG, "void Test_GET_Registers_SecureOTP()");

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
    ESP_LOGV(TAG, "void Test_GET_Registers_Status()");

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

#endif

mx35_err_t nand_mx35_init(const nand_mx35_config_t *cfg)
{
    if (cfg == NULL)
        return MX35_INVALID_ARGUMENT;

    esp_log_level_set(TAG, ESP_LOG_VERBOSE);
    ESP_LOGV(TAG, "mx35_err_t nand_mx35_config(...)");

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
    ESP_LOGV(TAG, "mx35_err_t nand_mx35_deinit()");

    if (mx35_ctx->spi)
        spi_bus_remove_device(mx35_ctx->spi);

    if (mx35_ctx)
        free(mx35_ctx);

    return MX35_OK;
}