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

#include "ecu_fw_manager.h"
#include "mx35lf1ge4ab.h"

#define NAND_SPI_HOST SPI2_HOST
#define NAND_PIN_MOSI 11
#define NAND_PIN_SCLK 12
#define NAND_PIN_MISO 13
#define NAND_PIN_CS   15
#define NAND_PIN_WP   16
#define NAND_PIN_HOLD 17
#define NAND_CLOCK_HZ (50 * 1000 * 1000)

static const char *TAG = "APP";

static nand_handle_t nand = NULL;
static ecu_manager_handle_t mgr = NULL;

#define CHECK(x, msg)                                                    \
    do                                                                   \
    {                                                                    \
        esp_err_t __e = (x);                                             \
        if (__e != ESP_OK)                                               \
        {                                                                \
            ESP_LOGE(TAG, "FAILED '%s': %s", msg, esp_err_to_name(__e)); \
            return __e;                                                  \
        }                                                                \
    } while (0)

static void print_sep(const char *title)
{
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "══════════════════════════════════════════════");
    if (title)
    {
        ESP_LOGI(TAG, "  %s", title);
    }
    ESP_LOGI(TAG, "══════════════════════════════════════════════");
}

static void print_stats(ecu_manager_handle_t mgr)
{
    uint32_t total, used, free_blk, bad;
    ecu_get_stats(mgr, &total, &used, &free_blk, &bad);
    ESP_LOGI(TAG, "  Total: %" PRIu32 " Blocks | Used: %" PRIu32 " | Free: %" PRIu32 " | Bad: %" PRIu32, total, used, free_blk, bad);
}

static void print_ecu_table(ecu_manager_handle_t mgr)
{
    ecu_info_t list[ECU_MAX_SLOTS];
    uint8_t count = 0;
    ecu_list(mgr, list, ECU_MAX_SLOTS, &count);
    if (count == 0)
    {
        ESP_LOGI(TAG, "  (no ECUs)");
        return;
    }

    ESP_LOGI(TAG, "  %-16s %-10s %10s %8s %8s %8s", "Name", "Version", "HW ID", "bin(B)", "prm(B)", "idx(B)");
    for (uint8_t i = 0; i < count; i++)
    {
        ESP_LOGI(
            TAG,
            "  %-16s %-10s 0x%08" PRIX32 " %8" PRIu32 " %8" PRIu32 " %8" PRIu32,
            list[i].ecu_name,
            list[i].fw_version,
            list[i].hw_id,
            list[i].bin_size,
            list[i].prm_size,
            list[i].idx_size);
    }
    ESP_LOGI(TAG, "  Total: %u ECU(s)", (unsigned) count);
}

