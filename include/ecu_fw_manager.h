#ifndef ECU_FW_MANAGER_H
#define ECU_FW_MANAGER_H

/**
 * ─────────────────────────────────────────────────────────────
 *  NAND MEMORY LAYOUT
 * ─────────────────────────────────────────────────────────────
 *
 *  ┌──────────────────────────────────────────────────────────┐
 *  │ Block 0 : Reserved                                       │
 *  ├──────────────────────────────────────────────────────────┤
 *  │ Block 1   (RESERVED — guaranteed good by the vendor)     │
 *  │   Page 0  : Superblock (master ECU table)                │
 *  │   Page 1  : Bad Block Table (BBT)                        │
 *  │   Page 3  : BBT backup                                   │
 *  ├──────────────────────────────────────────────────────────┤
 *  │ Blocks 2–1023   (DATA — ECU firmware storage)            │
 *  └──────────────────────────────────────────────────────────┘
 * 
 *  File layout within allocated blocks:
 *    Block N, Page 0 : [ecu_file_header_t 72 B][data: up to 1976 B]
 *    Block N, Pages 1–63 : [data: 2048 B each]
 *    Block N+1 ... : continuation
 */

#include "esp_err.h"
#include "mx35lf1ge4ab.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ECU_NAME_MAX_LEN    32U /**< Max ECU name length (incl. NUL)     */
#define ECU_VERSION_MAX_LEN 16U /**< Max firmware version string length  */
#define ECU_MAX_FILENAME    48U /**< Max file name length (incl. NUL)    */
#define ECU_MAX_SLOTS       10U

/** Block ranges */
#define ECU_RESERVED_BLOCK_START 1U
#define ECU_DATA_BLOCK_START     2U
#define ECU_DATA_BLOCK_END       (NAND_BLOCKS_TOTAL - 1U)

/** Special pages within Block 0 */
#define ECU_SUPERBLOCK_PAGE 0U /**< Superblock      */
#define ECU_BBT_PAGE        2U /**< Bad Block Table */
#define ECU_BBT_BAK_PAGE    4U /**< BBT backup      */

/** Magic numbers for structure validation */
#define ECU_SUPERBLOCK_MAGIC 0x4E414E44UL /**< "NAND"              */
#define ECU_SLOT_MAGIC       0x45435546UL /**< "ECUF"              */
#define ECU_FILE_MAGIC       0x46574C44UL /**< "FWLD"              */
#define ECU_BBT_MAGIC        0xBBBBBBBBUL
#define ECU_FORMAT_VERSION   0x0102U /**< Format version 1.2 */

/** File type identifier stored in ecu_file_header_t. */
typedef enum
{
    ECU_FILE_BIN = 0x01U, /**< Binary firmware image (.bin) */
    ECU_FILE_PRM = 0x02U, /**< Parameter file (.prm)        */
    ECU_FILE_IDX = 0x03U, /**< Index / map file (.idx)      */
} ecu_file_type_t;

/** Lifecycle state of a firmware slot. */
typedef enum
{
    ECU_SLOT_EMPTY = 0x00U,    /**< Available for use                  */
    ECU_SLOT_ACTIVE = 0x01U,   /**< Contains valid, committed firmware */
    ECU_SLOT_UPDATING = 0x02U, /**< Write operation in progress        */
    ECU_SLOT_INVALID = 0xFFU,  /**< Corrupted or failed write          */
} ecu_slot_status_t;

typedef struct
{
    uint32_t magic;                  /**< Must equal ECU_FILE_MAGIC       */
    uint8_t file_type;               /**< ecu_file_type_t value           */
    char filename[ECU_MAX_FILENAME]; /**< Original file name (e.g. "ECU_ENGINE.bin") */
    uint32_t file_size;              /**< Total data bytes (not including this header) */
    uint32_t crc32;                  /**< IEEE 802.3 CRC32 of all data bytes */
    uint32_t timestamp;              /**< Unix timestamp of the write     */
    uint16_t first_block;            /**< First NAND block allocated      */
    uint16_t num_blocks;             /**< Number of NAND blocks allocated */
    uint16_t format_version;         /**< ECU_FORMAT_VERSION              */
} ecu_file_header_t;

