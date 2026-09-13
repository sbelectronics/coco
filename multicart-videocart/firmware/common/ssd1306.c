#include "ssd1306.h"
#include "config.h"
#include "font5x7.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include <string.h>
#include <stdio.h>

static uint8_t s_fb[OLED_PAGES][OLED_W];
static uint8_t s_dirty;                 // bit per page
static bool    s_present;

static bool cmd(uint8_t c) {
    uint8_t buf[2] = { 0x00, c };
    return i2c_write_timeout_us(MC_I2C, MC_I2C_ADDR_OLED, buf, 2,
                                false, 4000) == 2;
}

bool oled_init(void) {
    // I2C itself is brought up by expander_init() (shared bus).
    static const uint8_t seq[] = {
        0xAE,                   // display off
        0xD5, 0x80,             // clock div
        0xA8, 0x3F,             // multiplex = 64
        0xD3, 0x00,             // display offset
        0x40,                   // start line 0
        0x8D, 0x14,             // charge pump on
        0x20, 0x02,             // PAGE addressing — flush_step targets
                                // pages with 0xB0|n, which horizontal
                                // mode would silently ignore
        0xA1,                   // segment remap (col 127 -> SEG0)
        0xC8,                   // COM scan descending
        0xDA, 0x12,             // COM pins
        0x81, 0x9F,             // contrast
        0xD9, 0xF1,             // pre-charge
        0xDB, 0x40,             // VCOMH
        0xA4,                   // display follows RAM
        0xA6,                   // normal (not inverted)
        0xAF,                   // display on
    };
    s_present = true;
    for (unsigned i = 0; i < sizeof(seq); i++) {
        if (!cmd(seq[i])) { s_present = false; break; }
    }
    if (!s_present) {
        printf("oled: SSD1306 not responding at 0x%02x\n", MC_I2C_ADDR_OLED);
        return false;
    }
    oled_clear();
    return true;
}

bool oled_present(void) { return s_present; }

void oled_clear(void) {
    memset(s_fb, 0, sizeof(s_fb));
    s_dirty = 0xFF;
}

void oled_clear_page(int page) {
    if (page < 0 || page >= OLED_PAGES) return;
    memset(s_fb[page], 0, OLED_W);
    s_dirty |= (uint8_t)(1u << page);
}

void oled_text_bar(int page, const char *s, bool invert) {
    if (page < 0 || page >= OLED_PAGES) return;
    memset(s_fb[page], invert ? 0xFF : 0x00, OLED_W);
    uint8_t *row = s_fb[page];
    int x = 1;
    for (; *s && x < OLED_W; s++) {
        unsigned ch = (unsigned char)*s;
        if (ch < 0x20 || ch > 0x7E) ch = '?';
        const uint8_t *g = font5x7[ch - 0x20];
        for (int c = 0; c < 6 && x < OLED_W; c++, x++) {
            uint8_t col = (c < 5) ? g[c] : 0x00;
            row[x] = invert ? (uint8_t)~col : col;
        }
    }
    s_dirty |= (uint8_t)(1u << page);
}


bool oled_flush_step(void) {
    if (!s_present || !s_dirty) return false;

    int page = 0;
    while (page < OLED_PAGES && !(s_dirty & (1u << page))) page++;
    if (page >= OLED_PAGES) { s_dirty = 0; return false; }

    // Page addressing keeps this to three short commands + one burst.
    if (!cmd((uint8_t)(0xB0 | page)) || !cmd(0x00) || !cmd(0x10)) {
        s_present = false;
        return false;
    }

    uint8_t buf[1 + OLED_W];
    buf[0] = 0x40;                       // Co=0, D/C=1: data stream
    memcpy(&buf[1], s_fb[page], OLED_W);
    int n = i2c_write_timeout_us(MC_I2C, MC_I2C_ADDR_OLED, buf, sizeof(buf),
                                 false, 30000);
    if (n != (int)sizeof(buf)) { s_present = false; return false; }

    s_dirty &= (uint8_t)~(1u << page);
    return s_dirty != 0;
}