/* DEMO 1: Streaming Write — simulates UART/TCP reception */
static esp_err_t demo_stream_write(ecu_manager_handle_t mgr)
{
    print_sep("DEMO 1: Streaming Write (ECU_ENGINE, 1 MB .bin)");

    const uint32_t TOTAL_SIZE = 1024U * 1024U; /* 1 MB     */
    const uint32_t CHUNK_SIZE = 4U * 1024U;    /* 4 KB por chunk */
    const uint32_t NUM_CHUNKS = TOTAL_SIZE / CHUNK_SIZE;

    /* Chunk buffer — fits on the stack, but we use heap for safety */
    uint8_t *chunk = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DEFAULT);
    if (!chunk)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "  Firmware: %" PRIu32 " KB in %" PRIu32 " chunks of %" PRIu32 " KB", TOTAL_SIZE / 1024, NUM_CHUNKS, CHUNK_SIZE / 1024);
    ESP_LOGI(TAG, "  Write RAM usage: %u bytes (1 NAND page)", NAND_PAGE_SIZE * 2);

    /* Open the writer */
    ecu_writer_t wr;
    int64_t t0 = esp_timer_get_time();

    esp_err_t ret = ecu_writer_begin(mgr, "ECU_ENGINE", "v2.5.0", 0x00010003UL, ECU_FILE_BIN, TOTAL_SIZE, &wr);
    if (ret != ESP_OK)
    {
        free(chunk);
        return ret;
    }

    ESP_LOGI(TAG, "  Writer opened. Starting transfer...");

    /* Simulate chunk reception (e.g. UART / TCP / XMODEM) */
    for (uint32_t i = 0; i < NUM_CHUNKS; i++)
    {
        /* Fill the chunk with identifiable test data */
        memset(chunk, (uint8_t) (i & 0xFFU), CHUNK_SIZE);
        chunk[0] = (uint8_t) (i >> 8);
        chunk[1] = (uint8_t) (i & 0xFF);

        ret = ecu_writer_write(&wr, chunk, CHUNK_SIZE);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "  ERROR int the chunk %" PRIu32 ": %s", i, esp_err_to_name(ret));
            ecu_writer_abort(&wr);
            free(chunk);
            return ret;
        }

        /* Log progress every 64 chunks (~256 KB) */
        if ((i % 64U) == 63U || i == NUM_CHUNKS - 1U)
        {
            ESP_LOGI(TAG, "  Progress: %" PRIu32 "/%" PRIu32 " KB", (i + 1U) * CHUNK_SIZE / 1024U, TOTAL_SIZE / 1024U);
        }
    }

    /* Commit the write */
    ret = ecu_writer_commit(&wr);
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000LL;
    free(chunk);

    if (ret == ESP_OK)
    {
        uint32_t kbps = (uint32_t) ((TOTAL_SIZE * 1000ULL) / (uint64_t) (elapsed_ms * 1024ULL));
        ESP_LOGI(TAG, "  Commit OK in %" PRId64 " ms (~%" PRIu32 " KB/s)", elapsed_ms, kbps);
    }
    return ret;
}

/* DEMO 2: Streaming Write with ABORT — simulates a transmission failure */
static esp_err_t demo_stream_abort(ecu_manager_handle_t mgr)
{
    print_sep("DEMO 2: Streaming Write com Abort (ECU_TEMP)");

    const uint32_t TOTAL = 64U * 1024U; /* 64 KB */
    const uint32_t CHUNK = 4096U;

    uint8_t chunk[CHUNK];
    memset(chunk, 0xCC, sizeof(chunk));

    ecu_writer_t wr;
    CHECK(ecu_writer_begin(mgr, "ECU_TEMP", "v0.1.0", 0xDEADBEEFUL, ECU_FILE_BIN, TOTAL, &wr), "writer_begin ECU_TEMP");

    /* Write 3 chunks then simulate a failure (e.g. communication timeout) */
    for (int i = 0; i < 3; i++)
    {
        CHECK(ecu_writer_write(&wr, chunk, sizeof(chunk)), "writer_write");
    }

    ESP_LOGW(TAG, "  Simulating transmission failure — aborting...");
    ecu_writer_abort(&wr);

    /* Verify that the ECU was not left in storage */
    bool exists = false;
    ecu_exists(mgr, "ECU_TEMP", &exists);
    ESP_LOGI(TAG, "  ECU_TEMP exists after abortion: %s", exists ? "YES (ERROR!)" : "NO");

    return exists ? ESP_FAIL : ESP_OK;
}

