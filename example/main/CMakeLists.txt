set(src "")

if(CONFIG_RUN_NAND_MX35_EXAMPLE)
    list(APPEND src "nand_mx35_example.c")
else()
    list(APPEND src "ecu_fw_manager_example.c")
endif()

idf_component_register(
    SRCS
        ${src}
    INCLUDE_DIRS "."
    REQUIRES
        mx35lf1ge4ab
)
