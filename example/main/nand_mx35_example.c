#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "mx35lf1ge4ab.h"

#define NAND_SPI_HOST SPI2_HOST
#define NAND_PIN_MOSI 11
#define NAND_PIN_SCLK 12
#define NAND_PIN_MISO 13
#define NAND_PIN_CS   15
#define NAND_PIN_WP   16                 /* -1 if not connected */
#define NAND_PIN_HOLD 17                 /* -1 if not connected */
#define NAND_CLOCK_HZ (50 * 1000 * 1000) /* 50 MHz — conservative */

#define TEST_BLOCK 100U
#define TEST_PAGE  0U

static const char *TAG = "NAND_EXAMPLE";

/** Log and return on failure. */
#define CHECK(x, label)                                                      \
    do                                                                       \
    {                                                                        \
        esp_err_t __e = (x);                                                 \
        if (__e != ESP_OK)                                                   \
        {                                                                    \
            ESP_LOGE(TAG, "FAILED  %-30s  %s", label, esp_err_to_name(__e)); \
            return __e;                                                      \
        }                                                                    \
        ESP_LOGI(TAG, "OK      %s", label);                                  \
    } while (0)

/** Log result but continue even on error. */
#define LOG_RESULT(x, label)                                                                \
    do                                                                                      \
    {                                                                                       \
        esp_err_t __e = (x);                                                                \
        if (__e != ESP_OK) ESP_LOGW(TAG, "WARN    %-30s  %s", label, esp_err_to_name(__e)); \
        else ESP_LOGI(TAG, "OK      %s", label);                                            \
    } while (0)

static void section(const char *title)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "── %s", title);
}

/*  EXAMPLE 1: Initialization and device identification */
static esp_err_t example_init(nand_handle_t *out)
{
    section("1. SPI bus + driver initialization");

    /* --- Initialize the SPI Master bus ---------------------------------- */
    spi_bus_config_t bus = {
        .mosi_io_num = NAND_PIN_MOSI,
        .miso_io_num = NAND_PIN_MISO,
        .sclk_io_num = NAND_PIN_SCLK,
        .quadwp_io_num = NAND_PIN_WP,   /* WP#  / SIO2 */
        .quadhd_io_num = NAND_PIN_HOLD, /* HOLD#/ SIO3 */
        .max_transfer_sz = NAND_SPI_MAX_TRANSFER,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };

    CHECK(spi_bus_initialize(NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO), "spi_bus_initialize");

    /* --- Register the NAND device on the bus ---------------------------- */
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
        .disable_ecc = true,
    };

    CHECK(nand_init(out, &cfg), "nand_init");

    /* --- Print device information --------------------------------------- */
    nand_info_t info;
    nand_get_info(*out, &info);

    ESP_LOGI(TAG, "  Manufacturer ID : 0x%02X  (%s)", info.manufacturer_id, info.manufacturer_id == NAND_MANUFACTURER_ID ? "Macronix" : "UNKNOWN");
    ESP_LOGI(TAG, "  Device ID       : 0x%02X  (%s)", info.device_id, info.device_id == NAND_DEVICE_ID ? "MX35LF1GE4AB" : "UNKNOWN");
    ESP_LOGI(TAG, "  Blocks          : %" PRIu32, info.total_blocks);
    ESP_LOGI(TAG, "  Pages / block   : %" PRIu32, info.pages_per_block);
    ESP_LOGI(TAG, "  Page size       : %" PRIu32 " + %" PRIu32 " B spare", info.page_size, info.spare_size);
    ESP_LOGI(TAG, "  Total capacity  : %" PRIu32 " MB data", (uint32_t) (NAND_TOTAL_SIZE / (1024UL * 1024UL)));
    ESP_LOGI(TAG, "  Internal ECC    : %s", info.ecc_enabled ? "ON" : "OFF");
    ESP_LOGI(TAG, "  Quad I/O        : %s", info.quad_enabled ? "ON" : "OFF");

    return ESP_OK;
}

