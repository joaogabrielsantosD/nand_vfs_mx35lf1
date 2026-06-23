#include "ecu_fw_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char TAG[] = "ECU_MGR";

struct ecu_mgr_t
{
    nand_handle_t nand;
    ecu_superblock_t sb;
    ecu_bbt_t bbt;
    SemaphoreHandle_t mutex;
    bool initialized;
};

#define MGR_LOCK(m)                                                     \
    do                                                                  \
    {                                                                   \
        if (xSemaphoreTake((m)->mutex, pdMS_TO_TICKS(10000)) != pdTRUE) \
        {                                                               \
            ESP_LOGE(TAG, "Mutex timeout");                             \
            return ESP_ERR_TIMEOUT;                                     \
        }                                                               \
    } while (0)

#define MGR_UNLOCK(m) xSemaphoreGive((m)->mutex)

#define MGR_CHECK(m)                                                 \
    do                                                               \
    {                                                                \
        if (!(m) || !(m)->initialized) return ESP_ERR_INVALID_STATE; \
    } while (0)

#define MGR_CHECK_ARG(p)                    \
    do                                      \
    {                                       \
        if (!(p))                           \
        {                                   \
            ESP_LOGE(TAG, "NULL argument"); \
            return ESP_ERR_INVALID_ARG;     \
        }                                   \
    } while (0)

#define RET_ON_ERR(x)                \
    do                               \
    {                                \
        esp_err_t _r = (x);          \
        if (_r != ESP_OK) return _r; \
    } while (0)

/** Update a running CRC32 with additional data (IEEE 802.3). */
static uint32_t ecu_crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
        {
            crc = (crc & 1U) ? (crc >> 1) ^ 0xEDB88320UL : (crc >> 1);
        }
    }
    return crc;
}

/** Compute the full CRC32 of a buffer (convenience wrapper). */
static uint32_t ecu_crc32(const uint8_t *data, size_t len)
{
    return ~ecu_crc32_update(0xFFFFFFFFUL, data, len);
}

static uint32_t ecu_bytes_to_blocks(uint32_t data_size)
{
    if (data_size == 0U)
    {
        return 0U;
    }
    uint32_t total = data_size + ECU_FILE_HEADER_SIZE;
    return (total + NAND_BLOCK_SIZE - 1U) / NAND_BLOCK_SIZE;
}

static inline bool bbt_is_bad(const ecu_bbt_t *bbt, uint16_t block)
{
    if (block >= NAND_BLOCKS_TOTAL)
    {
        return true;
    }
    return (bbt->bitmap[block / 8U] & (1U << (block % 8U))) != 0U;
}

static inline void bbt_mark_bad(ecu_bbt_t *bbt, uint16_t block)
{
    if (block >= NAND_BLOCKS_TOTAL)
    {
        return;
    }

    uint8_t mask = (uint8_t) (1U << (block % 8U));
    if (!(bbt->bitmap[block / 8U] & mask))
    {
        bbt->bitmap[block / 8U] |= mask;
        bbt->num_bad_blocks++;
    }
}

static esp_err_t nand_write_pg(nand_handle_t nand, uint16_t blk, uint8_t pg, const void *data, size_t sz)
{
    if (sz > NAND_PAGE_SIZE)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    if (!buf)
    {
        return ESP_ERR_NO_MEM;
    }

    memset(buf, 0xFF, NAND_PAGE_SIZE);
    memcpy(buf, data, sz);
    esp_err_t ret = nand_program_page(nand, blk, pg, buf, NULL);
    free(buf);
    return ret;
}

static esp_err_t nand_read_pg(nand_handle_t nand, uint16_t blk, uint8_t pg, void *out, size_t sz)
{
    if (sz > NAND_PAGE_SIZE)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    if (!buf)
    {
        return ESP_ERR_NO_MEM;
    }

    nand_ecc_status_t ecc;
    esp_err_t ret = nand_read_page(nand, blk, pg, buf, NULL, &ecc);
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_CRC)
    {
        memcpy(out, buf, sz);
        ret = (ecc == NAND_ECC_UNCORRECTED) ? ESP_ERR_INVALID_CRC : ESP_OK;
    }
    free(buf);
    return ret;
}

static esp_err_t sb_save(struct ecu_mgr_t *m)
{
    const uint16_t BLK = ECU_RESERVED_BLOCK_START;

    m->sb.header_crc32 = ecu_crc32((const uint8_t *) &m->sb, offsetof(ecu_superblock_t, header_crc32));

    RET_ON_ERR(nand_erase_block(m->nand, BLK));
    RET_ON_ERR(nand_write_pg(m->nand, BLK, ECU_SUPERBLOCK_PAGE_A, &m->sb, NAND_PAGE_SIZE));

    size_t sb_sz = sizeof(ecu_superblock_t);
    if (sb_sz > NAND_PAGE_SIZE)
    {
        const uint8_t *ptr = (const uint8_t *) &m->sb + NAND_PAGE_SIZE;
        size_t rem = sb_sz - NAND_PAGE_SIZE;
        RET_ON_ERR(nand_write_pg(m->nand, BLK, ECU_SUPERBLOCK_PAGE_B, ptr, rem < NAND_PAGE_SIZE ? rem : NAND_PAGE_SIZE));
    }

    m->bbt.crc32 = ecu_crc32(m->bbt.bitmap, sizeof(m->bbt.bitmap));
    size_t bbt_sz = sizeof(ecu_bbt_t);
    RET_ON_ERR(nand_write_pg(m->nand, BLK, ECU_BBT_PAGE, &m->bbt, bbt_sz < NAND_PAGE_SIZE ? bbt_sz : NAND_PAGE_SIZE));

    ESP_LOGD(TAG, "Superblock saved: slots=%" PRId16 " bad=%" PRId32, m->sb.num_slots, m->bbt.num_bad_blocks);
    return ESP_OK;
}

