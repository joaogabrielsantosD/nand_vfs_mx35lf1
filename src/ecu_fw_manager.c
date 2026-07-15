#include "ecu_fw_manager.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <inttypes.h>
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

static inline ecu_file_slot_t *file_slot(ecu_slot_entry_t *slot, ecu_file_type_t ftype)
{
    return &slot->files[(uint8_t) ftype - 1U];
}

static inline const ecu_file_slot_t *file_slot_c(const ecu_slot_entry_t *slot, ecu_file_type_t ftype)
{
    return &slot->files[(uint8_t) ftype - 1U];
}

static inline const char *file_ext(ecu_file_type_t ftype)
{
    switch (ftype)
    {
        case ECU_FILE_BIN: return "bin";
        case ECU_FILE_PRM: return "prm";
        case ECU_FILE_IDX: return "idx";
        default: return "bin";
    }
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

    uint8_t spare_buf[NAND_SPARE_SIZE];
    memset(spare_buf, 0xAA, NAND_SPARE_SIZE);
    spare_buf[0] = 0xFF;

    memset(buf, 0xFF, NAND_PAGE_SIZE);
    memcpy(buf, data, sz);
    esp_err_t ret = nand_program_page(nand, blk, pg, buf, spare_buf);
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

    uint8_t spare_buf[NAND_SPARE_SIZE];
    nand_ecc_status_t ecc;

    esp_err_t ret = nand_read_page(nand, blk, pg, buf, spare_buf, &ecc);
    if (ret == ESP_OK || ecc != NAND_ECC_UNCORRECTED)
    {
        memcpy(out, buf, sz);
    }
    free(buf);
    return ret;
}

static esp_err_t sb_save(struct ecu_mgr_t *m)
{
    m->sb.header_crc32 = ecu_crc32((const uint8_t *) &m->sb, offsetof(ecu_superblock_t, header_crc32));
    m->bbt.crc32 = ecu_crc32(m->bbt.bitmap, sizeof(m->bbt.bitmap));

    RET_ON_ERR(nand_erase_block(m->nand, ECU_RESERVED_BLOCK_START));

    // ESP_LOG_BUFFER_HEX_LEVEL(TAG, &m->sb, sizeof(m->sb), ESP_LOG_WARN);

    RET_ON_ERR(nand_write_pg(m->nand, ECU_RESERVED_BLOCK_START, ECU_SUPERBLOCK_PAGE, &m->sb, sizeof(m->sb)));
    RET_ON_ERR(nand_write_pg(m->nand, ECU_RESERVED_BLOCK_START, ECU_BBT_PAGE, &m->bbt, sizeof(m->bbt)));
    RET_ON_ERR(nand_write_pg(m->nand, ECU_RESERVED_BLOCK_START, ECU_BBT_BAK_PAGE, &m->bbt, sizeof(m->bbt)));

    ESP_LOGD(TAG, "Superblock saved: slots=%" PRIu16 " bad=%" PRIu32, m->sb.num_slots, m->bbt.num_bad_blocks);
    return ESP_OK;
}

