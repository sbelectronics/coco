// ======================================================================
// ui.c — OLED menu + encoder.
//
// I2C is 100 kHz (PCF8574-limited), so:
//   - the encoder is polled at MC_ENC_POLL_HZ (~250 us per read),
//   - the display is repainted a page at a time, one page per pump call,
//     and the cursor moves without repainting the whole list whenever
//     the window doesn't scroll.
// ======================================================================

#include "ui.h"
#include "config.h"
#include "ssd1306.h"
#include "expander.h"
#include "romfs.h"
#include "select.h"
#include "cfg.h"
#if MC_BOARD_VIDEOCART
#include "video.h"      // vdg_set_artifact
#include "audio.h"      // audio_set_gain
#endif

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include <stdio.h>
#include <string.h>

#define LIST_ROWS   7            // pages 1..7; page 0 is the header

// The menu has two virtual items ahead of the ROMs:
//   item 0 = Settings (device WiFi / strobe)
//   item 1 = Extended BASIC (boot the bare machine, no cart)
// ROMs occupy items 2..count+1.
#define SETTINGS_SLOT 0
#define BASIC_SLOT    1
#define FIRST_ROM     2
static int list_count(void) { return romfs_count() + FIRST_ROM; }
static const rom_entry_t *list_entry(int idx) {
    return (idx < FIRST_ROM) ? NULL : romfs_entry(idx - FIRST_ROM);
}

// strobe delay editing bounds (ns), on the OLED
#define STROBE_MIN   MC_STROBE_DELAY_NS_MIN
#define STROBE_MAX   MC_STROBE_DELAY_NS_MAX
#define STROBE_STEP  25u

typedef enum { UI_LIST, UI_SETTINGS, UI_DEVSET, UI_ABOUT } ui_mode_t;

static ui_mode_t s_mode;
static int  s_cursor;            // index into the ROM list
static int  s_top;               // first visible index
// per-ROM settings: 0=autostart 1=insert 2=delete 3=back
#define SET_ITEMS   4
static int  s_set_item;
static bool s_del_armed;         // delete needs a second press
// The ROM the settings screen operates on, BY NAME: list indices are
// unstable under concurrent web-side uploads/deletes/renames (rescan +
// re-sort), and an armed delete must never retarget to whatever ROM
// happens to occupy the old index.
static char s_set_file[ROMFS_NAME_MAX];
static char s_ip[20];
static bool s_have_ip;

// Device-settings screen. Named rather than numbered, because the
// videocart has three display/audio items the multicart does not and
// hand-kept indices would drift apart between the two boards.
typedef enum {
    DEV_WIFI = 0,
    DEV_STROBE,
#if MC_BOARD_VIDEOCART
    DEV_ARTIFACT,        // live
    DEV_HDMIAUD,         // staged: changes the scanout descriptor layout
    DEV_GAIN,            // live
    DEV_FILTER,          // live: the analogue output stage
    DEV_ONEBIT,          // live: single-bit level against the ladder
#endif
    DEV_SAVE,
    DEV_ABOUT,
    DEV_BACK,
    DEV_ITEMS
} dev_item_t;

// Five item rows fit on the panel (page 0 is the title, page 6 the
// "reboot to apply" hint), and the videocart now has ten items, so the
// list scrolls exactly like the ROM list does.
#define DEV_ROWS    5
static int      s_dev_item;
static int      s_dev_top;
static bool     s_strobe_edit;   // rotating the strobe value vs the item list
// Originals captured on entry so "Back (no save)" can drop staged edits
// without persisting them (cfg lives in RAM; another path could save it).
static uint8_t  s_wifi0;
static uint32_t s_strobe0;
#if MC_BOARD_VIDEOCART
static uint8_t  s_artifact0, s_hdmiaud0, s_gain0, s_filt0, s_ob0;
#endif

// encoder state
static const int8_t s_qtab[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0,
};
static uint8_t  s_qstate;
static int8_t   s_qaccum;
static bool     s_btn_down;
static absolute_time_t s_btn_t0;
static bool     s_long_fired;
static absolute_time_t s_next_poll;

