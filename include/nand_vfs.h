#ifndef NAND_VFS_H
#define NAND_VFS_H

#include "mx35lf1_driver.h"
#include <esp_vfs_fat.h>

typedef struct
{
    const char *base_path; /**< Mounting point. */

    uint8_t format_if_mounted_failed : 1; /**< Format the file system if it fails to mount */

    uint8_t init_spi_bus : 1; /**< If true, calls spi_bus_initialize internally */
    spi_bus_config_t spi_bus; /**< ESP-IDF SPI config */

    nand_config_t nand_conf; /**< SPI Host, CS configuration, frequency, and NAND ECC */
} vfs_nand_conf_t;

esp_err_t vfs_nand_register(const vfs_nand_conf_t *conf);
void vfs_nand_unregister(void);

bool vfs_nand_mounted(void);
esp_err_t vfs_nand_format(void);
esp_err_t vfs_nand_info(size_t *total_bytes, size_t *used_bytes);

#endif