/** Bytes available for data in the first page, after the header. */
#define ECU_FILE_HEADER_SIZE      ((uint32_t) sizeof(ecu_file_header_t))
#define ECU_FIRST_PAGE_DATA_SPACE (NAND_PAGE_SIZE - ECU_FILE_HEADER_SIZE)

typedef struct
{
    uint16_t first_block; /**< First NAND block allocated      */
    uint16_t num_blocks;  /**< Number of NAND blocks allocated */
    uint32_t size;
    uint32_t crc32;
} ecu_file_slot_t;

#define ECU_FILE_COUNT 3U /**< BIN, PRM, IDX */

typedef struct
{
    uint32_t magic;                        /**< Must equal ECU_SLOT_MAGIC   */
    uint8_t status;                        /**< ecu_slot_status_t           */
    uint8_t slot_id;                       /**< Index in the slot table     */
    char ecu_name[ECU_NAME_MAX_LEN];       /**< Unique ECU identifier       */
    char fw_version[ECU_VERSION_MAX_LEN];  /**< Firmware version string     */
    uint32_t hw_id;                        /**< Hardware revision ID        */
    uint32_t timestamp_created;            /**< Unix time of first write    */
    uint32_t timestamp_updated;            /**< Unix time of last write     */
    ecu_file_slot_t files[ECU_FILE_COUNT]; /**< index: ftype - 1            */
} ecu_slot_entry_t;

typedef struct
{
    uint32_t magic;            /**< ECU_SUPERBLOCK_MAGIC                    */
    uint16_t format_version;   /**< ECU_FORMAT_VERSION                      */
    uint16_t num_slots;        /**< Number of active slots                  */
    uint32_t data_block_start; /**< First data block (ECU_DATA_BLOCK_START) */
    uint32_t data_block_end;   /**< Last data block  (ECU_DATA_BLOCK_END)   */
    uint32_t timestamp;        /**< Last modification timestamp */
    uint32_t header_crc32;
    ecu_slot_entry_t slots[ECU_MAX_SLOTS]; /**< Slot table */
} ecu_superblock_t;

/**
 * @brief Bad Block Table — compact bitmap, one bit per block.
 *
 * 1024 blocks / 8 = 128 bytes of bitmap.
 * Stored in Page 2 of Block 0.
 */
#define ECU_BBT_BYTES (NAND_BLOCKS_TOTAL / 8U)

typedef struct
{
    uint32_t magic;                /**< ECU_BBT_MAGIC                       */
    uint32_t crc32;                /**< CRC32 of bitmap[]                   */
    uint32_t num_bad_blocks;       /**< Number of bits set in bitmap        */
    uint32_t timestamp;            /**< Last scan / update time             */
    uint8_t bitmap[ECU_BBT_BYTES]; /**< Bit N = 1 -> block N is bad          */
} ecu_bbt_t;

typedef struct ecu_mgr_t *ecu_manager_handle_t;

/** Summary information about one stored ECU (used for listings). */
typedef struct
{
    uint8_t slot_id;
    char ecu_name[ECU_NAME_MAX_LEN];
    char fw_version[ECU_VERSION_MAX_LEN];
    uint32_t hw_id;
    ecu_slot_status_t status;
    uint32_t bin_size; /**< 0 if not present */
    uint32_t prm_size; /**< 0 if not present */
    uint32_t idx_size; /**< 0 if not present */
    uint32_t timestamp_updated;
    bool has_bin;
    bool has_prm;
    bool has_idx;
} ecu_info_t;

/* ============================================================
 *  STREAMING WRITE CONTEXT  (ecu_writer_t)
 *
 *  Usage pattern:
 *
 *    ecu_writer_t wr;
 *    esp_err_t err = ecu_writer_begin(mgr, "ECU_ENGINE", "v2.0", hw_id,
 *                                     ECU_FILE_BIN, total_size, &wr);
 *    if (err != ESP_OK) { handle_error(); }
 *
 *    while (data_available) {
 *        err = ecu_writer_write(&wr, chunk_ptr, chunk_len);
 *        if (err != ESP_OK) { ecu_writer_abort(&wr); return; }
 *    }
 *
 *    err = ecu_writer_commit(&wr);
 *    // After commit() or abort(), the context is invalid.
 * ============================================================ */

