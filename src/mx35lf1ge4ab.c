#include "mx35lf1ge4ab.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h" /* esp_rom_delay_us()     */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

static const char NAND_LOG_TAG[] = "MX35LF1GE4AB";

struct nand_dev_t
{
    spi_device_handle_t spi; /**< ESP-IDF SPI device handle              */
    SemaphoreHandle_t mutex; /**< FreeRTOS mutex for exclusive bus access */
    nand_info_t info;        /**< Device capabilities (read at init)      */
    nand_io_mode_t io_mode;  /**< Active I/O width                        */

    /* DMA-capable buffers allocated once at init to avoid hot-path malloc */
    uint8_t *dma_tx; /**< TX staging buffer (NAND_SPI_MAX_TRANSFER bytes) */
    uint8_t *dma_rx; /**< RX staging buffer (NAND_SPI_MAX_TRANSFER bytes) */

    bool initialized;
};


/** Acquire the driver mutex; return ESP_ERR_TIMEOUT if it takes > 5 s. */
#define NAND_LOCK(h)                                                   \
    do                                                                 \
    {                                                                  \
        if (xSemaphoreTake((h)->mutex, pdMS_TO_TICKS(5000)) != pdTRUE) \
        {                                                              \
            ESP_LOGE(NAND_LOG_TAG, "Mutex timeout");                   \
            return ESP_ERR_TIMEOUT;                                    \
        }                                                              \
    } while (0)

#define NAND_UNLOCK(h) xSemaphoreGive((h)->mutex)

/** Guard: return ESP_ERR_INVALID_STATE if the handle is NULL or not initialized. */
#define NAND_CHECK(h)                                                  \
    do                                                                 \
    {                                                                  \
        if (!(h) || !(h)->initialized)                                 \
        {                                                              \
            ESP_LOGE(NAND_LOG_TAG, "Invalid or uninitialized handle"); \
            return ESP_ERR_INVALID_STATE;                              \
        }                                                              \
    } while (0)

/** Guard: return ESP_ERR_INVALID_ARG if a pointer argument is NULL. */
#define NAND_CHECK_ARG(p)                            \
    do                                               \
    {                                                \
        if (!(p))                                    \
        {                                            \
            ESP_LOGE(NAND_LOG_TAG, "NULL argument"); \
            return ESP_ERR_INVALID_ARG;              \
        }                                            \
    } while (0)

/** Propagate an error immediately if @p x returns != ESP_OK. */
#define RET_ON_ERR(x)                \
    do                               \
    {                                \
        esp_err_t _e = (x);          \
        if (_e != ESP_OK) return _e; \
    } while (0)

/* ------------ Private Functions ------------ */

/**
 * @brief Build the 24-bit row address for a given block and page.
 *
 * Encoding (datasheet section 7, Figures 8, 22, 23):
 *   RA[15:6] = block[9:0]
 *   RA[5:0]  = page[5:0]
 *   The 24-bit field is sent MSB-first over SPI as [RA23..16, RA15..8, RA7..0].
 *   RA[7:0] is a dummy byte (0x00) as specified in section 9-1.
 *
 * @param block  Block index [0 – 1023].
 * @param page   Page index  [0 – 63].
 * @param ra     Output: three bytes [RA23..16, RA15..8, RA7..0].
 */
static inline void build_row_addr(uint16_t block, uint8_t page, uint8_t ra[3])
{
    uint16_t row = ((uint16_t) block << 6) | (page & 0x3FU);
    ra[0] = (uint8_t) ((row >> 8) & 0xFFU);
    ra[1] = (uint8_t) (row & 0xFFU);
    ra[2] = 0x00U; /* 8-bit dummy — datasheet section 9-1 */
}

/**
 * @brief Build the 16-bit column address for a 1Gb device.
 *
 * Bit layout (datasheet Figure 9):
 *   [15:13] = 3 dummy bits (must be 0)
 *   [12:11] = Wrap[1:0]
 *   [10:0]  = CA[10:0]  (column byte offset, 0 – 2111)
 *
 * @param col   Column byte offset.
 * @param wrap  Wrap mode selector (NAND_WRAP_*).
 * @param ca    Output: two bytes [CA15..8, CA7..0].
 */
static inline void build_col_addr(uint16_t col, uint8_t wrap, uint8_t ca[2])
{
    uint16_t c = ((uint16_t) (wrap & 0x03U) << 11) | (col & 0x7FFU);
    ca[0] = (uint8_t) ((c >> 8) & 0x1FU);
    ca[1] = (uint8_t) (c & 0xFFU);
}

