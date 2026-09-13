#ifndef MULTICART_CONFIG_H
#define MULTICART_CONFIG_H

// VIDEOCART board configuration.
//
// Same macro names as multicart/config.h — common/ includes "config.h"
// and gets this one because the board directory is first on the include
// path (see common/common.cmake). Pin map is LOCKED by the board layout.

// OPTIONAL: see the note in multicart/config.h. Absent in a fresh clone.
#if defined(__has_include)
#  if __has_include("config_local.h")
#    include "config_local.h"
#  endif
#else
#  include "config_local.h"
#endif

#define MC_HOSTNAME             "coco-videocart"
#define MC_BOARD_TITLE          "CoCo Videocart"   // OLED/web/CDC branding
#define MC_PROMPT               "videocart> "
#define MC_FW_VERSION           "0.58.0-vc"

// ----------------------------------------------------------------------
// Clocks
//
// Core 1's snoop loop has to finish inside the CoCo's 1117 ns bus cycle.
// At 125 MHz its worst body measured 1336 ns, so it sometimes overran and
// lost the lap -- and with it that cycle's screen byte, register write or
// DAC edge. At 250 MHz the same work takes about 668 ns.
//
// Nothing timing-critical is allowed to move with it. clk_hstx is pinned
// so the pixel clock stays 25 MHz, and the PIO sweep state machines are
// pinned to MC_PIO_REF_KHZ so every hardware-proven sample position sits
// exactly where it did -- which matters because the sampler's delay slots
// are only 3 bits wide (it side-sets 2 of the 5) and SAMP_SETUP0 is
// already 7. They could not have been scaled even if we wanted to.
#define MC_SYS_CLK_KHZ          250000u
#define MC_HSTX_CLK_KHZ         125000u   // /5 = the 25 MHz pixel clock
#define MC_PIO_REF_KHZ          125000u   // sweep timing is expressed here

_Static_assert(MC_SYS_CLK_KHZ % MC_HSTX_CLK_KHZ == 0 &&
               MC_SYS_CLK_KHZ / MC_HSTX_CLK_KHZ <= 3,
               "clk_hstx divider is a 2-bit integer field: 1, 2 or 3 only");
_Static_assert(MC_SYS_CLK_KHZ % MC_PIO_REF_KHZ == 0,
               "PIO divider must be integral or the sweep samples jitter");

// ----------------------------------------------------------------------
// Pin map
//
// Unlike the multicart, the address bus is NOT directly visible: A0-A15,
// R/W, CTS* and SCS* arrive through three 74HCT153 muxes swept in four
// phases. Only the data bus and E are direct.
//
// 5V-tolerance rule (RP2350 digital pads GPIO0-25 only): every 5V mux
// output lands on GP9-GP22; GP26/27/28 are ADC pads and carry only
// 3.3V Pico-driven selects and I2C.
// ----------------------------------------------------------------------
#define MC_PIN_D0               0       // ..GP7 = D7 (direct, bidir)
#define MC_DATA_MASK            (0xFFu << MC_PIN_D0)

#define MC_PIN_E                8       // direct: cycle framing

#define MC_PIN_M0               9       // '153 outputs
#define MC_PIN_M1               10
#define MC_PIN_M2               11
#define MC_PIN_M4               20
#define MC_PIN_M3               22

#define MC_PIN_HSTX_BASE        12      // GP12..GP19 (fixed by hardware)

#define MC_PIN_SEL0             26      // mux selects: PIO side-set pair,
#define MC_PIN_SEL1             27      // consecutive by design
#define MC_PIN_SCL              21      // I2C0
#define MC_PIN_SDA              28

// Mux span read by the sweep SM in one IN: GP9..GP22 (14 bits). Bits for
// GP12..GP19 (HSTX) and GP21 (SCL) fall inside the span and are masked.
#define MC_MUX_IN_BASE          MC_PIN_M0
#define MC_MUX_IN_BITS          14
#define MC_MUXBIT_M0            (MC_PIN_M0 - MC_MUX_IN_BASE)   // 0
#define MC_MUXBIT_M1            (MC_PIN_M1 - MC_MUX_IN_BASE)   // 1
#define MC_MUXBIT_M2            (MC_PIN_M2 - MC_MUX_IN_BASE)   // 2
#define MC_MUXBIT_M4            (MC_PIN_M4 - MC_MUX_IN_BASE)   // 11
#define MC_MUXBIT_M3            (MC_PIN_M3 - MC_MUX_IN_BASE)   // 13

// Mux phase table (board mux arrangement):
//   phase | M0  M1  M2  M3   M4
//     0   | A0  A4  A8  A12  R/W
//     1   | A1  A5  A9  A13  CTS*
//     2   | A2  A6  A10 A14  SCS*
//     3   | A3  A7  A11 A15  CTS* (dup)
#define MC_PHASES               4

