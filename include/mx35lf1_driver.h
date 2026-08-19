#ifndef MX35LF1_DRIVER_H
#define MX35LF1_DRIVER_H

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
    NAND_RET_OK = 0,
    NAND_RET_BAD_SPI = -1,
    NAND_RET_TIMEOUT = -2,
    NAND_RET_DEVICE_ID = -3,
    NAND_RET_BAD_ADDRESS = -4,
    NAND_RET_INVALID_LEN = -5,
    NAND_RET_ECC_REFRESH = -6,
    NAND_RET_ECC_ERR = -7,
    NAND_RET_P_FAIL = -8,
    NAND_RET_E_FAIL = -9,
    NAND_RET_E_INVALID_ARG = -10,
} nand_err_t;

/* DEVICE GEOMETRY */
#define NAND_PAGE_SIZE        2048U                                   /**< Data bytes per page                      */
#define NAND_SPARE_SIZE       64U                                     /**< Spare / OOB bytes per page               */
#define NAND_PAGE_FULL_SIZE   (NAND_PAGE_SIZE + NAND_SPARE_SIZE)      /**< Total bytes per page (2112)              */
#define NAND_PAGES_PER_BLOCK  64U                                     /**< Pages per erase block                    */
#define NAND_BLOCKS_PER_LUN   1024U                                   /**< Total erase blocks                       */
#define NAND_BLOCK_SIZE       (NAND_PAGE_SIZE * NAND_PAGES_PER_BLOCK) /**< Data bytes per block (128 KB)            */
#define NAND_TOTAL_SIZE       (NAND_BLOCK_SIZE * NAND_BLOCKS_PER_LUN) /**< Total data capacity (128 MB)             */
#define NAND_MIN_GOOD_BLOCKS  1004U                                   /**< Minimum guaranteed good blocks           */
#define NAND_ECC_SEGMENTS     4U                                      /**< ECC segments per page                    */
#define NAND_ECC_SEGMENT_SIZE 512U                                    /**< Bytes per ECC segment                    */

#define NAND_LOG2_PAGE_SIZE       11
#define NAND_LOG2_PAGES_PER_BLOCK 6

#define NAND_MAX_PAGE_ADDRESS  (NAND_PAGES_PER_BLOCK - 1)  // zero-indexed
#define NAND_MAX_BLOCK_ADDRESS (NAND_BLOCKS_PER_LUN - 1)   // zero-indexed

/* SPI COMMAND OPCODES  (datasheet Table 1) */
#define CMD_WRITE_ENABLE   0x06U /**< Set Write Enable Latch (WEL)     */
#define CMD_WRITE_DISABLE  0x04U /**< Reset Write Enable Latch         */
#define CMD_GET_FEATURE    0x0FU /**< Read feature register            */
#define CMD_SET_FEATURE    0x1FU /**< Write feature register           */
#define CMD_PAGE_READ      0x13U /**< Transfer page: array -> cache    */
#define CMD_READ_CACHE_X1  0x03U /**< Read from cache ×1 (slow)        */
#define CMD_READ_CACHE_X1F 0x0BU /**< Read from cache ×1 (fast)        */
#define CMD_READ_CACHE_X2  0x3BU /**< Read from cache ×2 (dual)        */
#define CMD_READ_CACHE_X4  0x6BU /**< Read from cache ×4 (quad)        */
#define CMD_READ_CACHE_SEQ 0x31U /**< Page Read Cache Sequential       */
#define CMD_READ_CACHE_END 0x3FU /**< Page Read Cache End              */
#define CMD_READ_ID        0x9FU /**< Read manufacturer + device ID    */
#define CMD_ECC_STATUS     0x7CU /**< Read internal ECC status         */
#define CMD_PROG_LOAD      0x02U /**< Program Load (cache reset first) */
#define CMD_PROG_LOAD_RND  0x84U /**< Program Load Random Data         */
#define CMD_PROG_LOAD_X4   0x32U /**< Program Load ×4 (quad)           */
#define CMD_PROG_LOAD_RX4  0x34U /**< Program Load Random Data ×4      */
#define CMD_PROG_EXECUTE   0x10U /**< Execute: transfer cache -> array  */
#define CMD_BLOCK_ERASE    0xD8U /**< Erase an entire block            */
#define CMD_RESET          0xFFU /**< Software reset                   */

