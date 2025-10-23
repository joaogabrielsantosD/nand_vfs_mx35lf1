cmake_minimum_required(VERSION 3.10)

list(APPEND includedirs include)
list(APPEND srcs src/MX35LF1.c)
list(APPEND requires driver)
list(APPEND priv_requires spi_flash esp_timer)

idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${includedirs} REQUIRES ${requires} PRIV_REQUIRES ${priv_requires})

if(NOT CONFIG_FREERTOS_HZ EQUAL 1000)
    message(FATAL_ERROR "MX35LF1 library requires CONFIG_FREERTOS_HZ=1000"
                        "(currently ${CONFIG_FREERTOS_HZ})")
endif()