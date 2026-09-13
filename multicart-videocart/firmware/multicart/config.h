#ifndef MULTICART_CONFIG_H
#define MULTICART_CONFIG_H

// OPTIONAL build-time fallback for WiFi credentials. config_local.h is
// git-ignored (it holds a PSK), so it does not exist in a fresh clone —
// hence __has_include rather than a plain include, which would make the
// firmware unbuildable until you created the file. Copy
// common/config_local.h.example over it to use it. The runtime source of
// truth is the persistent flash cfg sector either way.
#if defined(__has_include)
#  if __has_include("config_local.h")
#    include "config_local.h"
#  endif
#else
#  include "config_local.h"
#endif

#define MC_HOSTNAME             "coco-multicart"
#define MC_BOARD_TITLE          "CoCo Multicart"   // OLED/web/CDC branding
#define MC_PROMPT               "multicart> "
#define MC_FW_VERSION           "0.5.1"

// ----------------------------------------------------------------------
// Pin map — LOCKED by the board layout. Do not change without changing
// hardware. A0..A13 and D0..D7 are contiguous by design so the serve
// loop can use single gpio port reads/writes.
// ----------------------------------------------------------------------
#define MC_PIN_A0               0       // ..GP13 = A13
#define MC_ADDR_BITS            14
#define MC_ADDR_MASK            0x3FFFu

#define MC_PIN_D0               14      // ..GP21 = D7
#define MC_DATA_MASK            (0xFFu << MC_PIN_D0)

#define MC_PIN_CTS              22      // !CTS, input (5V via tolerant pad)
#define MC_PIN_SDA              26      // I2C1: OLED + PCF8574 (3.3V only!)
#define MC_PIN_SCL              27
#define MC_PIN_STROBE           28      // SCS-write strobe via D2/'125 (3.3V)

// I2C bus. 100 kHz — the PCF8574 is a 100 kHz-max part.
#define MC_I2C                  i2c1
#define MC_I2C_BAUD             100000
#define MC_I2C_ADDR_EXPANDER    0x20    // PCF8574, A2..A0 = 000
#define MC_I2C_ADDR_OLED        0x3C    // GME12864-51 (SSD1306)

// PCF8574 bit map — LOCKED by the board layout.
#define MC_XP_ENC_A             (1u << 0)   // P0 encoder A     (input)
#define MC_XP_ENC_B             (1u << 1)   // P1 encoder B     (input)
#define MC_XP_ENC_SW            (1u << 2)   // P2 encoder push  (input)
#define MC_XP_SPARE_P3          (1u << 3)
#define MC_XP_SPARE_P4          (1u << 4)
#define MC_XP_RESET_EN          (1u << 5)   // P5 low = assert !RESET ('125 s3)
#define MC_XP_CART_EN           (1u << 6)   // P6 low = autostart gate on (s1)
#define MC_XP_SPARE_P7          (1u << 7)
// Idle/safe shadow: all inputs high, both enables released (high).
#define MC_XP_IDLE              0xFFu

// ----------------------------------------------------------------------
// Bus service
// ----------------------------------------------------------------------
// Bank-write sample delay after the STROBE falling edge, in nanoseconds.
// 600 ns is hardware-validated with no tuning needed. It stays runtime-
// tunable via cfg (SET STROBE) in case a different CoCo or a marginal
// part ever needs it.
#define MC_STROBE_DELAY_NS_DEFAULT  600
// Accepted range for the runtime-tunable delay. One definition shared by
// every interface that can set it (OLED menu, web API, USB console) so a
// value set one way can always be displayed and corrected by another.
#define MC_STROBE_DELAY_NS_MIN      100
#define MC_STROBE_DELAY_NS_MAX      900

// Active-image SRAM buffer. 128 KB covers the write-only banked megacart
// schemes; flat ROMs use the first 16 KB.
#define MC_IMAGE_BUF_BYTES      (128 * 1024)
#define MC_BANK_BYTES           (16 * 1024)

// ----------------------------------------------------------------------
// Flash layout — LittleFS data partition + cfg sector.
// Firmware A/B slots are managed by the linker/OTA, not referenced here.
// ----------------------------------------------------------------------
#define MC_FW_SLOT_A            0x000000u   // running firmware
#define MC_FW_SLOT_B            0x0C0000u   // OTA staging
#define MC_FW_SLOT_BYTES        0x0C0000u   // 768 KB each

#define MC_FS_FLASH_OFFSET      0x180000u
#define MC_FS_FLASH_BYTES       (0x3FF000u - MC_FS_FLASH_OFFSET)
#define MC_CFG_FLASH_OFFSET     0x3FF000u   // last 4 KB sector

// ----------------------------------------------------------------------
// UI
// ----------------------------------------------------------------------
#define MC_ENC_POLL_HZ          500     // ~250 us I2C read @100kHz, 12% bus
#define MC_LONGPRESS_MS         600
#define MC_RESET_PULSE_MS       120     // !RESET assert time on selection
#define MC_STUB_TIMEOUT_MS      1500    // cold-boot stub magic wait
// How long to leave the slot EMPTY after the cold boot starts, so BASIC
// runs its whole init and reaches the READY prompt before the cart
// appears. See select.c "insertion-style autostart".
#define MC_BASIC_BOOT_MS        2200

// Cold-boot stub signal. Detection verifies the strobe's ADDRESS only:
// address lines are valid at any sample delay, so the stub flow works
// before the delay has been scope-tuned. The data byte ($A5) is written
// by the stub but deliberately not checked (see bus.c).
#define MC_STUB_MAGIC_ADDR      0x3F5Fu // A0..A13 view of $FF5F
#define MC_STUB_MAGIC_DATA      0xA5u   // reference only

#define MC_WEB_PORT             80

#endif
