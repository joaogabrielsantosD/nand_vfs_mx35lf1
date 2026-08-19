#include "nand.h"

#include "mx35lf1_driver.h"
#include "nand_diskio.h"
#include <string.h>

int dhara_nand_is_bad(const struct dhara_nand *n, dhara_block_t b)
{
    nand_handle_t *h = diskio_nand_get_handle();
    row_address_t row = {.block = b, .page = 0};
    bool is_bad;

    int ret = nand_mx35lf1_block_is_bad(h, row, &is_bad);
    if (ret != NAND_RET_OK)
    {
        is_bad = true;
    }

    return (int) is_bad;
}

void dhara_nand_mark_bad(const struct dhara_nand *n, dhara_block_t b)
{
    nand_handle_t *h = diskio_nand_get_handle();
    row_address_t row = {.block = b, .page = 0};
    nand_mx35lf1_block_mark_bad(h, row);
}

int dhara_nand_erase(const struct dhara_nand *n, dhara_block_t b, dhara_error_t *err)
{
    nand_handle_t *h = diskio_nand_get_handle();
    row_address_t row = {.block = b, .page = 0};

    int ret = nand_mx35lf1_block_erase(h, row);
    if (ret == NAND_RET_OK)
    {
        return 0;  // sucess
    }

    if (ret == NAND_RET_E_FAIL)
    {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }

    return -1;
}

int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p, const uint8_t *data, dhara_error_t *err)
{
    nand_handle_t *h = diskio_nand_get_handle();
    row_address_t row = {.whole = p};

    int ret = nand_mx35lf1_page_program(h, row, 0, data, NAND_PAGE_SIZE);
    if (ret == NAND_RET_OK)
    {
        return 0;  // sucess
    }

    if (ret == NAND_RET_P_FAIL)
    {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }

    return -1;
}

int dhara_nand_is_free(const struct dhara_nand *n, dhara_page_t p)
{
    nand_handle_t *h = diskio_nand_get_handle();
    row_address_t row = {.whole = p};

    bool is_free;
    int ret = nand_mx35lf1_page_is_free(h, row, &is_free);
    if (ret != NAND_RET_OK)
    {
        is_free = false;
    }

    return (int) is_free;
}

int dhara_nand_read(const struct dhara_nand *n, dhara_page_t p, size_t offset, size_t length, uint8_t *data, dhara_error_t *err)
{
    nand_handle_t *h = diskio_nand_get_handle();
    row_address_t row = {.whole = p};

    int ret = nand_mx35lf1_page_read(h, row, offset, data, length);
    if (ret == NAND_RET_OK)
    {
        return 0;  // sucess
    }

    if (ret == NAND_RET_ECC_ERR)
    {
        dhara_set_error(err, DHARA_E_ECC);  // ECC failure
        return -1;
    }

    return -1;
}

int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst, dhara_error_t *err)
{
    nand_handle_t *h = diskio_nand_get_handle();
    row_address_t source = {.whole = src};
    row_address_t destination = {.whole = dst};

    int ret = nand_mx35lf1_page_copy(h, source, destination);
    if (ret == NAND_RET_OK)
    {
        return 0;
    }

    if (ret == NAND_RET_ECC_ERR)
    {
        dhara_set_error(err, DHARA_E_ECC);
        return -1;
    }

    if (ret == NAND_RET_P_FAIL)
    {
        dhara_set_error(err, DHARA_E_BAD_BLOCK);
        return -1;
    }

    return -1;
}
