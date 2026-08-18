#include "../include/nand_diskio.h"

#include "dhara/dhara/map.h"
#include "driver/spi_master.h"
#include <esp_log.h>

static const char TAG[] = "diskio";
static struct dhara_map map;
static struct dhara_nand nand;
static uint8_t dhara_buffer[NAND_PAGE_SIZE];
static nand_handle_t handle;
static nand_config_t config;

static DSTATUS disk_initialize_dhara(BYTE pdrv)
{
    if (pdrv != PDRV_NAND_FTL)
    {
        return STA_NODISK;
    }

    bool mounted = nand_mx35lf1_mounted(&handle);
    if (mounted)
    {
        return 0;
    }

    nand_mx35lf1_init(&handle, &config);

    nand.log2_page_size = NAND_LOG2_PAGE_SIZE;  // Assume 2048 (2^11)
    nand.log2_ppb = NAND_LOG2_PAGES_PER_BLOCK;  // Assume 64 (2^6)
    nand.num_blocks = NAND_BLOCKS_PER_LUN;

    dhara_map_init(&map, &nand, dhara_buffer, 5);
    dhara_error_t err = DHARA_E_NONE;
    if (dhara_map_resume(&map, &err) != 0)
    {
        ESP_LOGW(TAG, "map resum return dhara error - %s", dhara_strerror(err));
        dhara_map_clear(&map);
    }

    mounted = nand_mx35lf1_mounted(&handle);
    ESP_LOGI(TAG, "%s", mounted ? "nand handle init ok" : "error to init nand handle");
    return mounted ? 0 : STA_NOINIT;
}

static DSTATUS disk_status_dhara(unsigned char pdrv)
{
    if (pdrv != PDRV_NAND_FTL)
    {
        return STA_NODISK;
    }
    return nand_mx35lf1_mounted(&handle) ? 0 : STA_NOINIT;
}

static DRESULT disk_read_dhara(unsigned char pdrv, unsigned char *buff, uint32_t sector, unsigned count)
{
    if (pdrv != PDRV_NAND_FTL)
    {
        return STA_NODISK;
    }

    dhara_error_t err = DHARA_E_NONE;
    for (UINT i = 0; i < count; i++)
    {
        int ret = dhara_map_read(&map, sector, buff, &err);
        if (ret)
        {
            ESP_LOGE(TAG, "dhara read failed: %s", dhara_strerror(err));
            return RES_ERROR;
        }
        buff += NAND_PAGE_SIZE;
        sector++;
    }

    return RES_OK;
}

static DRESULT disk_write_dhara(unsigned char pdrv, const unsigned char *buff, uint32_t sector, unsigned count)
{
    if (pdrv != PDRV_NAND_FTL)
    {
        return STA_NODISK;
    }

    dhara_error_t err = DHARA_E_NONE;
    for (UINT i = 0; i < count; i++)
    {
        int ret = dhara_map_write(&map, sector, buff, &err);
        if (ret)
        {
            ESP_LOGE(TAG, "dhara write failed: %s", dhara_strerror(err));
            return RES_ERROR;
        }
        buff += NAND_PAGE_SIZE;
        sector++;
    }

    return RES_OK;
}

static DRESULT disk_ioctl_dhara(unsigned char pdrv, unsigned char cmd, void *buff)
{
    if (pdrv != PDRV_NAND_FTL)
    {
        return STA_NODISK;
    }

    dhara_error_t err = DHARA_E_NONE;
    switch (cmd)
    {
        case CTRL_SYNC:
        {
            int ret = dhara_map_sync(&map, &err);
            if (ret)
            {
                ESP_LOGE(TAG, "dhara sync failed: %s", dhara_strerror(err));
                return RES_ERROR;
            }

            break;
        }

        case GET_SECTOR_COUNT:
        {
            dhara_sector_t sector_count = dhara_map_capacity(&map);
            ESP_LOGD(TAG, "dhara capacity %" PRId32, sector_count);
            *(LBA_t *) buff = sector_count;
            break;
        }

        case GET_SECTOR_SIZE:
        {
            *(WORD *) buff = NAND_PAGE_SIZE;
            break;
        }

        case GET_BLOCK_SIZE:
        {
            *(DWORD *) buff = NAND_PAGES_PER_BLOCK;
            break;
        }

        case CTRL_TRIM:
        {
            LBA_t *args = (LBA_t *) buff;
            LBA_t start = args[0];
            LBA_t end = args[1];
            while (start <= end)
            {
                int ret = dhara_map_trim(&map, start, &err);
                if (ret)
                {
                    ESP_LOGE(TAG, "dhara trim failed: %s", dhara_strerror(err));
                    return RES_ERROR;
                }
                start++;
            }
            break;
        }

        default:
        {
            ESP_LOGE(TAG, "Unsupported IOCTL command: %d", cmd);
            return RES_PARERR;
        }
    }

    return RES_OK;
}

static const ff_diskio_impl_t dhara_diskio_impl = {
    .init = disk_initialize_dhara,
    .status = disk_status_dhara,
    .read = disk_read_dhara,
    .write = disk_write_dhara,
    .ioctl = disk_ioctl_dhara,
};

static FATFS fs;

void diskio_nand_dhara_init(nand_config_t *cfg)
{
    ESP_LOGI(TAG, "register dhara diskio");
    config = *cfg;
    ff_diskio_register(PDRV_NAND_FTL, &dhara_diskio_impl);
    if (f_mount(&fs, PDRV_NAND_FTL_PATH, 1) != FR_OK)
    {
        BYTE work_buff[FF_MAX_SS];
        f_mkfs(PDRV_NAND_FTL_PATH, 0, work_buff, sizeof(work_buff));
        f_mount(&fs, PDRV_NAND_FTL_PATH, 1);
    }
}

void diskio_nand_dhara_deinit(void)
{
    ESP_LOGI(TAG, "Unregister dhara diskio");
    f_mount(NULL, PDRV_NAND_FTL_PATH, 0);
    ff_diskio_register(PDRV_NAND_FTL, NULL);
    dhara_map_sync(&map, NULL);
}

nand_handle_t *diskio_nand_get_handle(void)
{
    return &handle;
}
