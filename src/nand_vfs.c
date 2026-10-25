#include "nand_vfs.h"

#include "sdkconfig.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nand_diskio.h"
#include <stdlib.h>
#include <string.h>

#define VFS_NAND_MAX_OPENED_FILES CONFIG_VFS_NAND_MAX_OPENED_FILES

#define VFS_NAND_BASE_PATH_MAX_LEN 15
#define VFS_NAND_FAT_DRIVE         "0:"

static const char TAG[] = "nand_vfs";

static char vfs_nand_base_path[VFS_NAND_BASE_PATH_MAX_LEN];
static bool s_registered;            /**< vfs_nand_register() succeeded                  */
static bool s_owns_bus;              /**< the SPI bus was initialized by this component   */
static spi_host_device_t s_spi_host; /**< host to free on unregister when s_owns_bus      */

esp_err_t vfs_nand_register(const vfs_nand_conf_t *conf)
{
    if (conf == NULL || conf->base_path == NULL)
    {
        ESP_LOGE(TAG, "Base path or nand config is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_registered)
    {
        ESP_LOGE(TAG, "NAND VFS is already registered at %s", vfs_nand_base_path);
        return ESP_ERR_INVALID_STATE;
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

    /* Only touch global state after every argument has been validated. */
    bool owns_bus = false;
    if (conf->init_spi_bus)
    {
        esp_err_t err = spi_bus_initialize(conf->nand_conf.spi_host, &conf->spi_bus, SPI_DMA_CH_AUTO);
        if (err == ESP_OK)
        {
            owns_bus = true;
        }

        else if (err == ESP_ERR_INVALID_STATE)
        {
            ESP_LOGW(TAG, "SPI bus already initialized by someone else: using it (it will not be freed)");
        }

        else
        {
            ESP_LOGE(TAG, "error in spi_bus_initialize - %s", esp_err_to_name(err));
            return err;
        }
    }

    strncpy(vfs_nand_base_path, conf->base_path, VFS_NAND_BASE_PATH_MAX_LEN - 1);
    vfs_nand_base_path[VFS_NAND_BASE_PATH_MAX_LEN - 1] = '\0';

    esp_err_t ret = diskio_nand_dhara_init(&conf->nand_conf, conf->format_if_mounted_failed);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "NAND/FTL initialization failed: %s", esp_err_to_name(ret));
        goto fail_bus;
    }

    FATFS *fs = NULL;
    esp_vfs_fat_conf_t f_cfg = {
        .base_path = vfs_nand_base_path,
        .fat_drive = VFS_NAND_FAT_DRIVE,
        .max_files = VFS_NAND_MAX_OPENED_FILES,
    };

    ret = esp_vfs_fat_register_cfg(&f_cfg, &fs);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_vfs_fat_register_cfg failed: %s", esp_err_to_name(ret));
        goto fail_diskio;
    }

    /* esp_vfs_fat_register_cfg() does not mount anything: do it here, now, so that a bad
     * volume is detected at registration time. */
    FRESULT fr = f_mount(fs, VFS_NAND_FAT_DRIVE, 1);
    if (fr == FR_NO_FILESYSTEM && conf->format_if_mounted_failed)
    {
        ESP_LOGW(TAG, "No filesystem found. Attempting to format...");
        ret = vfs_nand_format();
        if (ret == ESP_OK)
        {
            fr = f_mount(fs, VFS_NAND_FAT_DRIVE, 1);
        }
    }

    if (fr != FR_OK)
    {
        ESP_LOGE(TAG, "f_mount failed (%d)%s", (int) fr, (fr == FR_NO_FILESYSTEM) ? ": no filesystem (set format_if_mounted_failed to create one)" : "");
        ret = ESP_FAIL;
        goto fail_vfs;
    }

    s_registered = true;
    s_owns_bus = owns_bus;
    s_spi_host = conf->nand_conf.spi_host;
    return ESP_OK;

fail_vfs:
    esp_vfs_fat_unregister_path(vfs_nand_base_path);
fail_diskio:
    diskio_nand_dhara_deinit();
fail_bus:
    if (owns_bus)
    {
        spi_bus_free(conf->nand_conf.spi_host);
    }
    vfs_nand_base_path[0] = '\0';
    return ret;
}

bool vfs_nand_mounted(void)
{
    return s_registered && nand_mx35lf1_mounted(diskio_nand_get_handle());
}

void vfs_nand_unregister(void)
{
    if (!s_registered)
    {
        return;
    }

    esp_err_t ret = esp_vfs_fat_unregister_path(vfs_nand_base_path);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_vfs_fat_unregister_path failed: %s", esp_err_to_name(ret));
    }

    diskio_nand_dhara_deinit();

    if (s_owns_bus)
    {
        ret = spi_bus_free(s_spi_host);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "spi_bus_free failed: %s", esp_err_to_name(ret));
        }
    }

    s_owns_bus = false;
    s_registered = false;
    vfs_nand_base_path[0] = '\0';
}

esp_err_t vfs_nand_format(void)
{
    if (!nand_mx35lf1_mounted(diskio_nand_get_handle()))
    {
        return ESP_ERR_INVALID_STATE;
    }

    const MKFS_PARM opt = {(BYTE) FM_ANY, 0, 0, 0, 0};
    void *workbuf = malloc(FF_MAX_SS);
    if (!workbuf)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Formatting NAND FATFS...");
    FRESULT res = f_mkfs(VFS_NAND_FAT_DRIVE, &opt, workbuf, FF_MAX_SS);
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
    if (!s_registered)
    {
        return ESP_ERR_INVALID_STATE;
    }

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
