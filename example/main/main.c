#include <stdio.h>
#include <inttypes.h>
#include <sdkconfig.h>
#include "esp_err.h"
#include "esp_timer.h"

#include "string.h"

#include <MX35LF1.h>

const char *TAG = "NAND MX35 TEST";
#define BUFFER_SIZE 2048

mx35_err_t init_module(void)
{
    nand_mx35_config_t cfg = {
        .spi_pins =
            {
                .mosi_io = GPIO_NUM_11,
                .miso_io = GPIO_NUM_13,
                .sclk_io = GPIO_NUM_12,
                .cs_io = GPIO_NUM_15,
                .hd_io = GPIO_NUM_17,
                .wp_io = GPIO_NUM_16,
            },
    };

    ESP_LOGW(TAG, "init nand mx35");
    return mx35_init(&cfg);
}

mx35_err_t erase_block(uint16_t block)
{
    return mx35_erase_block(block);
}

mx35_err_t erase_all_blocks(void)
{
    return mx35_bulk_erase();
}

mx35_err_t load_message(void)
{
    uint8_t *message = (uint8_t *) malloc(BUFFER_SIZE);

    if (!message)
    {
        ESP_LOGE("MAIN", "Error to alocate buffer");
        return MX35_FAIL;
    }

    for (size_t i = 0; i < BUFFER_SIZE; i++)
    {
        *(message + i) = i % 256;
    }

    ESP_LOGW(TAG, "Write a buffer");
    uint16_t block = 1;
    uint8_t page = 0;
    uint16_t address = 0;
    erase_block(block);

    mx35_err_t ret = mx35_write_page(block, page, message, BUFFER_SIZE, &address);

    if (ret == MX35_OK)
    {
        ESP_LOGI(TAG, "Write ok");
        ESP_LOGI(TAG, "LAST BLOCK: %d AND LAST PAGE: %d", mx35_PageAddress_to_Block(address), mx35_PageAddress_to_Page(address));
    }

    else
    {
        ESP_LOGE(TAG, "Error to write the buffer");
    }

    free(message);
    return ret;
}

mx35_err_t load_static_message(uint8_t d)
{
    uint8_t *message = (uint8_t *) malloc(BUFFER_SIZE);

    if (!message)
    {
        ESP_LOGE("MAIN", "Error to alocate buffer");
        return MX35_FAIL;
    }

    for (size_t i = 0; i < BUFFER_SIZE; i++)
    {
        *(message + i) = d;
    }

    ESP_LOGW(TAG, "Write a buffer");
    uint16_t block = 2;
    uint8_t page = 0;
    uint16_t address = 0;
    erase_block(block);

    mx35_err_t ret = mx35_write_page(block, page, message, BUFFER_SIZE, &address);

    if (ret == MX35_OK)
    {
        ESP_LOGI(TAG, "Write ok");
        ESP_LOGI(TAG, "LAST BLOCK: %d AND LAST PAGE: %d", mx35_PageAddress_to_Block(address), mx35_PageAddress_to_Page(address));
    }

    else
    {
        ESP_LOGE(TAG, "Error to write the buffer");
    }

    free(message);
    return ret;
}

mx35_err_t load_big_message(void)
{
    uint8_t *message = (uint8_t *) malloc(BUFFER_SIZE * 2);

    if (!message)
    {
        ESP_LOGE("MAIN", "Error to alocate buffer");
        return MX35_FAIL;
    }

    for (size_t i = 0; i < BUFFER_SIZE * 2; i++)
    {
        *(message + i) = 0xBB;
    }

    ESP_LOGI(TAG, "Write a buffer");
    uint16_t block = 3;
    uint8_t page = 0;
    uint16_t address = 0;
    erase_block(block);

    int64_t load_time = esp_timer_get_time();
    mx35_err_t ret = mx35_write_page(block, page, message, BUFFER_SIZE * 2, &address);
    load_time = esp_timer_get_time() - load_time;
    ESP_LOGW(TAG, "Time to load big message: %" PRId64 " ms", load_time / 1000);

    if (ret == MX35_OK)
    {
        ESP_LOGI(TAG, "Write ok");
        ESP_LOGI(TAG, "LAST BLOCK: %d AND LAST PAGE: %d", mx35_PageAddress_to_Block(address), mx35_PageAddress_to_Page(address));
    }

    else
    {
        ESP_LOGE(TAG, "Error to write the buffer");
    }

    free(message);
    return ret;
}

