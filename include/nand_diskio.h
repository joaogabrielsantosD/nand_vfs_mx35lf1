#ifndef NAND_DISKIO_H
#define NAND_DISKIO_H

#include "mx35lf1_driver.h"
#include <diskio_impl.h>
#include <ff.h>

esp_err_t diskio_nand_dhara_init(const nand_config_t *cfg);
esp_err_t diskio_nand_dhara_deinit(void);
nand_handle_t *diskio_nand_get_handle(void);

#endif