/* EXAMPLE 2: Read, program, and erase a single page */
static esp_err_t example_read_program_erase(nand_handle_t h)
{
    section("2. Erase -> Program -> Read -> Verify");

    esp_err_t ret;

    /*
     * Allocate DMA-capable buffers.
     * All buffers passed to nand_program_page() and nand_read_page()
     * must reside in DMA-capable RAM on ESP32 targets.
     */
    uint8_t *write_buf = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    uint8_t *read_buf = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    uint8_t *spare_buf = heap_caps_malloc(NAND_SPARE_SIZE, MALLOC_CAP_DMA);

    if (!write_buf || !read_buf || !spare_buf)
    {
        ESP_LOGE(TAG, "DMA memory allocation failed");
        heap_caps_free(write_buf);
        heap_caps_free(read_buf);
        heap_caps_free(spare_buf);
        return ESP_ERR_NO_MEM;
    }

    /* --- Step 1: Erase the block ---------------------------------------- */
    ESP_LOGI(TAG, "  Erasing block %u...", TEST_BLOCK);
    int64_t t0 = esp_timer_get_time();
    ret = nand_erase_block(h, TEST_BLOCK);
    int64_t erase_us = esp_timer_get_time() - t0;

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "  Erase FAILED: %s", nand_err_to_str(ret));
        goto done;
    }
    ESP_LOGI(TAG, "  Erase OK  (%" PRId64 " µs)", erase_us);

    /* --- Step 2: Verify the block is all-0xFF after erase --------------- */
    nand_ecc_status_t ecc;
    ret = nand_read_page(h, TEST_BLOCK, TEST_PAGE, read_buf, spare_buf, &ecc);
    // ESP_LOG_BUFFER_HEX_LEVEL(TAG, read_buf, NAND_PAGE_SIZE, ESP_LOG_DEBUG);
    if (ret == ESP_OK)
    {
        bool all_ff = true;
        for (uint32_t i = 0; i < NAND_PAGE_SIZE; i++)
        {
            if (read_buf[i] != 0xFFU)
            {
                all_ff = false;
                break;
            }
        }
        ESP_LOGI(TAG, "  Post-erase data: %s", all_ff ? "all 0xFF : ok" : "NOT all 0xFF : failed");
        ESP_LOGI(TAG, "  Spare[0]: 0x%02X  ECC: %s", spare_buf[0], ecc == NAND_ECC_OK ? "OK" : ecc == NAND_ECC_CORRECTED ? "corrected" : "UNCORRECTABLE");
    }

    /* --- Step 3: Fill the write buffer with a recognizable pattern ------ */
    for (uint32_t i = 0; i < NAND_PAGE_SIZE; i++)
    {
        write_buf[i] = (uint8_t) (i & 0xFFU); /* 0x00 0x01 ... 0xFF 0x00 ... */
    }
    write_buf[0] = 0xDE;
    write_buf[1] = 0xAD;
    write_buf[2] = 0xBE;
    write_buf[3] = 0xEF;

    /* Spare: fill with 0xAA, leave spare[0] = 0xFF (good block marker) */
    memset(spare_buf, 0xAAU, NAND_SPARE_SIZE);
    spare_buf[0] = 0xFFU;

    /* --- Step 4: Program the page --------------------------------------- */
    ESP_LOGI(TAG, "  Programming block=%u page=%u...", TEST_BLOCK, TEST_PAGE);
    t0 = esp_timer_get_time();
    ret = nand_program_page(h, TEST_BLOCK, TEST_PAGE, write_buf, spare_buf);
    int64_t prog_us = esp_timer_get_time() - t0;

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "  Program FAILED: %s", nand_err_to_str(ret));
        goto done;
    }
    ESP_LOGI(TAG, "  Program OK  (%" PRId64 " µs)", prog_us);

    /* --- Step 5: Read back and compare ---------------------------------- */
    ESP_LOGI(TAG, "  Reading back block=%u page=%u...", TEST_BLOCK, TEST_PAGE);
    t0 = esp_timer_get_time();
    ret = nand_read_page(h, TEST_BLOCK, TEST_PAGE, read_buf, spare_buf, &ecc);
    int64_t read_us = esp_timer_get_time() - t0;

    // ESP_LOG_BUFFER_HEX_LEVEL(TAG, write_buf, NAND_PAGE_SIZE, ESP_LOG_DEBUG);
    // ESP_LOG_BUFFER_HEX_LEVEL(TAG, read_buf, NAND_PAGE_SIZE, ESP_LOG_DEBUG);

    if (ret != ESP_OK && ret != ESP_ERR_INVALID_CRC)
    {
        ESP_LOGE(TAG, "  Read FAILED: %s", nand_err_to_str(ret));
        goto done;
    }

    ESP_LOGI(TAG, "  Read OK  (%" PRId64 " µs)  ECC: %s", read_us, ecc == NAND_ECC_OK ? "no errors" : ecc == NAND_ECC_CORRECTED ? "corrected" : "UNCORRECTABLE");

    ESP_LOGI(TAG, "  First 4 bytes: 0x%02X 0x%02X 0x%02X 0x%02X", read_buf[0], read_buf[1], read_buf[2], read_buf[3]);
    ESP_LOGI(TAG, "  Spare[0]: 0x%02X  Spare[1]: 0x%02X", spare_buf[0], spare_buf[1]);

    /* Byte-for-byte comparison */
    bool match = (memcmp(write_buf, read_buf, NAND_PAGE_SIZE) == 0);
    ESP_LOGI(TAG, "  Data verification: %s", match ? "PASSED" : "FAILED");
    if (!match)
    {
        ret = ESP_FAIL;
    }

