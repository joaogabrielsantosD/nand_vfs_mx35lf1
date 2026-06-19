#ifndef MX35LF1GE4AB_H
#define MX35LF1GE4AB_H

/**
 * Manufacturer Part Number: MX35LF1GE4AB-Z4I
 * Device geometry:
 *   - Page:   2048 + 64 bytes spare  ->  2112 bytes total
 *   - Block:  64 pages               ->  128 KB data + 4 KB spare
 *   - Array:  1024 blocks            ->  128 MB data
 *   - ECC:    4-bit per 512-byte segment (enabled by default)
 *   - fSCLK:  up to 104 MHz
 */

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* DEVICE GEOMETRY */
#define NAND_PAGE_SIZE        2048U                                   /**< Data bytes per page                      */
#define NAND_SPARE_SIZE       64U                                     /**< Spare / OOB bytes per page               */
#define NAND_PAGE_FULL_SIZE   (NAND_PAGE_SIZE + NAND_SPARE_SIZE)      /**< Total bytes per page (2112)              */
#define NAND_PAGES_PER_BLOCK  64U                                     /**< Pages per erase block                    */
#define NAND_BLOCKS_TOTAL     1024U                                   /**< Total erase blocks                       */
#define NAND_BLOCK_SIZE       (NAND_PAGE_SIZE * NAND_PAGES_PER_BLOCK) /**< Data bytes per block (128 KB)            */
#define NAND_TOTAL_SIZE       (NAND_BLOCK_SIZE * NAND_BLOCKS_TOTAL)   /**< Total data capacity (128 MB)             */
#define NAND_MIN_GOOD_BLOCKS  1004U                                   /**< Minimum guaranteed good blocks           */
#define NAND_ECC_SEGMENTS     4U                                      /**< ECC segments per page                    */
#define NAND_ECC_SEGMENT_SIZE 512U                                    /**< Bytes per ECC segment                    */

/* SPI COMMAND OPCODES  (datasheet Table 1) */
#define NAND_CMD_WRITE_ENABLE   0x06U /**< Set Write Enable Latch (WEL)     */
#define NAND_CMD_WRITE_DISABLE  0x04U /**< Reset Write Enable Latch         */
#define NAND_CMD_GET_FEATURE    0x0FU /**< Read feature register            */
#define NAND_CMD_SET_FEATURE    0x1FU /**< Write feature register           */
#define NAND_CMD_PAGE_READ      0x13U /**< Transfer page: array -> cache    */
#define NAND_CMD_READ_CACHE_X1  0x03U /**< Read from cache ×1 (slow)        */
#define NAND_CMD_READ_CACHE_X1F 0x0BU /**< Read from cache ×1 (fast)        */
#define NAND_CMD_READ_CACHE_X2  0x3BU /**< Read from cache ×2 (dual)        */
#define NAND_CMD_READ_CACHE_X4  0x6BU /**< Read from cache ×4 (quad)        */
#define NAND_CMD_READ_CACHE_SEQ 0x31U /**< Page Read Cache Sequential       */
#define NAND_CMD_READ_CACHE_END 0x3FU /**< Page Read Cache End              */
#define NAND_CMD_READ_ID        0x9FU /**< Read manufacturer + device ID    */
#define NAND_CMD_ECC_STATUS     0x7CU /**< Read internal ECC status         */
#define NAND_CMD_PROG_LOAD      0x02U /**< Program Load (cache reset first) */
#define NAND_CMD_PROG_LOAD_RND  0x84U /**< Program Load Random Data         */
#define NAND_CMD_PROG_LOAD_X4   0x32U /**< Program Load ×4 (quad)           */
#define NAND_CMD_PROG_LOAD_RX4  0x34U /**< Program Load Random Data ×4      */
#define NAND_CMD_PROG_EXECUTE   0x10U /**< Execute: transfer cache -> array  */
#define NAND_CMD_BLOCK_ERASE    0xD8U /**< Erase an entire block            */
#define NAND_CMD_RESET          0xFFU /**< Software reset                   */