static esp_err_t sb_load(struct ecu_mgr_t *m)
{
    const uint16_t BLK = ECU_RESERVED_BLOCK_START;

    RET_ON_ERR(nand_read_pg(m->nand, BLK, ECU_SUPERBLOCK_PAGE_A, &m->sb, NAND_PAGE_SIZE));

    size_t sb_sz = sizeof(ecu_superblock_t);
    if (sb_sz > NAND_PAGE_SIZE)
    {
        uint8_t *ptr = (uint8_t *) &m->sb + NAND_PAGE_SIZE;
        size_t rem = sb_sz - NAND_PAGE_SIZE;
        uint8_t *tmp = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
        if (!tmp)
        {
            return ESP_ERR_NO_MEM;
        }

        esp_err_t ret = nand_read_pg(m->nand, BLK, ECU_SUPERBLOCK_PAGE_B, tmp, NAND_PAGE_SIZE);
        if (ret == ESP_OK)
        {
            memcpy(ptr, tmp, rem < NAND_PAGE_SIZE ? rem : NAND_PAGE_SIZE);
        }
        free(tmp);

        if (ret != ESP_OK)
        {
            return ret;
        }
    }

    size_t bbt_sz = sizeof(ecu_bbt_t);
    return nand_read_pg(m->nand, BLK, ECU_BBT_PAGE, &m->bbt, bbt_sz < NAND_PAGE_SIZE ? bbt_sz : NAND_PAGE_SIZE);
}

static esp_err_t alloc_blocks(struct ecu_mgr_t *m, uint32_t num_blocks, uint16_t *start)
{
    /* Bitmap of blocks already in use by active slots */
    static uint8_t in_use[NAND_BLOCKS_TOTAL / 8U];
    memset(in_use, 0, sizeof(in_use));

    for (uint8_t s = 0; s < ECU_MAX_SLOTS; s++)
    {
        const ecu_slot_entry_t *sl = &m->sb.slots[s];
        if (sl->status != ECU_SLOT_ACTIVE && sl->status != ECU_SLOT_UPDATING)
        {
            continue;
        }

        for (uint16_t b = sl->bin_first_block; b < sl->bin_first_block + sl->bin_num_blocks && b < NAND_BLOCKS_TOTAL; b++)
        {
            in_use[b / 8] |= (uint8_t) (1U << (b % 8));
        }

        for (uint16_t b = sl->prm_first_block; b < sl->prm_first_block + sl->prm_num_blocks && b < NAND_BLOCKS_TOTAL; b++)
        {
            in_use[b / 8] |= (uint8_t) (1U << (b % 8));
        }

        for (uint16_t b = sl->idx_first_block; b < sl->idx_first_block + sl->idx_num_blocks && b < NAND_BLOCKS_TOTAL; b++)
        {
            in_use[b / 8] |= (uint8_t) (1U << (b % 8));
        }
    }

    uint32_t count = 0;
    uint16_t first = 0;

    for (uint16_t blk = ECU_DATA_BLOCK_START; blk <= ECU_DATA_BLOCK_END; blk++)
    {
        bool bad = bbt_is_bad(&m->bbt, blk);
        bool used = (in_use[blk / 8] & (1U << (blk % 8))) != 0;

        if (!bad && !used)
        {
            if (count == 0)
            {
                first = blk;
            }

            if (++count >= num_blocks)
            {
                *start = first;
                return ESP_OK;
            }
        }

        else
        {
            count = 0;
        }
    }
    ESP_LOGE(TAG, "No contiguous free blocks (%u required)", (unsigned) num_blocks);
    return ESP_ERR_NO_MEM;
}

static int8_t find_slot(struct ecu_mgr_t *m, const char *name)
{
    int8_t free_slot = -1;
    for (uint8_t s = 0; s < ECU_MAX_SLOTS; s++)
    {
        ecu_slot_entry_t *sl = &m->sb.slots[s];
        if ((sl->status == ECU_SLOT_ACTIVE || sl->status == ECU_SLOT_UPDATING) && strncmp(sl->ecu_name, name, ECU_NAME_MAX_LEN) == 0)
        {
            return (int8_t) s;
        }

        if (free_slot < 0 && sl->status == ECU_SLOT_EMPTY)
        {
            free_slot = (int8_t) s;
        }
    }
    return free_slot;
}

static const ecu_slot_entry_t *find_active_slot(const struct ecu_mgr_t *m, const char *name)
{
    for (uint8_t s = 0; s < ECU_MAX_SLOTS; s++)
    {
        const ecu_slot_entry_t *sl = &m->sb.slots[s];
        if (sl->status == ECU_SLOT_ACTIVE && strncmp(sl->ecu_name, name, ECU_NAME_MAX_LEN) == 0)
        {
            return sl;
        }
    }
    return NULL;
}

/**
 * @brief Convert a logical page index to a physical NAND block and page
 *        to a physical NAND block and page number.
 *
 * Logical page 0 -> first_block, page 0 (contains the file header)
 * Logical page 1 -> first_block, page 1
 * ...
 * Logical page 63 -> first_block, page 63
 * Logical page 64 -> first_block+1, page 0
 */
static inline void logical_page_to_physical(uint32_t logical_page, uint16_t first_block, uint16_t *out_block, uint8_t *out_page)
{
    *out_block = first_block + (uint16_t) (logical_page / NAND_PAGES_PER_BLOCK);
    *out_page = (uint8_t) (logical_page % NAND_PAGES_PER_BLOCK);
}

/**
 * @brief Compute the logical page index for a given byte offset
 *        within the file (including the header at offset 0).
 */
static inline uint32_t byte_offset_to_logical_page(uint32_t byte_offset)
{
    return byte_offset / NAND_PAGE_SIZE;
}

