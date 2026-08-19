#include "nand_vfs.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <esp_log.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NAND_PATH          "/nand"
#define NAND_DIR_PATH      "/nand/app/"
#define NAND_DIR_PATH_FILE "/nand/app/firm.txt"
#define NAND_TEST_FILE     "/nand/test.txt"
#define NAND_TEST_OLD_FILE "/nand/old.txt"
#define NAND_TEST_NEW_FILE "/nand/new.txt"

const char data[] = "Write test on nand memory using POSIX functions in C";

#define MAX_LTFS_PATH_LEN 256

#define NAND_SPI_HOST SPI2_HOST
#define NAND_PIN_MOSI 11
#define NAND_PIN_SCLK 12
#define NAND_PIN_MISO 13
#define NAND_PIN_CS   15
#define NAND_PIN_WP   16                 /* -1 if not connected */
#define NAND_PIN_HOLD 17                 /* -1 if not connected */
#define NAND_CLOCK_HZ (50 * 1000 * 1000) /* 50 MHz — conservative */

static const char TAG[] = "NAND_VFS";

/* ------------------------------------------------------------------------ */
/* Path helpers                                                              */
/* ------------------------------------------------------------------------ */

/**
 * @brief Copies @p src into @p dst as a path buffer initializer.
 *
 * This is intentionally a simple string copy with assert-based contracts,
 * to avoid using formatting APIs (e.g., snprintf) for path buffer seeding.
 *
 * @param[out] dst     Destination buffer.
 * @param[in]  dst_sz  Destination buffer size.
 * @param[in]  src     Source string.
 */
static bool path_copy(char *dst, size_t dst_sz, const char *src)
{
    assert(dst != NULL);
    assert(src != NULL);
    assert(dst_sz > 0);

    const size_t src_len = strlen(src);
    if (dst_sz < src_len + 1)
    {
        return false;
    }

    memcpy(dst, src, src_len);
    dst[src_len] = '\0';
    return true;
}

/**
 * @brief Normalizes a path in-place.
 *
 * Rules:
 * - collapse repeated '/'
 * - remove '.' segments
 * - resolve '..' (without dropping leading '..' for relative paths)
 * - strip trailing '/' except for root ('/')
 * - empty relative path becomes '.'
 *
 * @param[in,out] path   Path buffer to normalize.
 * @param[in]     size   Size of the buffer.
 */