/* FEATURE REGISTER ADDRESSES  (datasheet Table 2-1) */
#define NAND_FEAT_BLOCK_PROT 0xA0U /**< Block Protection register        */
#define NAND_FEAT_SECURE_OTP 0xB0U /**< Secure OTP / Configuration reg.  */
#define NAND_FEAT_STATUS     0xC0U /**< Status register (read-only)      */

/* STATUS REGISTER BIT MASKS  (datasheet Table 9) */
#define NAND_SR_OIP           (1U << 0) /**< Operation In Progress (1=busy) */
#define NAND_SR_WEL           (1U << 1) /**< Write Enable Latch             */
#define NAND_SR_ERS_FAIL      (1U << 2) /**< Erase failure flag             */
#define NAND_SR_PGM_FAIL      (1U << 3) /**< Program failure flag           */
#define NAND_SR_ECC_S0        (1U << 4) /**< ECC status bit 0               */
#define NAND_SR_ECC_S1        (1U << 5) /**< ECC status bit 1               */
#define NAND_SR_CRBSY         (1U << 6) /**< Cache Read Busy                */
#define NAND_SR_ECC_MASK      (NAND_SR_ECC_S1 | NAND_SR_ECC_S0)
#define NAND_SR_ECC_NO_ERR    (0x00U << 4) /**< No bit errors               */
#define NAND_SR_ECC_CORRECTED (0x01U << 4) /**< 1–4 bits corrected          */
#define NAND_SR_ECC_UNCORRECT (0x02U << 4) /**< Uncorrectable error         */

/* CONFIGURATION REGISTER BIT MASKS  (address 0xB0) */
#define NAND_OTP_QE      (1U << 0) /**< Quad Enable (QE bit)           */
#define NAND_OTP_ECC_EN  (1U << 4) /**< Internal ECC enable            */
#define NAND_OTP_ENABLE  (1U << 6) /**< Secure OTP access enable       */
#define NAND_OTP_PROTECT (1U << 7) /**< Secure OTP protect (NV)        */

/* BLOCK PROTECTION REGISTER BIT MASKS  (address 0xA0) */
#define NAND_BP_SP            (1U << 0) /**< Solid Protection bit           */
#define NAND_BP_COMPLEMENTARY (1U << 1) /**< Complementary protection bit   */
#define NAND_BP_INVERT        (1U << 2) /**< Invert protection bit          */
#define NAND_BP_BP0           (1U << 3) /**< Block Protection bit 0         */
#define NAND_BP_BP1           (1U << 4) /**< Block Protection bit 1         */
#define NAND_BP_BP2           (1U << 5) /**< Block Protection bit 2         */
#define NAND_BP_BPRWD         (1U << 7) /**< BP Register Write Disable      */

/* DEVICE IDENTIFICATION  (datasheet Table 4)*/
#define NAND_MANUFACTURER_ID 0xC2U /**< Macronix manufacturer ID         */
#define NAND_DEVICE_ID       0x12U /**< MX35LF1GE4AB device ID           */

/* TIMING CONSTANTS  (datasheet Table 18) */
#define NAND_tRD_US      25U  /**< Page read time without ECC [µs]   */
#define NAND_tRD_ECC_US  70U  /**< Page read time with ECC [µs]      */
#define NAND_tPROG_US    600U /**< Max page program time [µs]        */
#define NAND_tERS_MS     4U   /**< Max block erase time [ms]         */
#define NAND_tRST_US     500U /**< Max reset time (worst: erase)[µs] */
#define NAND_POWER_ON_MS 2U   /**< Power-on stabilization time [ms]  */

/** Polling timeouts — 2× worst-case margin */
#define NAND_TIMEOUT_RESET_MS 10U
#define NAND_TIMEOUT_READ_MS  10U
#define NAND_TIMEOUT_PROG_MS  50U
#define NAND_TIMEOUT_ERASE_MS 100U

