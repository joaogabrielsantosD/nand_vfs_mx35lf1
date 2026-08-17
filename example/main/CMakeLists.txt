set(src "")
list(APPEND src "nand_driver_test.c")

idf_component_register(
    SRCS
        ${src}
    INCLUDE_DIRS "."
    REQUIRES
        mx35lf1ge4ab
)