/* FEATURE REGISTER ADDRESSES */
#define NAND_FEAT_BLOCK_PROT 0xA0U /**< Block Protection register        */
#define NAND_FEAT_SECURE_OTP 0xB0U /**< Secure OTP / Configuration reg.  */
#define NAND_FEAT_STATUS     0xC0U /**< Status register (read-only)      */

/* DEVICE IDENTIFICATION */
#define NAND_MANUFACTURER_ID 0xC2U /**< Macronix manufacturer ID         */
#define NAND_DEVICE_ID       0x12U /**< MX35LF1GE4AB device ID           */

/* WRAP ADDRESS BITS  (datasheet Table 3, 1Gb only) */
// #define NAND_WRAP_2112 0x00U /**< Wrap length = 2112 bytes         */
// #define NAND_WRAP_2048 0x01U /**< Wrap length = 2048 bytes         */
// #define NAND_WRAP_64   0x02U /**< Wrap length = 64 bytes           */
// #define NAND_WRAP_16   0x03U /**< Wrap length = 16 bytes           */

#define NAND_SPI_MAX_TRANSFER (NAND_PAGE_FULL_SIZE + 8U) /* 2120 bytes */

#define NAND_IO_UNUSED (-1)

//// @brief Read-only information about the device, populated by nand_init().
typedef struct
{
    uint8_t manufacturer_id;  /**< 0xC2 (Macronix)                          */
    uint8_t device_id;        /**< 0x12 (MX35LF1GE4AB)                      */
    uint32_t total_blocks;    /**< 1024                                     */
    uint32_t pages_per_block; /**< 64                                       */
    uint32_t page_size;       /**< 2048 bytes                               */
    uint32_t spare_size;      /**< 64 bytes                                 */
    bool ecc_enabled;         /**< Internal ECC currently active            */
} nand_info_t;

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
    int dma_chan;               /**< DMA channel — use SPI_DMA_CH_AUTO        */
    bool disable_ecc;           /**< True to disable the ECC feature, false otherwise */
} nand_config_t;

/// @brief Nand row address
typedef union
{
    uint32_t whole;
    struct
    {
        /// @brief valid range 0-63
        uint32_t page : 6;

        /// @brief valid range 0-1023
        uint32_t block : 26;  // complete the 32 bits (6 + 26 == 32)
    };
} row_address_t;

/// @brief Nand column address (valid range 0-2111)
typedef uint16_t column_address_t;

typedef struct nand_dev_t
{
    spi_device_handle_t spi; /**< ESP-IDF SPI device handle              */
    nand_info_t info;        /**< Device capabilities (read at init)      */

    bool initialized;
} nand_handle_t;

int nand_mx35lf1_init(nand_handle_t *h, const nand_config_t *cfg);
int nand_mx35lf1_deinit(nand_handle_t *h);
int nand_mx35lf1_page_program(nand_handle_t *h, row_address_t row, column_address_t column, const uint8_t *data_in, size_t write_len);
int nand_mx35lf1_page_read(nand_handle_t *h, row_address_t row, column_address_t column, uint8_t *data_out, size_t read_len);
int nand_mx35lf1_page_copy(nand_handle_t *h, row_address_t src, row_address_t dst);
int nand_mx35lf1_block_erase(nand_handle_t *h, row_address_t row);
int nand_mx35lf1_block_is_bad(nand_handle_t *h, row_address_t row, bool *is_bad);
int nand_mx35lf1_block_mark_bad(nand_handle_t *h, row_address_t row);
int nand_mx35lf1_page_is_free(nand_handle_t *h, row_address_t row, bool *is_free);
int nand_mx35lf1_clear(nand_handle_t *h);

bool nand_mx35lf1_mounted(nand_handle_t *h);
int nand_mx35lf1_get_info(nand_handle_t *h, nand_info_t *info_out);

#endif  // MX35LF1_DRIVER_H
