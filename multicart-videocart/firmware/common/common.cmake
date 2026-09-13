# ======================================================================
# common.cmake — sources shared by every Pico-based CoCo cartridge.
#
# A board directory (multicart/, videocart/) supplies exactly four things
# of its own:
#
#   config.h        pin map + board constants
#   bus.c           the bus service implementation
#   bus_pio.pio     its PIO programs
#   main.c          boot order / board-specific init
#
# Everything here is board-agnostic. The seam is common/bus.h: nothing
# above it knows whether addresses arrive on direct GPIOs (multicart) or
# through a '153 mux sweep (videocart).
#
# INCLUDE ORDER MATTERS: the board directory must come before
# ${MC_COMMON_DIR} on the include path so that #include "config.h" from
# a common source resolves to the board's config, not a stale one.
# ======================================================================

set(MC_COMMON_DIR   ${CMAKE_CURRENT_LIST_DIR})
set(MC_LITTLEFS_DIR ${CMAKE_CURRENT_LIST_DIR}/../lib/littlefs)

set(MC_COMMON_SOURCES
    ${MC_COMMON_DIR}/cfg.c
    ${MC_COMMON_DIR}/romfs.c
    ${MC_COMMON_DIR}/select.c
    ${MC_COMMON_DIR}/expander.c
    ${MC_COMMON_DIR}/ssd1306.c
    ${MC_COMMON_DIR}/ui.c
    ${MC_COMMON_DIR}/net.c
    ${MC_COMMON_DIR}/web.c
    ${MC_COMMON_DIR}/ota.c
    ${MC_COMMON_DIR}/cdcmenu.c
    ${MC_LITTLEFS_DIR}/lfs.c
    ${MC_LITTLEFS_DIR}/lfs_util.c
)

set(MC_COMMON_LIBS
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
    hardware_i2c
    hardware_irq
    hardware_flash
    hardware_watchdog
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mdns
)

# LittleFS: no malloc, no assert/debug spew in the shipped build.
set(MC_COMMON_DEFS
    LFS_NO_MALLOC
    LFS_NO_DEBUG
    LFS_NO_WARN
    LFS_NO_ASSERT
)

# -Wno-format-truncation: snprintf-into-a-fixed-buffer is this codebase's
# deliberate string-safety pattern (OLED rows, status lines and the HTTP
# method field are all meant to clip rather than grow). The one place
# where truncation was actually harmful — over-long filenames becoming
# unusable catalog entries — is handled explicitly in romfs_rescan().
set(MC_COMMON_WARN_OPTS -Wall -Wextra -Wno-format-truncation)
