#ifndef NAND_DISKIO_H
#define NAND_DISKIO_H

#include "mx35lf1_driver.h"
#include <diskio_impl.h>
#include <ff.h>

#define PDRV_NAND_FTL      0
#define PDRV_NAND_FTL_PATH "0:"

void diskio_nand_dhara_init(nand_config_t *cfg);
void diskio_nand_dhara_deinit(void);
nand_handle_t *diskio_nand_get_handle(void);

#endif