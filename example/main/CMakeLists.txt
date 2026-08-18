set(src "")
# list(APPEND src "nand_driver_test.c")
list(APPEND src "nand_vfs_test_.c")

idf_component_register(
    SRCS
        ${src}
    INCLUDE_DIRS "."
    REQUIRES
        mx35lf1ge4ab
)