/* DEMO 3: Streaming Read — reads large firmware chunk by chunk */
static esp_err_t demo_stream_read(ecu_manager_handle_t mgr)
{
    print_sep("DEMO 3: Streaming Read (ECU_ENGINE, 1 MB)");

    ecu_reader_t rd;
    CHECK(ecu_reader_open(mgr, "ECU_ENGINE", ECU_FILE_BIN, &rd), "reader_open");

    ESP_LOGI(TAG, "  File: %" PRIu32 " bytes (%" PRIu32 " KB)", rd.file_size, rd.file_size / 1024U);
    ESP_LOGI(TAG, "  Read  RAM usage: %u bytes (1 NAND page cache)", NAND_PAGE_SIZE);

    /* Read in 4 KB chunks and simulate forwarding to the target ECU */
    const uint32_t CHUNK_SIZE = 4U * 1024U;
    uint8_t *chunk = heap_caps_malloc(CHUNK_SIZE, MALLOC_CAP_DEFAULT);
    if (!chunk)
    {
        ecu_reader_close(&rd);
        return ESP_ERR_NO_MEM;
    }

    uint32_t total_sent = 0;
    int64_t t0 = esp_timer_get_time();

    while (rd.bytes_read < rd.file_size)
    {
        size_t got = 0;
        esp_err_t ret = ecu_reader_read(&rd, chunk, 1, CHUNK_SIZE, &got);
        if (ret != ESP_OK || got == 0)
        {
            ESP_LOGE(TAG, "  Read error: %s", esp_err_to_name(ret));
            ecu_reader_close(&rd);
            free(chunk);
            return ret != ESP_OK ? ret : ESP_FAIL;
        }

        /* Here we would send chunk[0..got-1] to the ECU over CAN/UART/OTA */
        total_sent += (uint32_t) got;

        /* Log progress every 256 KB */
        if ((total_sent % (256U * 1024U)) == 0U || rd.bytes_read == rd.file_size)
        {
            ESP_LOGI(TAG, "  Read: %" PRIu32 "/%" PRIu32 " KB", total_sent / 1024U, rd.file_size / 1024U);
        }
    }

    free(chunk);

    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000LL;
    uint32_t kbps = (uint32_t) ((total_sent * 1000ULL) / (uint64_t) (elapsed_ms * 1024ULL));

    /* close() validates the accumulated CRC32 */
    esp_err_t ret = ecu_reader_close(&rd);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "  CRC32 checked OK");
        ESP_LOGI(TAG, "  Read: %" PRIu32 " KB em %" PRId64 " ms (~%" PRIu32 " KB/s)", total_sent / 1024U, elapsed_ms, kbps);
    }

    else
    {
        ESP_LOGE(TAG, "  CRC32 FAILED: %s", esp_err_to_name(ret));
    }

    return ret;
}

#ifdef CONFIG_ECU_FW_MANAGER_SEEK_EXAMPLE
/* DEMO 4: Seek — random access within the firmware */
static esp_err_t demo_stream_seek(ecu_manager_handle_t mgr)
{
    print_sep("DEMO 4: Seek (Reads the beginning and end of the firmware.)");

    ecu_reader_t rd;
    CHECK(ecu_reader_open(mgr, "ECU_ENGINE", ECU_FILE_BIN, &rd), "reader_open seek demo");

    uint8_t header_bytes[8];
    size_t got = 0;

    /* Read the first 8 bytes of the firmware (offset 0) */
    CHECK(ecu_reader_read(&rd, header_bytes, 1, sizeof(header_bytes), &got), "read header");
    ESP_LOGI(
        TAG,
        "  Bytes 0-7: %02X %02X %02X %02X %02X %02X %02X %02X",
        header_bytes[0],
        header_bytes[1],
        header_bytes[2],
        header_bytes[3],
        header_bytes[4],
        header_bytes[5],
        header_bytes[6],
        header_bytes[7]);

    /* Seek to the last 16 bytes (simulated symbol table) */
    uint32_t tail_offset = rd.file_size >= 16U ? rd.file_size - 16U : 0U;
    CHECK(ecu_reader_seek(&rd, tail_offset), "seek to tail");

    uint8_t tail_bytes[16];
    CHECK(ecu_reader_read(&rd, tail_bytes, 1, sizeof(tail_bytes), &got), "read tail");
    ESP_LOGI(TAG, "  Final Bytes (offset %" PRIu32 "): %02X %02X ... %02X %02X", tail_offset, tail_bytes[0], tail_bytes[1], tail_bytes[14], tail_bytes[15]);

    /* Seek to the middle and read 4 bytes */
    uint32_t mid = rd.file_size / 2U;
    CHECK(ecu_reader_seek(&rd, mid), "seek to middle");
    uint8_t mid_bytes[4];
    CHECK(ecu_reader_read(&rd, mid_bytes, 1, sizeof(mid_bytes), &got), "read middle");
    ESP_LOGI(TAG, "  Bytes in the middle (offset %" PRIu32 "): %02X %02X %02X %02X", mid, mid_bytes[0], mid_bytes[1], mid_bytes[2], mid_bytes[3]);

    /* After seek, CRC is not verified on close (by design) */
    ecu_reader_close(&rd);
    ESP_LOGI(TAG, "  Seek demo completed");
    return ESP_OK;
}
#endif