static bool s_repaint_all;

void ui_set_ip(const char *ip) {
    snprintf(s_ip, sizeof(s_ip), "%s", ip);
    s_have_ip = (s_ip[0] != 0);      // "" = link lost: stop showing it
    s_repaint_all = true;
}

void ui_notify_list_changed(void) {
    int n = list_count();
    if (s_cursor >= n) s_cursor = n - 1;      // n >= 2 (Settings + BASIC)
    if (s_top > s_cursor) s_top = s_cursor;

    // The list changed under us: an armed delete no longer refers to
    // what the user saw. In per-ROM settings, follow the file to its new
    // index, or bail out if it's gone.
    s_del_armed = false;
    if (s_mode == UI_SETTINGS) {
        int i = romfs_index_of(s_set_file);
        if (i < 0) {
            s_mode = UI_LIST;
        } else {
            s_cursor = i + FIRST_ROM;
            if (s_cursor < s_top)              s_top = s_cursor;
            if (s_cursor >= s_top + LIST_ROWS) s_top = s_cursor - LIST_ROWS + 1;
        }
    }
    s_repaint_all = true;
}

void ui_init(void) {
    oled_init();
    s_mode = UI_LIST;
    s_cursor = BASIC_SLOT;           // land on BASIC, not Settings
    s_top = 0;
    s_repaint_all = true;
    s_next_poll = get_absolute_time();

    // Start the cursor on whatever is already being served (empty active
    // = nothing served = the BASIC slot, which is the default).
    const char *act = select_active();
    if (act && act[0]) {
        int i = romfs_index_of(act);
        if (i >= 0) {
            s_cursor = i + FIRST_ROM;
            if (s_cursor >= LIST_ROWS) s_top = s_cursor - LIST_ROWS + 1;
        }
    }
}

// --------------------------------------------------------------- draw --

static void draw_header(void) {
    char line[OLED_COLS + 1];
    if (select_busy()) {
        snprintf(line, sizeof(line), "%s", select_status());
    } else if (s_mode == UI_ABOUT) {
        snprintf(line, sizeof(line), "%s", "About");
    } else if (s_mode == UI_DEVSET) {
        snprintf(line, sizeof(line), "%s", "Settings");
    } else if (s_mode == UI_SETTINGS) {
        const rom_entry_t *e = romfs_find(s_set_file);
        snprintf(line, sizeof(line), "SET: %s", e ? e->title : "");
    } else if (s_have_ip) {
        snprintf(line, sizeof(line), "%s", s_ip);
    } else {
        snprintf(line, sizeof(line), "%s", MC_BOARD_TITLE);
    }
    oled_text_bar(0, line, false);
}

static void draw_list_row(int row) {
    int idx = s_top + row;
    char line[OLED_COLS + 1];
    bool sel = (idx == s_cursor);

    if (idx == SETTINGS_SLOT) {
        snprintf(line, sizeof(line), " %-*s", OLED_COLS - 1, "Settings");
        oled_text_bar(1 + row, line, sel);
        return;
    }
    if (idx == BASIC_SLOT) {
        // Active mark when nothing is being served (BASIC is what runs).
        bool act = (select_active()[0] == 0);
        snprintf(line, sizeof(line), "%c%-*s", act ? '*' : ' ',
                 OLED_COLS - 1, "Extended BASIC");
        oled_text_bar(1 + row, line, sel);
        return;
    }

    const rom_entry_t *e = list_entry(idx);
    if (!e) {
        oled_clear_page(1 + row);
        return;
    }
    bool act = !strcmp(e->file, select_active());
    snprintf(line, sizeof(line), "%c%-*s", act ? '*' : ' ',
             OLED_COLS - 1, e->title);
    oled_text_bar(1 + row, line, sel);
}