typedef struct
{
    /* Public read-only fields */
    uint32_t total_size;    /**< Exact byte count declared in begin() */
    uint32_t bytes_written; /**< Data bytes written so far            */

    /* Internal fields — do not access directly */
    ecu_manager_handle_t _mgr;
    ecu_file_type_t _ftype;
    uint8_t _slot_idx;
    char _ecu_name[ECU_NAME_MAX_LEN];
    char _fw_version[ECU_VERSION_MAX_LEN];
    uint32_t _hw_id;

    uint16_t _first_block; /**< First NAND block allocated            */
    uint16_t _num_blocks;  /**< Total NAND blocks allocated           */
    uint16_t _cur_block;   /**< Current block being written           */
    uint8_t _cur_page;     /**< Current page within the current block */

    uint32_t _crc_accum; /**< Running CRC32 accumulator (IEEE 802.3) */

    /**
     * Internal page buffer — DMA-capable, NAND_PAGE_SIZE × 2 bytes.
     * Allocated in ecu_writer_begin(), freed in commit() / abort().
     *
     * Byte layout:
     *   [0 .. PAGE_SIZE-1]         : working buffer (page being assembled)
     *   [PAGE_SIZE .. 2*PAGE_SIZE-1]: first-page hold buffer — the first
     *     page is NOT written to NAND until commit(), when the file header
     *     (including the final CRC32) is prepended and the page is written last.
     */
    uint8_t *_page_buf;
    uint16_t _page_fill; /**< Valid bytes currently in _page_buf */
    bool _first_page;    /**< True until the first page has been flushed */
    bool _valid;         /**< False after commit() or abort() */
} ecu_writer_t;

/* ============================================================
 *  STREAMING READ CONTEXT  (ecu_reader_t)
 *
 *  Usage pattern (fread style):
 *
 *    ecu_reader_t rd;
 *    esp_err_t err = ecu_reader_open(mgr, "ECU_ENGINE", ECU_FILE_BIN, &rd);
 *    uint8_t buf[4096];
 *    size_t  got = 0;
 *    while (true) {
 *        err = ecu_reader_read(&rd, buf, 1, sizeof(buf), &got);
 *        if (err != ESP_OK || got == 0) break;
 *        process(buf, got);
 *    }
 *    err = ecu_reader_close(&rd);
 * ============================================================ */

typedef struct
{
    /* Public read-only fields */
    uint32_t file_size;  /**< Total data bytes in the file             */
    uint32_t bytes_read; /**< Data bytes returned to the caller so far */

    /* Internal fields — do not access directly */
    ecu_manager_handle_t _mgr;
    uint16_t _first_block;
    uint32_t _expected_crc;
    uint32_t _crc_accum;

    uint8_t *_page_cache; /**< Single-page cache — DMA-capable, NAND_PAGE_SIZE bytes */
    uint16_t _cache_block;
    uint8_t _cache_page;
    bool _cache_valid;

    bool _valid; /**< False after ecu_reader_close() */
} ecu_reader_t;

/**
 * @brief Initialize the ECU firmware manager.
 *
 * Loads the superblock and BBT from NAND. If they are absent or
 * corrupted (e.g. first use), the manager formats Block 0 automatically
 * and runs a full bad-block scan (~10 s for 1024 blocks).
 *
 * @param[out] out_handle  Receives the allocated manager handle.
 * @param[in]  nand        Initialized NAND driver handle.
 * @return ESP_OK or an esp_err_t error code.
 */
esp_err_t ecu_manager_init(ecu_manager_handle_t *out_handle, nand_handle_t nand);

/**
 * @brief Release all resources held by the manager.
 */
esp_err_t ecu_manager_deinit(ecu_manager_handle_t mgr);

/**
 * @brief Erase Block 0 and re-initialize the management area.
 *
 * ⚠ Destroys ALL stored ECU records. Use with caution.
 */
esp_err_t ecu_manager_format(ecu_manager_handle_t mgr);