/* WRAP ADDRESS BITS  (datasheet Table 3, 1Gb only) */
#define NAND_WRAP_2112 0x00U /**< Wrap length = 2112 bytes         */
#define NAND_WRAP_2048 0x01U /**< Wrap length = 2048 bytes         */
#define NAND_WRAP_64   0x02U /**< Wrap length = 64 bytes           */
#define NAND_WRAP_16   0x03U /**< Wrap length = 16 bytes           */

#define NAND_SPI_MAX_TRANSFER (NAND_PAGE_FULL_SIZE + 8U) /* 2120 bytes */

#define NAND_IO_UNUSED (-1)

/**
 * @brief Simplified ECC status returned by read operations.
 */
typedef enum
{
    NAND_ECC_OK = 0,          /**< No bit errors detected                  */
    NAND_ECC_CORRECTED = 1,   /**< 1–4 bit errors corrected by hardware    */
    NAND_ECC_UNCORRECTED = 2, /**< More than 4 errors; data is unreliable  */
} nand_ecc_status_t;

/**
 * @brief Detailed ECC status from the ECCSR register (command 0x7C).
 *
 * Reports the worst-case segment among the four 512-byte ECC segments
 * of the last page that was read.
 */
typedef enum
{
    NAND_ECCSR_NO_ERROR = 0x00,  /**< No bit error                          */
    NAND_ECCSR_1BIT = 0x01,      /**< 1-bit error corrected                 */
    NAND_ECCSR_2BIT = 0x02,      /**< 2-bit errors corrected                */
    NAND_ECCSR_3BIT = 0x03,      /**< 3-bit errors corrected                */
    NAND_ECCSR_4BIT = 0x04,      /**< 4-bit errors corrected                */
    NAND_ECCSR_UNCORRECT = 0x0F, /**< Uncorrectable — data loss             */
} nand_eccsr_t;

/**
 * @brief SPI I/O width.
 */
typedef enum
{
    NAND_IO_X1 = 1, /**< Single I/O (standard SPI)                        */
    NAND_IO_X2 = 2, /**< Dual I/O                                         */
    NAND_IO_X4 = 4, /**< Quad I/O (requires QE bit = 1)                   */
} nand_io_mode_t;

/**
 * @brief Read-only information about the device, populated by nand_init().
 */
typedef struct
{
    uint8_t manufacturer_id;  /**< 0xC2 (Macronix)                          */
    uint8_t device_id;        /**< 0x12 (MX35LF1GE4AB)                      */
    uint32_t total_blocks;    /**< 1024                                     */
    uint32_t pages_per_block; /**< 64                                       */
    uint32_t page_size;       /**< 2048 bytes                               */
    uint32_t spare_size;      /**< 64 bytes                                 */
    bool ecc_enabled;         /**< Internal ECC currently active            */
    bool quad_enabled;        /**< Quad I/O mode currently active           */
} nand_info_t;

/**
 * @brief Logical address within the NAND array.
 */
typedef struct
{
    uint16_t block;  /**< Block index  [0 – 1023]                          */
    uint8_t page;    /**< Page index   [0 – 63]                            */
    uint16_t column; /**< Byte offset  [0 – 2111]                          */
} nand_addr_t;

/**
 * @brief Hardware configuration passed to nand_init().
 *
 * The SPI bus must be initialized with spi_bus_initialize() BEFORE calling
 * nand_init(). The driver registers the device onto that bus internally.
 */
typedef struct
{
    spi_host_device_t spi_host; /**< SPI peripheral: SPI2_HOST or SPI3_HOST   */
    int pin_cs;                 /**< Chip-Select GPIO (active LOW)            */
    int pin_mosi;               /**< MOSI / SIO0                              */
    int pin_miso;               /**< MISO / SIO1                              */
    int pin_sclk;               /**< Serial clock                             */
    int pin_wp;                 /**< WP# / SIO2  (-1 if unused)              */
    int pin_hold;               /**< HOLD# / SIO3 (-1 if unused)             */
    int clock_speed_hz;         /**< SPI clock frequency in Hz (≤ 104 MHz) */
    nand_io_mode_t io_mode;     /**< Desired I/O width                        */
    int dma_chan;               /**< DMA channel — use SPI_DMA_CH_AUTO        */
} nand_config_t;


