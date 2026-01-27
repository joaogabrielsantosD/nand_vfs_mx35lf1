#ifndef __MX35LF1_H__
#define __MX35LF1_H__

/*
 * Details about the module 
 *
 * Manufacturer: Macronix International Co., Ltd.
 * Part Number: MX35LF1GE4AcB-Z4I-TR
 * Datasheet: https://mouser.com/datasheet/2/819/MX35LF1GE4AB_2c_3V_2c_1Gb_2c_v1_9-3371019.pdf
 * 
 * NAND FLASH library
*/

#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_err.h"
#include "sdkconfig.h"
#include "MX35LF1_Registers.h"

#define Block_To_Page(a) ((int) (a * NUM_PAGES_PER_BLOCK))  // Convert the Block address to Page address
#define Page_To_Block(b) ((int) (b / NUM_PAGES_PER_BLOCK))  // Convert the Page address to Block address

typedef struct
{
    gpio_num_t mosi_io; /**< SPI MOSI pin */
    gpio_num_t miso_io; /**< SPI MISO pin */
    gpio_num_t sclk_io; /**< SPI clock pin */
    gpio_num_t cs_io;   /**< SPI chip-select pin */
    gpio_num_t hd_io;   /**< Hold pin */
    gpio_num_t wp_io;   /**< Write-protect pin */

} nand_mx35_spi_pins_t;

typedef struct nand_mx35_dev_t
{
    nand_mx35_spi_pins_t spi_pins; /**< SPI pin mapping */

} nand_mx35_config_t;

typedef struct
{
    nand_mx35_config_t cfg;     /**< Device configuration */
    spi_device_handle_t spi;    /**< SPI device handle */
    spi_host_device_t spi_host; /**< SPI host */

} nand_mx35_context_t;


// Status for function @return
typedef uint8_t mx35_err_t;

#define MX35_OK               0  // Operation Completed/OK
#define MX35_FAIL             1  // Operation Failed/ERROR
#define MX35_READ_FAIL        2  // Error in Read operation
#define MX35_WRITE_FAIL       3  // Error in Write operation
#define MX35_INVALID_ARGUMENT 4  // Invalid argument passed to function
#define MX35_NO_MEM           5  // Memory allocation error
#define MX35_INVALID_BLOCK    6  // Invalid block (bad block)


/**
 * @brief Initialize the MX35 NAND device.
 *
 * @param[in] cfg Pointer to device configuration.
 *
 * @return MX35 operation status.
 */
mx35_err_t nand_mx35_init(const nand_mx35_config_t *cfg);

/**
 * @brief Deinitialize the MX35 NAND device.
 *
 * @return MX35 operation status.
 */
mx35_err_t nand_mx35_deinit(void);

/**
 * @brief Erase a specific NAND block.
 *
 * @param[in] block Block index to erase.
 *
 * @return MX35 operation status.
 */
mx35_err_t nand_mx35_erase_block(uint16_t block);

/**
 * @brief Erase the entire NAND memory.
 *
 * @return MX35 operation status.
 */
mx35_err_t nand_mx35_bulk_erase(void);

/**
 * @brief Write data to a NAND page.
 *
 * @param[in]  block Block index.
 * @param[in]  page Page index within the block.
 * @param[in]  buffer Data buffer to write.
 * @param[in]  len Number of bytes to write.
 * @param[out] page_address Absolute page address used.
 *
 * @return MX35 operation status.
 */
mx35_err_t nand_mx35_write_page(uint16_t block, uint8_t page, uint8_t *buffer, size_t len, uint16_t *page_address);

/**
 * @brief Read data from a NAND page.
 *
 * @param[in]  block Block index.
 * @param[in]  page Page index within the block.
 * @param[out] buffer Data buffer to store read data.
 * @param[in]  len Number of bytes to read.
 * @param[out] page_address Absolute page address used.
 *
 * @return MX35 operation status.
 */
mx35_err_t nand_mx35_read_page(uint16_t block, uint8_t page, uint8_t *buffer, size_t len, uint16_t *page_address);

#endif  // __MX35LF1_H__