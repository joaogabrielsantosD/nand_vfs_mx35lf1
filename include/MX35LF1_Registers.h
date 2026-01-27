#ifndef __MX35LF1_REGISTERS_H__
#define __MX35LF1_REGISTERS_H__

#include "driver/gpio.h"

// Definition of commands (OpCodes)
#define CMD_GET_FEATURES               0x0F  // Get features
#define CMD_SET_FEATURES               0x1F  // Set features
#define CMD_PAGE_READ                  0x13  // Array read
#define CMD_READ_FROM_CACHE            0x03  // Output cache data on MISO
#define CMD_READ_RANDOM_DATA           0x0B  // Output cache data on MISO
#define CMD_READ_FROM_CACHE_X2         0x3B  // Output cache data on MOSI and MISO
#define CMD_READ_FROM_CACHE_X4         0x6B  // Output cache data on MOSI, MISO, WP#, HOLD#
#define CMD_PAGE_READ_CACHE_SEQUENTIAL 0x31  // The next page data is transferred to buffer
#define CMD_PAGE_READ_CACHE_END        0x3F  // The last page data is transferred to buffer
#define CMD_READ_MANUFACTURER_ID       0x9F  // Read device ID
#define CMD_BLOCK_ERASE                0xD8  // Block erase
#define CMD_PROGRAM_EXECUTE            0x10  // Enter block/page address, no data, execute
#define CMD_PROGRAM_LOAD_X1            0x02  // Load program data with cache reset first
#define CMD_PROGRAM_LOAD_RANDOM_DATA   0x84  // Load program data without cache reset
#define CMD_WRITE_ENABLE               0x06  // Setting Write Enable Latch (WEL) bit
#define CMD_WRITE_DISABLE              0x04  // Reset Write Enable Latch (WEL) bit
#define CMD_PROGRAM_LOAD_X4            0x32  // Program Load operation with X4 data input
#define CMD_QUAD_IO_PROGRAM_INPUT      0X34  // Program Load random data operation with X4 data input
#define CMD_RESET                      0xFF  // Reset the device

// Defining addresses and internal resources
#define REG_BLOCK_PROTECTION    0xA0  // Address to Block Protection Register
#define REG_SECURE_OTP          0xB0  // Address to Secure OTP Register
#define REG_STATUS              0xC0  // Address to Status Register
#define REG_INTERNAL_ECC_STATUS 0x7C  // Internal ECC Status Output
#define MANUFACTURER_ID         0xC2  // Manufacturer ID, explicit in datasheet
#define DEVICE_ID               0x12  // Device ID, explicit in datasheet

// Size Settings
#define BLOCK_SIZE               1024   // Block size (0-1023)
#define NUM_PAGES_PER_BLOCK      64     // Number of pages per block (0-63)
#define MAX_PAGE_SIZE            65535  // All pages in the NAND FLASH (1024 * 64)
#define PAGE_SIZE                2112   // Page size (in bytes) (0-2111) (2048 + 64)
#define PAGE_SIZE_WITHOUT_ECC    2048   // 2048 Bytes for data in one page inside the block
#define PAGE_SIZE_ECC_PROTECTION 64     // Reserved Bytes to ECC Protection data and blocks

// Wrap Read address
#define FINAL_PAGE_ADDRESS_2112 0b00  // Read all bytes in the page
#define FINAL_PAGE_ADDRESS_2048 0b01  // Read until byte 2048
#define FINAL_PAGE_ADDRESS_64   0b10  // Read until byte 64
#define FINAL_PAGE_ADDRESS_16   0b11  // Read until byte 16
#define FINAL_PAGE(p)           (p == 0b00 ? 2112 : p == 0b01 ? 2048 : p == 0b10 ? 64 : 16)

// Status and auxiliary values
#define STATUS_BUSY  0x01  // Device busy flag
#define STATUS_READY 0x00  // Device ready flag
#define OIP_BIT      (1 << 0)
#define WEL_BIT      (1 << 1)
#define ERS_FAIL_BIT (1 << 2)
#define PGM_FAIL_BIT (1 << 3)
#define ECC_S0_BIT   (1 << 4)
#define ECC_S1_BIT   (1 << 5)
#define CRBSY_BIT    (1 << 6)

// Secure OTP values
#define QE_BIT             (1 << 0)
#define ECC_ENABLED_BIT    (1 << 4)
#define SECURE_OTP_ENABLE  (1 << 6)
#define SECURE_OTP_PROTECT (1 << 7)

// Block Protection values
/* check the datasheet to see all options (page 36) */
#define ALL_UNLOCKED       0b00000000
#define ALL_LOCKED         0b00011100
#define ALL_LOCKED_MAX_BIT 0b00011111
#define SP_BIT             (1 << 0)
#define COMPLEMENTARY_BIT  (1 << 1)
#define INVERT_BIT         (1 << 2)
#define BP0_BIT            (1 << 3)
#define BP1_BIT            (1 << 4)
#define BP2_BIT            (1 << 5)
#define BPRWD_BIT          (1 << 7)

// Internal ECC Status values
#define ECCSR0_BIT (1 << 0)
#define ECCSR1_BIT (1 << 1)
#define ECCSR2_BIT (1 << 2)
#define ECCSR3_BIT (1 << 3)

#endif  // __MX35LF1_REGISTERS_H__
