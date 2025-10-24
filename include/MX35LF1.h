#ifndef __MX35LF1_H__
#define __MX35LF1_H__

/*
 * Details about the module 
 *
 * Macronix MX35LF1GE4AcB-Z4I-TR
 * NAND FLASH library
 * Datasheet: https://mouser.com/datasheet/2/819/MX35LF1GE4AB_2c_3V_2c_1Gb_2c_v1_9-3371019.pdf
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
    gpio_num_t mosi_io;
    gpio_num_t miso_io;
    gpio_num_t sclk_io;
    gpio_num_t cs_io;

    gpio_num_t hd_io;
    gpio_num_t wp_io;

} nand_mx35_spi_pins_t;

typedef struct nand_mx35_dev_t
{
    nand_mx35_spi_pins_t spi_pins;

} nand_mx35_config_t;

typedef struct
{
    nand_mx35_config_t cfg;
    spi_device_handle_t spi;
    spi_host_device_t spi_host;

} nand_mx35_context_t;

typedef nand_mx35_context_t *nand_mx35_handle_t;

// Status for function @return
typedef uint8_t mx35_err_t;

#define MX35_OK               0  // Operation Completed/OK
#define MX35_FAIL             1  // Operation Failed/ERROR
#define MX35_READ_FAIL        2  // Error in Read operation
#define MX35_WRITE_FAIL       3  // Error in Write operation
#define MX35_INVALID_ARGUMENT 4
#define MX35_NO_MEM           5

mx35_err_t nand_mx35_init(const nand_mx35_config_t *cfg);

mx35_err_t nand_mx35_deinit(void);

#endif  // __MX35LF1_H__