esp_err_t ecu_manager_init(ecu_manager_handle_t *out_handle, nand_handle_t nand)
{
    if (!out_handle || !nand)
    {
        return ESP_ERR_INVALID_ARG;
    }

#ifdef CONFIG_EALIVE_ECU_FW_MANAGER_DEBUG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif

    struct ecu_mgr_t *m = calloc(1, sizeof(struct ecu_mgr_t));
    if (!m)
    {
        return ESP_ERR_NO_MEM;
    }

    m->nand = nand;
    m->mutex = xSemaphoreCreateMutex();
    if (!m->mutex)
    {
        free(m);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = sb_load(m);

    bool valid = (ret == ESP_OK && m->sb.magic == ECU_SUPERBLOCK_MAGIC && m->sb.format_version == ECU_FORMAT_VERSION);
    if (valid)
    {
        uint32_t crc = ecu_crc32((const uint8_t *) &m->sb, offsetof(ecu_superblock_t, header_crc32));
        valid = (crc == m->sb.header_crc32);
        if (!valid)
        {
            ESP_LOGW(TAG, "Superblock CRC invalid — reformatting");
        }
    }

    else
    {
        ESP_LOGI(TAG, "Superblock not found — formatting");
    }

    if (!valid)
    {
        ret = ecu_manager_format(m);
        if (ret != ESP_OK)
        {
            goto fail;
        }
    }

    m->initialized = true;
    *out_handle = m;
    ESP_LOGI(TAG, "Initialized: %" PRId16 " ECUs, %" PRId32 " bad blocks", m->sb.num_slots, m->bbt.num_bad_blocks);
    return ESP_OK;

fail:
    vSemaphoreDelete(m->mutex);
    free(m);
    return ret;
}

esp_err_t ecu_manager_deinit(ecu_manager_handle_t mgr)
{
    MGR_CHECK(mgr);
    struct ecu_mgr_t *m = mgr;
    m->initialized = false;
    vSemaphoreDelete(m->mutex);
    free(m);
    return ESP_OK;
}

esp_err_t ecu_manager_format(ecu_manager_handle_t mgr)
{
    MGR_CHECK_ARG(mgr);
    struct ecu_mgr_t *m = mgr;

    ESP_LOGW(TAG, "Formatting management area...");
    memset(&m->sb, 0, sizeof(m->sb));
    memset(&m->bbt, 0, sizeof(m->bbt));

    m->sb.magic = ECU_SUPERBLOCK_MAGIC;
    m->sb.format_version = ECU_FORMAT_VERSION;
    m->sb.data_block_start = ECU_DATA_BLOCK_START;
    m->sb.data_block_end = ECU_DATA_BLOCK_END;
    m->sb.timestamp = (uint32_t) (esp_timer_get_time() / 1000000LL);

    for (uint8_t s = 0; s < ECU_MAX_SLOTS; s++)
    {
        m->sb.slots[s].status = ECU_SLOT_EMPTY;
        m->sb.slots[s].slot_id = s;
        m->sb.slots[s].magic = ECU_SLOT_MAGIC;
    }
    m->bbt.magic = ECU_BBT_MAGIC;

    RET_ON_ERR(ecu_manager_scan_bbt(m));
    return sb_save(m);
}

esp_err_t ecu_manager_scan_bbt(ecu_manager_handle_t mgr)
{
    MGR_CHECK_ARG(mgr);
    struct ecu_mgr_t *m = mgr;

    ESP_LOGI(TAG, "Scanning for bad blocks...");
    memset(m->bbt.bitmap, 0, sizeof(m->bbt.bitmap));
    m->bbt.num_bad_blocks = 0;

    for (uint16_t b = 0; b < NAND_BLOCKS_TOTAL; b++)
    {
        bool is_bad = false;
        if (nand_is_bad_block(m->nand, b, &is_bad) == ESP_OK && is_bad)
        {
            bbt_mark_bad(&m->bbt, b);
        }
    }

    m->bbt.magic = ECU_BBT_MAGIC;
    m->bbt.crc32 = ecu_crc32(m->bbt.bitmap, sizeof(m->bbt.bitmap));
    m->bbt.timestamp = (uint32_t) (esp_timer_get_time() / 1000000LL);

    ESP_LOGI(TAG, "Scan complete: %" PRId32 " bad blocks", m->bbt.num_bad_blocks);
    return ESP_OK;
}

/**
 * @brief Erase all NAND blocks belonging to a slot (internal helper).
 */
static void erase_slot_blocks(nand_handle_t nand, const ecu_slot_entry_t *sl)
{
    for (uint16_t b = sl->bin_first_block; b < sl->bin_first_block + sl->bin_num_blocks; b++)
    {
        nand_erase_block(nand, b);
    }

    if (sl->prm_num_blocks)
    {
        for (uint16_t b = sl->prm_first_block; b < sl->prm_first_block + sl->prm_num_blocks; b++)
        {
            nand_erase_block(nand, b);
        }
    }

    if (sl->idx_num_blocks)
    {
        for (uint16_t b = sl->idx_first_block; b < sl->idx_first_block + sl->idx_num_blocks; b++)
        {
            nand_erase_block(nand, b);
        }
    }
}

esp_err_t ecu_writer_begin(ecu_manager_handle_t mgr, const char *ecu_name, const char *fw_version, uint32_t hw_id, ecu_file_type_t ftype, uint32_t total_size, ecu_writer_t *writer)
{
    MGR_CHECK(mgr);
    MGR_CHECK_ARG(ecu_name);
    MGR_CHECK_ARG(writer);
    if (total_size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct ecu_mgr_t *m = mgr;
    memset(writer, 0, sizeof(ecu_writer_t));

    /* Allocate the working buffer and the first-page hold buffer */
    writer->_page_buf = heap_caps_malloc(NAND_PAGE_SIZE * 2U, MALLOC_CAP_DMA);
    if (!writer->_page_buf)
    {
        return ESP_ERR_NO_MEM;
    }

    /* _page_buf[0..PAGE_SIZE-1]           : working buffer (page in assembly) */
    /* _page_buf[PAGE_SIZE..2*PAGE_SIZE-1] : first-page hold buffer —        */
    /*                                       written last in commit()        */
    memset(writer->_page_buf, 0xFF, NAND_PAGE_SIZE * 2U);

    MGR_LOCK(m);

    int8_t slot_idx = find_slot(m, ecu_name);
    if (slot_idx < 0)
    {
        MGR_UNLOCK(m);
        free(writer->_page_buf);
        writer->_page_buf = NULL;
        return ESP_ERR_NO_MEM;
    }

    ecu_slot_entry_t *slot = &m->sb.slots[(uint8_t) slot_idx];

    /* Erase old data if the slot already existed */
    if (slot->status == ECU_SLOT_ACTIVE)
    {
        ESP_LOGI(TAG, "Replacing '%s' ftype=%u", ecu_name, (unsigned) ftype);
        /* Erase only the blocks belonging to the file type being replaced */
        switch (ftype)
        {
            case ECU_FILE_BIN:
                for (uint16_t b = slot->bin_first_block; b < slot->bin_first_block + slot->bin_num_blocks; b++)
                {
                    nand_erase_block(m->nand, b);
                }
                break;
            case ECU_FILE_PRM:
                for (uint16_t b = slot->prm_first_block; b < slot->prm_first_block + slot->prm_num_blocks; b++)
                {
                    nand_erase_block(m->nand, b);
                }
                break;
            case ECU_FILE_IDX:
                for (uint16_t b = slot->idx_first_block; b < slot->idx_first_block + slot->idx_num_blocks; b++)
                {
                    nand_erase_block(m->nand, b);
                }
                break;
        }
    }

    /* Initialize / re-initialize the slot */
    if (slot->status != ECU_SLOT_ACTIVE && slot->status != ECU_SLOT_UPDATING)
    {
        memset(slot, 0, sizeof(ecu_slot_entry_t));
        slot->magic = ECU_SLOT_MAGIC;
        slot->slot_id = (uint8_t) slot_idx;
    }
    slot->status = ECU_SLOT_UPDATING;
    strlcpy(slot->ecu_name, ecu_name, ECU_NAME_MAX_LEN);
    strlcpy(slot->fw_version, fw_version ? fw_version : "", ECU_VERSION_MAX_LEN);
    slot->hw_id = hw_id;

    /* Allocate NAND blocks for this file */
    uint16_t num_blk = (uint16_t) ecu_bytes_to_blocks(total_size);
    uint16_t first_blk = 0;

    /* Pre-fill slot fields so alloc_blocks skips these blocks during search */
    switch (ftype)
    {
        case ECU_FILE_BIN:
            slot->bin_first_block = 0;
            slot->bin_num_blocks = num_blk;
            slot->bin_size = total_size;
            break;
        case ECU_FILE_PRM:
            slot->prm_first_block = 0;
            slot->prm_num_blocks = num_blk;
            slot->prm_size = total_size;
            break;
        case ECU_FILE_IDX:
            slot->idx_first_block = 0;
            slot->idx_num_blocks = num_blk;
            slot->idx_size = total_size;
            break;
    }

    esp_err_t ret = alloc_blocks(m, num_blk, &first_blk);
    if (ret != ESP_OK)
    {
        MGR_UNLOCK(m);
        free(writer->_page_buf);
        writer->_page_buf = NULL;
        return ret;
    }

    /* Atualiza slot com os blocos reais */
    switch (ftype)
    {
        case ECU_FILE_BIN:
            slot->bin_first_block = first_blk;
            slot->bin_num_blocks = num_blk;
            break;
        case ECU_FILE_PRM:
            slot->prm_first_block = first_blk;
            slot->prm_num_blocks = num_blk;
            break;
        case ECU_FILE_IDX:
            slot->idx_first_block = first_blk;
            slot->idx_num_blocks = num_blk;
            break;
    }

    /* Save superblock as UPDATING (atomic: if reset here the slot is detectable) */
    sb_save(m);

    MGR_UNLOCK(m);

    /* Erase the newly allocated blocks */
    for (uint16_t b = first_blk; b < first_blk + num_blk; b++)
    {
        ret = nand_erase_block(m->nand, b);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to erase block %u", b);
            free(writer->_page_buf);
            writer->_page_buf = NULL;
            return ret;
        }
    }

    /* Populate the writer context */
    writer->_mgr = mgr;
    writer->_ftype = ftype;
    writer->_slot_idx = (uint8_t) slot_idx;
    writer->_first_block = first_blk;
    writer->_num_blocks = num_blk;
    writer->_cur_block = first_blk;
    writer->_cur_page = 0U;
    writer->_crc_accum = 0xFFFFFFFFUL;         /* initial CRC32 state */
    writer->_page_fill = ECU_FILE_HEADER_SIZE; /* reserve space for the file header */
    writer->_first_page = true;
    writer->total_size = total_size;
    writer->bytes_written = 0U;
    strlcpy(writer->_ecu_name, ecu_name, ECU_NAME_MAX_LEN);
    strlcpy(writer->_fw_version, fw_version ? fw_version : "", ECU_VERSION_MAX_LEN);
    writer->_hw_id = hw_id;
    writer->_valid = true;

    ESP_LOGI(TAG, "Writer aberto: '%s' ftype=%u size=%u blocos=%u..%u", ecu_name, (unsigned) ftype, (unsigned) total_size, first_blk, (unsigned) (first_blk + num_blk - 1U));

    return ESP_OK;
}

/**
 * @brief Internal flush: write the current page buffer to NAND.
 *
 * If this is the first page (_first_page == true), the content is copied
 * para o buffer reservado (_page_buf + NAND_PAGE_SIZE) em vez de
 * into the hold buffer instead of being written — it will be written last
 */
static esp_err_t writer_flush_page(ecu_writer_t *wr)
{
    if (wr->_page_fill == 0 || (wr->_first_page && wr->_page_fill == ECU_FILE_HEADER_SIZE))
    {
        /* Nothing to write beyond the empty header placeholder */
        return ESP_OK;
    }

    struct ecu_mgr_t *m = wr->_mgr;

    /* Pad unused bytes with 0xFF (erased state) */
    if (wr->_page_fill < NAND_PAGE_SIZE)
    {
        memset(wr->_page_buf + wr->_page_fill, 0xFF, NAND_PAGE_SIZE - wr->_page_fill);
    }

    if (wr->_first_page)
    {
        /* Copy the first page into the hold buffer; commit() will write it last */
        memcpy(wr->_page_buf + NAND_PAGE_SIZE, wr->_page_buf, NAND_PAGE_SIZE);
        wr->_first_page = false;
        wr->_page_fill = 0;
        /* Advance to the next page */
        wr->_cur_page++;
        if (wr->_cur_page >= NAND_PAGES_PER_BLOCK)
        {
            wr->_cur_page = 0;
            wr->_cur_block++;
        }
        return ESP_OK;
    }

    /* Write a normal page to NAND */
    esp_err_t ret = nand_program_page(m->nand, wr->_cur_block, wr->_cur_page, wr->_page_buf, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Program page falhou blk=%u pg=%u: %s", wr->_cur_block, wr->_cur_page, esp_err_to_name(ret));
        return ret;
    }

    /* Advance write cursor */
    wr->_page_fill = 0;
    wr->_cur_page++;
    if (wr->_cur_page >= NAND_PAGES_PER_BLOCK)
    {
        wr->_cur_page = 0;
        wr->_cur_block++;
    }

    return ESP_OK;
}

esp_err_t ecu_writer_write(ecu_writer_t *writer, const uint8_t *data, uint32_t len)
{
    if (!writer || !writer->_valid)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!data || len == 0)
    {
        return ESP_OK;
    }

    if (writer->bytes_written + len > writer->total_size)
    {
        ESP_LOGE(TAG, "Write excede total_size (%u + %u > %u)", (unsigned) writer->bytes_written, (unsigned) len, (unsigned) writer->total_size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t remaining = len;
    const uint8_t *src = data;

    while (remaining > 0)
    {
        /* Space remaining in the current page buffer */
        uint16_t space = (uint16_t) (NAND_PAGE_SIZE - writer->_page_fill);

        uint32_t chunk = (remaining < (uint32_t) space) ? remaining : (uint32_t) space;

        /* Copy into the page buffer and update the CRC accumulator */
        memcpy(writer->_page_buf + writer->_page_fill, src, chunk);
        writer->_crc_accum = ecu_crc32_update(writer->_crc_accum, src, chunk);

        writer->_page_fill += (uint16_t) chunk;
        writer->bytes_written += chunk;
        src += chunk;
        remaining -= chunk;

        /* Se o buffer ficou cheio, faz flush para a NAND */
        if (writer->_page_fill >= NAND_PAGE_SIZE)
        {
            esp_err_t ret = writer_flush_page(writer);
            if (ret != ESP_OK)
            {
                writer->_valid = false;
                return ret;
            }
        }
    }

    return ESP_OK;
}

esp_err_t ecu_writer_commit(ecu_writer_t *writer)
{
    if (!writer || !writer->_valid)
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* Verify that exactly total_size bytes were written */
    if (writer->bytes_written != writer->total_size)
    {
        ESP_LOGE(TAG, "commit: written=%u != declared total=%u", (unsigned) writer->bytes_written, (unsigned) writer->total_size);
        ecu_writer_abort(writer);
        return ESP_ERR_INVALID_SIZE;
    }

    struct ecu_mgr_t *m = writer->_mgr;
    esp_err_t ret;

    /* Flush the last partial page (if it contains any data) */
    if (writer->_page_fill > 0 && !writer->_first_page)
    {
        ret = writer_flush_page(writer);
        if (ret != ESP_OK)
        {
            writer->_valid = false;
            return ret;
        }
    }

    else if (writer->_page_fill > 0 && writer->_first_page)
    {
        /* Small file: everything fits in a single page */
        ret = writer_flush_page(writer);
        if (ret != ESP_OK)
        {
            writer->_valid = false;
            return ret;
        }
    }

    /* Finalize CRC32 */
    uint32_t final_crc = ~writer->_crc_accum;

    /* Build the file header and place it into the first-page hold buffer */
    uint8_t *first_page_buf = writer->_page_buf + NAND_PAGE_SIZE;
    ecu_file_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = ECU_FILE_MAGIC;
    hdr.file_type = (uint8_t) writer->_ftype;
    hdr.file_size = writer->total_size;
    hdr.crc32 = final_crc;
    hdr.first_block = writer->_first_block;
    hdr.num_blocks = writer->_num_blocks;
    hdr.format_version = ECU_FORMAT_VERSION;
    hdr.timestamp = (uint32_t) (esp_timer_get_time() / 1000000LL);

    char fname[ECU_MAX_FILENAME];
    snprintf(fname, sizeof(fname), "%s.%s", writer->_ecu_name, writer->_ftype == ECU_FILE_BIN ? "bin" : writer->_ftype == ECU_FILE_PRM ? "prm" : "idx");
    strlcpy(hdr.filename, fname, ECU_MAX_FILENAME);

    memcpy(first_page_buf, &hdr, sizeof(hdr));

    /* Write Page 0 (held in RAM since begin()) */
    ret = nand_program_page(m->nand, writer->_first_block, 0U, first_page_buf, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to program first page: %s", esp_err_to_name(ret));
        writer->_valid = false;
        return ret;
    }

    /* Update the slot in the superblock */
    MGR_LOCK(m);
    ecu_slot_entry_t *slot = &m->sb.slots[writer->_slot_idx];

    switch (writer->_ftype)
    {
        case ECU_FILE_BIN:
            slot->bin_crc32 = final_crc;
            slot->bin_size = writer->total_size;
            break;
        case ECU_FILE_PRM:
            slot->prm_crc32 = final_crc;
            slot->prm_size = writer->total_size;
            break;
        case ECU_FILE_IDX:
            slot->idx_crc32 = final_crc;
            slot->idx_size = writer->total_size;
            break;
    }

    slot->status = ECU_SLOT_ACTIVE;
    slot->timestamp_updated = (uint32_t) (esp_timer_get_time() / 1000000LL);

    /* Increment slot count only on first activation */
    bool already_counted = false;
    for (uint8_t s = 0; s < ECU_MAX_SLOTS; s++)
    {
        if (s == writer->_slot_idx)
        {
            continue;
        }

        if (m->sb.slots[s].status == ECU_SLOT_ACTIVE && strncmp(m->sb.slots[s].ecu_name, slot->ecu_name, ECU_NAME_MAX_LEN) == 0)
        {
            already_counted = true;
            break;
        }
    }

    if (!already_counted)
    {
        m->sb.num_slots++;
    }

    ret = sb_save(m);
    MGR_UNLOCK(m);

    /* Release resources */
    free(writer->_page_buf);
    writer->_page_buf = NULL;
    writer->_valid = false;

    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Commit OK: '%s' ftype=%u size=%u crc=0x%08X", writer->_ecu_name, (unsigned) writer->_ftype, (unsigned) writer->total_size, (unsigned) final_crc);
    }

    return ret;
}

esp_err_t ecu_writer_abort(ecu_writer_t *writer)
{
    if (!writer || !writer->_valid)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct ecu_mgr_t *m = writer->_mgr;

    /* Erase the newly allocated blocks */
    for (uint16_t b = writer->_first_block; b < writer->_first_block + writer->_num_blocks; b++)
    {
        nand_erase_block(m->nand, b);
    }

    /* Clear file-specific fields in the superblock slot */
    MGR_LOCK(m);
    ecu_slot_entry_t *slot = &m->sb.slots[writer->_slot_idx];

    /* Clear only the metadata for the file that was being written */
    switch (writer->_ftype)
    {
        case ECU_FILE_BIN:
            slot->bin_first_block = 0;
            slot->bin_num_blocks = 0;
            slot->bin_size = 0;
            slot->bin_crc32 = 0;
            break;
        case ECU_FILE_PRM:
            slot->prm_first_block = 0;
            slot->prm_num_blocks = 0;
            slot->prm_size = 0;
            slot->prm_crc32 = 0;
            break;
        case ECU_FILE_IDX:
            slot->idx_first_block = 0;
            slot->idx_num_blocks = 0;
            slot->idx_size = 0;
            slot->idx_crc32 = 0;
            break;
    }

    /* If no file was written successfully, free the entire slot */
    if (slot->bin_size == 0 && slot->prm_size == 0 && slot->idx_size == 0)
    {
        memset(slot, 0, sizeof(ecu_slot_entry_t));
        slot->status = ECU_SLOT_EMPTY;
        slot->slot_id = writer->_slot_idx;
        slot->magic = ECU_SLOT_MAGIC;
    }

    else
    {
        slot->status = ECU_SLOT_ACTIVE; /* Previously committed files are still valid */
    }

    sb_save(m);
    MGR_UNLOCK(m);

    free(writer->_page_buf);
    writer->_page_buf = NULL;
    writer->_valid = false;

    ESP_LOGW(TAG, "Writer aborted: '%s' ftype=%u", writer->_ecu_name, (unsigned) writer->_ftype);
    return ESP_OK;
}

/**
 * @brief Load a NAND page into the reader cache if not already present.
 *
 * @param logical_page  Logical page index within the file.
 *                      Page 0 = first page (contains the file header).
 */
static esp_err_t reader_load_page(ecu_reader_t *rd, uint32_t logical_page)
{
    uint16_t blk;
    uint8_t pg;
    logical_page_to_physical(logical_page, rd->_first_block, &blk, &pg);

    /* Cache hit: the requested page is already loaded */
    if (rd->_cache_valid && rd->_cache_block == blk && rd->_cache_page == pg)
    {
        return ESP_OK;
    }

    struct ecu_mgr_t *m = rd->_mgr;
    nand_ecc_status_t ecc;

    esp_err_t ret = nand_read_page(m->nand, blk, pg, rd->_page_cache, NULL, &ecc);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_CRC)
    {
        return ret;
    }

    if (ecc == NAND_ECC_UNCORRECTED)
    {
        ESP_LOGE(TAG, "Uncorrectable ECC: block=%u page=%u", blk, pg);
        return ESP_ERR_INVALID_CRC;
    }

    rd->_cache_block = blk;
    rd->_cache_page = pg;
    rd->_cache_valid = true;
    return ESP_OK;
}

