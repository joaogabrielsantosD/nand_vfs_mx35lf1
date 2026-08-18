#ifndef NAND_VFS_H
#define NAND_VFS_H

#include "mx35lf1_driver.h"
#include <esp_vfs_fat.h>

esp_err_t mount_vfs_nand(const char *base_path, nand_config_t *cfg);
void unmount_vfs_nand(void);

#endif