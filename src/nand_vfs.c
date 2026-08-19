#include "nand_vfs.h"

#include "esp_err.h"
#include "esp_log.h"
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

    size_t len = strlen(base_path);

    /* Need room for the terminating '\0' as well. */
    if (len == 0 || len >= VFS_NAND_BASE_PATH_MAX_LEN)
    {
        ESP_LOGE(TAG, "Base path length must be between 1 and %d characters", VFS_NAND_BASE_PATH_MAX_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    if ((base_path[0] != '/') || (len > 1 && base_path[len - 1] == '/'))
    {
        ESP_LOGE(TAG, "prefix has to start with \"/\" and not end with \"/\"");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(vfs_nand_base_path, base_path, VFS_NAND_BASE_PATH_MAX_LEN - 1);
    vfs_nand_base_path[VFS_NAND_BASE_PATH_MAX_LEN - 1] = '\0';

    diskio_nand_dhara_init(cfg);

    FATFS *fs_dummy;
    esp_vfs_fat_conf_t f_cfg = {
        .base_path = vfs_nand_base_path,
        .fat_drive = PDRV_NAND_FTL_PATH,
        .max_files = CONFIG_VFS_NAND_MAX_OPENED_FILES,
    };

    esp_err_t ret = esp_vfs_fat_register_cfg(&f_cfg, &fs_dummy);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_vfs_fat_register_cfg failed: %s", esp_err_to_name(ret));
        diskio_nand_dhara_deinit();
        vfs_nand_base_path[0] = '\0';
        return ret;
    }

    return ESP_OK;
}

void unmount_vfs_nand(void)
{
    esp_err_t ret = esp_vfs_fat_unregister_path(vfs_nand_base_path);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_vfs_fat_unregister_path failed: %s", esp_err_to_name(ret));
    }

    diskio_nand_dhara_deinit();
    vfs_nand_base_path[0] = '\0';
}