esp_err_t ecu_reader_open(ecu_manager_handle_t mgr, const char *ecu_name, ecu_file_type_t ftype, ecu_reader_t *reader)
{
    MGR_CHECK(mgr);
    MGR_CHECK_ARG(ecu_name);
    MGR_CHECK_ARG(reader);

    struct ecu_mgr_t *m = mgr;
    memset(reader, 0, sizeof(ecu_reader_t));

    const ecu_slot_entry_t *sl = find_active_slot(m, ecu_name);
    if (!sl)
    {
        return ESP_ERR_NOT_FOUND;
    }

    /* Resolve the first block and file size for the requested file type */
    uint16_t first_blk = 0;
    uint32_t file_size = 0;
    uint32_t exp_crc = 0;

    switch (ftype)
    {
        case ECU_FILE_BIN:
            if (sl->bin_size == 0)
            {
                return ESP_ERR_NOT_FOUND;
            }
            first_blk = sl->bin_first_block;
            file_size = sl->bin_size;
            exp_crc = sl->bin_crc32;
            break;
        case ECU_FILE_PRM:
            if (sl->prm_size == 0)
            {
                return ESP_ERR_NOT_FOUND;
            }
            first_blk = sl->prm_first_block;
            file_size = sl->prm_size;
            exp_crc = sl->prm_crc32;
            break;
        case ECU_FILE_IDX:
            if (sl->idx_size == 0)
            {
                return ESP_ERR_NOT_FOUND;
            }
            first_blk = sl->idx_first_block;
            file_size = sl->idx_size;
            exp_crc = sl->idx_crc32;
            break;
        default: return ESP_ERR_INVALID_ARG;
    }

    /* Allocate the DMA-capable page cache */
    reader->_page_cache = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    if (!reader->_page_cache)
    {
        return ESP_ERR_NO_MEM;
    }

    /* Load the first page to validate the file magic number */
    reader->_first_block = first_blk;
    reader->_mgr = mgr;

    esp_err_t ret = reader_load_page(reader, 0U);
    if (ret != ESP_OK)
    {
        free(reader->_page_cache);
        reader->_page_cache = NULL;
        return ret;
    }

    const ecu_file_header_t *hdr = (const ecu_file_header_t *) reader->_page_cache;
    if (hdr->magic != ECU_FILE_MAGIC)
    {
        ESP_LOGE(TAG, "Invalid file magic at block %u: 0x%08X", first_blk, (unsigned) hdr->magic);
        free(reader->_page_cache);
        reader->_page_cache = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    reader->file_size = file_size;
    reader->bytes_read = 0U;
    reader->_expected_crc = exp_crc;
    reader->_crc_accum = 0xFFFFFFFFUL;
    reader->_cache_valid = true;
    reader->_valid = true;

    ESP_LOGD(TAG, "Reader opened: '%s' ftype=%u size=%u", ecu_name, (unsigned) ftype, (unsigned) file_size);

    return ESP_OK;
}

esp_err_t ecu_reader_read(ecu_reader_t *reader, void *buf, size_t size, size_t count, size_t *out_count)
{
    if (!reader || !reader->_valid)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!buf || size == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_count)
    {
        *out_count = 0;
    }

    uint32_t want_bytes = (uint32_t) (size * count);
    uint32_t available = reader->file_size - reader->bytes_read;

    /* Limita ao que ainda há para ler */
    if (want_bytes > available)
    {
        want_bytes = available;
    }

    if (want_bytes == 0)
    {
        return ESP_OK; /* EOF */
    }

    uint8_t *dst = (uint8_t *) buf;
    uint32_t done = 0U;

    while (done < want_bytes)
    {
        /*
         * Absolute byte offset within the stored layout (header + data):
         *   abs_offset = ECU_FILE_HEADER_SIZE + bytes_read + done
         * This maps directly to a logical page index and an intra-page offset.
         */
        uint32_t abs_offset = ECU_FILE_HEADER_SIZE + reader->bytes_read + done;
        uint32_t logical_pg = abs_offset / NAND_PAGE_SIZE;
        uint16_t pg_offset = (uint16_t) (abs_offset % NAND_PAGE_SIZE);

        /* Load the required page into cache (no-op on cache hit) */
        esp_err_t ret = reader_load_page(reader, logical_pg);
        if (ret != ESP_OK)
        {
            return ret;
        }

        /* Copy available bytes from the cached page */
        uint16_t avail_in_page = (uint16_t) (NAND_PAGE_SIZE - pg_offset);
        uint32_t to_copy = want_bytes - done;
        if (to_copy > (uint32_t) avail_in_page)
        {
            to_copy = (uint32_t) avail_in_page;
        }

        memcpy(dst + done, reader->_page_cache + pg_offset, to_copy);

        /* Acumula CRC sobre os bytes de dados retornados */
        reader->_crc_accum = ecu_crc32_update(reader->_crc_accum, dst + done, to_copy);
        done += to_copy;
    }

    reader->bytes_read += done;

    /* Return the number of complete elements to the caller */
    if (out_count)
    {
        *out_count = done / size;
    }

    return ESP_OK;
}

