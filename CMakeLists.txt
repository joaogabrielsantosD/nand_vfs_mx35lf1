idf_component_register(
    SRCS
        "src/mx35lf1_driver.c"
        # "src/nand_diskio.c"
        # "src/nand_vfs.c"
        "src/dhara/dhara/error.c"
        "src/dhara/dhara/journal.c"
        "src/dhara/dhara/map.c"
        "src/dhara/dhara/nand.c"
    INCLUDE_DIRS
        "include"
        "src/dhara/dhara"
    REQUIRES
        driver
        esp_timer
        esp_rom
        freertos
        heap
        log
        spi_flash
        fatfs
)