static void draw_settings(void) {
    const rom_entry_t *e = romfs_find(s_set_file);
    char line[OLED_COLS + 1];

    snprintf(line, sizeof(line), "%-*s", OLED_COLS,
             e ? e->file : "");
    oled_text_bar(1, line, false);

    snprintf(line, sizeof(line), "Autostart: %s",
             (e && e->autostart) ? "ON" : "OFF");
    oled_text_bar(2, line, s_set_item == 0);

    // "Insert" = wait for BASIC to finish booting, then start the cart
    // the way pushing one into a running machine does. ~2 s slower;
    // needed by cassette conversions that expect a fully-set-up BASIC.
    snprintf(line, sizeof(line), "Insert st: %s",
             (e && e->insert) ? "ON" : "OFF");
    oled_text_bar(3, line, s_set_item == 1);

    snprintf(line, sizeof(line), "%s",
             s_del_armed ? "Delete? press again" : "Delete ROM");
    oled_text_bar(4, line, s_set_item == 2);

    snprintf(line, sizeof(line), "Back");
    oled_text_bar(5, line, s_set_item == 3);

    snprintf(line, sizeof(line), "%s  %uK",
             (e && e->scheme) ? "banked" : "flat",
             e ? (unsigned)(e->size / 1024) : 0u);
    oled_text_bar(6, line, false);
    oled_clear_page(7);
}

static void draw_about(void) {
    char line[OLED_COLS + 1];
    snprintf(line, sizeof(line), "%s", MC_BOARD_TITLE);
    oled_text_bar(1, line, false);
    snprintf(line, sizeof(line), "v%s", MC_FW_VERSION);
    oled_text_bar(2, line, false);
    oled_text_bar(4, "Dr. Scott M Baker", false);
    oled_text_bar(5, "www.smbaker.com", false);
    oled_text_bar(7, "Press to return", false);
    oled_clear_page(3);
    oled_clear_page(6);
}

// One item's text. Kept separate from the row loop so the scrolling
// window stays trivial and the item list is readable top to bottom.
static void devset_text(dev_item_t it, char *line, size_t n) {
    const cfg_t *c = cfg_get();
    switch (it) {
    case DEV_WIFI:
        snprintf(line, n, "*WiFi:  %s", c->wifi.enabled ? "ON" : "OFF");
        break;
    case DEV_STROBE:
        // A marker makes it obvious when rotation edits the value rather
        // than moving through the list.
        snprintf(line, n, "*Strobe:%c%u ns%c",
                 s_strobe_edit ? '<' : ' ',
                 (unsigned)c->strobe_delay_ns,
                 s_strobe_edit ? '>' : ' ');
        break;
#if MC_BOARD_VIDEOCART
    case DEV_ARTIFACT:
        snprintf(line, n, "Artifact: %s",
                 c->artifact == 0 ? "off" : c->artifact == 1 ? "A" : "B");
        break;
    case DEV_HDMIAUD:
        // Shows the SETTING and, when they disagree, what the scanout is
        // actually doing -- the setting is staged behind a reboot, so the
        // two can legitimately differ, and a mismatch that persists across
        // a reboot means the setting is not reaching hstx_init.
        snprintf(line, n, "*HDMI: %s%s", c->hdmi_audio ? "ON" : "OFF",
                 (c->hdmi_audio != 0) == hstx_hdmi_audio() ? ""
                     : (hstx_hdmi_audio() ? " act:ON" : " act:OFF"));
        break;
    case DEV_GAIN:
        snprintf(line, n, "Volume:  %d", (int)c->audio_gain);
        break;
#endif
#if MC_BOARD_VIDEOCART
    case DEV_FILTER: {
        static const char *const F[4] = { "off", "gentle", "medium", "heavy" };
        snprintf(line, n, "Filter: %s", F[c->audio_filter & 3u]);
        break;
    }
    case DEV_ONEBIT: {
        static const char *const B[4] = { "off", "1/4", "1/2", "full" };
        snprintf(line, n, "1-bit lvl: %s", B[c->audio_onebit & 3u]);
        break;
    }
#endif
    case DEV_SAVE:  snprintf(line, n, "Save & Reboot");  break;

    case DEV_ABOUT: snprintf(line, n, "About");          break;
    default:        snprintf(line, n, "Back (no save)"); break;
    }
}