esp_err_t ecu_reader_seek(ecu_reader_t *reader, uint32_t offset)
{
    if (!reader || !reader->_valid)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (offset > reader->file_size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Reposiciona o cursor. O cache pode continuar válido se o seek
     * Stay within the same page (cache-optimization — no NAND re-read needed).
     * The CRC accumulator is reset because data is no longer read sequentially;
     * ecu_reader_close() will skip CRC validation after any seek.
     */
    reader->bytes_read = offset;
    reader->_crc_accum = 0xFFFFFFFFUL; /* CRC inválido após seek */

    /* Invalidate cache only if the seek lands on a different page */
    uint32_t new_abs = ECU_FILE_HEADER_SIZE + offset;
    if (reader->_cache_valid)
    {
        uint32_t logical_pg = new_abs / NAND_PAGE_SIZE;
        uint16_t blk;
        uint8_t pg;
        logical_page_to_physical(logical_pg, reader->_first_block, &blk, &pg);
        if (blk != reader->_cache_block || pg != reader->_cache_page)
        {
            reader->_cache_valid = false;
        }
    }

    return ESP_OK;
}

esp_err_t ecu_reader_close(ecu_reader_t *reader)
{
    if (!reader || !reader->_valid)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ESP_OK;

    /* Verify CRC only when the file was read completely */
    if (reader->bytes_read == reader->file_size)
    {
        uint32_t final_crc = ~reader->_crc_accum;
        if (final_crc != reader->_expected_crc)
        {
            ESP_LOGE(TAG, "CRC mismatch: computed=0x%08X expected=0x%08X", (unsigned) final_crc, (unsigned) reader->_expected_crc);
            ret = ESP_ERR_INVALID_CRC;
        }

        else
        {
            ESP_LOGD(TAG, "CRC verificado OK (0x%08X)", (unsigned) final_crc);
        }
    }

    else
    {
        ESP_LOGD(TAG, "Reader closed (partial read: %u/%u bytes)", (unsigned) reader->bytes_read, (unsigned) reader->file_size);
    }

    free(reader->_page_cache);
    reader->_page_cache = NULL;
    reader->_valid = false;

    return ret;
}

esp_err_t ecu_delete_firmware(ecu_manager_handle_t mgr, const char *ecu_name)
{
    MGR_CHECK(mgr);
    MGR_CHECK_ARG(ecu_name);
    struct ecu_mgr_t *m = mgr;

    MGR_LOCK(m);
    for (uint8_t s = 0; s < ECU_MAX_SLOTS; s++)
    {
        ecu_slot_entry_t *sl = &m->sb.slots[s];
        if (sl->status != ECU_SLOT_ACTIVE)
        {
            continue;
        }

        if (strncmp(sl->ecu_name, ecu_name, ECU_NAME_MAX_LEN) != 0)
        {
            continue;
        }

        MGR_UNLOCK(m);
        erase_slot_blocks(m->nand, sl);
        MGR_LOCK(m);

        memset(sl, 0, sizeof(ecu_slot_entry_t));
        sl->status = ECU_SLOT_EMPTY;
        sl->slot_id = s;
        sl->magic = ECU_SLOT_MAGIC;
        if (m->sb.num_slots > 0)
        {
            m->sb.num_slots--;
        }

        esp_err_t ret = sb_save(m);
        MGR_UNLOCK(m);
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "ECU '%s' deleted", ecu_name);
        }
        return ret;
    }
    MGR_UNLOCK(m);
    return ESP_ERR_NOT_FOUND;
}