// I2C — i2c0 here (multicart uses i2c1); same 100 kHz, same addresses.
#define MC_I2C                  i2c0
#define MC_I2C_BAUD             100000
#define MC_I2C_ADDR_EXPANDER    0x20
#define MC_I2C_ADDR_OLED        0x3C

// PCF8574 bit map — identical to the multicart.
#define MC_XP_ENC_A             (1u << 0)
#define MC_XP_ENC_B             (1u << 1)
#define MC_XP_ENC_SW            (1u << 2)
#define MC_XP_SPARE_P3          (1u << 3)
#define MC_XP_SPARE_P4          (1u << 4)
#define MC_XP_RESET_EN          (1u << 5)
#define MC_XP_CART_EN           (1u << 6)
#define MC_XP_SPARE_P7          (1u << 7)
#define MC_XP_IDLE              0xFFu

// ----------------------------------------------------------------------
// Bus service
// ----------------------------------------------------------------------
// 14-bit CTS window into the served image (as multicart).
#define MC_ADDR_MASK            0x3FFFu
// Full 16-bit address, reassembled from the sweep, for the mirror and
// for register decoding.
#define MC_FULL_ADDR_MASK       0xFFFFu

// Write-capture sample delay after E RISING, in nanoseconds. 6809 write
// data is valid from ~225 ns after E rises. Scope-tuned at bring-up via
// the cfg field (same knob and provisioning path as the multicart's
// strobe delay; only the trigger differs — E here, the '125 strobe
// there).
#define MC_STROBE_DELAY_NS_DEFAULT  400
// Accepted range for the runtime-tunable delay; shared by the OLED menu,
// the web API and the USB console so all three agree.
#define MC_STROBE_DELAY_NS_MIN      100
#define MC_STROBE_DELAY_NS_MAX      900

// 96 KB = THREE 32 KB serve-table slots: 2 image banks + 1 permanent stub
// slot (videocart/bus.c). The multicart keeps 128 KB -- its copy of this
// constant is separate and it has no serve tables at all.
#define MC_IMAGE_BUF_BYTES      (96 * 1024)
#define MC_BANK_BYTES           (16 * 1024)

// 64K shadow of CoCo RAM, filled from snooped writes. The SAM can point
// the 6K display window at any 512-byte boundary, so the whole map is
// mirrored rather than chasing the window.
#define MC_MIRROR_BYTES         (64 * 1024)

// Registers worth forwarding to core 0 (video/audio state).
#define MC_REG_PIA0_CRA         0xFF01u   // analog mux select bit
#define MC_REG_PIA0_CRB         0xFF03u   // analog mux select bit
#define MC_REG_PIA1_DA          0xFF20u   // 6-bit DAC in bits 7..2
#define MC_REG_PIA1_CRA         0xFF21u
#define MC_REG_PIA1_DB          0xFF22u   // VDG mode bits + 1-bit sound
#define MC_REG_PIA1_CRB         0xFF23u   // sound enable (CB2)
#define MC_REG_SAM_BASE         0xFFC0u   // ..0xFFD3: V0-V2, F0-F6 pairs
#define MC_REG_SAM_END          0xFFD3u
#define MC_REG_BANK_BASE        0xFF40u   // ..0xFF5F: cart bank registers
#define MC_REG_BANK_END         0xFF5Fu

// ----------------------------------------------------------------------
// Flash layout — identical to the multicart (same 4 MB part, same OTA
// slots), so ota.c and cfg.c need no board awareness.
// ----------------------------------------------------------------------
#define MC_FW_SLOT_A            0x000000u
#define MC_FW_SLOT_B            0x0C0000u
#define MC_FW_SLOT_BYTES        0x0C0000u
#define MC_FS_FLASH_OFFSET      0x180000u
#define MC_FS_FLASH_BYTES       (0x3FF000u - MC_FS_FLASH_OFFSET)
#define MC_CFG_FLASH_OFFSET     0x3FF000u

// ----------------------------------------------------------------------
// UI (as multicart)
// ----------------------------------------------------------------------
#define MC_ENC_POLL_HZ          500
#define MC_LONGPRESS_MS         600
#define MC_RESET_PULSE_MS       120
#define MC_STUB_TIMEOUT_MS      1500
// Slot stays empty this long after the cold boot so BASIC reaches READY
// before the cart appears (select.c "insertion-style autostart").
#define MC_BASIC_BOOT_MS        2200

// The stub signals by writing $A5 to $FF5F. This board has no strobe
// pin: the write is decoded from the snooped address, so the full
// 16-bit address is matched (the multicart only sees A0-A13).
#define MC_STUB_MAGIC_ADDR      0xFF5Fu
#define MC_STUB_MAGIC_DATA      0xA5u

#define MC_WEB_PORT             80

#endif
