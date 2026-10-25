#ifndef MX35LF1_CMD_H
#define MX35LF1_CMD_H

/* SPI COMMAND OPCODES  (datasheet Table 1) */
#define CMD_WRITE_ENABLE   0x06U /**< Set Write Enable Latch (WEL)     */
#define CMD_WRITE_DISABLE  0x04U /**< Reset Write Enable Latch         */
#define CMD_GET_FEATURE    0x0FU /**< Read feature register            */
#define CMD_SET_FEATURE    0x1FU /**< Write feature register           */
#define CMD_PAGE_READ      0x13U /**< Transfer page: array -> cache    */
#define CMD_READ_CACHE_X1  0x03U /**< Read from cache ×1 (slow)        */
#define CMD_READ_CACHE_X1F 0x0BU /**< Read from cache ×1 (fast)        */
#define CMD_READ_CACHE_X2  0x3BU /**< Read from cache ×2 (dual)        */
#define CMD_READ_CACHE_X4  0x6BU /**< Read from cache ×4 (quad)        */
#define CMD_READ_CACHE_SEQ 0x31U /**< Page Read Cache Sequential       */
#define CMD_READ_CACHE_END 0x3FU /**< Page Read Cache End              */
#define CMD_READ_ID        0x9FU /**< Read manufacturer + device ID    */
#define CMD_ECC_STATUS     0x7CU /**< Read internal ECC status         */
#define CMD_PROG_LOAD      0x02U /**< Program Load (cache reset first) */
#define CMD_PROG_LOAD_RND  0x84U /**< Program Load Random Data         */
#define CMD_PROG_LOAD_X4   0x32U /**< Program Load ×4 (quad)           */
#define CMD_PROG_LOAD_RX4  0x34U /**< Program Load Random Data ×4      */
#define CMD_PROG_EXECUTE   0x10U /**< Execute: transfer cache -> array  */
#define CMD_BLOCK_ERASE    0xD8U /**< Erase an entire block            */
#define CMD_RESET          0xFFU /**< Software reset                   */

/* FEATURE REGISTER ADDRESSES */
#define NAND_FEAT_BLOCK_PROT 0xA0U /**< Block Protection register        */
#define NAND_FEAT_SECURE_OTP 0xB0U /**< Secure OTP / Configuration reg.  */
#define NAND_FEAT_STATUS     0xC0U /**< Status register (read-only)      */

/* DEVICE IDENTIFICATION */
#define NAND_MANUFACTURER_ID 0xC2U /**< Macronix manufacturer ID         */
#define NAND_DEVICE_ID       0x12U /**< MX35LF1GE4AB device ID           */

/* WRAP ADDRESS BITS  (datasheet Table 3, 1Gb only) */
// #define NAND_WRAP_2112 0x00U /**< Wrap length = 2112 bytes         */
// #define NAND_WRAP_2048 0x01U /**< Wrap length = 2048 bytes         */
// #define NAND_WRAP_64   0x02U /**< Wrap length = 64 bytes           */
// #define NAND_WRAP_16   0x03U /**< Wrap length = 16 bytes           */

#endif  // MX35LF1_CMD_H