static void path_normalize(char *path, size_t size)
{
    assert(path != NULL);
    assert(size > 0);
    assert(strnlen(path, size) < size);  // Ensure input is NULL-terminated

    size_t in_len = strlen(path);
    assert(in_len < MAX_LTFS_PATH_LEN);
    const bool absolute = (in_len > 0 && path[0] == '/');

    // strtok_r needs a mutable buffer
    char tmp[MAX_LTFS_PATH_LEN];
    memcpy(tmp, path, in_len + 1);

    // Stack of segment pointers (into tmp).
    const size_t max_segments = MAX_LTFS_PATH_LEN / 2 + 1;  // safe upper bound (worst case is 2 char segments like '/a/a/a.../')
    const char *segments[max_segments];
    size_t seg_count = 0;

    char *saveptr = NULL;
    char *tok = strtok_r(tmp, "/", &saveptr);
    while (tok != NULL)
    {
        if (strcmp(tok, ".") == 0)
        {
            tok = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        if (strcmp(tok, "..") == 0)
        {
            if (absolute)
            {
                // if we're not at root, pop the last segment
                if (seg_count > 0)
                {
                    seg_count--;
                }
            }

            else
            {
                if (seg_count > 0 && strcmp(segments[seg_count - 1], "..") != 0)
                {
                    // Pop a real segment.
                    seg_count--;
                }

                else
                {
                    // Keep leading ".." for relative paths and allow stacking "../../x".
                    assert(seg_count < max_segments);  // Should never overflow
                    segments[seg_count++] = tok;
                }
            }

            tok = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        assert(seg_count < max_segments);  // Should never overflow
        segments[seg_count++] = tok;
        tok = strtok_r(NULL, "/", &saveptr);
    }

    // Rebuild into path.
    // Note: This should not overflow if segments come only from tokenization
    size_t pos = 0;
    if (absolute)
    {
        path[pos++] = '/';
    }

    for (size_t i = 0; i < seg_count; i++)
    {
        const size_t seg_len = strlen(segments[i]);

        if (pos > 0 && path[pos - 1] != '/')
        {
            path[pos++] = '/';
        }

        memcpy(path + pos, segments[i], seg_len);
        pos += seg_len;
    }

    // Empty path
    if (pos == 0)
    {
        assert(size >= 2);

        if (absolute)
        {
            path[0] = '/';
            path[1] = '\0';
            return;
        }

        path[0] = '.';
        path[1] = '\0';
        return;
    }

    // Strip trailing '/' except for root.
    if (pos > 1 && path[pos - 1] == '/')
    {
        pos--;
    }

    path[pos] = '\0';
    return;
}

/**
 * @brief Appends a path to @p base, normalizing.
 *
 * Do not use with base URLs (http://<something>.com/) as it will break them.
 *
 * @param[out] base       Base path segment.
 * @param[in]  base_size  Size of the base path buffer.
 * @param[in]  append     Path segment to be appended.
 */
static bool path_join(char *base, size_t base_size, const char *append)
{
    assert(base != NULL);
    assert(append != NULL);
    assert(base_size > 0);

    // Must not be used with URLs.
    assert(strstr(base, "://") == NULL);

    const size_t llen = strnlen(base, MAX_LTFS_PATH_LEN);
    const size_t rlen = strnlen(append, MAX_LTFS_PATH_LEN);

    // Ensure we have space for base + '/' + append + '\0'
    if (base_size < llen + 1 + rlen + 1)
    {
        return false;
    }

    base[llen] = '/';
    memcpy(base + llen + 1, append, rlen);
    base[llen + 1 + rlen] = '\0';

    path_normalize(base, base_size);

    return true;
}

/* ------------------------------------------------------------------------ */
/* Test helpers                                                              */
/* ------------------------------------------------------------------------ */

static void ltfs_log_dir(const char *full_path)
{
    DIR *dp = opendir(full_path);
    if (!dp)
    {
        ESP_LOGE(TAG, "Failed to open directory: %s", full_path);
        return;
    }

    ESP_LOGI(TAG, "Listing directory: %s", full_path);

    struct dirent *entry;
    struct stat st;
    while ((entry = readdir(dp)) != NULL)
    {
        char entry_path[256];
        if (!path_copy(entry_path, sizeof(entry_path), full_path) || !path_join(entry_path, sizeof(entry_path), entry->d_name))
        {
            ESP_LOGW(TAG, "Failed to build path for entry: %s", entry->d_name);
            break;
        }

        if (stat(entry_path, &st) == 0)
        {
            if (S_ISDIR(st.st_mode))
            {
                ESP_LOGW(TAG, "DIR  : %s", entry->d_name);
            }

            else
            {
                ESP_LOGW(TAG, "FILE : %s (%ld bytes)", entry->d_name, st.st_size);
            }
        }

        else
        {
            ESP_LOGW(TAG, "Failed to stat: %s", entry_path);
        }
    }

    closedir(dp);
}

static void list_file_data(const char *full_path)
{
    struct stat st;
    if (stat(full_path, &st) != 0)
    {
        ESP_LOGE(TAG, "Failed to stat file: %s", full_path);
        return;
    }
    ESP_LOGW(TAG, "File size=%d - exist=%s", (int) st.st_size, S_ISREG(st.st_mode) ? "YES" : "NO");
}

static bool write_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open file %s", path);
        return false;
    }

    size_t w = fwrite((uint8_t *) data, 1, sizeof(data), f);
    if (w != sizeof(data))
    {
        ESP_LOGE(TAG, "Failed to write to file.");
        fclose(f);
        return false;
    }

    if (fclose(f) != 0)
    {
        ESP_LOGE(TAG, "Failed to close file (errno=%d msg=%s)", errno, strerror(errno));
        return false;
    }

    return true;
}