typedef struct nand_dev_t *nand_handle_t;


/**
 * @brief Initialize the NAND driver and verify device identity.
 *
 * Steps performed internally:
 *  1. Register the SPI device onto the bus provided in @p config.
 *  2. Wait for power-on stabilization (2 ms).
 *  3. Issue RESET (FFh) and poll until ready.
 *  4. Read and verify the device ID (must be 0xC2 / 0x12).
 *  5. Read current ECC and protection settings.
 *  6. Remove all block protection.
 *  7. Create an internal FreeRTOS mutex for thread-safe access.
 *
 * @param[out] out_handle  Receives the allocated driver handle.
 * @param[in]  config      Hardware configuration (pins, SPI host, clock).
 * @return     ESP_OK on success.
 *             ESP_ERR_NO_MEM      if heap allocation fails.
 *             ESP_ERR_NOT_FOUND   if the device ID does not match.
 *             Other esp_err_t     on SPI or timeout errors.
 */
esp_err_t nand_init(nand_handle_t *out_handle, const nand_config_t *config);

/**
 * @brief Release all resources held by the driver.
 *
 * Removes the SPI device from the bus and deletes the internal mutex.
 * The handle is invalid after this call.
 *
 * @param[in] h  Driver handle returned by nand_init().
 * @return ESP_OK or ESP_ERR_INVALID_ARG.
 */
esp_err_t nand_deinit(nand_handle_t h);

/**
 * @brief Issue a software RESET command (FFh).
 *
 * Resets the device state machine. The OIP bit in the status register
 * goes high during the reset and clears when done; use nand_wait_ready()
 * to poll for completion.
 */
esp_err_t nand_reset(nand_handle_t h);

/**
 * @brief Read the manufacturer ID and device ID (READ ID, 9Fh).
 *
 * @param[out] manufacturer  Expected: 0xC2 (Macronix).
 * @param[out] device        Expected: 0x12 (MX35LF1GE4AB).
 */
esp_err_t nand_read_id(nand_handle_t h, uint8_t *manufacturer, uint8_t *device);

/**
 * @brief Copy device information into caller-supplied struct.
 *
 * Returns a snapshot of the information gathered at nand_init() time.
 */
esp_err_t nand_get_info(nand_handle_t h, nand_info_t *info);

/**
 * @brief Read a feature register (GET FEATURE, 0Fh).
 *
 * @param[in]  addr  Register address: NAND_FEAT_BLOCK_PROT (0xA0),
 *                   NAND_FEAT_SECURE_OTP (0xB0), or NAND_FEAT_STATUS (0xC0).
 * @param[out] val   Value read from the register.
 */
esp_err_t nand_get_feature(nand_handle_t h, uint8_t addr, uint8_t *val);

/**
 * @brief Write a feature register (SET FEATURE, 1Fh).
 *
 * @param[in] addr  Register address (see nand_get_feature).
 * @param[in] val   Value to write.
 */
esp_err_t nand_set_feature(nand_handle_t h, uint8_t addr, uint8_t val);

/**
 * @brief Shortcut: read the status register (GET FEATURE at 0xC0).
 *
 * @param[out] status  Raw status byte; test with NAND_SR_* bitmasks.
 */
esp_err_t nand_get_status(nand_handle_t h, uint8_t *status);

/**
 * @brief Poll the OIP bit until the device is ready or timeout expires.
 *
 * Uses esp_timer_get_time() for microsecond-accurate timeout tracking.
 * Polls every 200 µs using esp_rom_delay_us() to avoid FreeRTOS tick
 * granularity limitations for short operations (tRD = 25 µs).
 *
 * On completion, also checks ERS_FAIL, PGM_FAIL and the ECC status.
 *
 * @param[in] timeout_ms  Maximum wait time in milliseconds.
 * @return ESP_OK              — device is ready with no errors.
 *         ESP_ERR_TIMEOUT     — device did not become ready in time.
 *         ESP_ERR_FLASH_OP_FAIL — erase or program failure reported.
 *         ESP_ERR_INVALID_CRC  — uncorrectable ECC error reported.
 */
