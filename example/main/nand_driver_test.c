#include "mx35lf1_driver.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <esp_log.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// #define TEST_BLOCK 23U
// #define TEST_PAGE  5U

#define TEST_BLOCK 773U
#define TEST_PAGE  0U

#define NAND_SPI_HOST SPI2_HOST
#define NAND_PIN_MOSI 11
#define NAND_PIN_SCLK 12
#define NAND_PIN_MISO 13
#define NAND_PIN_CS   15
#define NAND_PIN_WP   16                 /* -1 if not connected */
#define NAND_PIN_HOLD 17                 /* -1 if not connected */
#define NAND_CLOCK_HZ (50 * 1000 * 1000) /* 40 MHz — conservative */

static const char TAG[] = "NAND_STORAGE";
static const char msg[] = "Exemplo de teste para driver da memoria nand mx35lf1 para implementar no dongle v3....";
static char receive[sizeof(msg)];
static char copied_data[sizeof(msg)];
static nand_handle_t handle;

void app_main(void)
{
    ESP_LOGI(TAG, "=== Testing NAND Driver ===");
    // esp_log_level_set("mx35lf1", ESP_LOG_DEBUG);

    spi_bus_config_t bus = {
        .mosi_io_num = NAND_PIN_MOSI,
        .miso_io_num = NAND_PIN_MISO,
        .sclk_io_num = NAND_PIN_SCLK,
        .quadwp_io_num = NAND_PIN_WP,   /* WP#  / SIO2 */
        .quadhd_io_num = NAND_PIN_HOLD, /* HOLD#/ SIO3 */
        .max_transfer_sz = NAND_SPI_MAX_TRANSFER,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };

    spi_bus_initialize(NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO);

    nand_config_t cfg = {
        .spi_host = NAND_SPI_HOST,
        .pin_cs = NAND_PIN_CS,
        .pin_mosi = NAND_PIN_MOSI,
        .pin_miso = NAND_PIN_MISO,
        .pin_sclk = NAND_PIN_SCLK,
        .pin_wp = NAND_PIN_WP,
        .pin_hold = NAND_PIN_HOLD,
        .clock_speed_hz = NAND_CLOCK_HZ,
        .dma_chan = SPI_DMA_CH_AUTO,
        .disable_ecc = false,
    };

    ESP_LOGI(TAG, "=== Testing Init NAND Driver ===");
    bool ok = nand_mx35lf1_init(&handle, &cfg) == NAND_RET_OK;
    const char *response = ok ? "Init function ok" : "Error in Init function";
    ESP_LOGI(TAG, "%s", response);
    if (!ok)
    {
        return;
    }

    nand_info_t info;
    nand_mx35lf1_get_info(&handle, &info);

    ESP_LOGI(TAG, "  Manufacturer ID : 0x%02X  (%s)", info.manufacturer_id, info.manufacturer_id == NAND_MANUFACTURER_ID ? "Macronix" : "UNKNOWN");
    ESP_LOGI(TAG, "  Device ID       : 0x%02X  (%s)", info.device_id, info.device_id == NAND_DEVICE_ID ? "MX35LF1GE4AB" : "UNKNOWN");
    ESP_LOGI(TAG, "  Blocks          : %" PRIu32, info.total_blocks);
    ESP_LOGI(TAG, "  Pages / block   : %" PRIu32, info.pages_per_block);
    ESP_LOGI(TAG, "  Page size       : %" PRIu32 " + %" PRIu32 " B spare", info.page_size, info.spare_size);
    ESP_LOGI(TAG, "  Total capacity  : %" PRIu32 " MB data", (uint32_t) (NAND_TOTAL_SIZE / (1024UL * 1024UL)));
    ESP_LOGI(TAG, "  Internal ECC    : %s", info.ecc_enabled ? "ON" : "OFF");
    ESP_LOGI(TAG, "  Mounted         : %s", nand_mx35lf1_mounted(&handle) ? "ON" : "OFF");

    // nand_mx35lf1_clear(&handle);
    // for (int i = 0; i < NAND_BLOCKS_PER_LUN; i++)
    // {
    //     bool bad;
    //     row_address_t row = {.block = i, .page = 0};
    //     int ret = nand_mx35lf1_block_is_bad(&handle, row, &bad);
    //     // if (ret != NAND_RET_OK)
    //     // {
    //     // nand_mx35lf1_block_mark_bad(&handle, row);
    //     // bad = true;
    //     // }
    //     ESP_LOGI(TAG, "Block %d is bad? %s", i, bad ? "YES" : "NO");
    // }
    // return;

    row_address_t row = {.block = TEST_BLOCK, .page = TEST_PAGE};
    column_address_t column = 0;

    ESP_LOGI(TAG, "=== Testing Block Erase NAND Driver ===");

    ok = nand_mx35lf1_block_erase(&handle, row) == NAND_RET_OK;
    response = ok ? "Block erase function ok" : "Error in Block erase function";
    ESP_LOGI(TAG, "%s", response);
    if (!ok)
    {
        return;
    }
    ESP_LOGI(TAG, "Erase function passed");

    ESP_LOGI(TAG, "=== Testing Block is Bad NAND Driver ===");
    bool is_bad = false;
    ok = nand_mx35lf1_block_is_bad(&handle, row, &is_bad) == NAND_RET_OK;
    response = ok ? "Block is bad function ok" : "Error in Block is bad function";
    ESP_LOGI(TAG, "%s", response);
    if (!ok)
    {
        return;
    }
    ESP_LOGI(TAG, "Block is bad function passed - is bad? %s", is_bad ? "YES" : "NO");

    ESP_LOGI(TAG, "=== Testing Block Erase NAND Driver ===");

    ok = nand_mx35lf1_block_erase(&handle, row) == NAND_RET_OK;
    response = ok ? "Block erase function ok" : "Error in Block erase function";
    ESP_LOGI(TAG, "%s", response);
    if (!ok)
    {
        return;
    }
    ESP_LOGI(TAG, "Erase function passed");

    ESP_LOGI(TAG, "=== Testing Block is Free NAND Driver ===");
    bool is_free = false;
    ok = nand_mx35lf1_page_is_free(&handle, row, &is_free) == NAND_RET_OK;
    response = ok ? "Block is free function ok" : "Error in Block is free function";
    ESP_LOGI(TAG, "%s", response);
    if (!ok)
    {
        return;
    }
    ESP_LOGI(TAG, "Block is free function passed - is free? %s", is_bad ? "YES" : "NO");

    ESP_LOGI(TAG, "=== Testing Page Program NAND Driver ===");
    ESP_LOGI(TAG, "Program buffer");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, (uint8_t *) msg, sizeof(msg), ESP_LOG_WARN);

    ok = nand_mx35lf1_page_program(&handle, row, column, (const uint8_t *) msg, sizeof(msg)) == NAND_RET_OK;
    response = ok ? "Page program function ok" : "Error in Page program function";
    ESP_LOGI(TAG, "%s", response);
    if (!ok)
    {
        return;
    }
    ESP_LOGI(TAG, "Program function passed");

    ESP_LOGI(TAG, "=== Testing Read Program NAND Driver ===");
    ok = nand_mx35lf1_page_read(&handle, row, column, (uint8_t *) receive, sizeof(receive)) == NAND_RET_OK;
    response = ok ? "Page read function ok" : "Error in Page read function";
    ESP_LOGI(TAG, "%s", response);
    if (!ok)
    {
        return;
    }
    ESP_LOGI(TAG, "Readed buffer");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, (uint8_t *) receive, sizeof(receive), ESP_LOG_WARN);

    ESP_LOGI(TAG, "Read function passed");
    ESP_LOGI(TAG, "%s", receive);

    ESP_LOGI(TAG, "=== Testing Block is Free NAND Driver ===");
    is_free = false;
    ok = nand_mx35lf1_page_is_free(&handle, row, &is_free) == NAND_RET_OK;
    response = ok ? "Block is free function ok" : "Error in Block is free function";
    ESP_LOGI(TAG, "%s", response);
    if (!ok)
    {
        return;
    }
    ESP_LOGI(TAG, "Block is free function passed - is free? %s", is_bad ? "YES" : "NO");

    ESP_LOGI(TAG, "=== Testing Page Copy NAND Driver ===");
    row_address_t row_dst = {.block = TEST_BLOCK + 1, .page = TEST_PAGE};
    column_address_t column_dst = 0;

    ok = nand_mx35lf1_page_copy(&handle, row, row_dst) == NAND_RET_OK;
    response = ok ? "Page copy function ok" : "Error in Page copy function";
    ESP_LOGI(TAG, "%s", response);
    if (!ok)
    {
        return;
    }
    ESP_LOGI(TAG, "Copy function passed");

    ESP_LOGI(TAG, "=== Testing Copied Read Program NAND Driver ===");
    ok = nand_mx35lf1_page_read(&handle, row_dst, column_dst, (uint8_t *) copied_data, sizeof(copied_data)) == NAND_RET_OK;
    response = ok ? "Page read function ok" : "Error in Page read function";
    ESP_LOGI(TAG, "%s", response);
    if (!ok)
    {
        return;
    }
    ESP_LOGI(TAG, "Readed buffer");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, (uint8_t *) copied_data, sizeof(copied_data), ESP_LOG_WARN);

    ESP_LOGI(TAG, "Read function passed");
    ESP_LOGI(TAG, "%s", copied_data);
}