/* DEMO 5: Streaming Write of .prm and .idx for ECU_ENGINE */
static esp_err_t demo_stream_multifile(ecu_manager_handle_t mgr)
{
    print_sep("DEMO 5: Stream Write .prm e .idx (ECU_ENGINE)");

    const uint32_t PRM_SIZE = 16U * 1024U; /* 16 KB */
    const uint32_t IDX_SIZE = 4U * 1024U;  /*  4 KB */

    uint8_t *buf = heap_caps_malloc(4096U, MALLOC_CAP_DEFAULT);
    if (!buf)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret;

    /* Write .prm */
    ecu_writer_t wr;
    ret = ecu_writer_begin(mgr, "ECU_ENGINE", "v2.5.0", 0x00010003UL, ECU_FILE_PRM, PRM_SIZE, &wr);
    if (ret != ESP_OK)
    {
        free(buf);
        return ret;
    }

    uint32_t written = 0;
    while (written < PRM_SIZE)
    {
        uint32_t chunk = PRM_SIZE - written;
        if (chunk > 4096U)
        {
            chunk = 4096U;
        }

        memset(buf, 0x11U, chunk);

        ret = ecu_writer_write(&wr, buf, chunk);
        if (ret != ESP_OK)
        {
            ecu_writer_abort(&wr);
            free(buf);
            return ret;
        }
        written += chunk;
    }

    ret = ecu_writer_commit(&wr);
    if (ret != ESP_OK)
    {
        free(buf);
        return ret;
    }
    ESP_LOGI(TAG, "  .prm written: %" PRIu32 " KB", PRM_SIZE / 1024);

    /* Write .idx */
    ret = ecu_writer_begin(mgr, "ECU_ENGINE", "v2.5.0", 0x00010003UL, ECU_FILE_IDX, IDX_SIZE, &wr);
    if (ret != ESP_OK)
    {
        free(buf);
        return ret;
    }

    memset(buf, 0x22U, IDX_SIZE);

    ret = ecu_writer_write(&wr, buf, IDX_SIZE);
    if (ret != ESP_OK)
    {
        ecu_writer_abort(&wr);
        free(buf);
        return ret;
    }

    ret = ecu_writer_commit(&wr);
    if (ret != ESP_OK)
    {
        free(buf);
        return ret;
    }
    ESP_LOGI(TAG, "  .idx written: %" PRIu32 " KB", IDX_SIZE / 1024);

    free(buf);
    return ESP_OK;
}

/* DEMO 6: ecu_verify_firmware using streaming (constant RAM) */
static esp_err_t demo_verify(ecu_manager_handle_t mgr)
{
    print_sep("DEMO 6: Integrity Verification (streaming, ~2 KB RAM)");

    bool bin_ok, prm_ok, idx_ok;
    int64_t t0 = esp_timer_get_time();
    CHECK(ecu_verify_firmware(mgr, "ECU_ENGINE", &bin_ok, &prm_ok, &idx_ok), "verify_firmware");
    int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000LL;

    ESP_LOGI(TAG, "  ECU_ENGINE (checked in %" PRId64 " ms):", elapsed_ms);
    ESP_LOGI(TAG, "    .bin CRC32: %s", bin_ok ? "OK" : "FAILED");
    ESP_LOGI(TAG, "    .prm CRC32: %s", prm_ok ? "OK" : "FAILED");
    ESP_LOGI(TAG, "    .idx CRC32: %s", idx_ok ? "OK" : "FAILED");

    return (bin_ok && prm_ok && idx_ok) ? ESP_OK : ESP_FAIL;
}

