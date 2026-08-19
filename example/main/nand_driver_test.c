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
#define NAND_CLOCK_HZ (50 * 1000 * 1000) /* 50 MHz — conservative */

static const char TAG[] = "NAND_STORAGE";
static const char msg[] = "Sample test message for the mx35lf1 NAND driver to test the funcionality";
static char receive[sizeof(msg)];
static char copied_data[sizeof(msg)];
static nand_handle_t handle;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------ */

static bool log_result(const char *test_name, bool ok, const char *ok_msg, const char *err_msg)
{
    ESP_LOGI(TAG, "=== Testing %s ===", test_name);
    ESP_LOGI(TAG, "%s", ok ? ok_msg : err_msg);
    return ok;
}

static bool test_init(void)
{
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

    bool ok = nand_mx35lf1_init(&handle, &cfg) == NAND_RET_OK;
    return log_result("Init", ok, "Init function ok", "Error in Init function");
}

static void print_info(void)
{
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
}

static bool test_block_erase(row_address_t row)
{
    bool ok = nand_mx35lf1_block_erase(&handle, row) == NAND_RET_OK;
    return log_result("Block Erase", ok, "Erase function passed", "Error in Block erase function");
}

static bool test_block_is_bad(row_address_t row, bool *is_bad)
{
    bool ok = nand_mx35lf1_block_is_bad(&handle, row, is_bad) == NAND_RET_OK;
    if (!log_result("Block Is Bad", ok, "Block is bad function ok", "Error in Block is bad function"))
    {
        return false;
    }
    ESP_LOGI(TAG, "Block is bad function passed - is bad? %s", *is_bad ? "YES" : "NO");
    return true;
}

static bool test_page_is_free(row_address_t row, bool *is_free)
{
    bool ok = nand_mx35lf1_page_is_free(&handle, row, is_free) == NAND_RET_OK;
    if (!log_result("Page Is Free", ok, "Block is free function ok", "Error in Block is free function"))
    {
        return false;
    }
    ESP_LOGI(TAG, "Block is free function passed - is free? %s", *is_free ? "YES" : "NO");
    return true;
}

static bool test_page_program(row_address_t row, column_address_t column)
{
    ESP_LOGI(TAG, "=== Testing Page Program ===");
    ESP_LOGI(TAG, "Program buffer");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, (uint8_t *) msg, sizeof(msg), ESP_LOG_WARN);

    bool ok = nand_mx35lf1_page_program(&handle, row, column, (const uint8_t *) msg, sizeof(msg)) == NAND_RET_OK;
    ESP_LOGI(TAG, "%s", ok ? "Page program function ok" : "Error in Page program function");
    if (ok)
    {
        ESP_LOGI(TAG, "Program function passed");
    }
    return ok;
}

static bool test_page_read(row_address_t row, column_address_t column, char *buffer, size_t size)
{
    ESP_LOGI(TAG, "=== Testing Page Read ===");
    bool ok = nand_mx35lf1_page_read(&handle, row, column, (uint8_t *) buffer, size) == NAND_RET_OK;
    ESP_LOGI(TAG, "%s", ok ? "Page read function ok" : "Error in Page read function");
    if (!ok)
    {
        return false;
    }
    ESP_LOGI(TAG, "Read buffer");
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, (uint8_t *) buffer, size, ESP_LOG_WARN);
    ESP_LOGI(TAG, "Read function passed");
    ESP_LOGI(TAG, "%s", buffer);
    return true;
}

static bool test_page_copy(row_address_t src, row_address_t dst)
{
    bool ok = nand_mx35lf1_page_copy(&handle, src, dst) == NAND_RET_OK;
    return log_result("Page Copy", ok, "Copy function passed", "Error in Page copy function");
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                               */
/* ------------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "=== Testing NAND Driver ===");
    // esp_log_level_set("mx35lf1", ESP_LOG_DEBUG);

    if (!test_init())
    {
        return;
    }
    print_info();

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
    row_address_t row_dst = {.block = TEST_BLOCK + 1, .page = TEST_PAGE};
    column_address_t column = 0;
    column_address_t column_dst = 0;
    bool is_bad = false;
    bool is_free = false;

    if (!test_block_erase(row))
    {
        return;
    }

    if (!test_block_is_bad(row, &is_bad))
    {
        return;
    }

    if (!test_block_erase(row))
    {
        return;
    }

    if (!test_page_is_free(row, &is_free))
    {
        return;
    }

    if (!test_page_program(row, column))
    {
        return;
    }

    if (!test_page_read(row, column, receive, sizeof(receive)))
    {
        return;
    }

    if (!test_page_is_free(row, &is_free))
    {
        return;
    }

    if (!test_page_copy(row, row_dst))
    {
        return;
    }

    if (!test_page_read(row_dst, column_dst, copied_data, sizeof(copied_data)))
    {
        return;
    }
}