done:
    heap_caps_free(write_buf);
    heap_caps_free(read_buf);
    heap_caps_free(spare_buf);
    return ret;
}

/* EXAMPLE 3: Write multiple pages across a block */
static esp_err_t example_multipage_write(nand_handle_t h)
{
    section("3. Multi-page write (first 4 pages of block)");

    const uint8_t PAGES_TO_WRITE = 4U;
    esp_err_t ret;

    uint8_t *buf = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    if (!buf)
    {
        return ESP_ERR_NO_MEM;
    }

    /* Erase first */
    ret = nand_erase_block(h, TEST_BLOCK);
    if (ret != ESP_OK)
    {
        free(buf);
        return ret;
    }

    /* Program pages 0..3 */
    for (uint8_t pg = 0; pg < PAGES_TO_WRITE; pg++)
    {
        /*
         * Fill each page with its page number repeated:
         *   Page 0 -> all 0x00
         *   Page 1 -> all 0x01
         *   ...
         */
        memset(buf, pg, NAND_PAGE_SIZE);
        buf[0] = 0xABU;
        buf[1] = pg;

        ret = nand_program_page(h, TEST_BLOCK, pg, buf, NULL);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "  Program page %u FAILED: %s", pg, nand_err_to_str(ret));
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "  Page %u programmed OK", pg);
    }

    /* Read back and spot-check */
    for (uint8_t pg = 0; pg < PAGES_TO_WRITE; pg++)
    {
        nand_ecc_status_t ecc;
        ret = nand_read_page(h, TEST_BLOCK, pg, buf, NULL, &ecc);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "  Read page %u FAILED: %s", pg, nand_err_to_str(ret));
            free(buf);
            return ret;
        }

        bool ok = (buf[0] == 0xABU) && (buf[1] == pg);
        ESP_LOGI(TAG, "  Page %u: [0]=0x%02X [1]=0x%02X  %s", pg, buf[0], buf[1], ok ? "ok" : "mismatch");
    }

    free(buf);
    return ESP_OK;
}

