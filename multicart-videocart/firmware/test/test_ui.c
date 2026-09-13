// ======================================================================
// test_ui.c — the real OLED menu (ui.c) + real SSD1306 driver
// (ssd1306.c) running against a wire-level model of the display and a
// scripted encoder on the fake expander.
//
// The SSD1306 model interprets the actual I2C byte stream (page
// addressing, column pointers, data bursts), so a protocol bug in the
// driver — not just a logic bug in the menu — fails the test. Screens
// are dumped as ASCII art for the report.
//
// Covers: initial paint, cursor movement via full quadrature detent
// cycles (both directions), scrolling window, short-press select,
// long-press settings entry, autostart toggle, the two-press delete
// confirmation (including disarm-on-rotate), list-changed retargeting,
// and the empty-list hint.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "config.h"
#include "romfs.h"
#include "select.h"
#include "ssd1306.h"

uint64_t shim_now_us;

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
} while (0)

// ---- SSD1306 device model (wire level) -------------------------------
static uint8_t dev_ram[8][128];
static int dev_page, dev_col;
static int dev_errors;

static void dev_cmd(uint8_t c) {
    if ((c & 0xF8) == 0xB0)      dev_page = c & 7;
    else if ((c & 0xF0) == 0x00) dev_col = (dev_col & 0xF0) | (c & 0x0F);
    else if ((c & 0xF0) == 0x10) dev_col = (dev_col & 0x0F) | ((c & 0x0F) << 4);
    // all other commands (init sequence) accepted silently
}

// ---- fake PCF8574 (encoder) ------------------------------------------
static uint8_t xp_pins = 0xFF;      // open-drain idle: all high

int shim_i2c_write(uint8_t addr, const uint8_t *src, size_t len) {
    if (addr == MC_I2C_ADDR_OLED) {
        if (len < 1) { dev_errors++; return -1; }
        if (src[0] == 0x00) {
            for (size_t i = 1; i < len; i++) dev_cmd(src[i]);
        } else if (src[0] == 0x40) {
            for (size_t i = 1; i < len; i++) {
                if (dev_col > 127) { dev_errors++; break; }
                dev_ram[dev_page][dev_col++] = src[i];
            }
        } else { dev_errors++; return -1; }
        return (int)len;
    }
    if (addr == MC_I2C_ADDR_EXPANDER) return (int)len;   // gate writes
    dev_errors++;
    return -1;
}
int shim_i2c_read(uint8_t addr, uint8_t *dst, size_t len) {
    if (addr == MC_I2C_ADDR_EXPANDER && len == 1) { dst[0] = xp_pins; return 1; }
    dev_errors++;
    return -1;
}

// expander.c is real enough to skip: ui.c only needs expander_read().
void expander_init(void) {}
bool expander_ok(void) { return true; }
uint8_t expander_read(void) { return xp_pins; }
void expander_assert(uint8_t b) { (void)b; }
void expander_release(uint8_t b) { (void)b; }

// ---- fake cfg (device settings screen) -------------------------------
#include "cfg.h"
static cfg_t s_cfg;
cfg_t *cfg_get(void)  { return &s_cfg; }
bool   cfg_save(void) { return true; }

// ---- fake romfs ------------------------------------------------------
static rom_entry_t roms[10];
static int nroms;
static char deleted[ROMFS_NAME_MAX];
static char toggled[ROMFS_NAME_MAX];

int romfs_count(void) { return nroms; }
const rom_entry_t *romfs_entry(int i) {
    return (i >= 0 && i < nroms) ? &roms[i] : NULL;
}
int romfs_index_of(const char *f) {
    for (int i = 0; i < nroms; i++)
        if (!strcmp(roms[i].file, f)) return i;
    return -1;
}
const rom_entry_t *romfs_find(const char *f) {
    int i = romfs_index_of(f);
    return i < 0 ? NULL : &roms[i];
}
bool romfs_set_meta(const char *f, const char *t, int a, int s, int ins) {
    (void)t; (void)s;
    int i = romfs_index_of(f);
    if (i < 0) return false;
    if (a >= 0)   roms[i].autostart = (uint8_t)a;
    if (ins >= 0) roms[i].insert    = (uint8_t)ins;
    snprintf(toggled, sizeof(toggled), "%s", f);
    return true;
}
bool romfs_delete(const char *f) {
    int i = romfs_index_of(f);
    if (i < 0) return false;
    snprintf(deleted, sizeof(deleted), "%s", f);
    memmove(&roms[i], &roms[i + 1], (size_t)(nroms - i - 1) * sizeof(roms[0]));
    nroms--;
    return true;
}