static bool verify_file_crc(ecu_manager_handle_t mgr, const char *ecu_name, ecu_file_type_t ftype)
{
    ecu_reader_t rd;
    if (ecu_reader_open(mgr, ecu_name, ftype, &rd) != ESP_OK)
    {
        return false;
    }

    /* Read the file completely in 2 KB chunks */
    uint8_t *chunk = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    if (!chunk)
    {
        ecu_reader_close(&rd);
        return false;
    }

    while (rd.bytes_read < rd.file_size)
    {
        size_t got = 0;
        uint32_t want = rd.file_size - rd.bytes_read;
        if (want > NAND_PAGE_SIZE)
        {
            want = NAND_PAGE_SIZE;
        }

        if (ecu_reader_read(&rd, chunk, 1, want, &got) != ESP_OK || got == 0)
        {
            break;
        }
    }
    free(chunk);

    /* ecu_reader_close verifica o CRC */
    return (ecu_reader_close(&rd) == ESP_OK);
}

esp_err_t ecu_verify_firmware(ecu_manager_handle_t mgr, const char *ecu_name, bool *bin_ok, bool *prm_ok, bool *idx_ok)
{
    MGR_CHECK(mgr);
    MGR_CHECK_ARG(ecu_name);

    struct ecu_mgr_t *m = mgr;
    const ecu_slot_entry_t *sl = find_active_slot(m, ecu_name);
    if (!sl)
    {
        return ESP_ERR_NOT_FOUND;
    }

    if (bin_ok)
    {
        *bin_ok = (sl->bin_size > 0) ? verify_file_crc(mgr, ecu_name, ECU_FILE_BIN) : true;
    }

    if (prm_ok)
    {
        *prm_ok = (sl->prm_size > 0) ? verify_file_crc(mgr, ecu_name, ECU_FILE_PRM) : true;
    }

    if (idx_ok)
    {
        *idx_ok = (sl->idx_size > 0) ? verify_file_crc(mgr, ecu_name, ECU_FILE_IDX) : true;
    }

    return ESP_OK;
}