static esp_err_t bbt_read_and_check(struct ecu_mgr_t *m, uint8_t page, ecu_bbt_t *out)
{
    RET_ON_ERR(nand_read_pg(m->nand, ECU_RESERVED_BLOCK_START, page, out, sizeof(*out)));

    if (out->magic != ECU_BBT_MAGIC)
    {
        return ESP_ERR_INVALID_CRC;
    }

    uint32_t crc = ecu_crc32(out->bitmap, sizeof(out->bitmap));
    if (crc != out->crc32)
    {
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static esp_err_t sb_load(struct ecu_mgr_t *m)
{
    RET_ON_ERR(nand_read_pg(m->nand, ECU_RESERVED_BLOCK_START, ECU_SUPERBLOCK_PAGE, &m->sb, sizeof(m->sb)));

    esp_err_t ret = bbt_read_and_check(m, ECU_BBT_PAGE, &m->bbt);
    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Primary BBT invalid, trying backup");
        ret = bbt_read_and_check(m, ECU_BBT_BAK_PAGE, &m->bbt);
    }
    return ret;
}

static esp_err_t alloc_blocks(struct ecu_mgr_t *m, uint32_t num_blocks, uint16_t *start)
{
    uint8_t in_use[NAND_BLOCKS_TOTAL / 8U];
    memset(in_use, 0, sizeof(in_use));

    for (uint8_t s = 0; s < ECU_MAX_SLOTS; s++)
    {
        const ecu_slot_entry_t *sl = &m->sb.slots[s];
        if (sl->status != ECU_SLOT_ACTIVE && sl->status != ECU_SLOT_UPDATING)
        {
            continue;
        }

        for (uint8_t f = 0; f < ECU_FILE_COUNT; f++)
        {
            const ecu_file_slot_t *fs = &sl->files[f];
            for (uint16_t b = fs->first_block; b < fs->first_block + fs->num_blocks && b < NAND_BLOCKS_TOTAL; b++)
            {
                in_use[b / 8U] |= (uint8_t) (1U << (b % 8U));
            }
        }
    }

    uint32_t count = 0;
    uint16_t first = 0;

    for (uint16_t blk = ECU_DATA_BLOCK_START; blk <= ECU_DATA_BLOCK_END; blk++)
    {
        bool bad = bbt_is_bad(&m->bbt, blk);
        bool used = (in_use[blk / 8U] & (1U << (blk % 8U))) != 0;

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

static void slot_reset_empty(ecu_slot_entry_t *slot, uint8_t slot_id)
{
    memset(slot, 0, sizeof(*slot));
    slot->status = ECU_SLOT_EMPTY;
    slot->slot_id = slot_id;
    slot->magic = ECU_SLOT_MAGIC;
}

/**
 * @brief Convert a logical page index to a physical NAND block and page.
 *
 * Logical page 0 -> first_block, page 0 (contains the file header)
 * Logical page 63 -> first_block, page 63
 * Logical page 64 -> first_block+1, page 0
 */
static inline void logical_page_to_physical(uint32_t logical_page, uint16_t first_block, uint16_t *out_block, uint8_t *out_page)
{
    *out_block = first_block + (uint16_t) (logical_page / NAND_PAGES_PER_BLOCK);
    *out_page = (uint8_t) (logical_page % NAND_PAGES_PER_BLOCK);
}

esp_err_t ecu_manager_init(ecu_manager_handle_t *out_handle, nand_handle_t nand)
{
    MGR_CHECK_ARG(out_handle);
    MGR_CHECK_ARG(nand);

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
    ESP_LOGI(TAG, "Initialized: %" PRIu16 " ECUs, %" PRIu32 " bad blocks", m->sb.num_slots, m->bbt.num_bad_blocks);
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

static esp_err_t scan_bbt_locked(struct ecu_mgr_t *m)
{
    ESP_LOGI(TAG, "Scanning for bad blocks...");
    memset(m->bbt.bitmap, 0, sizeof(m->bbt.bitmap));
    m->bbt.num_bad_blocks = 0;

    for (uint16_t b = 1; b < NAND_BLOCKS_TOTAL; b++)
    {
        bool is_bad = false;
        if (nand_is_bad_block(m->nand, b, &is_bad) == ESP_OK && is_bad)
        {
            bbt_mark_bad(&m->bbt, b);
        }
    }

    m->bbt.magic = ECU_BBT_MAGIC;
    m->bbt.timestamp = (uint32_t) (esp_timer_get_time() / 1000000LL);

    ESP_LOGI(TAG, "Scan complete: %" PRIu32 " bad blocks", m->bbt.num_bad_blocks);
    return ESP_OK;
}

esp_err_t ecu_manager_scan_bbt(ecu_manager_handle_t mgr)
{
    MGR_CHECK_ARG(mgr);
    struct ecu_mgr_t *m = mgr;

    MGR_LOCK(m);
    esp_err_t ret = scan_bbt_locked(m);
    if (ret == ESP_OK)
    {
        ret = sb_save(m);
    }
    MGR_UNLOCK(m);
    return ret;
}

esp_err_t ecu_manager_format(ecu_manager_handle_t mgr)
{
    MGR_CHECK_ARG(mgr);
    struct ecu_mgr_t *m = mgr;

    ESP_LOGW(TAG, "Formatting management area...");

    MGR_LOCK(m);
    memset(&m->sb, 0, sizeof(m->sb));
    memset(&m->bbt, 0, sizeof(m->bbt));

    m->sb.magic = ECU_SUPERBLOCK_MAGIC;
    m->sb.format_version = ECU_FORMAT_VERSION;
    m->sb.data_block_start = ECU_DATA_BLOCK_START;
    m->sb.data_block_end = ECU_DATA_BLOCK_END;
    m->sb.timestamp = (uint32_t) (esp_timer_get_time() / 1000000LL);

    for (uint8_t s = 0; s < ECU_MAX_SLOTS; s++)
    {
        slot_reset_empty(&m->sb.slots[s], s);
    }
    m->bbt.magic = ECU_BBT_MAGIC;

    esp_err_t ret = scan_bbt_locked(m);
    if (ret == ESP_OK)
    {
        ret = sb_save(m);
    }
    MGR_UNLOCK(m);
    return ret;
}

/**
 * @brief Erase all NAND blocks belonging to one file of a slot.
 */
static void erase_file_blocks(nand_handle_t nand, const ecu_file_slot_t *fs)
{
    for (uint16_t b = fs->first_block; b < fs->first_block + fs->num_blocks; b++)
    {
        nand_erase_block(nand, b);
    }
}

/** Erase all NAND blocks belonging to a slot (all 3 files). */
static void erase_slot_blocks(nand_handle_t nand, const ecu_slot_entry_t *sl)
{
    for (uint8_t f = 0; f < ECU_FILE_COUNT; f++)
    {
        erase_file_blocks(nand, &sl->files[f]);
    }
}

esp_err_t ecu_writer_begin(ecu_manager_handle_t mgr, const char *ecu_name, const char *fw_version, uint32_t hw_id, ecu_file_type_t ftype, uint32_t total_size, ecu_writer_t *writer)
{
    MGR_CHECK(mgr);
    MGR_CHECK_ARG(ecu_name);
    MGR_CHECK_ARG(writer);
    if (total_size == 0 || (ftype != ECU_FILE_BIN && ftype != ECU_FILE_PRM && ftype != ECU_FILE_IDX))
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct ecu_mgr_t *m = mgr;
    memset(writer, 0, sizeof(ecu_writer_t));

    writer->_page_buf = heap_caps_malloc(NAND_PAGE_SIZE * 2U, MALLOC_CAP_DMA);
    if (!writer->_page_buf)
    {
        return ESP_ERR_NO_MEM;
    }
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
    bool slot_was_new = (slot->status != ECU_SLOT_ACTIVE && slot->status != ECU_SLOT_UPDATING);

    // ecu_slot_entry_t slot_backup = *slot;

    if (slot->status == ECU_SLOT_ACTIVE)
    {
        ESP_LOGI(TAG, "Replacing '%s' ftype=%u", ecu_name, (unsigned) ftype);
        erase_file_blocks(m->nand, file_slot(slot, ftype));
    }

    if (slot_was_new)
    {
        slot_reset_empty(slot, (uint8_t) slot_idx);
    }
    slot->status = ECU_SLOT_UPDATING;
    strlcpy(slot->ecu_name, ecu_name, ECU_NAME_MAX_LEN);
    strlcpy(slot->fw_version, fw_version ? fw_version : "", ECU_VERSION_MAX_LEN);
    slot->hw_id = hw_id;

    uint16_t num_blk = (uint16_t) ecu_bytes_to_blocks(total_size);
    ecu_file_slot_t *fs = file_slot(slot, ftype);

    fs->first_block = 0;
    fs->num_blocks = num_blk;
    fs->size = total_size;

    uint16_t first_blk = 0;
    esp_err_t ret = alloc_blocks(m, num_blk, &first_blk);
    if (ret != ESP_OK)
    {
        // *slot = slot_backup;
        MGR_UNLOCK(m);
        free(writer->_page_buf);
        writer->_page_buf = NULL;
        return ret;
    }

    fs->first_block = first_blk;
    fs->num_blocks = num_blk;

    // ESP_LOGW(TAG, "FIRST BLOCK: %d", fs->first_block);
    // ESP_LOGW(TAG, "NUM OF BLOCKS: %d", fs->num_blocks);

    ret = sb_save(m);
    if (ret != ESP_OK)
    {
        // *slot = slot_backup;
        MGR_UNLOCK(m);
        free(writer->_page_buf);
        writer->_page_buf = NULL;
        return ret;
    }

    MGR_UNLOCK(m);

    for (uint16_t b = first_blk; b < first_blk + num_blk; b++)
    {
        ret = nand_erase_block(m->nand, b);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to erase block %u", b);

            MGR_LOCK(m);
            // *slot = slot_backup;
            sb_save(m);
            MGR_UNLOCK(m);

            free(writer->_page_buf);
            writer->_page_buf = NULL;
            return ret;
        }
    }

    writer->_mgr = mgr;
    writer->_ftype = ftype;
    writer->_slot_idx = (uint8_t) slot_idx;
    writer->_first_block = first_blk;
    writer->_num_blocks = num_blk;
    writer->_cur_block = first_blk;
    writer->_cur_page = 0U;
    writer->_crc_accum = 0xFFFFFFFFUL;
    writer->_page_fill = ECU_FILE_HEADER_SIZE;
    writer->_first_page = true;
    writer->total_size = total_size;
    writer->bytes_written = 0U;
    strlcpy(writer->_ecu_name, ecu_name, ECU_NAME_MAX_LEN);
    strlcpy(writer->_fw_version, fw_version ? fw_version : "", ECU_VERSION_MAX_LEN);
    writer->_hw_id = hw_id;
    writer->_valid = true;

    ESP_LOGI(TAG, "Writer opened: '%s' ftype=%u size=%u blocks=%u..%u", ecu_name, (unsigned) ftype, (unsigned) total_size, first_blk, (unsigned) (first_blk + num_blk - 1U));

    return ESP_OK;
}

/** @brief Internal flush: write the current page buffer to NAND. */
static esp_err_t writer_flush_page(ecu_writer_t *wr)
{
    if (wr->_page_fill == 0 || (wr->_first_page && wr->_page_fill == ECU_FILE_HEADER_SIZE))
    {
        return ESP_OK;
    }

    struct ecu_mgr_t *m = wr->_mgr;

    if (wr->_page_fill < NAND_PAGE_SIZE)
    {
        memset(wr->_page_buf + wr->_page_fill, 0xFF, NAND_PAGE_SIZE - wr->_page_fill);
    }

    if (wr->_first_page)
    {
        memcpy(wr->_page_buf + NAND_PAGE_SIZE, wr->_page_buf, NAND_PAGE_SIZE);
        wr->_first_page = false;
        wr->_page_fill = 0;
        wr->_cur_page++;
        if (wr->_cur_page >= NAND_PAGES_PER_BLOCK)
        {
            wr->_cur_page = 0;
            wr->_cur_block++;
        }
        return ESP_OK;
    }

    esp_err_t ret = nand_program_page(m->nand, wr->_cur_block, wr->_cur_page, wr->_page_buf, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Program page failed blk=%u pg=%u: %s", wr->_cur_block, wr->_cur_page, esp_err_to_name(ret));
        return ret;
    }

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
        ESP_LOGE(TAG, "Write exceeds total_size (%u + %u > %u)", (unsigned) writer->bytes_written, (unsigned) len, (unsigned) writer->total_size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t remaining = len;
    const uint8_t *src = data;

    while (remaining > 0)
    {
        uint16_t space = (uint16_t) (NAND_PAGE_SIZE - writer->_page_fill);
        uint32_t chunk = (remaining < (uint32_t) space) ? remaining : (uint32_t) space;

        memcpy(writer->_page_buf + writer->_page_fill, src, chunk);
        writer->_crc_accum = ecu_crc32_update(writer->_crc_accum, src, chunk);

        writer->_page_fill += (uint16_t) chunk;
        writer->bytes_written += chunk;
        src += chunk;
        remaining -= chunk;

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

    if (writer->bytes_written != writer->total_size)
    {
        ESP_LOGE(TAG, "commit: written=%u != declared total=%u", (unsigned) writer->bytes_written, (unsigned) writer->total_size);
        ecu_writer_abort(writer);
        return ESP_ERR_INVALID_SIZE;
    }

    struct ecu_mgr_t *m = writer->_mgr;
    esp_err_t ret;

    if (writer->_page_fill > 0)
    {
        ret = writer_flush_page(writer);
        if (ret != ESP_OK)
        {
            writer->_valid = false;
            return ret;
        }
    }

    uint32_t final_crc = ~writer->_crc_accum;

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
    snprintf(fname, sizeof(fname), "%s.%s", writer->_ecu_name, file_ext(writer->_ftype));
    strlcpy(hdr.filename, fname, ECU_MAX_FILENAME);

    memcpy(first_page_buf, &hdr, sizeof(hdr));

    ret = nand_program_page(m->nand, writer->_first_block, 0U, first_page_buf, NULL);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to program first page: %s", esp_err_to_name(ret));
        writer->_valid = false;
        return ret;
    }

    MGR_LOCK(m);
    ecu_slot_entry_t *slot = &m->sb.slots[writer->_slot_idx];
    ecu_file_slot_t *fs = file_slot(slot, writer->_ftype);
    fs->crc32 = final_crc;
    fs->size = writer->total_size;

    slot->status = ECU_SLOT_ACTIVE;
    slot->timestamp_updated = (uint32_t) (esp_timer_get_time() / 1000000LL);
    if (slot->timestamp_created == 0)
    {
        slot->timestamp_created = slot->timestamp_updated;
    }

    uint8_t c = 0;
    ecu_get_active_slot_count(m, &c);
    m->sb.num_slots = c;

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

    for (uint16_t b = writer->_first_block; b < writer->_first_block + writer->_num_blocks; b++)
    {
        nand_erase_block(m->nand, b);
    }

    MGR_LOCK(m);
    ecu_slot_entry_t *slot = &m->sb.slots[writer->_slot_idx];
    ecu_file_slot_t *fs = file_slot(slot, writer->_ftype);
    memset(fs, 0, sizeof(*fs));

    bool any_file = false;
    for (uint8_t f = 0; f < ECU_FILE_COUNT; f++)
    {
        if (slot->files[f].size > 0)
        {
            any_file = true;
            break;
        }
    }

    if (!any_file)
    {
        slot_reset_empty(slot, writer->_slot_idx);
    }

    else
    {
        slot->status = ECU_SLOT_ACTIVE;
    }

    sb_save(m);
    MGR_UNLOCK(m);

    free(writer->_page_buf);
    writer->_page_buf = NULL;
    writer->_valid = false;

    ESP_LOGW(TAG, "Writer aborted: '%s' ftype=%u", writer->_ecu_name, (unsigned) writer->_ftype);
    return ESP_OK;
}

/** @brief Load a NAND page into the reader cache if not already present. */
static esp_err_t reader_load_page(ecu_reader_t *rd, uint32_t logical_page)
{
    uint16_t blk;
    uint8_t pg;
    logical_page_to_physical(logical_page, rd->_first_block, &blk, &pg);

    if (rd->_cache_valid && rd->_cache_block == blk && rd->_cache_page == pg)
    {
        return ESP_OK; /* cache hit */
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
    if (ftype != ECU_FILE_BIN && ftype != ECU_FILE_PRM && ftype != ECU_FILE_IDX)
    {
        return ESP_ERR_INVALID_ARG;
    }

    struct ecu_mgr_t *m = mgr;
    memset(reader, 0, sizeof(ecu_reader_t));

    const ecu_slot_entry_t *sl = find_active_slot(m, ecu_name);
    if (!sl)
    {
        return ESP_ERR_NOT_FOUND;
    }

    const ecu_file_slot_t *fs = file_slot_c(sl, ftype);
    if (fs->size == 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    reader->_page_cache = heap_caps_malloc(NAND_PAGE_SIZE, MALLOC_CAP_DMA);
    if (!reader->_page_cache)
    {
        return ESP_ERR_NO_MEM;
    }

    reader->_first_block = fs->first_block;
    reader->_mgr = mgr;

    esp_err_t ret = reader_load_page(reader, 0U);
    if (ret != ESP_OK)
    {
        free(reader->_page_cache);
        reader->_page_cache = NULL;
        return ret;
    }

    // ESP_LOG_BUFFER_HEX_LEVEL(TAG, reader->_page_cache, NAND_PAGE_SIZE, ESP_LOG_WARN);
    const ecu_file_header_t *hdr = (const ecu_file_header_t *) reader->_page_cache;
    if (hdr->magic != ECU_FILE_MAGIC)
    {
        ESP_LOGE(TAG, "Invalid file magic at block %u: 0x%08X", fs->first_block, (unsigned) hdr->magic);
        free(reader->_page_cache);
        reader->_page_cache = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    reader->file_size = fs->size;
    reader->bytes_read = 0U;
    reader->_expected_crc = fs->crc32;
    reader->_crc_accum = 0xFFFFFFFFUL;
    reader->_cache_valid = true;
    reader->_valid = true;

    ESP_LOGD(TAG, "Reader opened: '%s' ftype=%u size=%u", ecu_name, (unsigned) ftype, (unsigned) fs->size);

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
        uint32_t abs_offset = ECU_FILE_HEADER_SIZE + reader->bytes_read + done;
        uint32_t logical_pg = abs_offset / NAND_PAGE_SIZE;
        uint16_t pg_offset = (uint16_t) (abs_offset % NAND_PAGE_SIZE);

        esp_err_t ret = reader_load_page(reader, logical_pg);
        if (ret != ESP_OK)
        {
            return ret;
        }

        uint16_t avail_in_page = (uint16_t) (NAND_PAGE_SIZE - pg_offset);
        uint32_t to_copy = want_bytes - done;
        if (to_copy > (uint32_t) avail_in_page)
        {
            to_copy = (uint32_t) avail_in_page;
        }

        memcpy(dst + done, reader->_page_cache + pg_offset, to_copy);
        reader->_crc_accum = ecu_crc32_update(reader->_crc_accum, dst + done, to_copy);
        done += to_copy;
    }

    reader->bytes_read += done;

    if (out_count)
    {
        *out_count = done / size;
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
            ESP_LOGD(TAG, "CRC verification OK (0x%08X)", (unsigned) final_crc);
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
        if (sl->status != ECU_SLOT_ACTIVE || strncmp(sl->ecu_name, ecu_name, ECU_NAME_MAX_LEN) != 0)
        {
            continue;
        }

        MGR_UNLOCK(m);
        erase_slot_blocks(m->nand, sl);
        MGR_LOCK(m);

        slot_reset_empty(sl, s);
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

        if (ftype == ECU_FILE_PRM)
        {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, chunk, want, ESP_LOG_WARN);
        }
    }
    free(chunk);

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
        *bin_ok = (file_slot_c(sl, ECU_FILE_BIN)->size > 0) ? verify_file_crc(mgr, ecu_name, ECU_FILE_BIN) : true;
    }

    if (prm_ok)
    {
        *prm_ok = (file_slot_c(sl, ECU_FILE_PRM)->size > 0) ? verify_file_crc(mgr, ecu_name, ECU_FILE_PRM) : true;
    }

    if (idx_ok)
    {
        *idx_ok = (file_slot_c(sl, ECU_FILE_IDX)->size > 0) ? verify_file_crc(mgr, ecu_name, ECU_FILE_IDX) : true;
    }

    return ESP_OK;
}

static void slot_to_info(const ecu_slot_entry_t *sl, ecu_info_t *info)
{
    const ecu_file_slot_t *bin = file_slot_c(sl, ECU_FILE_BIN);
    const ecu_file_slot_t *prm = file_slot_c(sl, ECU_FILE_PRM);
    const ecu_file_slot_t *idx = file_slot_c(sl, ECU_FILE_IDX);

    info->slot_id = sl->slot_id;
    info->status = (ecu_slot_status_t) sl->status;
    info->hw_id = sl->hw_id;
    info->bin_size = bin->size;
    info->prm_size = prm->size;
    info->idx_size = idx->size;
    info->timestamp_updated = sl->timestamp_updated;
    info->has_bin = (bin->num_blocks > 0 && bin->size > 0);
    info->has_prm = (prm->num_blocks > 0 && prm->size > 0);
    info->has_idx = (idx->num_blocks > 0 && idx->size > 0);
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

esp_err_t ecu_get_active_slot_count(ecu_manager_handle_t mgr, uint8_t *count)
{
    MGR_CHECK(mgr);
    MGR_CHECK_ARG(count);

    struct ecu_mgr_t *m = mgr;
    uint8_t n = 0;
    for (uint8_t s = 0; s < ECU_MAX_SLOTS; s++)
    {
        if (m->sb.slots[s].status == ECU_SLOT_ACTIVE)
        {
            // ESP_LOGD(TAG, "%s: %" PRId8, m->sb.slots[s].ecu_name, m->sb.slots[s].slot_id);
            n++;
        }
    }
    *count = n;
    // ESP_LOGD(TAG, "ECU slots count %" PRId8, n);
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
            for (uint8_t f = 0; f < ECU_FILE_COUNT; f++)
            {
                used += sl->files[f].num_blocks;
            }
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