/**
 * @brief Full-duplex SPI transfer: send TX bytes, receive RX bytes.
 *
 * If @p tx is NULL the TX buffer is filled with 0xFF (dummy clocks).
 * If @p rx is NULL the received bytes are discarded.
 *
 * The ESP-IDF DMA engine requires source/destination buffers to reside
 * in DMA-capable RAM. We always copy through the pre-allocated
 * h->dma_tx / h->dma_rx buffers to satisfy this requirement.
 */
static esp_err_t spi_xfer(struct nand_dev_t *h, const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (len == 0U)
    {
        return ESP_OK;
    }

    if (len > NAND_SPI_MAX_TRANSFER)
    {
        ESP_LOGE(NAND_LOG_TAG, "spi_xfer: len=%zu exceeds max=%u", len, NAND_SPI_MAX_TRANSFER);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Stage TX into the DMA-capable buffer */
    if (tx)
    {
        memcpy(h->dma_tx, tx, len);
    }

    else
    {
        memset(h->dma_tx, 0xFFU, len); /* dummy bytes for read phase */
    }

    spi_transaction_t t = {
        .length = len * 8U, /* ESP-IDF uses bits, not bytes */
        .tx_buffer = h->dma_tx,
        .rx_buffer = rx ? h->dma_rx : NULL,
    };

    esp_err_t ret = (len <= 64U) ? spi_device_polling_transmit(h->spi, &t) : spi_device_transmit(h->spi, &t);

    if (ret != ESP_OK)
    {
        ESP_LOGE(NAND_LOG_TAG, "SPI transfer failed: %s", esp_err_to_name(ret));
        return ESP_ERR_INVALID_STATE;
    }

    /* Copy received bytes out of the DMA buffer */
    if (rx)
    {
        memcpy(rx, h->dma_rx, len);
    }

    return ESP_OK;
}

/** Convenience wrapper: transmit only, no RX. */
static inline esp_err_t spi_write(struct nand_dev_t *h, const uint8_t *tx, size_t len)
{
    return spi_xfer(h, tx, NULL, len);
}

/* ------------ Public Functions ------------ */

esp_err_t nand_init(nand_handle_t *out_handle, const nand_config_t *config)
{
    esp_err_t ret;

    NAND_CHECK_ARG(out_handle);
    NAND_CHECK_ARG(config);

#if CONFIG_EALIVE_NAND_MX35_DEBUG
    esp_log_level_set(NAND_LOG_TAG, ESP_LOG_DEBUG);
#endif

    /* Allocate the opaque driver handle */
    struct nand_dev_t *h = calloc(1, sizeof(struct nand_dev_t));
    if (!h)
    {
        ESP_LOGE(NAND_LOG_TAG, "No memory for driver handle");
        return ESP_ERR_NO_MEM;
    }

    /* Allocate DMA-capable TX and RX staging buffers */
    h->dma_tx = heap_caps_malloc(NAND_SPI_MAX_TRANSFER, MALLOC_CAP_DMA);
    h->dma_rx = heap_caps_malloc(NAND_SPI_MAX_TRANSFER, MALLOC_CAP_DMA);
    if (!h->dma_tx || !h->dma_rx)
    {
        ESP_LOGE(NAND_LOG_TAG, "No DMA memory (%u bytes x 2)", NAND_SPI_MAX_TRANSFER);
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* Create the access mutex */
    h->mutex = xSemaphoreCreateMutex();
    if (!h->mutex)
    {
        ESP_LOGE(NAND_LOG_TAG, "Failed to create mutex");
        ret = ESP_ERR_NO_MEM;
        goto fail_alloc;
    }

    h->io_mode = config->io_mode;

    /*
     * Register the NAND as an SPI Master device.
     *
     * CPOL=0, CPHA=0 -> SPI Mode 0 (datasheet section 6, Serial Mode 0).
     * The chip also supports Mode 3 (CPOL=1, CPHA=1); Mode 0 is preferred.
     * CS# is managed automatically by the driver via cs_io_num.
     * queue_size=4 allows up to 4 in-flight transactions before blocking.
     */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = config->clock_speed_hz,
        .mode = 0, /* CPOL=0, CPHA=0 — Serial Mode 0 */
        .spics_io_num = config->pin_cs,
        .queue_size = 4,
        .pre_cb = NULL,
        .post_cb = NULL,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
    };

    ret = spi_bus_add_device(config->spi_host, &devcfg, &h->spi);
    if (ret != ESP_OK)
    {
        ESP_LOGE(NAND_LOG_TAG, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        goto fail_mutex;
    }

    /* Mark initialized so internal functions can be called below */
    h->initialized = true;

    /* Wait for VCC stabilization (datasheet section 12-1: min 1 ms) */
    vTaskDelay(pdMS_TO_TICKS(NAND_POWER_ON_MS + 1U));

    /* Issue RESET and wait for the device to become ready */
    ret = nand_reset(h);
    if (ret != ESP_OK)
    {
        goto fail_spi;
    }

    ret = nand_wait_ready(h, NAND_TIMEOUT_RESET_MS);
    if (ret != ESP_OK)
    {
        goto fail_spi;
    }

    /* Verify manufacturer and device ID */
    uint8_t mfr = 0U, dev_id = 0U;
    ret = nand_read_id(h, &mfr, &dev_id);
    if (ret != ESP_OK)
    {
        goto fail_spi;
    }

    if (mfr != NAND_MANUFACTURER_ID || dev_id != NAND_DEVICE_ID)
    {
        ESP_LOGE(
            NAND_LOG_TAG,
            "Device ID mismatch: got MFR=0x%02X DEV=0x%02X, "
            "expected 0x%02X / 0x%02X",
            mfr,
            dev_id,
            NAND_MANUFACTURER_ID,
            NAND_DEVICE_ID);
        ret = ESP_ERR_NOT_FOUND;
        goto fail_spi;
    }

    /* Populate device info */
    h->info.manufacturer_id = mfr;
    h->info.device_id = dev_id;
    h->info.total_blocks = NAND_BLOCKS_TOTAL;
    h->info.pages_per_block = NAND_PAGES_PER_BLOCK;
    h->info.page_size = NAND_PAGE_SIZE;
    h->info.spare_size = NAND_SPARE_SIZE;

    /* Read current ECC and Quad state from the Configuration register */
    uint8_t feat = 0U;
    ret = nand_get_feature(h, NAND_FEAT_SECURE_OTP, &feat);
    if (ret != ESP_OK)
    {
        goto fail_spi;
    }

    h->info.ecc_enabled = (feat & NAND_OTP_ECC_EN) != 0U;
    h->info.quad_enabled = (feat & NAND_OTP_QE) != 0U;

    /* Enable Quad mode if requested and not already active */
    if (config->io_mode == NAND_IO_X4 && !h->info.quad_enabled)
    {
        ret = nand_quad_enable(h, true);
        if (ret != ESP_OK)
        {
            goto fail_spi;
        }
    }

    /* Remove all block protection so the array is writable */
    ret = nand_unprotect_all(h);
    if (ret != ESP_OK)
    {
        goto fail_spi;
    }

    ESP_LOGI(
        NAND_LOG_TAG,
        "Ready: MFR=0x%02X DEV=0x%02X  blocks=%" PRId32 " pages/block=%" PRId32 " ECC=%s  I/O=x%d  clock=%d Hz",
        mfr,
        dev_id,
        h->info.total_blocks,
        h->info.pages_per_block,
        h->info.ecc_enabled ? "on" : "off",
        (int) config->io_mode,
        config->clock_speed_hz);

    *out_handle = h;
    return ESP_OK;

fail_spi:
    spi_bus_remove_device(h->spi);
fail_mutex:
    vSemaphoreDelete(h->mutex);
fail_alloc:
    heap_caps_free(h->dma_tx);
    heap_caps_free(h->dma_rx);
fail:
    free(h);
    *out_handle = NULL;
    return ret;
}

esp_err_t nand_deinit(nand_handle_t h)
{
    NAND_CHECK_ARG(h);
    struct nand_dev_t *dev = h;
    dev->initialized = false;
    spi_bus_remove_device(dev->spi);
    vSemaphoreDelete(dev->mutex);
    heap_caps_free(dev->dma_tx);
    heap_caps_free(dev->dma_rx);
    free(dev);
    ESP_LOGI(NAND_LOG_TAG, "Driver released");
    return ESP_OK;
}

esp_err_t nand_reset(nand_handle_t h)
{
    NAND_CHECK_ARG(h);
    struct nand_dev_t *dev = h;
    NAND_LOCK(dev);
    uint8_t cmd = NAND_CMD_RESET;
    esp_err_t ret = spi_write(dev, &cmd, 1U);
    NAND_UNLOCK(dev);
    if (ret == ESP_OK)
    {
        ESP_LOGD(NAND_LOG_TAG, "RESET sent");
    }
    return ret;
}

esp_err_t nand_read_id(nand_handle_t h, uint8_t *manufacturer, uint8_t *device)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_ARG(manufacturer);
    NAND_CHECK_ARG(device);

    struct nand_dev_t *dev = h;

    /*
     * READ ID (9Fh) — datasheet Figure 15:
     * CS# low -> [9Fh][1 dummy byte] -> [MFR][DEV] -> CS# high
     * All four bytes are exchanged in a single SPI transaction.
     */
    uint8_t tx[4] = {NAND_CMD_READ_ID, 0x00U, 0x00U, 0x00U};
    uint8_t rx[4] = {0};

    NAND_LOCK(dev);
    esp_err_t ret = spi_xfer(dev, tx, rx, sizeof(tx));
    NAND_UNLOCK(dev);

    if (ret != ESP_OK)
    {
        return ret;
    }

    /* rx[0] = echo of command, rx[1] = echo of dummy, rx[2] = MFR, rx[3] = DEV */
    *manufacturer = rx[2];
    *device = rx[3];

    ESP_LOGD(NAND_LOG_TAG, "READ ID -> MFR=0x%02X DEV=0x%02X", *manufacturer, *device);
    return ESP_OK;
}

esp_err_t nand_get_info(nand_handle_t h, nand_info_t *info)
{
    NAND_CHECK(h);
    NAND_CHECK_ARG(info);
    memcpy(info, &((struct nand_dev_t *) h)->info, sizeof(nand_info_t));
    return ESP_OK;
}

esp_err_t nand_get_feature(nand_handle_t h, uint8_t addr, uint8_t *val)
{
    NAND_CHECK_ARG(h);
    NAND_CHECK_ARG(val);
    struct nand_dev_t *dev = h;

    /*
     * GET FEATURE (0Fh) — datasheet Figure 6:
     * [0Fh][addr]-> [data byte]
     * Three bytes total in one SPI transaction.
     */
    uint8_t tx[3] = {NAND_CMD_GET_FEATURE, addr, 0x00U};
    uint8_t rx[3] = {0};

    NAND_LOCK(dev);
    esp_err_t ret = spi_xfer(dev, tx, rx, sizeof(tx));
    NAND_UNLOCK(dev);

    if (ret == ESP_OK)
    {
        *val = rx[2];
    }
    return ret;
}

esp_err_t nand_set_feature(nand_handle_t h, uint8_t addr, uint8_t val)
{
    NAND_CHECK_ARG(h);
    struct nand_dev_t *dev = h;

    /*
     * SET FEATURE (1Fh) — datasheet Figure 7:
     *   [1Fh][addr][data byte]
     */
    uint8_t tx[3] = {NAND_CMD_SET_FEATURE, addr, val};

    NAND_LOCK(dev);
    esp_err_t ret = spi_write(dev, tx, sizeof(tx));
    NAND_UNLOCK(dev);
    return ret;
}

esp_err_t nand_get_status(nand_handle_t h, uint8_t *status)
{
    return nand_get_feature(h, NAND_FEAT_STATUS, status);
}

esp_err_t nand_wait_ready(nand_handle_t h, uint32_t timeout_ms)
{
    NAND_CHECK_ARG(h);

    int64_t deadline_us = esp_timer_get_time() + (int64_t) timeout_ms * 1000LL;
    uint8_t sr = 0U;
    esp_err_t ret;

    for (;;)
    {
        ret = nand_get_status(h, &sr);
        if (ret != ESP_OK)
        {
            return ret;
        }

        if (!(sr & NAND_SR_OIP))
        {
            /* Device is ready — inspect the status bits for operation errors */
            if (sr & NAND_SR_ERS_FAIL)
            {
                ESP_LOGE(NAND_LOG_TAG, "Erase failure (SR=0x%02X)", sr);
                return ESP_ERR_INVALID_STATE;
            }

            if (sr & NAND_SR_PGM_FAIL)
            {
                ESP_LOGE(NAND_LOG_TAG, "Program failure (SR=0x%02X)", sr);
                return ESP_ERR_INVALID_STATE;
            }

            if ((sr & NAND_SR_ECC_MASK) == NAND_SR_ECC_UNCORRECT)
            {
                ESP_LOGE(NAND_LOG_TAG, "Uncorrectable ECC error (SR=0x%02X)", sr);
                return ESP_ERR_INVALID_CRC;
            }
            return ESP_OK;
        }

        if (esp_timer_get_time() > deadline_us)
        {
            ESP_LOGE(NAND_LOG_TAG, "wait_ready timeout (%u ms, SR=0x%02X)", (unsigned) timeout_ms, sr);
            return ESP_ERR_TIMEOUT;
        }

        /* Poll every 200 µs — fine-grained enough for tRD=25 µs */
        esp_rom_delay_us(200U);
    }
}

esp_err_t nand_ecc_enable(nand_handle_t h, bool enable)
{
    NAND_CHECK(h);
    struct nand_dev_t *dev = h;

    uint8_t feat = 0U;
    RET_ON_ERR(nand_get_feature(h, NAND_FEAT_SECURE_OTP, &feat));

    if (enable)
    {
        feat |= NAND_OTP_ECC_EN;
    }

    else
    {
        feat &= ~NAND_OTP_ECC_EN;
    }

    RET_ON_ERR(nand_set_feature(h, NAND_FEAT_SECURE_OTP, feat));

    dev->info.ecc_enabled = enable;
    ESP_LOGI(NAND_LOG_TAG, "Internal ECC: %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

esp_err_t nand_read_ecc_status(nand_handle_t h, nand_eccsr_t *eccsr)
{
    NAND_CHECK(h);
    NAND_CHECK_ARG(eccsr);
    struct nand_dev_t *dev = h;

    /*
     * ECC Status Read (7Ch) — datasheet Figure 17:
     *   [7Ch][1 dummy byte] -> [ECCSR byte]
     */
    uint8_t tx[3] = {NAND_CMD_ECC_STATUS, 0x00U, 0x00U};
    uint8_t rx[3] = {0};

    NAND_LOCK(dev);
    esp_err_t ret = spi_xfer(dev, tx, rx, sizeof(tx));
    NAND_UNLOCK(dev);

    if (ret == ESP_OK)
    {
        *eccsr = (nand_eccsr_t) (rx[2] & 0x0FU);
        ESP_LOGD(NAND_LOG_TAG, "ECCSR=0x%02X", (unsigned) *eccsr);
    }
    return ret;
}

esp_err_t nand_unprotect_all(nand_handle_t h)
{
    ESP_LOGD(NAND_LOG_TAG, "Removing all block protection");
    /* BP[2:0] = 000 -> all unlocked (datasheet Table 7) */
    return nand_set_feature(h, NAND_FEAT_BLOCK_PROT, 0x00U);
}

esp_err_t nand_protect_all(nand_handle_t h)
{
    ESP_LOGD(NAND_LOG_TAG, "Protecting entire array");
    /* BP[2:0] = 111 -> all locked (datasheet Table 7) */
    return nand_set_feature(h, NAND_FEAT_BLOCK_PROT, NAND_BP_BP0 | NAND_BP_BP1 | NAND_BP_BP2);
}

esp_err_t nand_set_protection(nand_handle_t h, uint8_t val)
{
    return nand_set_feature(h, NAND_FEAT_BLOCK_PROT, val);
}

/**
 * @brief Issue WRITE ENABLE (06h) — must be called while the mutex is held.
 *
 * Sets the WEL bit, which is required before any PROGRAM LOAD,
 * PROGRAM EXECUTE, or BLOCK ERASE command.
 */
static esp_err_t nand_write_enable_locked(struct nand_dev_t *dev)
{
    uint8_t cmd = NAND_CMD_WRITE_ENABLE;
    return spi_write(dev, &cmd, 1U);
}

esp_err_t nand_read_page(nand_handle_t h, uint16_t block, uint8_t page, uint8_t *data, uint8_t *spare, nand_ecc_status_t *ecc_stat)
{
    NAND_CHECK(h);
    if (block >= NAND_BLOCKS_TOTAL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (page >= NAND_PAGES_PER_BLOCK)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!data && !spare)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct nand_dev_t *dev = h;
    esp_err_t ret;
    uint8_t ra[3], ca[2];

    build_row_addr(block, page, ra);
    build_col_addr(0x0000U, NAND_WRAP_2112, ca);

    NAND_LOCK(dev);

    /*
     * Step 1 — PAGE READ (13h), datasheet Figure 8:
     *   [13h][RA23][RA15][RA7] -> CS# HIGH -> device busy for tRD
     * The device transfers the selected page from the array into
     * its internal page register (cache).
     */
    uint8_t tx[4] = {NAND_CMD_PAGE_READ, ra[0], ra[1], ra[2]};
    ret = spi_write(dev, tx, sizeof(tx));
    if (ret != ESP_OK)
    {
        NAND_UNLOCK(dev);
        return ret;
    }

    NAND_UNLOCK(dev);

    /* Wait for the array-to-cache transfer to complete (outside the lock) */
    uint32_t tmo = dev->info.ecc_enabled ? NAND_TIMEOUT_READ_MS * 3U : NAND_TIMEOUT_READ_MS;
    ret = nand_wait_ready(h, tmo);

    /* Capture ECC status from the status register */
    uint8_t sr = 0U;
    nand_get_status(h, &sr);
    if (ecc_stat)
    {
        uint8_t ecc_bits = sr & NAND_SR_ECC_MASK;
        if (ecc_bits == NAND_SR_ECC_NO_ERR)
        {
            *ecc_stat = NAND_ECC_OK;
        }

        else if (ecc_bits == NAND_SR_ECC_CORRECTED)
        {
            *ecc_stat = NAND_ECC_CORRECTED;
            ESP_LOGW(NAND_LOG_TAG, "ECC corrected error: block=%u page=%u", block, page);
        }

        else
        {
            *ecc_stat = NAND_ECC_UNCORRECTED;
        }
    }

    if (ret == ESP_ERR_INVALID_CRC)
    {
        ESP_LOGE(NAND_LOG_TAG, "Uncorrectable ECC: block=%u page=%u", block, page);
        return ret;
    }

    if (ret != ESP_OK)
    {
        return ret;
    }

    NAND_LOCK(dev);

    /*
     * Step 2 — READ FROM CACHE (0Bh), datasheet Figure 9:
     *   [0Bh][Wrap+CA_high][CA_low][1 dummy byte] -> data bytes ...
     *
     * The entire transaction (4-byte header + data payload) is built
     * in the DMA TX buffer and sent as a single SPI frame so that
     * CS# stays asserted throughout.
     */
    if (data)
    {
        size_t hdr_len = 4U; /* cmd(1) + ca(2) + dummy(1) */
        size_t total_len = hdr_len + NAND_PAGE_SIZE;

        memset(dev->dma_tx, 0xFFU, total_len);
        dev->dma_tx[0] = NAND_CMD_READ_CACHE_X1F;
        dev->dma_tx[1] = ca[0];
        dev->dma_tx[2] = ca[1];
        dev->dma_tx[3] = 0x00U; /* 1 dummy byte */

        spi_transaction_t t = {
            .length = total_len * 8U,
            .tx_buffer = dev->dma_tx,
            .rx_buffer = dev->dma_rx,
        };

        ret = spi_device_transmit(dev->spi, &t);
        if (ret == ESP_OK)
        {
            memcpy(data, dev->dma_rx + hdr_len, NAND_PAGE_SIZE);
        }
    }

    /* Read spare area separately if requested (column = 2048) */
    if (ret == ESP_OK && spare)
    {
        build_col_addr(NAND_PAGE_SIZE, NAND_WRAP_64, ca);

        size_t hdr_len = 4U;
        size_t total_len = hdr_len + NAND_SPARE_SIZE;

        memset(dev->dma_tx, 0xFFU, total_len);
        dev->dma_tx[0] = NAND_CMD_READ_CACHE_X1F;
        dev->dma_tx[1] = ca[0];
        dev->dma_tx[2] = ca[1];
        dev->dma_tx[3] = 0x00U;

        spi_transaction_t t = {
            .length = total_len * 8U,
            .tx_buffer = dev->dma_tx,
            .rx_buffer = dev->dma_rx,
        };

        ret = spi_device_transmit(dev->spi, &t);
        if (ret == ESP_OK)
        {
            memcpy(spare, dev->dma_rx + hdr_len, NAND_SPARE_SIZE);
        }
    }

    NAND_UNLOCK(dev);
    return ret;
}

esp_err_t nand_read(nand_handle_t h, nand_addr_t addr, uint8_t *buf, size_t len)
{
    NAND_CHECK(h);
    NAND_CHECK_ARG(buf);

    uint8_t *page_buf = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    if (!page_buf)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    size_t bytes_done = 0U;
    uint16_t block = addr.block;
    uint8_t page = addr.page;
    uint16_t col = addr.column;

    while (bytes_done < len)
    {
        if (block >= NAND_BLOCKS_TOTAL)
        {
            ret = ESP_ERR_INVALID_ARG;
            break;
        }

        ret = nand_read_page(h, block, page, page_buf, NULL, NULL);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_CRC)
        {
            break;
        }

        size_t avail = NAND_PAGE_SIZE - col;
        size_t to_copy = ((len - bytes_done) < avail) ? (len - bytes_done) : avail;
        memcpy(buf + bytes_done, page_buf + col, to_copy);
        bytes_done += to_copy;
        col = 0U;

        if (++page >= NAND_PAGES_PER_BLOCK)
        {
            page = 0U;
            block++;
        }
    }

    free(page_buf);
    return ret;
}

esp_err_t nand_program_page(nand_handle_t h, uint16_t block, uint8_t page, const uint8_t *data, const uint8_t *spare)
{
    NAND_CHECK(h);
    if (block >= NAND_BLOCKS_TOTAL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (page >= NAND_PAGES_PER_BLOCK)
    {
        return ESP_ERR_INVALID_ARG;
    }
    NAND_CHECK_ARG(data);

    struct nand_dev_t *dev = h;
    esp_err_t ret;
    uint8_t ra[3], ca[2];

    build_row_addr(block, page, ra);
    build_col_addr(0x0000U, NAND_WRAP_2112, ca);

    NAND_LOCK(dev);

    /*
     * Step 1 — WRITE ENABLE (06h), datasheet Figure 4.
     * Sets the WEL bit; required before every program/erase operation.
     */
    ret = nand_write_enable_locked(dev);
    if (ret != ESP_OK)
    {
        NAND_UNLOCK(dev);
        return ret;
    }

    /*
     * Step 2 — PROGRAM LOAD (02h), datasheet Figure 18:
     *   [02h][CA_high][CA_low][data bytes 0..2047][spare bytes 0..63]
     *
     * The command header (3 bytes) and the full page data are packed
     * into the DMA TX buffer for a single SPI transaction, keeping
     * CS# asserted for the entire payload.
     */
    const size_t hdr_len = 3U; /* cmd(1) + ca_high(1) + ca_low(1) */
    const size_t spare_len = spare ? NAND_SPARE_SIZE : 0U;
    const size_t total_len = hdr_len + NAND_PAGE_SIZE + spare_len;

    dev->dma_tx[0] = NAND_CMD_PROG_LOAD;
    dev->dma_tx[1] = ca[0];
    dev->dma_tx[2] = ca[1];
    memcpy(dev->dma_tx + hdr_len, data, NAND_PAGE_SIZE);
    if (spare)
    {
        memcpy(dev->dma_tx + hdr_len + NAND_PAGE_SIZE, spare, NAND_SPARE_SIZE);
    }

    spi_transaction_t t = {
        .length = total_len * 8U,
        .tx_buffer = dev->dma_tx,
        .rx_buffer = NULL,
    };

    ret = spi_device_transmit(dev->spi, &t);
    if (ret != ESP_OK)
    {
        ESP_LOGE(NAND_LOG_TAG, "PROGRAM LOAD failed: %s", esp_err_to_name(ret));
        NAND_UNLOCK(dev);
        return ret;
    }

    /*
     * Step 3 — PROGRAM EXECUTE (10h), datasheet Figure 22:
     *   [10h][RA23][RA15][RA7] -> CS# HIGH -> device busy for tPROG
     * Transfers the loaded cache to the NAND array.
     */
    uint8_t tx[4] = {NAND_CMD_PROG_EXECUTE, ra[0], ra[1], ra[2]};
    ret = spi_write(dev, tx, sizeof(tx));

    NAND_UNLOCK(dev);

    if (ret != ESP_OK)
    {
        return ret;
    }

    /* Poll for completion outside the mutex */
    ret = nand_wait_ready(h, NAND_TIMEOUT_PROG_MS);
    if (ret != ESP_OK)
    {
        ESP_LOGE(NAND_LOG_TAG, "Program failed: block=%u page=%u err=%s", block, page, esp_err_to_name(ret));
    }

    else
    {
        ESP_LOGD(NAND_LOG_TAG, "Program OK: block=%u page=%u", block, page);
    }
    return ret;
}

esp_err_t nand_erase_block(nand_handle_t h, uint16_t block)
{
    NAND_CHECK(h);
    if (block >= NAND_BLOCKS_TOTAL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct nand_dev_t *dev = h;
    esp_err_t ret;
    uint8_t ra[3];

    build_row_addr(block, 0U, ra);

    NAND_LOCK(dev);

    /* WRITE ENABLE */
    ret = nand_write_enable_locked(dev);
    if (ret != ESP_OK)
    {
        NAND_UNLOCK(dev);
        return ret;
    }

    /*
     * BLOCK ERASE (D8h) — datasheet Figure 23:
     *   [D8h][RA23][RA15][RA7 dummy] -> CS# HIGH -> device busy for tBE
     * The 24-bit address includes a 16-bit row address + 8-bit dummy.
     * Any address within the target block is valid.
     */
    uint8_t tx[4] = {NAND_CMD_BLOCK_ERASE, ra[0], ra[1], ra[2]};
    ret = spi_write(dev, tx, sizeof(tx));

    NAND_UNLOCK(dev);

    if (ret != ESP_OK)
    {
        return ret;
    }

    ret = nand_wait_ready(h, NAND_TIMEOUT_ERASE_MS);
    if (ret != ESP_OK)
    {
        ESP_LOGE(NAND_LOG_TAG, "Erase failed: block=%u err=%s", block, esp_err_to_name(ret));
    }

    else
    {
        ESP_LOGD(NAND_LOG_TAG, "Erase OK: block=%u", block);
    }
    return ret;
}

esp_err_t nand_is_bad_block(nand_handle_t h, uint16_t block, bool *is_bad)
{
    NAND_CHECK(h);
    NAND_CHECK_ARG(is_bad);
    if (block >= NAND_BLOCKS_TOTAL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t spare0[NAND_SPARE_SIZE] = {0};
    uint8_t spare1[NAND_SPARE_SIZE] = {0};
    esp_err_t ret;

    /*
     * Datasheet section 11-1:
     * A block is bad if spare[0] of page 0 OR page 1 is not 0xFF.
     * Good blocks ship with all cells set to 0xFF.
     */
    ret = nand_read_page(h, block, 0U, NULL, spare0, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_CRC)
    {
        return ret;
    }

    ret = nand_read_page(h, block, 1U, NULL, spare1, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_CRC)
    {
        return ret;
    }

    *is_bad = (spare0[0] == 0x00U) || (spare1[0] == 0x00U);

    if (*is_bad)
    {
        ESP_LOGW(NAND_LOG_TAG, "Bad block detected: %u (spare0[0]=0x%02X spare1[0]=0x%02X)", block, spare0[0], spare1[0]);
    }
    return ESP_OK;
}

esp_err_t nand_mark_bad_block(nand_handle_t h, uint16_t block)
{
    NAND_CHECK(h);
    if (block >= NAND_BLOCKS_TOTAL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct nand_dev_t *dev = h;
    esp_err_t ret;
    uint8_t ra[3], ca[2];

    /* Column address = start of spare area (byte 2048 of the page) */
    build_row_addr(block, 0U, ra);
    build_col_addr(NAND_PAGE_SIZE, 0U, ca);

    NAND_LOCK(dev);

    ret = nand_write_enable_locked(dev);
    if (ret != ESP_OK)
    {
        NAND_UNLOCK(dev);
        return ret;
    }

    /* Program only the spare area: spare[0] = 0x00, rest = 0xFF */
    uint8_t spare_buf[NAND_SPARE_SIZE];
    memset(spare_buf, 0xFFU, sizeof(spare_buf));
    spare_buf[0] = 0x00U; /* bad block marker */

    const size_t total = 3U + NAND_SPARE_SIZE;
    dev->dma_tx[0] = NAND_CMD_PROG_LOAD;
    dev->dma_tx[1] = ca[0];
    dev->dma_tx[2] = ca[1];
    memcpy(dev->dma_tx + 3U, spare_buf, NAND_SPARE_SIZE);

    spi_transaction_t t = {
        .length = total * 8U,
        .tx_buffer = dev->dma_tx,
        .rx_buffer = NULL,
    };

    ret = spi_device_transmit(dev->spi, &t);
    if (ret == ESP_OK)
    {
        uint8_t tx[4] = {NAND_CMD_PROG_EXECUTE, ra[0], ra[1], ra[2]};
        ret = spi_write(dev, tx, sizeof(tx));
    }

    NAND_UNLOCK(dev);

    if (ret == ESP_OK)
    {
        ret = nand_wait_ready(h, NAND_TIMEOUT_PROG_MS);
    }

    if (ret == ESP_OK)
    {
        ESP_LOGW(NAND_LOG_TAG, "Block %u marked as bad", block);
    }
    return ret;
}

esp_err_t nand_quad_enable(nand_handle_t h, bool enable)
{
    NAND_CHECK(h);
    struct nand_dev_t *dev = h;

    uint8_t feat = 0U;
    RET_ON_ERR(nand_get_feature(h, NAND_FEAT_SECURE_OTP, &feat));

    if (enable)
    {
        feat |= NAND_OTP_QE;
    }

    else
    {
        feat &= ~NAND_OTP_QE;
    }

    RET_ON_ERR(nand_set_feature(h, NAND_FEAT_SECURE_OTP, feat));

    dev->info.quad_enabled = enable;
    ESP_LOGI(NAND_LOG_TAG, "Quad I/O: %s", enable ? "enabled" : "disabled");
    return ESP_OK;
}

void nand_linear_to_addr(uint32_t linear, nand_addr_t *addr)
{
    if (!addr)
    {
        return;
    }
    uint32_t page_idx = linear / NAND_PAGE_SIZE;
    addr->column = (uint16_t) (linear % NAND_PAGE_SIZE);
    addr->page = (uint8_t) (page_idx % NAND_PAGES_PER_BLOCK);
    addr->block = (uint16_t) (page_idx / NAND_PAGES_PER_BLOCK);
}

uint32_t nand_addr_to_linear(const nand_addr_t *addr)
{
    if (!addr)
    {
        return 0U;
    }
    return (uint32_t) addr->block * NAND_PAGES_PER_BLOCK * NAND_PAGE_SIZE + (uint32_t) addr->page * NAND_PAGE_SIZE + addr->column;
}

const char *nand_err_to_str(esp_err_t err)
{
    return esp_err_to_name(err);
}