static bool read_file(const char *path, char *buffer, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open file %s", path);
        return false;
    }

    size_t r = fread((uint8_t *) buffer, 1, size, f);

    if (fclose(f) != 0)
    {
        ESP_LOGE(TAG, "Failed to close file (errno=%d msg=%s)", errno, strerror(errno));
        return false;
    }

    ESP_LOGD(TAG, "Read %u bytes from file: %s", (unsigned) r, path);
    ESP_LOGD(TAG, "%s", buffer);
    list_file_data(path);
    return true;
}

static void test_file_without_dir(void)
{
    if (!write_file(NAND_TEST_FILE))
    {
        return;
    }

    char receive[sizeof(data)];
    read_file(NAND_TEST_FILE, receive, sizeof(receive));
}

static void test_file_with_dir(void)
{
    if (!write_file(NAND_DIR_PATH_FILE))
    {
        return;
    }

    char receive[sizeof(data)];
    read_file(NAND_DIR_PATH_FILE, receive, sizeof(receive));
}

static void test_file_with_fseek(void)
{
    char receive[sizeof(data)];
    FILE *fr = fopen(NAND_TEST_FILE, "rb");
    if (!fr)
    {
        ESP_LOGE(TAG, "Failed to open file %s", NAND_TEST_FILE);
        return;
    }

    fseek(fr, 19, SEEK_SET);
    size_t r = fread((uint8_t *) receive, 1, sizeof(receive), fr);

    if (fclose(fr) != 0)
    {
        ESP_LOGE(TAG, "Failed to close file (errno=%d msg=%s)", errno, strerror(errno));
        return;
    }
    ESP_LOGD(TAG, "Read %u bytes from file: %s", (unsigned) r, NAND_TEST_FILE);
    ESP_LOGD(TAG, "%s", receive);
    list_file_data(NAND_TEST_FILE);
}

static bool test_mount(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = NAND_PIN_MOSI,
        .miso_io_num = NAND_PIN_MISO,
        .sclk_io_num = NAND_PIN_SCLK,
        .quadwp_io_num = NAND_PIN_WP,   /* WP#  / SIO2 */
        .quadhd_io_num = NAND_PIN_HOLD, /* HOLD#/ SIO3 */
        .max_transfer_sz = NAND_SPI_MAX_TRANSFER,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    spi_bus_initialize(NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO);

    nand_config_t cfg = {
        .spi_host = NAND_SPI_HOST,
        .pin_cs = NAND_PIN_CS,
        .clock_speed_hz = NAND_CLOCK_HZ,
        .disable_ecc = false,
    };

    ESP_LOGI(TAG, "Mount vfs nand");
    bool ok = mount_vfs_nand(NAND_PATH, &cfg) == ESP_OK;
    ESP_LOGI(TAG, "%s", ok ? "Mounted success" : "Mounted error");
    return ok;
}

static void test_rename_and_unlink(void)
{
    ltfs_log_dir(NAND_PATH);
    test_file_without_dir();
    rename(NAND_TEST_OLD_FILE, NAND_TEST_NEW_FILE);
    ltfs_log_dir(NAND_PATH);
    unlink(NAND_TEST_NEW_FILE);
    ltfs_log_dir(NAND_PATH);
}

static void test_mkdir_and_file(void)
{
    mkdir(NAND_DIR_PATH, 0777);
    ltfs_log_dir(NAND_DIR_PATH);
    test_file_with_dir();

    struct stat st;
    if (stat(NAND_DIR_PATH, &st) != 0)
    {
        ESP_LOGE(TAG, "Failed to stat file: %s", NAND_DIR_PATH);
        return;
    }
    ESP_LOGI(TAG, "%s is dir? %s", NAND_DIR_PATH, S_ISDIR(st.st_mode) ? "YES" : "NO");
    ltfs_log_dir(NAND_DIR_PATH);
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                               */
/* ------------------------------------------------------------------------ */

void app_main(void)
{
    // esp_log_level_set("mx35lf1", ESP_LOG_DEBUG);
    esp_log_level_set(TAG, ESP_LOG_DEBUG);

    if (!test_mount())
    {
        return;
    }

    if (!write_file(NAND_TEST_OLD_FILE))
    {
        return;
    }

    test_file_with_fseek();
    test_rename_and_unlink();
    test_mkdir_and_file();

    ESP_LOGI(TAG, "Unmount vfs nand");
    unmount_vfs_nand();
}