static void slot_to_info(const ecu_slot_entry_t *sl, ecu_info_t *info)
{
    info->slot_id = sl->slot_id;
    info->status = (ecu_slot_status_t) sl->status;
    info->hw_id = sl->hw_id;
    info->bin_size = sl->bin_size;
    info->prm_size = sl->prm_size;
    info->idx_size = sl->idx_size;
    info->timestamp_updated = sl->timestamp_updated;
    info->has_bin = (sl->bin_num_blocks > 0 && sl->bin_size > 0);
    info->has_prm = (sl->prm_num_blocks > 0 && sl->prm_size > 0);
    info->has_idx = (sl->idx_num_blocks > 0 && sl->idx_size > 0);
    strlcpy(info->ecu_name, sl->ecu_name, ECU_NAME_MAX_LEN);
    strlcpy(info->fw_version, sl->fw_version, ECU_VERSION_MAX_LEN);
}

esp_err_t ecu_get_info(ecu_manager_handle_t mgr, const char *ecu_name, ecu_info_t *info)
{
    MGR_CHECK(mgr);
    MGR_CHECK_ARG(ecu_name);
    MGR_CHECK_ARG(info);

    const ecu_slot_entry_t *sl = find_active_slot(mgr, ecu_name);
    if (!sl)
    {
        return ESP_ERR_NOT_FOUND;
    }
    slot_to_info(sl, info);
    return ESP_OK;
}