mx35_err_t read_message(void)
{
    uint8_t *recv = (uint8_t *) malloc(2048);
    if (recv == NULL)
    {
        ESP_LOGE("MAIN", "Cannot create the receiver buffer");
        return MX35_NO_MEM;
    }

    ESP_LOGI("TEST", "Read a buffer");
    mx35_err_t result = mx35_read_page(1, 0, recv, 2048, NULL);
    if (result == MX35_OK)
    {
        ESP_LOGI("MAIN", "Read ok");
        ESP_LOG_BUFFER_HEX_LEVEL("MAIN", recv, 2048, ESP_LOG_DEBUG);
    }
    free(recv);
    return result;
}

mx35_err_t read_static_message(void)
{
    uint8_t *recv = (uint8_t *) malloc(2048);
    if (recv == NULL)
    {
        ESP_LOGE("MAIN", "Cannot create the receiver buffer");
        return MX35_NO_MEM;
    }

    ESP_LOGI("TEST", "Read a buffer");
    mx35_err_t result = mx35_read_page(2, 0, recv, 2048, NULL);
    if (result == MX35_OK)
    {
        ESP_LOGI("MAIN", "Read ok");
        ESP_LOG_BUFFER_HEX_LEVEL("MAIN", recv, 2048, ESP_LOG_DEBUG);
    }
    free(recv);
    return result;
}

mx35_err_t read_big_message(void)
{
    uint8_t *recv = (uint8_t *) malloc(BUFFER_SIZE * 2);
    if (recv == NULL)
    {
        ESP_LOGE("MAIN", "Cannot create the receiver buffer");
        return MX35_NO_MEM;
    }

    ESP_LOGW("TEST", "Read a buffer");

    int64_t read_time = esp_timer_get_time();
    mx35_err_t result = mx35_read_page(3, 0, recv, BUFFER_SIZE * 2, NULL);
    read_time = esp_timer_get_time() - read_time;
    ESP_LOGW(TAG, "Time to read big message: %" PRId64 " ms", read_time / 1000);

    if (result == MX35_OK)
    {
        ESP_LOGI("MAIN", "Read ok");
        ESP_LOG_BUFFER_HEX_LEVEL("MAIN", recv, BUFFER_SIZE * 2, ESP_LOG_DEBUG);
    }

    free(recv);
    return result;
}

void app_main()
{
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    esp_log_level_set("MAIN", ESP_LOG_INFO);
    esp_log_level_set("TEST", ESP_LOG_INFO);

    mx35_err_t result = init_module();
    if (result != MX35_OK)
    {
        ESP_LOGE("MAIN", "Error to initialize the nand_mx35 module");
        goto end_example;
    }
    ESP_LOGD(TAG, "NAND MX35 initialized successfully");

    uint8_t bad[20] = {0};
    mx35_get_bad_block_array(bad, sizeof(bad));
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, bad, mx35_get_bad_block_count(), ESP_LOG_WARN);
    ESP_LOGW(TAG, "Page Address of the first bad block: 0x%4x", mx35_PageAddress(bad[0], 0));

    result = MX35_OK;
    // result = erase_all_blocks();
    if (result != MX35_OK)
    {
        ESP_LOGE("MAIN", "Error to erase the block");
        goto end_example;
    }
    ESP_LOGD(TAG, "Erased block successfully");

    result = load_big_message();
    result = load_message();
    if (result != MX35_OK)
    {
        ESP_LOGE("MAIN", "Error to write the buffer");
        goto end_example;
    }
    ESP_LOGD(TAG, "Buffer written successfully");

    result = read_big_message();
    result = read_message();
    if (result != MX35_OK)
    {
        ESP_LOGE("MAIN", "Error to read the buffer");
        goto end_example;
    }
    ESP_LOGD(TAG, "Buffer readed successfully");

    result |= load_static_message(0xD3);
    result |= read_static_message();
    result |= load_static_message(0xC4);
    result |= read_static_message();
    if (result != MX35_OK)
    {
        ESP_LOGE("MAIN", "Error to read the buffer");
        goto end_example;
    }
    ESP_LOGD(TAG, "Buffer readed successfully");

end_example:
    if (result == MX35_OK)
    {
        ESP_LOGW(TAG, "Deinit nand mx35");
        mx35_deinit();
        ESP_LOGD(TAG, "NAND MX35 deinitialized successfully");
    }
}