/* DEMO 7: Sub-page reads — 1 byte at a time (page-cache stress test) */
static esp_err_t demo_subpage_read(ecu_manager_handle_t mgr)
{
    print_sep("DEMO 7: Sub-page Read (1 byte at a time, first 16 bytes)");

    ecu_reader_t rd;
    CHECK(ecu_reader_open(mgr, "ECU_ABS", ECU_FILE_BIN, &rd), "reader_open subpage");

    /* Read 16 bytes, 1 byte at a time — exercises the page cache */
    for (int i = 0; i < 16 && rd.bytes_read < rd.file_size; i++)
    {
        uint8_t byte = 0;
        size_t got = 0;
        esp_err_t ret = ecu_reader_read(&rd, &byte, 1, 1, &got);
        if (ret != ESP_OK || got == 0)
        {
            break;
        }
        ESP_LOGI(TAG, "  byte[%2d] = 0x%02X", i, byte);
    }

    ecu_reader_close(&rd);
    return ESP_OK;
}

static esp_err_t run_all_demos(nand_handle_t nand, ecu_manager_handle_t mgr)
{
    print_sep("Initial State");
    print_stats(mgr);
    print_ecu_table(mgr);

    CHECK(demo_stream_write(mgr), "demo_stream_write");
    CHECK(demo_stream_abort(mgr), "demo_stream_abort");
    CHECK(demo_stream_read(mgr), "demo_stream_read");
#ifdef ECU_FW_MANAGER_SEEK_EXAMPLE
    CHECK(demo_stream_seek(mgr), "demo_stream_seek");
#endif
    CHECK(demo_stream_multifile(mgr), "demo_stream_multifile");
    CHECK(demo_verify(mgr), "demo_verify");
    CHECK(demo_subpage_read(mgr), "demo_subpage_read");

    print_sep("Final state");
    print_stats(mgr);
    print_ecu_table(mgr);

    return ESP_OK;
}

static void nand_task(void *pv)
{
    esp_err_t ret;

    print_sep("MX35LF1GE4AB — ECU Firmware Manager with Streaming");
    ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());

    /* 1. Initialize the SPI bus */
    spi_bus_config_t bus = {
        .mosi_io_num = NAND_PIN_MOSI,
        .miso_io_num = NAND_PIN_MISO,
        .sclk_io_num = NAND_PIN_SCLK,
        .quadwp_io_num = NAND_PIN_WP,
        .quadhd_io_num = NAND_PIN_HOLD,
        .max_transfer_sz = NAND_SPI_MAX_TRANSFER,
    };

    ret = spi_bus_initialize(NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "SPI init: %s", esp_err_to_name(ret));
        goto end;
    }

    /* 2. Initialize the NAND driver */
    nand_config_t ncfg = {
        .spi_host = NAND_SPI_HOST,
        .pin_cs = NAND_PIN_CS,
        .pin_mosi = NAND_PIN_MOSI,
        .pin_miso = NAND_PIN_MISO,
        .pin_sclk = NAND_PIN_SCLK,
        .pin_wp = NAND_PIN_WP,
        .pin_hold = NAND_PIN_HOLD,
        .clock_speed_hz = NAND_CLOCK_HZ,
        .io_mode = NAND_IO_X1,
        .disable_ecc = false,
        .dma_chan = SPI_DMA_CH_AUTO,
    };

    ret = nand_init(&nand, &ncfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "NAND init: %s", esp_err_to_name(ret));
        goto end;
    }

    /* 3. Initialize the firmware manager */
    ret = ecu_manager_init(&mgr, nand);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "MGR init: %s", esp_err_to_name(ret));
        goto end;
    }

    /* 4. Run demos */
    ret = run_all_demos(nand, mgr);

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "All demos completed successfully!");
    }

    else
    {
        ESP_LOGE(TAG, "Demo failed: %s", esp_err_to_name(ret));
    }

end:
    if (mgr)
    {
        ecu_manager_deinit(mgr);
    }

    if (nand)
    {
        nand_deinit(nand);
        spi_bus_free(NAND_SPI_HOST);
    }
    ESP_LOGI(TAG, "Final heap: %" PRIu32 " bytes", esp_get_free_heap_size());
    vTaskDelete(NULL);
}

void app_main(void)
{
    xTaskCreate(nand_task, "nand_task", 16 * 1024, NULL, tskIDLE_PRIORITY + 1, NULL);
}
