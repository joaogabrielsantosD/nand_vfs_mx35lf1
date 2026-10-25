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

- Translation of 2048-byte logical sectors (one per NAND page) to physical pages. FatFs must allow 2048-byte sectors, so keep `CONFIG_FATFS_SECTOR_4096` (the ESP-IDF default); a compile-time check enforces it.
- Journaling and recovery from sudden power loss (_power-cut safety_).
- Integrated _Wear Leveling_ and _Garbage Collection_ algorithms.
- Detection and isolation of dynamically failing blocks.

### 3. DiskIO Layer (`nand_diskio.h` / `.c`)

Connects the Dhara library to the `diskio` interface defined by ESP-IDF's FatFS.

- Sector read and write operations.
- Disk status control and `ioctl` commands.

### 4. VFS Layer (`nand_vfs.h` / `.c`)

Provides functions to initialize the FTL, format the volume (if necessary), and register the VFS mount point (e.g., `/nand`).

- Enables the use of standard C library functions (`fopen`, `fwrite`, `fread`, `fclose`, `unlink`, etc.).
- Configuration via `vfs_nand_conf_t`, supporting custom mount points, optional internal SPI bus initialization, and auto-formatting on mount failure.
- VFS lifecycle management functions (`vfs_nand_register`, `vfs_nand_unregister`).
- Utility APIs to check mount status (`vfs_nand_mounted`), force format the volume (`vfs_nand_format`), and query storage usage (`vfs_nand_info`).

---

### Example

The `example/main/` directory contains two types of example code:

- `nand_driver_test.c`: Demonstrates how to use the NAND memory driver at the lowest level.
- `nand_vfs_test_.c`: Demonstrates Virtual File System (VFS) integration for using the NAND memory with standard C functions. Internally, the functions called in this example use the FTL, Wear Leveling, and Garbage Collection layers.

---

## Behavior notes

- `vfs_nand_register()` mounts the FAT volume itself. It only formats when the mount fails with
  "no filesystem" **and** `format_if_mounted_failed` is set; I/O or ECC errors never trigger a format.
- The bad block table is read once in `nand_mx35lf1_init()` and kept in RAM (128 bytes).
- All `nand_mx35lf1_*` calls are serialized by an internal recursive mutex.
- Data is committed to the NAND on `fclose()`/`fsync()` (FatFs `CTRL_SYNC` -> `dhara_map_sync`).