static void draw_devset(void) {
    char line[OLED_COLS + 1];

    // Keep the cursor inside the window.
    if (s_dev_item < s_dev_top)             s_dev_top = s_dev_item;
    if (s_dev_item >= s_dev_top + DEV_ROWS) s_dev_top = s_dev_item - DEV_ROWS + 1;
    if (s_dev_top > DEV_ITEMS - DEV_ROWS)   s_dev_top = DEV_ITEMS - DEV_ROWS;
    if (s_dev_top < 0)                      s_dev_top = 0;

    for (int r = 0; r < DEV_ROWS; r++) {
        int it = s_dev_top + r;
        if (it >= DEV_ITEMS) { oled_clear_page(1 + r); continue; }
        devset_text((dev_item_t)it, line, sizeof(line));
        oled_text_bar(1 + r, line, it == s_dev_item);
    }

    // Only SOME of these are staged, so a blanket "reboot to apply" would
    // be wrong -- artifact, volume, filter and 1-bit level take effect as
    // the knob turns. The staged items carry a '*' marker instead of
    // being named here, because a legend that names them does not fit a
    // 21-column panel and a truncated legend is worse than none.
    _Static_assert(sizeof("* = needs reboot") - 1 <= OLED_COLS,
                   "device-settings legend must fit the panel");
    oled_text_bar(6, "* = needs reboot", false);
    oled_clear_page(7);
}

static void repaint(void) {
    draw_header();
    if (s_mode == UI_SETTINGS) { draw_settings(); return; }
    if (s_mode == UI_ABOUT)    { draw_about();    return; }
    if (s_mode == UI_DEVSET)   { draw_devset();   return; }
    for (int r = 0; r < LIST_ROWS; r++) draw_list_row(r);
    // With no ROMs, only Settings + BASIC show; hint how to add ROMs so a
    // near-empty screen doesn't read as "broken".
    if (romfs_count() == 0)
        oled_text_bar(3, s_have_ip ? "Upload via web UI"
                                   : "Set WiFi via USB", false);
}

// -------------------------------------------------------------- input --

static void cursor_move(int delta) {
    int n = list_count();            // always >= 2 (Settings + BASIC)

    int prev = s_cursor;
    s_cursor += delta;
    if (s_cursor < 0)  s_cursor = 0;
    if (s_cursor >= n) s_cursor = n - 1;
    if (s_cursor == prev) return;

    int old_top = s_top;
    if (s_cursor < s_top)                 s_top = s_cursor;
    if (s_cursor >= s_top + LIST_ROWS)    s_top = s_cursor - LIST_ROWS + 1;

    if (s_top != old_top) {
        s_repaint_all = true;             // window scrolled: full list
    } else {
        // Cheap path: only the two affected rows.
        draw_list_row(prev - s_top);
        draw_list_row(s_cursor - s_top);
    }
}

// One detent of rotation, +1 (CW) or -1 (CCW).
static void on_rotate(int dir) {
    if (s_mode == UI_LIST) {
        cursor_move(dir);
        return;
    }
    if (s_mode == UI_SETTINGS) {
        s_set_item = (s_set_item + (dir > 0 ? 1 : SET_ITEMS - 1))
                     % SET_ITEMS;
        s_del_armed = false;
        s_repaint_all = true;
        return;
    }
    if (s_mode == UI_ABOUT) return;    // nothing to scroll through
    // UI_DEVSET
    if (s_strobe_edit) {
        cfg_t *c = cfg_get();
        long v = (long)c->strobe_delay_ns + dir * (long)STROBE_STEP;
        if (v < (long)STROBE_MIN) v = STROBE_MIN;
        if (v > (long)STROBE_MAX) v = STROBE_MAX;
        c->strobe_delay_ns = (uint32_t)v;
    } else {
        s_dev_item = (s_dev_item + (dir > 0 ? 1 : DEV_ITEMS - 1))
                     % DEV_ITEMS;
    }
    s_repaint_all = true;
}

