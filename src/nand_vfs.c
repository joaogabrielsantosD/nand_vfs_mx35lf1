#include "nand_vfs.h"

#include "esp_err.h"
#include "nand_diskio.h"
#include <sdkconfig.h>
#include <string.h>

#define VFS_NAND_BASE_PATH_MAX_LEN 15

static const char TAG[] = "nand_vfs";
static char vfs_nand_base_path[VFS_NAND_BASE_PATH_MAX_LEN];

esp_err_t mount_vfs_nand(const char *base_path, nand_config_t *cfg)
{
    if (base_path == NULL || cfg == NULL)
    {
        ESP_LOGE(TAG, "Base path or nand config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    int len = strlen(base_path);
    if (len > VFS_NAND_BASE_PATH_MAX_LEN)
    {
        ESP_LOGE(TAG, "Base path lenghth is bigger than the maximum value - %d", VFS_NAND_BASE_PATH_MAX_LEN);
        return ESP_ERR_INVALID_ARG;
    }

    if (len >= 2 && ((base_path[0] != '/') || (base_path[len - 1] == '/')))
    {
        ESP_LOGE(TAG, "prefix has to start with \"/\" and not end with \"/\"");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(vfs_nand_base_path, base_path, VFS_NAND_BASE_PATH_MAX_LEN);

    diskio_nand_dhara_init(cfg);
    FATFS *fs_dummy;
    esp_vfs_fat_conf_t f_confg = {
        .base_path = vfs_nand_base_path,
        .fat_drive = PDRV_NAND_FTL_PATH,
        .max_files = CONFIG_VFS_NAND_MAX_OPENED_FILES,
    };
    return esp_vfs_fat_register_cfg(&f_confg, &fs_dummy);
}

void unmount_vfs_nand(void)
{
    esp_vfs_fat_unregister_path(vfs_nand_base_path);
    diskio_nand_dhara_deinit();
}
