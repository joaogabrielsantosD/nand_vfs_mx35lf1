#include "nand_vfs.h"

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nand_diskio.h"
#include <string.h>

#define VFS_NAND_MAX_OPENED_FILES CONFIG_VFS_NAND_MAX_OPENED_FILES

#define VFS_NAND_BASE_PATH_MAX_LEN 15

static const char TAG[] = "nand_vfs";
static char vfs_nand_base_path[VFS_NAND_BASE_PATH_MAX_LEN];

esp_err_t vfs_nand_register(const vfs_nand_conf_t *conf)
{
    if (conf == NULL || conf->base_path == NULL)
    {
        ESP_LOGE(TAG, "Base path or nand config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (conf->nand_conf.spi_host != SPI2_HOST && conf->nand_conf.spi_host != SPI3_HOST)
    {
        ESP_LOGE(TAG, "You must select a valid SPI host device (SPI2_HOST or SPI3_HOST).");
        return ESP_ERR_INVALID_ARG;
    }

    size_t len = strlen(conf->base_path);

    /* Need room for the terminating '\0' as well. */
    if (len == 0 || len >= VFS_NAND_BASE_PATH_MAX_LEN)
    {
        ESP_LOGE(TAG, "Base path length must be between 1 and %d characters", VFS_NAND_BASE_PATH_MAX_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }

    if ((conf->base_path[0] != '/') || (len > 1 && conf->base_path[len - 1] == '/'))
    {
        ESP_LOGE(TAG, "prefix has to start with \"/\" and not end with \"/\"");
        return ESP_ERR_INVALID_ARG;
    }

    strncpy(vfs_nand_base_path, conf->base_path, VFS_NAND_BASE_PATH_MAX_LEN - 1);
    vfs_nand_base_path[VFS_NAND_BASE_PATH_MAX_LEN - 1] = '\0';

    if (conf->init_spi_bus)
    {
        esp_err_t err = spi_bus_initialize(conf->nand_conf.spi_host, &conf->spi_bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "error in spi_bus_initialize - %s", esp_err_to_name(err));
        }
    }

    esp_err_t err = diskio_nand_dhara_init(&conf->nand_conf);
    if (err != ESP_OK)
    {
        return err;
    }

    FATFS *fs_dummy;
    esp_vfs_fat_conf_t f_cfg = {
        .base_path = vfs_nand_base_path,
        .fat_drive = "0:",
        .max_files = VFS_NAND_MAX_OPENED_FILES,
    };

    esp_err_t ret = esp_vfs_fat_register_cfg(&f_cfg, &fs_dummy);
    if (ret != ESP_OK && conf->format_if_mounted_failed)
    {
        ESP_LOGW(TAG, "Mount failed (%s). Attempting to format...", esp_err_to_name(ret));
        if (vfs_nand_format() == ESP_OK)
        {
            ret = esp_vfs_fat_register_cfg(&f_cfg, &fs_dummy);
        }
    }

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_vfs_fat_register_cfg failed: %s", esp_err_to_name(ret));
        diskio_nand_dhara_deinit();
        if (conf->init_spi_bus)
        {
            spi_bus_free(conf->nand_conf.spi_host);
        }
        vfs_nand_base_path[0] = '\0';
        return ret;
    }

    return ESP_OK;
}

bool vfs_nand_mounted(void)
{
    nand_handle_t *h = diskio_nand_get_handle();
    return nand_mx35lf1_mounted(h);
}

void vfs_nand_unregister(void)
{
    esp_err_t ret = esp_vfs_fat_unregister_path(vfs_nand_base_path);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_vfs_fat_unregister_path failed: %s", esp_err_to_name(ret));
    }

    diskio_nand_dhara_deinit();
    vfs_nand_base_path[0] = '\0';
}

esp_err_t vfs_nand_format(void)
{
    const MKFS_PARM opt = {(BYTE) FM_ANY, 0, 0, 0, 0};
    void *workbuf = malloc(4096);
    if (!workbuf)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Formatting NAND FATFS...");
    FRESULT res = f_mkfs("0:", &opt, workbuf, 4096);
    free(workbuf);

    if (res != FR_OK)
    {
        ESP_LOGE(TAG, "f_mkfs failed (%d)", res);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "NAND FATFS formatted successfully");

    return ESP_OK;
}

esp_err_t vfs_nand_info(size_t *total_bytes, size_t *used_bytes)
{
    uint64_t total = 0;
    uint64_t free_bytes = 0;

    esp_err_t err = esp_vfs_fat_info(vfs_nand_base_path, &total, &free_bytes);
    if (err == ESP_OK)
    {
        if (total_bytes)
        {
            *total_bytes = (size_t) total;
        }

        if (used_bytes)
        {
            *used_bytes = (size_t) (total - free_bytes);
        }
    }

    else
    {
        ESP_LOGE(TAG, "esp_vfs_fat_info failed: %s", esp_err_to_name(err));
    }

    return err;
}