static void do_short_press(void) {
    if (s_mode == UI_LIST) {
        if (s_cursor == SETTINGS_SLOT) {
            const cfg_t *c = cfg_get();
            s_wifi0   = c->wifi.enabled;      // snapshot for "Back (no save)"
            s_strobe0 = c->strobe_delay_ns;
#if MC_BOARD_VIDEOCART
            s_artifact0 = c->artifact;
            s_hdmiaud0  = c->hdmi_audio;
            s_gain0     = c->audio_gain;
            s_filt0     = c->audio_filter;
            s_ob0       = c->audio_onebit;
#endif
            s_mode = UI_DEVSET;
            s_dev_item = 0;
            s_dev_top  = 0;
            s_strobe_edit = false;
        } else if (s_cursor == BASIC_SLOT) {
            select_basic();
        } else {
            const rom_entry_t *e = list_entry(s_cursor);
            if (e) select_rom(e->file);
        }
        s_repaint_all = true;
        return;
    }

    if (s_mode == UI_ABOUT) {          // any press returns to Settings
        s_mode = UI_DEVSET;            // staged edits stay staged
        s_repaint_all = true;
        return;
    }

    if (s_mode == UI_DEVSET) {
        cfg_t *c = cfg_get();
        switch ((dev_item_t)s_dev_item) {
        case DEV_WIFI:   // staged — applies on reboot
            c->wifi.enabled = c->wifi.enabled ? 0 : 1;
            break;
        case DEV_STROBE: // toggle value-edit mode (it has a wide range)
            s_strobe_edit = !s_strobe_edit;
            break;
#if MC_BOARD_VIDEOCART
        // These have two or three values, so a press just cycles them --
        // no edit mode to enter and leave. Artifact and volume are judged
        // by eye and ear, so they apply LIVE; waiting for a reboot to see
        // whether a phase is right would make them useless.
        case DEV_ARTIFACT:
            c->artifact = (uint8_t)((c->artifact + 1u) % 3u);
            vdg_set_artifact(c->artifact);
            break;
        case DEV_HDMIAUD:
            c->hdmi_audio = c->hdmi_audio ? 0 : 1;   // staged
            break;
        case DEV_GAIN:
            c->audio_gain = (uint8_t)((c->audio_gain + 1u) % 4u);
            audio_set_gain(c->audio_gain);
            break;
        case DEV_FILTER:
            c->audio_filter = (uint8_t)((c->audio_filter + 1u) % 4u);
            audio_set_filter(c->audio_filter);
            break;
        case DEV_ONEBIT:
            c->audio_onebit = (uint8_t)((c->audio_onebit + 1u) % 4u);
            audio_set_onebit(c->audio_onebit);
            break;
#endif
        case DEV_SAVE:
            oled_text_bar(0, "Saving + reboot", false);
            for (int i = 0; i < 16 && oled_flush_step(); i++) { }
            cfg_save();
            watchdog_reboot(0, 0, 50);
            for (;;) tight_loop_contents();
        case DEV_ABOUT:
            s_mode = UI_ABOUT;
            break;
        case DEV_BACK:   // restore snapshot, drop staged edits
        default:         // any future item defaults to the harmless action
            c->wifi.enabled     = s_wifi0;
            c->strobe_delay_ns  = s_strobe0;
#if MC_BOARD_VIDEOCART
            // The live ones were applied as they were turned, so undoing
            // them means putting the hardware back too, not just the cfg.
            c->artifact   = s_artifact0;
            c->hdmi_audio = s_hdmiaud0;
            c->audio_gain   = s_gain0;
            c->audio_filter = s_filt0;
            c->audio_onebit = s_ob0;
            // Both are LIVE, so restoring cfg alone would leave the old
            // value applied and the menu disagreeing with the sound.
            audio_set_filter(c->audio_filter);
            audio_set_onebit(c->audio_onebit);
            vdg_set_artifact(c->artifact);
            audio_set_gain(c->audio_gain);
#endif
            s_mode = UI_LIST;
            s_strobe_edit = false;
            break;
        }
        s_repaint_all = true;
        return;
    }

    // per-ROM settings — resolve by NAME every press (see s_set_file)
    const rom_entry_t *e = romfs_find(s_set_file);
    if (!e) { s_mode = UI_LIST; s_repaint_all = true; return; }

    switch (s_set_item) {
    case 0:
        romfs_set_meta(e->file, NULL, e->autostart ? 0 : 1, -1, -1);
        break;
    case 1:
        romfs_set_meta(e->file, NULL, -1, -1, e->insert ? 0 : 1);
        break;
    case 2:
        // Deletion is the one destructive act on this device; a single
        // encoder click is too easy to land by accident, so the first
        // press arms and the second one deletes.
        if (!s_del_armed) {
            s_del_armed = true;
        } else {
            s_del_armed = false;
            romfs_delete(e->file);
            ui_notify_list_changed();
            s_mode = UI_LIST;
        }
        break;
    case 3:  // Back
    default: // any future item defaults to the harmless action
        s_mode = UI_LIST;
        break;
    }
    s_repaint_all = true;
}