esp_err_t nand_wait_ready(nand_handle_t h, uint32_t timeout_ms);

/**
 * @brief Enable or disable the internal 4-bit ECC engine.
 *
 * The ECC engine is enabled by default after power-on.
 * When disabled, the host controller is responsible for ECC.
 */
esp_err_t nand_ecc_enable(nand_handle_t h, bool enable);

/**
 * @brief Read the detailed ECC status register (command 0x7Ch).
 *
 * Reports the worst-case error count across all four 512-byte ECC
 * segments of the last page read. Updated after every PAGE READ (13h).
 *
 * @param[out] eccsr  Detailed status; one of the NAND_ECCSR_* values.
 */
esp_err_t nand_read_ecc_status(nand_handle_t h, nand_eccsr_t *eccsr);

/**
 * @brief Remove all block protection (BP[2:0] = 000 -> all unlocked).
 *
 * Called automatically by nand_init(). Call again if nand_protect_all()
 * was used and a write operation is needed.
 */
esp_err_t nand_unprotect_all(nand_handle_t h);

/**
 * @brief Protect the entire array (BP[2:0] = 111 -> all locked).
 */
esp_err_t nand_protect_all(nand_handle_t h);

/**
 * @brief Write the Block Protection register directly.
 *
 * Use NAND_BP_* bitmasks to compose the desired value.
 * Refer to datasheet Table 7 for the protection area definitions.
 *
 * @param[in] val  Value to write to register 0xA0.
 */
esp_err_t nand_set_protection(nand_handle_t h, uint8_t val);

/**
 * @brief Read one complete page (data area + optional spare area).
 *
 * Sequence:
 *  1. PAGE READ (13h): transfer selected page from array to internal cache.
 *  2. Poll OIP until ready (tRD = 25 µs typical, 70 µs with ECC).
 *  3. READ FROM CACHE (0Bh): clock out data bytes over SPI (DMA).
 *
 * Data and spare buffers must be allocated in DMA-capable RAM when using
 * this function directly (use heap_caps_malloc(MALLOC_CAP_DMA)).
 * If the buffers are in regular heap, the driver copies through its own
 * internal DMA buffers transparently.
 *
 * @param[in]  block     Block index [0 – 1023].
 * @param[in]  page      Page index  [0 – 63].
 * @param[out] data      Destination for page data (>= NAND_PAGE_SIZE bytes).
 *                       Pass NULL to skip reading the data area.
 * @param[out] spare     Destination for spare bytes (>= NAND_SPARE_SIZE bytes).
 *                       Pass NULL to skip reading the spare area.
 * @param[out] ecc_stat  ECC result after the read; pass NULL to ignore.
 *
 * @return ESP_OK               — success (data may have been ECC-corrected).
 *         ESP_ERR_INVALID_CRC  — uncorrectable ECC error; data is unreliable.
 *         Other esp_err_t      — SPI or timeout error.
 */
esp_err_t nand_read_page(nand_handle_t h, uint16_t block, uint8_t page, uint8_t *data, uint8_t *spare, nand_ecc_status_t *ecc_stat);

/**
 * @brief Read an arbitrary number of bytes from a logical address.
 *
 * Internally splits the request across as many pages as necessary.
 * Allocates a temporary page buffer from the DMA heap per call.
 *
 * @param[in]  addr  Starting address (block, page, column).
 * @param[out] buf   Destination buffer.
 * @param[in]  len   Number of bytes to read.
 */
esp_err_t nand_read(nand_handle_t h, nand_addr_t addr, uint8_t *buf, size_t len);