/**
 * @brief Scan every block and rebuild the Bad Block Table in RAM and NAND.
 *
 * Long operation — approximately 10 seconds for 1024 blocks.
 * Recommended only on first use or when BBT integrity is suspect.
 */
esp_err_t ecu_manager_scan_bbt(ecu_manager_handle_t mgr);

/**
 * @brief Begin a streaming write operation for one file of an ECU.
 *
 * Actions performed:
 *  1. Locate or create a slot for ecu_name.
 *  2. Erase old blocks for this file type (if the ECU already exists).
 *  3. Allocate the required number of NAND blocks.
 *  4. Erase the newly allocated blocks.
 *  5. Mark the slot as UPDATING in the superblock (crash-safe).
 *  6. Allocate two DMA page buffers (4 KB total) for the write context.
 *
 * The superblock is only updated to ACTIVE in ecu_writer_commit().
 * If power is lost between begin() and commit(), the slot remains in
 * UPDATING state and can be detected on the next ecu_manager_init().
 *
 * @note To write all three files (.bin, .prm, .idx), call
 *       begin/write/commit three times with the same ecu_name,
 *       using a different ftype each time.
 *
 * @param[in]  total_size  Exact number of data bytes that will be written.
 *                         ecu_writer_commit() verifies this matches bytes_written.
 * @param[out] writer      Caller-allocated context (stack or heap).
 */
esp_err_t
ecu_writer_begin(ecu_manager_handle_t mgr, const char *ecu_name, const char *fw_version, uint32_t hw_id, ecu_file_type_t ftype, uint32_t total_size, ecu_writer_t *writer);

/**
 * @brief Append a data chunk to the open streaming write (analogous to fwrite).
 *
 * Internally buffers data in a single NAND page (2048 bytes) and only
 * calls nand_program_page() when the buffer is full. Any chunk size is
 * accepted — sub-page and multi-page chunks are handled transparently.
 * The CRC32 is accumulated over every byte passed here.
 *
 * The first page of the file is held in RAM until commit() so that the
 * final CRC32 can be embedded in the file header before it is written.
 *
 * @param[in] data  Pointer to the data chunk.
 * @param[in] len   Number of bytes in this chunk.
 * @return ESP_ERR_INVALID_SIZE if bytes_written + len > total_size.
 */
esp_err_t ecu_writer_write(ecu_writer_t *writer, const uint8_t *data, uint32_t len);

/**
 * @brief Finalize and commit the written file.
 *
 * Steps:
 *  1. Flush the last partial page to NAND.
 *  2. Verify that bytes_written == total_size.
 *  3. Finalize the CRC32.
 *  4. Build the file header (with the correct CRC) and write Page 0 to NAND.
 *  5. Update the slot metadata (CRC, size, status = ACTIVE) in the superblock.
 *  6. Save the superblock to NAND.
 *  7. Free internal buffers and invalidate the context.
 *
 * @return ESP_ERR_INVALID_SIZE if bytes_written != total_size.
 */
esp_err_t ecu_writer_commit(ecu_writer_t *writer);

/**
 * @brief Cancel the write and release all resources.
 *
 * Erases the allocated NAND blocks, clears the slot entry and saves the
 * superblock. The writer context is invalidated.
 * Call this on any error path after a successful ecu_writer_begin().
 * 
 * @return ESP_OK - abort ok
 *         ESP_ERR_INVALID_ARG - invalid arguments
 *         ESP_ERR_TIMEOUT - mutex timeout error
 */
esp_err_t ecu_writer_abort(ecu_writer_t *writer);

/**
 * @brief Open a file for streaming read access.
 *
 * Reads the file header from NAND, validates the magic number, and
 * allocates a one-page cache (2 KB DMA). The read cursor is positioned
 * at byte 0 of the file data (the header is transparent to the caller).
 *
 * @param[in]  ecu_name  ECU identifier.
 * @param[in]  ftype     File type to open (BIN, PRM, or IDX).
 * @param[out] reader    Caller-allocated context (stack or heap).
 * @return ESP_ERR_NOT_FOUND if the ECU or the requested file does not exist.
 */