static void do_long_press(void) {
    if (s_mode == UI_LIST) {
        // Only ROMs have per-ROM settings; the virtual slots don't.
        const rom_entry_t *e = list_entry(s_cursor);
        if (e) {
            s_mode = UI_SETTINGS;
            s_set_item = 0;
            snprintf(s_set_file, sizeof(s_set_file), "%s", e->file);
        }
    } else if (s_mode == UI_DEVSET || s_mode == UI_ABOUT) {
        // Long-press bails out without saving. UI_ABOUT is only reachable
        // FROM the settings screen, so it must drop the staged edits too:
        // cfg lives in RAM and the next ROM selection calls cfg_save(),
        // which would otherwise persist values the user walked away from.
        cfg_t *c = cfg_get();
        c->wifi.enabled    = s_wifi0;
        c->strobe_delay_ns = s_strobe0;
        s_mode = UI_LIST;
        s_strobe_edit = false;
    } else {
        s_mode = UI_LIST;
    }
    s_del_armed = false;
    s_repaint_all = true;
}

static void poll_encoder(void) {
    uint8_t p = expander_read();
    uint8_t a = (p & MC_XP_ENC_A) ? 1 : 0;
    uint8_t b = (p & MC_XP_ENC_B) ? 1 : 0;
    bool    sw_down = !(p & MC_XP_ENC_SW);        // switch closes to GND

    // ---- rotation ----
    // A/B are swapped relative to the decode table on the actual boards
    // (the encoder reads backwards otherwise), so B is the high phase
    // bit. Flipping this reverses the scroll direction.
    s_qstate = (uint8_t)(((s_qstate << 2) | (b << 1) | a) & 0x0F);
    s_qaccum += s_qtab[s_qstate];
    while (s_qaccum >= 4)  { s_qaccum -= 4; on_rotate(+1); }
    while (s_qaccum <= -4) { s_qaccum += 4; on_rotate(-1); }

    // ---- button ----
    if (sw_down && !s_btn_down) {
        s_btn_down = true;
        s_long_fired = false;
        s_btn_t0 = get_absolute_time();
    } else if (sw_down && s_btn_down && !s_long_fired) {
        if (absolute_time_diff_us(s_btn_t0, get_absolute_time())
                >= MC_LONGPRESS_MS * 1000) {
            s_long_fired = true;
            do_long_press();
        }
    } else if (!sw_down && s_btn_down) {
        s_btn_down = false;
        if (!s_long_fired) do_short_press();
    }
}

// --------------------------------------------------------------- pump --

void ui_pump(void) {
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(now, s_next_poll) <= 0) {
        s_next_poll = delayed_by_us(now, 1000000 / MC_ENC_POLL_HZ);
        poll_encoder();

        // Header carries live status (selection progress), so refresh it
        // whenever the sequencer is doing something.
        static select_state_t last_state = SEL_IDLE;
        select_state_t st = select_state();
        if (st != last_state) { last_state = st; s_repaint_all = true; }
    }

    if (s_repaint_all) {
        s_repaint_all = false;
        repaint();
    }

    // One page per call keeps the loop responsive (~13 ms of I2C each).
    oled_flush_step();
}