/**
 * @brief Program one complete page.
 *
 * Sequence:
 *  1. WRITE ENABLE (06h) — sets the WEL bit.
 *  2. PROGRAM LOAD (02h) — sends data and optional spare over SPI into cache.
 *  3. PROGRAM EXECUTE (10h) — transfers cache to the NAND array.
 *  4. Poll OIP until ready (tPROG = 300 µs typical, 600 µs max).
 *
 * @note The target block **must** be erased before programming.
 *       Writing to a page that has not been erased causes silent data
 *       corruption — a fundamental constraint of NAND flash technology.
 *
 * @param[in] block  Block index [0 – 1023].
 * @param[in] page   Page index  [0 – 63].
 * @param[in] data   Data to write (NAND_PAGE_SIZE bytes, must not be NULL).
 * @param[in] spare  Spare bytes to write (NAND_SPARE_SIZE bytes), or NULL
 *                   to fill the spare area with 0xFF.
 *
 * @return ESP_OK               — success.
 *         ESP_ERR_FLASH_OP_FAIL — the device reported a program failure.
 *         Other esp_err_t      — SPI or timeout error.
 */
esp_err_t nand_program_page(nand_handle_t h, uint16_t block, uint8_t page, const uint8_t *data, const uint8_t *spare);

/**
 * @brief Erase one block (128 KB of data + 4 KB spare -> all bytes set to 0xFF).
 *
 * Sequence:
 *  1. WRITE ENABLE (06h).
 *  2. BLOCK ERASE (D8h) with the 24-bit row address.
 *  3. Poll OIP until ready (tBE = 1 ms typical, 3.5 ms max).
 *
 * Any address within the target block is a valid row address.
 *
 * @param[in] block  Block index to erase [0 – 1023].
 *
 * @return ESP_OK               — success; all cells in the block are 0xFF.
 *         ESP_ERR_FLASH_OP_FAIL — the device reported an erase failure.
 *         Other esp_err_t      — SPI or timeout error.
 */
esp_err_t nand_erase_block(nand_handle_t h, uint16_t block);

/**
 * @brief Check whether a block is marked as bad.
 *
 * A block is considered bad if the first byte of the spare area on
 * page 0 OR page 1 is not 0xFF (datasheet section 11-1).
 *
 * @param[in]  block   Block index to check.
 * @param[out] is_bad  Set to true if the block is bad.
 */
esp_err_t nand_is_bad_block(nand_handle_t h, uint16_t block, bool *is_bad);

/**
 * @brief Permanently mark a block as bad.
 *
 * Writes 0x00 to spare byte 0 of page 0 without erasing the block first,
 * so that the factory-programmed bad block marker is preserved.
 *
 * @param[in] block  Block index to mark as bad.
 */
esp_err_t nand_mark_bad_block(nand_handle_t h, uint16_t block);

/**
 * @brief Enable or disable Quad I/O mode (QE bit in register 0xB0).
 *
 * When QE = 1:
 *  - SIO2 and SIO3 are used as data lines (WP# and HOLD# are disabled).
 *  - Hardware Protection Mode (HPM) is unavailable.
 *  - The QE bit is volatile and resets to 0 on power-cycle.
 *
 * @param[in] enable  true to enable Quad mode, false to disable.
 */
esp_err_t nand_quad_enable(nand_handle_t h, bool enable);

/**
 * @brief Convert a linear byte address to a block/page/column struct.
 *
 * @param[in]  linear  Byte offset from the start of the data array [0 .. NAND_TOTAL_SIZE-1].
 * @param[out] addr    Corresponding block, page, and column.
 */
void nand_linear_to_addr(uint32_t linear, nand_addr_t *addr);

/**
 * @brief Convert a block/page/column address to a linear byte offset.
 *
 * @param[in] addr  Structured address.
 * @return Linear byte offset.
 */
uint32_t nand_addr_to_linear(const nand_addr_t *addr);

/**
 * @brief Return a human-readable string for an esp_err_t code.
 *
 * Thin wrapper around esp_err_to_name().
 */
const char *nand_err_to_str(esp_err_t err);

#endif /* MX35LF1GE4AB_H */