/* EXAMPLE 4: ECC status inspection */
static esp_err_t example_ecc_status(nand_handle_t h)
{
    section("4. ECC status inspection");

    /*
     * The internal ECC engine is enabled by default (ECC bit in the
     * Configuration register at address 0xB0).
     *
     * After a PAGE READ, the device reports the worst-case error count
     * across its four 512-byte ECC segments via:
     *   a) The ECC_S1:ECC_S0 bits in the Status Register (coarse: OK / corrected / fail)
     *   b) The ECCSR register read with command 0x7C (fine: 0–4 bits / uncorrectable)
     */

    nand_info_t info;
    nand_get_info(h, &info);
    ESP_LOGI(TAG, "  Internal ECC: %s", info.ecc_enabled ? "enabled" : "disabled");

    /* Read a freshly programmed page and check the detailed ECC register */
    uint8_t *buf = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    if (!buf)
    {
        return ESP_ERR_NO_MEM;
    }

    nand_ecc_status_t ecc_simple;
    esp_err_t ret = nand_read_page(h, TEST_BLOCK, 0U, buf, NULL, &ecc_simple);
    free(buf);

    if (ret != ESP_OK && ret != ESP_ERR_INVALID_CRC)
    {
        return ret;
    }

    const char *ecc_str[] = {"OK (no errors)", "Corrected (1–4 bits)", "UNCORRECTABLE"};
    ESP_LOGI(TAG, "  After read — simplified ECC: %s", ecc_str[(int) ecc_simple]);

    /* Read the detailed ECCSR register (command 7Ch) */
    nand_eccsr_t eccsr;
    ret = nand_read_ecc_status(h, &eccsr);
    if (ret == ESP_OK)
    {
        const char *detail;
        switch (eccsr)
        {
            case NAND_ECCSR_NO_ERROR: detail = "no bit errors"; break;
            case NAND_ECCSR_1BIT: detail = "1-bit error fixed"; break;
            case NAND_ECCSR_2BIT: detail = "2-bit errors fixed"; break;
            case NAND_ECCSR_3BIT: detail = "3-bit errors fixed"; break;
            case NAND_ECCSR_4BIT: detail = "4-bit errors fixed"; break;
            case NAND_ECCSR_UNCORRECT: detail = "UNCORRECTABLE"; break;
            default: detail = "unknown"; break;
        }
        ESP_LOGI(TAG, "  Detailed ECCSR (0x7C): 0x%02X -> %s", (unsigned) eccsr, detail);
    }

    /* Disable and re-enable ECC */
    CHECK(nand_ecc_enable(h, false), "nand_ecc_enable(false)");
    CHECK(nand_ecc_enable(h, true), "nand_ecc_enable(true)");

    return ESP_OK;
}

/* EXAMPLE 5: Bad block detection and marking */
static esp_err_t example_bad_blocks(nand_handle_t h)
{
    section("5. Bad block scan (first 32 blocks)");

    uint32_t bad_count = 0;

    for (uint16_t blk = 0; blk < 32U; blk++)
    {
        bool is_bad = false;
        esp_err_t ret = nand_is_bad_block(h, blk, &is_bad);
        if (ret != ESP_OK)
        {
            ESP_LOGW(TAG, "  Block %3u: read error (%s)", blk, nand_err_to_str(ret));
            continue;
        }

        if (is_bad)
        {
            ESP_LOGW(TAG, "  Block %3u: BAD", blk);
            bad_count++;
        }

        else
        {
            ESP_LOGD(TAG, "  Block %3u: good", blk);
        }
    }

    ESP_LOGI(TAG, "  %u bad block(s) found in first 32 blocks", (unsigned) bad_count);

    /*
     * Demonstration: mark a scratch block as bad, verify, then restore.
     *
     * NOTE: In a production system you should NEVER erase a block before
     * checking for the factory bad block marker — the marker may be erased.
     * Here we use a fresh block that we know is good.
     */
    const uint16_t SCRATCH_BLOCK = TEST_BLOCK + 1U;

    ESP_LOGI(TAG, "  Marking block %u as bad for demonstration...", SCRATCH_BLOCK);
    LOG_RESULT(nand_mark_bad_block(h, SCRATCH_BLOCK), "nand_mark_bad_block");

    bool is_bad = false;
    LOG_RESULT(nand_is_bad_block(h, SCRATCH_BLOCK, &is_bad), "nand_is_bad_block after mark");
    ESP_LOGI(TAG, "  Block %u bad after mark: %s", SCRATCH_BLOCK, is_bad ? "YES" : "NO (unexpected)");

    /*
     * Restore: erase the block (clears the marker) so subsequent tests work.
     * In practice you would record this in your Bad Block Table instead.
     */
    LOG_RESULT(nand_erase_block(h, SCRATCH_BLOCK), "nand_erase_block (restore)");
    LOG_RESULT(nand_is_bad_block(h, SCRATCH_BLOCK, &is_bad), "nand_is_bad_block after erase");
    ESP_LOGI(TAG, "  Block %u bad after erase: %s", SCRATCH_BLOCK, is_bad ? "YES" : "NO (restored)");

    return ESP_OK;
}

