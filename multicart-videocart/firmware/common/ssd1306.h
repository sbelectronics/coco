#ifndef MULTICART_SSD1306_H
#define MULTICART_SSD1306_H

// GME12864-51 (SSD1306 128x64, I2C 0x3C) — framebuffer with per-page
// dirty tracking. The bus is 100 kHz (PCF8574-limited), so a full frame
// costs ~100 ms; everything here is built to push only changed pages,
// one page (~13 ms) per flush step.

#include <stdint.h>
#include <stdbool.h>

#define OLED_W        128
#define OLED_PAGES    8
#define OLED_COLS     21          // 128 / 6 px per glyph

bool oled_init(void);
bool oled_present(void);

void oled_clear(void);
void oled_clear_page(int page);
// Fill the whole page with a highlight bar behind the text.
void oled_text_bar(int page, const char *s, bool invert);

// Push at most one dirty page. Returns true if more remain.
bool oled_flush_step(void);

#endif
