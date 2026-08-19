```
MacronixMX35LF1/
├── CMakeLists.txt          # Component build configuration for ESP-IDF
├── Kconfig                 # Configuration options via menuconfig
├── include/
│   ├── mx35lf1_driver.h    # Low-level driver definitions and API
│   ├── nand_diskio.h       # DiskIO interface (FatFS integration)
│   └── nand_vfs.h          # VFS mounting interface
├── src/
│   ├── mx35lf1_driver.c    # SPI command implementation and NAND operations
│   ├── nand_diskio.c       # Implementation of FatFS functions
│   ├── nand_vfs.c          # Dhara FTL initialization and VFS registration
│   └── dhara/              # Integrated Dhara Flash Translation Layer (FTL)
├── example/                # ESP-IDF example project
│   ├── CMakeLists.txt
│   ├── Makefile
│   ├── sdkconfig.defaults
│   └── main/
│       ├── CMakeLists.txt
│       ├── nand_driver_test.c   # Test of the driver's raw functions
│       └── nand_vfs_test_.c     # Test of VFS/FatFS mounting and operations
```
---

## Architecture and Components

### 1. Low-Level Driver (`mx35lf1_driver.h` / `.c`)
Direct communication between the ESP32 SPI bus and the MX35LF1 chip.
- SPI bus initialization and JEDEC ID reading.
- Page read and write operations (2048 bytes + OOB/Spare area).
- Block erase (128 KB per block).
- Status register management (ECC status, WIP - Write In Progress).
- Factory Bad Block detection and runtime marking.

### 2. Dhara FTL Layer (`src/dhara/`)
Dhara is a lightweight FTL designed for NAND Flash.
- Translation of 512-byte logical sectors to physical NAND pages.
- Journaling and recovery from sudden power loss (*power-cut safety*).
- Integrated *Wear Leveling* and *Garbage Collection* algorithms.
- Detection and isolation of dynamically failing blocks.

### 3. DiskIO Layer (`nand_diskio.h` / `.c`)
Connects the Dhara library to the `diskio` interface defined by ESP-IDF's FatFS.
- Sector read and write operations.
- Disk status control and `ioctl` commands.

### 4. VFS Layer (`nand_vfs.h` / `.c`)
Provides functions to initialize the FTL, format the volume (if necessary), and register the VFS mount point (e.g., `/nand`).
- Enables the use of standard C library functions (`fopen`, `fwrite`, `fread`, `fclose`, `unlink`, etc.).

---

### Example

The `example/main/` directory contains two types of example code:
- `nand_driver_test.c`: Demonstrates how to use the NAND memory driver at the lowest level.
- `nand_vfs_test_.c`: Demonstrates Virtual File System (VFS) integration for using the NAND memory with standard C functions. Internally, the functions called in this example use the FTL, Wear Leveling, and Garbage Collection layers.