// ---- fake select -----------------------------------------------------
static char sel_file[ROMFS_NAME_MAX];
static char sel_active[ROMFS_NAME_MAX];
bool select_rom(const char *f) {
    snprintf(sel_file, sizeof(sel_file), "%s", f);
    snprintf(sel_active, sizeof(sel_active), "%s", f);
    return true;
}
static bool basic_selected;
bool select_basic(void) {
    basic_selected = true;
    sel_active[0] = 0;               // BASIC = nothing served
    return true;
}
bool select_busy(void) { return false; }
select_state_t select_state(void) { return SEL_IDLE; }
const char *select_status(void) { return "Ready"; }
const char *select_active(void) { return sel_active; }

#include "ui.c"             // code under test (statics visible)

// ---- drivers ---------------------------------------------------------

// One encoder-poll interval + a display flush slot.
static void tick(void) { shim_advance_us(2100); ui_pump(); }
static void settle(void) { for (int i = 0; i < 20; i++) tick(); }

// One full detent: 4 quadrature transitions. The EC12 idles with both
// lines pulled HIGH (11); a detent walks the Gray cycle and comes back
// to 11. fwd=1 one direction, fwd=0 the other.
static void detent(int fwd) {
    static const uint8_t seq_f[4] = { 0x02, 0x00, 0x01, 0x03 };
    static const uint8_t seq_r[4] = { 0x01, 0x00, 0x02, 0x03 };
    const uint8_t *s = fwd ? seq_f : seq_r;
    for (int i = 0; i < 4; i++) {
        xp_pins = (uint8_t)((xp_pins & ~0x03) | s[i]);
        tick();
    }
}

static void press_ms(uint32_t ms) {
    xp_pins &= (uint8_t)~MC_XP_ENC_SW;
    for (uint32_t t = 0; t < ms; t += 2) tick();
    xp_pins |= MC_XP_ENC_SW;
    tick();
}

// ---- screen inspection ----------------------------------------------
static int page_lit(int p) {
    int n = 0;
    for (int c = 0; c < 128; c++)
        for (int b = 0; b < 8; b++) n += (dev_ram[p][c] >> b) & 1;
    return n;
}
// The inverted (selected) bar is by far the most-lit page.
static int selected_page(void) {
    int best = -1, bestn = 0;
    for (int p = 1; p < 8; p++) {
        int n = page_lit(p);
        if (n > bestn) { bestn = n; best = p; }
    }
    return (bestn > 500) ? best : -1;   // an inverted bar is ~900+ lit
}

static void ascii_dump(const char *name) {
    char path[64];
    snprintf(path, sizeof(path), "ui_%s.txt", name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 128; x++)
            fputc((dev_ram[y / 8][x] >> (y % 8)) & 1 ? '#' : '.', f);
        fputc('\n', f);
    }
    fclose(f);
}

