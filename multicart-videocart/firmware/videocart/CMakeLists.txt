cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w)
set(PICO_PLATFORM rp2350)

include(../pico_sdk_import.cmake)

project(coco_videocart C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

include(../common/common.cmake)

add_executable(coco-videocart
    main.c
    bus.c                       # '153 mux-sweep bus service (this board)
    hstx.c                      # DVI/HDMI scanout
    audio.c                     # CoCo sound -> HDMI audio islands
    vdg.c                       # MC6847 renderer
    ${MC_COMMON_SOURCES}
)

pico_generate_pio_header(coco-videocart ${CMAKE_CURRENT_LIST_DIR}/bus_pio.pio)

# This directory FIRST: common sources include "config.h" and must get
# the videocart pin map (see common/common.cmake).
target_include_directories(coco-videocart PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${MC_COMMON_DIR}
    ${MC_LITTLEFS_DIR}
)

# MC_BOARD_VIDEOCART trims the lwIP pbuf pool (common/lwipopts.h). This
# board spends 96 KB of SRAM on serve tables plus 125 KB on the HDMI
# framebuffer, so it cannot afford the multicart's network buffering.
# Costs upload throughput only; the multicart keeps the larger pool.
target_compile_definitions(coco-videocart PRIVATE ${MC_COMMON_DEFS}
                           MC_BOARD_VIDEOCART=1)
target_compile_options(coco-videocart PRIVATE ${MC_COMMON_WARN_OPTS})

# The bus hot loop is timing-critical and must not be sacrificed to -Os.
set_source_files_properties(bus.c PROPERTIES COMPILE_OPTIONS "-O3")

target_link_libraries(coco-videocart ${MC_COMMON_LIBS} hardware_clocks)

pico_enable_stdio_usb(coco-videocart 1)
pico_enable_stdio_uart(coco-videocart 0)

pico_add_extra_outputs(coco-videocart)