/* EXAMPLE 6: Block protection configuration */
static esp_err_t example_protection(nand_handle_t h)
{
    section("6. Block protection register");

    uint8_t prot_val = 0;
    CHECK(nand_get_feature(h, NAND_FEAT_BLOCK_PROT, &prot_val), "read protection register");
    ESP_LOGI(TAG, "  Current value: 0x%02X", prot_val);

    /*
     * Datasheet Table 7 — selected protection configurations:
     *
     *   BP[2:0]  Invert  Complementary  Protected area
     *   000        x         x           None  (all unlocked)
     *   001        0         0           Upper 1/64
     *   010        0         0           Upper 1/32
     *   011        0         0           Upper 1/16
     *   100        0         0           Upper 1/8
     *   101        0         0           Upper 1/4
     *   110        0         0           Upper 1/2
     *   111        x         x           All  (all locked)
     */

    ESP_LOGI(TAG, "  Protecting upper 1/4 of the array (BP[2:0]=101)...");
    uint8_t upper_quarter = NAND_BP_BP2 | NAND_BP_BP0; /* 101 */
    CHECK(nand_set_protection(h, upper_quarter), "nand_set_protection(upper 1/4)");

    CHECK(nand_get_feature(h, NAND_FEAT_BLOCK_PROT, &prot_val), "read protection after set");
    ESP_LOGI(TAG, "  Register after set: 0x%02X  BP2=%u BP1=%u BP0=%u", prot_val, (prot_val >> 5) & 1U, (prot_val >> 4) & 1U, (prot_val >> 3) & 1U);

    /* Restore: remove all protection */
    CHECK(nand_unprotect_all(h), "nand_unprotect_all");
    CHECK(nand_get_feature(h, NAND_FEAT_BLOCK_PROT, &prot_val), "read after unprotect");
    ESP_LOGI(TAG, "  Register after unprotect: 0x%02X  (should be 0x00)", prot_val);

    return ESP_OK;
}

/* EXAMPLE 7: Feature register read/write */
static esp_err_t example_feature_regs(nand_handle_t h)
{
    section("7. Feature register inspection");

    uint8_t val;

    /* Status register (read-only) */
    CHECK(nand_get_feature(h, NAND_FEAT_STATUS, &val), "GET_FEATURE STATUS (0xC0)");
    ESP_LOGI(TAG, "  Status register (0xC0): 0x%02X", val);
    ESP_LOGI(
        TAG,
        "    OIP=%u  WEL=%u  ERS_FAIL=%u  PGM_FAIL=%u  ECC=%u%u  CRBSY=%u",
        (val >> 0) & 1U,
        (val >> 1) & 1U,
        (val >> 2) & 1U,
        (val >> 3) & 1U,
        (val >> 5) & 1U,
        (val >> 4) & 1U,
        (val >> 6) & 1U);

    /* Configuration register (ECC, Quad Enable) */
    CHECK(nand_get_feature(h, NAND_FEAT_SECURE_OTP, &val), "GET_FEATURE CFG (0xB0)");
    ESP_LOGI(TAG, "  Config register (0xB0): 0x%02X", val);
    ESP_LOGI(TAG, "    QE=%u  ECC_EN=%u  OTP_EN=%u  OTP_PROT=%u", (val >> 0) & 1U, (val >> 4) & 1U, (val >> 6) & 1U, (val >> 7) & 1U);

    /* Block Protection register */
    CHECK(nand_get_feature(h, NAND_FEAT_BLOCK_PROT, &val), "GET_FEATURE BP (0xA0)");
    ESP_LOGI(TAG, "  Block protection (0xA0): 0x%02X", val);
    ESP_LOGI(
        TAG,
        "    SP=%u  COMP=%u  INV=%u  BP2=%u BP1=%u BP0=%u  BPRWD=%u",
        (val >> 0) & 1U,
        (val >> 1) & 1U,
        (val >> 2) & 1U,
        (val >> 5) & 1U,
        (val >> 4) & 1U,
        (val >> 3) & 1U,
        (val >> 7) & 1U);

    return ESP_OK;
}