int main(void) {
    // Ten ROMs so the 7-row window must scroll.
    nroms = 10;
    for (int i = 0; i < nroms; i++) {
        snprintf(roms[i].file, sizeof(roms[i].file), "game%02d.rom", i);
        snprintf(roms[i].title, sizeof(roms[i].title), "GAME %02d", i);
        roms[i].autostart = 1;
        roms[i].size = 8192;
    }

    ui_init();
    settle();
    CHECK(dev_errors == 0, "SSD1306 protocol errors: %d", dev_errors);
    CHECK(page_lit(0) > 0, "header empty");
    // Item 0 is Settings, item 1 is "Extended BASIC"; cursor starts on
    // BASIC (page 2).
    CHECK(selected_page() == 2, "cursor bar not on the BASIC slot (page %d)",
          selected_page());
    ascii_dump("list_top");

    // ---- rotation: establish which detent direction is "down" ----
    // From BASIC (page 2): the down direction reaches game00 (page 3), the
    // up direction reaches Settings (page 1).
    detent(1); settle();
    int down = (selected_page() == 3) ? 1 : 0;
    CHECK(selected_page() == (down ? 3 : 1), "detent(1) moved to an unexpected row");
    detent(0); settle();                          // reverse -> back to BASIC
    CHECK(selected_page() == 2, "didn't return to the BASIC slot (page %d)",
          selected_page());

    // ---- short-press the BASIC slot selects it ----
    press_ms(80); settle();
    CHECK(basic_selected, "BASIC slot short-press didn't select BASIC");
    CHECK(sel_file[0] == 0, "BASIC select must not set a ROM file ('%s')",
          sel_file);

    // ---- long-press on the BASIC slot does nothing (no per-ROM settings) --
    press_ms(MC_LONGPRESS_MS + 100); settle();
    CHECK(s_mode == UI_LIST, "long-press on BASIC slot entered settings");

    // ---- scroll: past the window, pin to last row ----
    for (int i = 0; i < 10; i++) { detent(down); }
    settle();
    CHECK(selected_page() == 7, "cursor should pin to last row while "
          "scrolling (page %d)", selected_page());
    ascii_dump("list_scrolled");

    // back to the top
    for (int i = 0; i < 12; i++) { detent(!down); }
    settle();
    CHECK(selected_page() == 1, "cursor didn't return to top");

    // ---- first ROM (item 2: after Settings + BASIC) short-press selects --
    detent(down); detent(down); settle();       // index 2 = game00
    press_ms(80); settle();
    CHECK(!strcmp(sel_file, "game00.rom"), "selected '%s'", sel_file);

    // ---- long press enters settings on that ROM ----
    press_ms(MC_LONGPRESS_MS + 100); settle();
    CHECK(s_mode == UI_SETTINGS, "long press didn't enter settings");
    CHECK(!strcmp(s_set_file, "game00.rom"), "settings on '%s'", s_set_file);
    ascii_dump("settings");

    // ---- autostart toggle (item 0) ----
    press_ms(80); settle();
    CHECK(!strcmp(toggled, "game00.rom") && roms[0].autostart == 0,
          "autostart toggle failed");

    // ---- insertion-style start toggle (item 1) ----
    detent(down); settle();
    CHECK(s_set_item == 1, "not on insert item (%d)", s_set_item);
    CHECK(roms[0].insert == 0, "insert should default OFF");
    press_ms(80); settle();
    CHECK(roms[0].insert == 1, "insert toggle didn't set the flag");
    press_ms(80); settle();
    CHECK(roms[0].insert == 0, "insert toggle didn't clear the flag");

    // ---- delete: two-press confirmation ----
    detent(down); settle();                     // item 2 = delete
    CHECK(s_set_item == 2, "not on delete item (%d)", s_set_item);
    press_ms(80); settle();                     // arm
    CHECK(deleted[0] == 0, "deleted on FIRST press!");
    CHECK(s_del_armed, "delete not armed");
    ascii_dump("delete_armed");
    detent(down); settle();                     // rotating must disarm
    CHECK(!s_del_armed, "rotation didn't disarm delete");
    detent(!down); settle();                    // back to delete item
    press_ms(80); settle();                     // arm again
    press_ms(80); settle();                     // confirm
    CHECK(!strcmp(deleted, "game00.rom"), "delete confirmed wrong file '%s'",
          deleted);
    CHECK(s_mode == UI_LIST, "not back in list after delete");
    CHECK(nroms == 9, "fake romfs count %d", nroms);

    // ---- list-changed retargeting in settings ----
    // Enter settings on whatever ROM the cursor is on, then delete a
    // DIFFERENT rom behind the UI's back: settings must follow the file.
    press_ms(MC_LONGPRESS_MS + 100); settle();
    CHECK(s_mode == UI_SETTINGS, "enter settings for retarget test");
    char cur[ROMFS_NAME_MAX];
    snprintf(cur, sizeof(cur), "%s", s_set_file);
    const char *other = strcmp(cur, "game09.rom") ? "game09.rom" : "game08.rom";
    detent(down); detent(down); settle();               // item 2 = delete
    press_ms(80); settle();                             // arm delete
    CHECK(s_del_armed, "arm for retarget test");
    romfs_delete(other);                        // web-side delete
    ui_notify_list_changed();
    settle();
    CHECK(!s_del_armed, "list change must disarm delete");
    CHECK(s_mode == UI_SETTINGS && !strcmp(s_set_file, cur),
          "settings lost its file after list change ('%s' != '%s')",
          s_set_file, cur);
    press_ms(MC_LONGPRESS_MS + 100); settle();  // leave settings
    CHECK(s_mode == UI_LIST, "long press didn't exit settings");

    // ---- delete the settings target out from under it ----
    press_ms(MC_LONGPRESS_MS + 100); settle();
    romfs_delete(s_set_file);
    ui_notify_list_changed();
    settle();
    CHECK(s_mode == UI_LIST, "settings survived its file's deletion");

    // ---- empty list: only Settings + BASIC remain ----
    nroms = 0;
    ui_notify_list_changed();
    settle();
    CHECK(selected_page() == 2, "BASIC slot should be selected when empty (page %d)",
          selected_page());
    CHECK(page_lit(3) > 0, "no empty-list hint");
    basic_selected = false;
    press_ms(80); settle();                     // BASIC still selectable
    CHECK(basic_selected, "BASIC slot not selectable on an empty list");
    ascii_dump("empty");

    // ---- device settings screen (Settings slot at index 0) ----
    for (int i = 0; i < 4; i++) detent(!down);  // up to the Settings slot
    settle();
    CHECK(selected_page() == 1, "Settings slot at top (page %d)", selected_page());
    s_cfg.wifi.enabled = 1;
    s_cfg.strobe_delay_ns = 600;
    press_ms(80); settle();                     // enter device settings
    CHECK(s_mode == UI_DEVSET, "short-press on Settings didn't enter dev settings");
    ascii_dump("devset");
    // item 0 = WiFi: toggle it
    press_ms(80); settle();
    CHECK(s_cfg.wifi.enabled == 0, "WiFi toggle failed");
    // item 1 = Strobe: enter value-edit, one step down, exit
    detent(down); settle();
    CHECK(s_dev_item == 1, "not on Strobe item (%d)", s_dev_item);
    press_ms(80); settle();                     // enter strobe edit
    CHECK(s_strobe_edit, "didn't enter strobe edit");
    uint32_t before = s_cfg.strobe_delay_ns;
    detent(!down); settle();                    // one step -> -STROBE_STEP
    CHECK(s_cfg.strobe_delay_ns == before - STROBE_STEP,
          "strobe edit didn't change value (%u)", (unsigned)s_cfg.strobe_delay_ns);
    press_ms(80); settle();                     // exit strobe edit
    CHECK(!s_strobe_edit, "didn't exit strobe edit");
    // navigate item 1 -> 2 (Save&Reboot, do NOT press) -> 3 (About)
    detent(down); detent(down); settle();
    CHECK(s_dev_item == 3, "not on About item (%d)", s_dev_item);
    press_ms(80); settle();
    CHECK(s_mode == UI_ABOUT, "About didn't open (%d)", s_mode);
    // rotation does nothing on About; any press returns to Settings
    detent(down); settle();
    CHECK(s_mode == UI_ABOUT && s_dev_item == 3,
          "rotation shouldn't leave About (mode=%d item=%d)",
          s_mode, s_dev_item);
    press_ms(80); settle();
    CHECK(s_mode == UI_DEVSET, "About didn't return to Settings (%d)", s_mode);
    // 3 (About) -> 4 (Back), press Back
    detent(down); settle();
    CHECK(s_dev_item == 4, "not on Back item (%d)", s_dev_item);
    press_ms(80); settle();
    CHECK(s_mode == UI_LIST, "Back didn't return to the list");
    // Back (no save) restores the staged values captured on entry
    CHECK(s_cfg.wifi.enabled == 1 && s_cfg.strobe_delay_ns == 600,
          "Back didn't restore staged values (wifi=%u strobe=%u)",
          s_cfg.wifi.enabled, (unsigned)s_cfg.strobe_delay_ns);

    CHECK(dev_errors == 0, "SSD1306 protocol errors: %d", dev_errors);

    if (failures) { printf("test_ui: %d FAILURES\n", failures); return 1; }
    printf("test_ui: OK (Settings+BASIC slots, quadrature both ways, scroll, "
           "select, per-ROM settings, delete confirm+disarm, retarget, empty "
           "hint, device settings: WiFi toggle + strobe edit + Back-restores)\n");
    return 0;
}