esp_err_t ecu_reader_open(ecu_manager_handle_t mgr, const char *ecu_name, ecu_file_type_t ftype, ecu_reader_t *reader);

/**
 * @brief Read data from the open file (analogous to fread).
 *
 *   ecu_reader_read(&rd, buf, size, count, &got)
 *   ≡  fread(buf, size, count, stream)
 *
 * Returns the number of complete elements (each of @p size bytes) read
 * in *out_count. A return of 0 with ESP_OK signals end-of-file.
 *
 * The internal page cache is checked first; a NAND read is issued only
 * when the requested data is not already in cache. This makes sub-page
 * reads (e.g. 1 byte at a time) efficient as long as consecutive reads
 * stay within the same 2048-byte page.
 *
 * The CRC32 accumulator is updated over every byte returned.
 *
 * @param[in]  size      Size of one element in bytes.
 * @param[in]  count     Number of elements requested.
 * @param[out] out_count Elements actually read; may be NULL.
 */
esp_err_t ecu_reader_read(ecu_reader_t *reader, void *buf, size_t size, size_t count, size_t *out_count);

/**
 * @brief Close the reader and validate the CRC32 if fully read.
 *
 * CRC validation is performed only when bytes_read == file_size AND
 * ecu_reader_seek() was never called. In all other cases the check is
 * skipped, but the page cache is still freed and the context invalidated.
 *
 * @return ESP_OK or ESP_ERR_INVALID_CRC.
 */
esp_err_t ecu_reader_close(ecu_reader_t *reader);

/**
 * @brief Delete a stored ECU firmware (all three files).
 *
 * Erases the allocated NAND blocks, clears the slot entry, and updates
 * the superblock.
 */
esp_err_t ecu_delete_firmware(ecu_manager_handle_t mgr, const char *ecu_name);

/**
 * @brief Verify the CRC32 of each file for a stored ECU.
 *
 * Uses the streaming reader internally — only 2 KB of RAM regardless
 * of file size.
 *
 * @param[out] bin_ok / prm_ok / idx_ok  Pass NULL to skip a particular file.
 */
esp_err_t ecu_verify_firmware(ecu_manager_handle_t mgr, const char *ecu_name, bool *bin_ok, bool *prm_ok, bool *idx_ok);

/** @brief Retrieve information about an ECU by name. */
esp_err_t ecu_get_info(ecu_manager_handle_t mgr, const char *ecu_name, ecu_info_t *info);

/** @brief Retrieve information about an ECU by its slot index. */
esp_err_t ecu_get_info_by_slot(ecu_manager_handle_t mgr, uint8_t slot_id, ecu_info_t *info);

/**
 * @brief List all active ECUs.
 *
 * @param[out] list       Array to fill.
 * @param[in]  list_size  Capacity of the array.
 * @param[out] count      Number of entries written.
 */
esp_err_t ecu_list(ecu_manager_handle_t mgr, ecu_info_t *list, uint8_t list_size, uint8_t *count);

/**
 * @brief Get the number of active ECU slots.
 *
 * @param[in]  mgr    ECU manager handle.
 * @param[out] count  Number of active slots.
 *
 * @return
 *      - ESP_OK on success
 *      - ESP_ERR_INVALID_STATE if the manager is not initialized
 *      - ESP_ERR_INVALID_ARG if count is NULL
 */
esp_err_t ecu_get_active_slot_count(ecu_manager_handle_t mgr, uint8_t *count);

/** @brief Check whether an ECU with the given name exists. */
esp_err_t ecu_exists(ecu_manager_handle_t mgr, const char *ecu_name, bool *exists);

/**
 * @brief Return block-usage statistics for the data area.
 *
 * @param[out] total_blocks  Total data blocks (16 – 1023).
 * @param[out] used_blocks   Blocks currently occupied by ECU files.
 * @param[out] free_blocks   Blocks available for new allocations.
 * @param[out] bad_blocks    Blocks marked bad in the BBT.
 */
esp_err_t ecu_get_stats(ecu_manager_handle_t mgr, uint32_t *total_blocks, uint32_t *used_blocks, uint32_t *free_blocks, uint32_t *bad_blocks);

#endif /* ECU_FW_MANAGER_H */