esp_err_t ecu_get_info_by_slot(ecu_manager_handle_t mgr, uint8_t slot_id, ecu_info_t *info)
{
    MGR_CHECK(mgr);
    MGR_CHECK_ARG(info);
    if (slot_id >= ECU_MAX_SLOTS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct ecu_mgr_t *m = mgr;
    const ecu_slot_entry_t *sl = &m->sb.slots[slot_id];
    if (sl->status != ECU_SLOT_ACTIVE)
    {
        return ESP_ERR_NOT_FOUND;
    }
    slot_to_info(sl, info);
    return ESP_OK;
}

esp_err_t ecu_list(ecu_manager_handle_t mgr, ecu_info_t *list, uint8_t list_size, uint8_t *count)
{
    MGR_CHECK(mgr);
    MGR_CHECK_ARG(list);
    MGR_CHECK_ARG(count);

    struct ecu_mgr_t *m = mgr;
    uint8_t n = 0;
    for (uint8_t s = 0; s < ECU_MAX_SLOTS && n < list_size; s++)
    {
        if (m->sb.slots[s].status == ECU_SLOT_ACTIVE)
        {
            slot_to_info(&m->sb.slots[s], &list[n++]);
        }
    }
    *count = n;
    return ESP_OK;
}

esp_err_t ecu_exists(ecu_manager_handle_t mgr, const char *ecu_name, bool *exists)
{
    MGR_CHECK(mgr);
    MGR_CHECK_ARG(ecu_name);
    MGR_CHECK_ARG(exists);
    *exists = (find_active_slot(mgr, ecu_name) != NULL);
    return ESP_OK;
}

esp_err_t ecu_get_stats(ecu_manager_handle_t mgr, uint32_t *total_blocks, uint32_t *used_blocks, uint32_t *free_blocks, uint32_t *bad_blocks)
{
    MGR_CHECK(mgr);
    struct ecu_mgr_t *m = mgr;

    uint32_t total = ECU_DATA_BLOCK_END - ECU_DATA_BLOCK_START + 1U;
    uint32_t bad = m->bbt.num_bad_blocks;
    uint32_t used = 0;

    for (uint8_t s = 0; s < ECU_MAX_SLOTS; s++)
    {
        const ecu_slot_entry_t *sl = &m->sb.slots[s];
        if (sl->status == ECU_SLOT_ACTIVE)
        {
            used += sl->bin_num_blocks + sl->prm_num_blocks + sl->idx_num_blocks;
        }
    }

    if (total_blocks)
    {
        *total_blocks = total;
    }

    if (used_blocks)
    {
        *used_blocks = used;
    }

    if (free_blocks)
    {
        *free_blocks = (total > bad + used) ? total - bad - used : 0;
    }

    if (bad_blocks)
    {
        *bad_blocks = bad;
    }
    return ESP_OK;
}
