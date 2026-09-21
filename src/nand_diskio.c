#include "nand_diskio.h"

#include "sdkconfig.h"

#include "dhara/dhara/map.h"
#include "driver/spi_master.h"
#include <esp_log.h>
#include <stdlib.h>

#define PDRV_NAND_FTL      0
#define PDRV_NAND_FTL_PATH "0:"

#define DHARA_GC_RATIO CONFIG_DHARA_GARBAGE_COLLECTOR_RATIO

static const char TAG[] = "diskio";

typedef struct
{
    struct dhara_map map;
    struct dhara_nand nand;
    uint8_t dhara_buffer[NAND_PAGE_SIZE];
    nand_handle_t handle;
    nand_config_t config;
    bool in_use;
    FATFS fs;
} diskio_nand_ctx_t;

static diskio_nand_ctx_t s_ctxs = {0};

static diskio_nand_ctx_t *get_ctx(BYTE pdrv)
{
    if (pdrv != PDRV_NAND_FTL)
    {
        return NULL;
    }

    return &s_ctxs;
}

static DSTATUS disk_initialize_dhara(BYTE pdrv)
{
    diskio_nand_ctx_t *ctx = get_ctx(pdrv);
    if (!ctx || !ctx->in_use)
    {
        return STA_NODISK;
    }

    bool mounted = nand_mx35lf1_mounted(&ctx->handle);
    if (mounted)
    {
        return 0;
    }

    if (nand_mx35lf1_init(&ctx->handle, &ctx->config) != NAND_RET_OK)
    {
        ESP_LOGE(TAG, "Failed to init the nand driver");
        return STA_NOINIT;
    }

    ctx->nand.log2_page_size = NAND_LOG2_PAGE_SIZE;  // Assume 2048 (2^11)
    ctx->nand.log2_ppb = NAND_LOG2_PAGES_PER_BLOCK;  // Assume 64 (2^6)
    ctx->nand.num_blocks = NAND_BLOCKS_PER_LUN;

    dhara_map_init(&ctx->map, &ctx->nand, ctx->dhara_buffer, DHARA_GC_RATIO);
    dhara_error_t err = DHARA_E_NONE;
    if (dhara_map_resume(&ctx->map, &err) != 0)
    {
        ESP_LOGW(TAG, "map resume return dhara error - %s", dhara_strerror(err));
        dhara_map_clear(&ctx->map);
    }

    mounted = nand_mx35lf1_mounted(&ctx->handle);
    ESP_LOGI(TAG, "%s", mounted ? "nand handle init ok" : "error to init nand handle");
    return mounted ? 0 : STA_NOINIT;
}

static DSTATUS disk_status_dhara(unsigned char pdrv)
{
    diskio_nand_ctx_t *ctx = get_ctx(pdrv);
    if (!ctx || !ctx->in_use)
    {
        return STA_NODISK;
    }
    return nand_mx35lf1_mounted(&ctx->handle) ? 0 : STA_NOINIT;
}

static DRESULT disk_read_dhara(unsigned char pdrv, unsigned char *buff, uint32_t sector, unsigned count)
{
    diskio_nand_ctx_t *ctx = get_ctx(pdrv);
    if (!ctx || !ctx->in_use)
    {
        return RES_PARERR;
    }

    dhara_error_t err = DHARA_E_NONE;
    for (UINT i = 0; i < count; i++)
    {
        int ret = dhara_map_read(&ctx->map, sector, buff, &err);
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
    diskio_nand_ctx_t *ctx = get_ctx(pdrv);
    if (!ctx || !ctx->in_use)
    {
        return RES_PARERR;
    }

    dhara_error_t err = DHARA_E_NONE;
    for (UINT i = 0; i < count; i++)
    {
        int ret = dhara_map_write(&ctx->map, sector, buff, &err);
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
    diskio_nand_ctx_t *ctx = get_ctx(pdrv);
    if (!ctx || !ctx->in_use)
    {
        return RES_PARERR;
    }

    dhara_error_t err = DHARA_E_NONE;
    switch (cmd)
    {
        case CTRL_SYNC:
        {
            int ret = dhara_map_sync(&ctx->map, &err);
            if (ret)
            {
                ESP_LOGE(TAG, "dhara sync failed: %s", dhara_strerror(err));
                return RES_ERROR;
            }
            break;
        }

        case GET_SECTOR_COUNT:
        {
            dhara_sector_t sector_count = dhara_map_capacity(&ctx->map);
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
                int ret = dhara_map_trim(&ctx->map, start, &err);
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

esp_err_t diskio_nand_dhara_init(const nand_config_t *cfg)
{
    uint8_t pdrv = PDRV_NAND_FTL;

    diskio_nand_ctx_t *ctx = get_ctx(pdrv);
    if (ctx->in_use)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "register dhara diskio %d", pdrv);
    ctx->config = *cfg;
    ctx->in_use = true;

    ff_diskio_register(pdrv, &dhara_diskio_impl);

    if (f_mount(&ctx->fs, PDRV_NAND_FTL_PATH, 1) != FR_OK)
    {
        BYTE work_buff[FF_MAX_SS];
        f_mkfs(PDRV_NAND_FTL_PATH, 0, work_buff, sizeof(work_buff));
        f_mount(&ctx->fs, PDRV_NAND_FTL_PATH, 1);
    }
    return ESP_OK;
}

esp_err_t diskio_nand_dhara_deinit(void)
{
    uint8_t pdrv = PDRV_NAND_FTL;

    diskio_nand_ctx_t *ctx = get_ctx(pdrv);
    if (!ctx->in_use)
    {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Unregister dhara diskio %d", pdrv);

    f_mount(NULL, PDRV_NAND_FTL_PATH, 0);
    ff_diskio_register(pdrv, NULL);
    dhara_map_sync(&ctx->map, NULL);
    nand_mx35lf1_deinit(&ctx->handle);

    ctx->in_use = false;
    return ESP_OK;
}

nand_handle_t *diskio_nand_get_handle(void)
{
    return &s_ctxs.handle;
}