/* EXAMPLE 8: Address conversion utilities */
static void example_address_conversion(void)
{
    section("8. Linear address ↔ block/page/column conversion");

    /*
     * The NAND array is organized as:
     *   1024 blocks × 64 pages × 2048 bytes = 134,217,728 bytes total
     *
     * nand_linear_to_addr(offset) decomposes an offset as:
     *   column = offset % 2048
     *   page   = (offset / 2048) % 64
     *   block  = (offset / 2048) / 64
     */

    const uint32_t test_offsets[] = {
        0U,                                         /* first byte of the array */
        2047U,                                      /* last byte of page 0     */
        2048U,                                      /* first byte of page 1    */
        NAND_BLOCK_SIZE - 1U,                       /* last byte of block 0    */
        NAND_BLOCK_SIZE,                            /* first byte of block 1   */
        NAND_BLOCK_SIZE * 100U + 2048U * 5U + 128U, /* arbitrary middle offset */
        NAND_TOTAL_SIZE - 1U,                       /* very last byte          */
    };

    for (size_t i = 0; i < sizeof(test_offsets) / sizeof(test_offsets[0]); i++)
    {
        uint32_t linear = test_offsets[i];
        nand_addr_t addr;
        nand_linear_to_addr(linear, &addr);

        /* Round-trip: convert back and verify */
        uint32_t roundtrip = nand_addr_to_linear(&addr);
        bool ok = (roundtrip == linear);

        ESP_LOGI(
            TAG,
            "  0x%08" PRIX32 " -> block=%4u  page=%2u  col=%4u  "
            "round-trip: %s",
            linear,
            addr.block,
            addr.page,
            addr.column,
            ok ? "OK" : "MISMATCH");
    }
}

/* EXAMPLE 9: Timing measurements */
static esp_err_t example_timing(nand_handle_t h)
{
    section("9. Timing measurements");

    esp_err_t ret;
    int64_t t0, elapsed_us;
    uint8_t *buf = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    if (!buf)
    {
        return ESP_ERR_NO_MEM;
    }
    memset(buf, 0x5AU, NAND_PAGE_SIZE);

    /* Erase */
    t0 = esp_timer_get_time();
    ret = nand_erase_block(h, TEST_BLOCK);
    elapsed_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "  Erase  block %-4u : %" PRId64 " µs  (datasheet max: %u µs)", TEST_BLOCK, elapsed_us, NAND_tERS_MS * 1000U);
    if (ret != ESP_OK)
    {
        free(buf);
        return ret;
    }

    /* Program */
    t0 = esp_timer_get_time();
    ret = nand_program_page(h, TEST_BLOCK, 0U, buf, NULL);
    elapsed_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "  Program page  0  : %" PRId64 " µs  (datasheet max: %u µs)", elapsed_us, NAND_tPROG_US);
    if (ret != ESP_OK)
    {
        free(buf);
        return ret;
    }

    /* Read (with ECC) */
    nand_ecc_status_t ecc;
    t0 = esp_timer_get_time();
    ret = nand_read_page(h, TEST_BLOCK, 0U, buf, NULL, &ecc);
    elapsed_us = esp_timer_get_time() - t0;
    ESP_LOGI(TAG, "  Read   page  0   : %" PRId64 " µs  (datasheet max ECC: %u µs)", elapsed_us, NAND_tRD_ECC_US);

    free(buf);
    return ret;
}

static void nand_task(void *pv)
{
    ESP_LOGI(TAG, "======================================================");
    ESP_LOGI(TAG, "  MX35LF1GE4AB — Low-level driver examples");
    ESP_LOGI(TAG, "======================================================");
    ESP_LOGI(TAG, "Free heap at start: %" PRIu32 " bytes", esp_get_free_heap_size());

    nand_handle_t h = NULL;

    /* Run all examples */
    if (example_init(&h) != ESP_OK)
    {
        goto fail;
    }

    if (example_read_program_erase(h) != ESP_OK)
    {
        goto fail;
    }

    if (example_multipage_write(h) != ESP_OK)
    {
        goto fail;
    }

    if (example_ecc_status(h) != ESP_OK)
    {
        goto fail;
    }

    if (example_bad_blocks(h) != ESP_OK)
    {
        goto fail;
    }

    if (example_protection(h) != ESP_OK)
    {
        goto fail;
    }

    if (example_feature_regs(h) != ESP_OK)
    {
        goto fail;
    }

    example_address_conversion(); /* pure calculation, no NAND I/O */
    if (example_timing(h) != ESP_OK)
    {
        goto fail;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "All examples completed successfully");
    goto done;

fail:
    ESP_LOGE(TAG, "Example run FAILED");

done:
    if (h)
    {
        nand_deinit(h);
        spi_bus_free(NAND_SPI_HOST);
    }
    ESP_LOGI(TAG, "Free heap at exit : %" PRIu32 " bytes", esp_get_free_heap_size());
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    xTaskCreate(nand_task, "nand_example", 8 * 1024, NULL, 1, NULL);
}
