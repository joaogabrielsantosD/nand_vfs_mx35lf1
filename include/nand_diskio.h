#ifndef NAND_DISKIO_H
#define NAND_DISKIO_H

#include "mx35lf1_driver.h"
#include <diskio_impl.h>
#include <ff.h>
#include <stdbool.h>

/**
 * @brief Initializes the NAND + Dhara FTL and registers the FatFs diskio driver (drive 0).
 *
 * This function does NOT mount or format any filesystem; that is the job of the VFS layer.
 *
 * @param cfg                NAND hardware configuration.
 * @param reset_on_corrupt   If true and Dhara cannot resume its journal for a reason other than
 *                           "blank media", start from an empty map (existing data is lost).
 *                           If false the initialization fails instead.
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already initialized, ESP_FAIL if the
 *         NAND/FTL could not be brought up.
 */
esp_err_t diskio_nand_dhara_init(const nand_config_t *cfg, bool reset_on_corrupt);
esp_err_t diskio_nand_dhara_deinit(void);
nand_handle_t *diskio_nand_get_handle(void);

#endif
