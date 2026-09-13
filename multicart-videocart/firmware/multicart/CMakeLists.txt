cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w)
set(PICO_PLATFORM rp2350)

include(../pico_sdk_import.cmake)

project(coco_multicart C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

include(../common/common.cmake)

add_executable(coco-multicart
    main.c
    bus.c                       # direct-GPIO bus service (this board)
    ${MC_COMMON_SOURCES}
)

pico_generate_pio_header(coco-multicart ${CMAKE_CURRENT_LIST_DIR}/bus_pio.pio)

# This directory FIRST: common sources include "config.h" and must get
# the multicart pin map (see common/common.cmake).
target_include_directories(coco-multicart PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${MC_COMMON_DIR}
    ${MC_LITTLEFS_DIR}
)

target_compile_definitions(coco-multicart PRIVATE ${MC_COMMON_DEFS})
target_compile_options(coco-multicart PRIVATE ${MC_COMMON_WARN_OPTS})

# The bus hot loop is timing-critical and must not be sacrificed to -Os.
set_source_files_properties(bus.c PROPERTIES COMPILE_OPTIONS "-O3")

target_link_libraries(coco-multicart ${MC_COMMON_LIBS})

pico_enable_stdio_usb(coco-multicart 1)
pico_enable_stdio_uart(coco-multicart 0)

pico_add_extra_outputs(coco-multicart)
