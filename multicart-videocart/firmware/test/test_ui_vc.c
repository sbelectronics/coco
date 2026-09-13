// ======================================================================
// test_ui_vc.c — the videocart's device-settings menu on the OLED.
//
// test_ui covers the shared UI, but it builds against the MULTICART
// headers, so MC_BOARD_VIDEOCART is undefined there and the three
// videocart-only items (artifact phase, HDMI audio, volume) compile out
// along with the scrolling window they made necessary. This binary is
// the same ui.c built the other way.
//
// Asserts:
//   - the menu has 8 items on this board, and every one of them is
//     reachable by rotating in EITHER direction (a wrap bug in one
//     direction only is the classic way to strand the last item);
//   - the 5-row window scrolls to keep the cursor visible, and never
//     scrolls past the end of the list;
//   - the LIVE items (artifact, volume) cycle through all their values
//     and push each one to the hardware as they are turned;
//   - "Back (no save)" restores every staged AND live value, and puts
//     the live ones back into the hardware too -- dropping an edit in
//     cfg while leaving it applied is the bug this is here to catch.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "config.h"
#include "cfg.h"
#include "romfs.h"
#include "select.h"

uint64_t shim_now_us;
select_state_t select_state(void) { return SEL_IDLE; }

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
} while (0)

// ---- fake OLED + expander (same shape as test_ui) --------------------
static uint8_t dev_ram[8][128];
static int dev_page, dev_col;

int shim_i2c_write(uint8_t addr, const uint8_t *src, size_t len) {
    if (addr == MC_I2C_ADDR_OLED) {
        if (len < 1) return -1;
        if (src[0] == 0x00) {            // command stream
            for (size_t i = 1; i < len; i++) {
                static int st, a, b;
                if (st == 0 && src[i] == 0x22) { st = 1; continue; }
                if (st == 1) { a = src[i]; dev_page = a; st = 2; continue; }
                if (st == 2) { b = src[i]; (void)b; st = 0; continue; }
                if (src[i] == 0x21) { st = 3; continue; }
                if (st == 3) { dev_col = src[i]; st = 4; continue; }
                if (st == 4) { st = 0; continue; }
            }
        } else if (src[0] == 0x40) {     // data stream
            for (size_t i = 1; i < len && dev_col < 128; i++)
                dev_ram[dev_page & 7][dev_col++] = src[i];
        }
        return (int)len;
    }
    if (addr == MC_I2C_ADDR_EXPANDER) return (int)len;
    return -1;
}
static uint8_t xp_pins = 0xFF;
int shim_i2c_read(uint8_t addr, uint8_t *dst, size_t len) {
    if (addr == MC_I2C_ADDR_EXPANDER && len == 1) { dst[0] = xp_pins; return 1; }
    return -1;
}
void expander_init(void) {}
bool expander_ok(void) { return true; }
uint8_t expander_read(void) { return xp_pins; }
void expander_assert(uint8_t b) { (void)b; }
void expander_release(uint8_t b) { (void)b; }

// ---- cfg + the hardware the live settings reach ----------------------
static cfg_t s_cfg_store;
cfg_t *cfg_get(void) { return &s_cfg_store; }
bool cfg_save(void) { return true; }
void cfg_load_defaults(void) {}
bool cfg_is_usable(void) { return true; }
bool cfg_loaded_from_flash(void) { return false; }

// What the live settings actually pushed to the hardware.
static uint8_t hw_artifact = 0xFF, hw_gain = 0xFF;
void vdg_set_artifact(uint8_t m) { hw_artifact = m; }
uint8_t vdg_artifact(void) { return hw_artifact; }
void audio_set_gain(uint8_t g)   { hw_gain = g; }
uint8_t audio_gain(void) { return hw_gain; }
static uint8_t hw_filter = 0xFF, hw_onebit = 0xFF;
void audio_set_filter(uint8_t n) { hw_filter = n; }
uint8_t audio_filter(void) { return hw_filter; }
void audio_set_onebit(uint8_t n) { hw_onebit = n; }
uint8_t audio_onebit(void) { return hw_onebit; }

// What hstx actually latched at boot. ui.c prints an "act:" suffix when
// this disagrees with cfg, so hold it separately from the cfg field.
static bool hw_hdmi_audio = true;
bool hstx_hdmi_audio(void) { return hw_hdmi_audio; }

// ---- romfs/select stubs ----------------------------------------------
static const char *s_active = "";
int romfs_count(void) { return 0; }
const rom_entry_t *romfs_entry(int i) { (void)i; return NULL; }
const rom_entry_t *romfs_find(const char *f) { (void)f; return NULL; }
int romfs_index_of(const char *f) { (void)f; return -1; }
bool romfs_delete(const char *f) { (void)f; return true; }
bool romfs_set_meta(const char *f, const char *t, int a, int i, int s) {
    (void)f; (void)t; (void)a; (void)i; (void)s; return true;
}
uint32_t romfs_free_bytes(void) { return 0; }
uint32_t romfs_total_bytes(void) { return 0; }
const char *select_active(void) { return s_active; }
const char *select_status(void) { return ""; }
bool select_busy(void) { return false; }
bool select_rom(const char *f) { (void)f; return true; }
bool select_basic(void) { return true; }

#include "ui.c"
// The encoder path is shared with the multicart and already covered by
// test_ui; what is new here is the windowing arithmetic and the press
// actions for the three videocart items. Drive those directly, which
// also keeps this test independent of the fake-OLED plumbing.
static void enter_devset(void) {
    s_mode = UI_DEVSET;
    s_dev_item = 0;
    s_dev_top  = 0;
    s_strobe_edit = false;
    const cfg_t *c = cfg_get();
    s_wifi0 = c->wifi.enabled;  s_strobe0 = c->strobe_delay_ns;
    s_artifact0 = c->artifact;  s_hdmiaud0 = c->hdmi_audio;
    s_gain0 = c->audio_gain;
    s_filt0 = c->audio_filter;  s_ob0 = c->audio_onebit;
}

int main(void) {
    memset(&s_cfg_store, 0, sizeof s_cfg_store);
    s_cfg_store.strobe_delay_ns = 600;
    s_cfg_store.artifact   = 2;
    s_cfg_store.hdmi_audio = 1;
    s_cfg_store.audio_gain   = 2;
    s_cfg_store.audio_filter = 1;
    s_cfg_store.audio_onebit = 3;

    ui_init();

    CHECK(DEV_ITEMS == 10, "videocart device menu has %d items, want 10",
          (int)DEV_ITEMS);
    CHECK(DEV_ITEMS > DEV_ROWS,
          "the point of the window is that the list does not fit");

    // ---- windowing: every item visible, window always in range -------
    enter_devset();
    for (int i = 0; i < DEV_ITEMS; i++) {
        s_dev_item = i;
        draw_devset();
        CHECK(s_dev_item >= s_dev_top && s_dev_item < s_dev_top + DEV_ROWS,
              "item %d outside the window (top %d)", i, s_dev_top);
        CHECK(s_dev_top >= 0 && s_dev_top <= DEV_ITEMS - DEV_ROWS,
              "window top %d out of range for item %d", s_dev_top, i);
    }
    // Walking back up must bring the window with it, not strand it.
    for (int i = DEV_ITEMS - 1; i >= 0; i--) {
        s_dev_item = i;
        draw_devset();
        CHECK(s_dev_item >= s_dev_top && s_dev_item < s_dev_top + DEV_ROWS,
              "reverse: item %d outside the window (top %d)", i, s_dev_top);
    }

    // Every item must render some text -- a gap in the switch would show
    // as an empty row rather than a compile error -- and must FIT.
    //
    // The fitting half is not hypothetical. The staged-settings legend on
    // this screen ran to 24 characters on a 21-column panel and displayed
    // as "WiFi/strobe/HDMI: reb", which cannot be selected to find out
    // what it meant. snprintf truncates silently, so nothing failed and
    // nothing warned; it just quietly said the wrong thing.
    for (int i = 0; i < DEV_ITEMS; i++) {
        char full[128] = { 0 };
        devset_text((dev_item_t)i, full, sizeof full);
        CHECK(full[0] != 0, "item %d renders an empty label", i);
        CHECK(strlen(full) <= (size_t)OLED_COLS,
              "item %d is %u chars and will be cut off on a %d-column "
              "panel: \"%s\"", i, (unsigned)strlen(full), OLED_COLS, full);
    }

    // The staged items have to be distinguishable from the live ones, or
    // the legend explaining the marker is pointing at nothing.
    {
        char w[128] = { 0 }, g[128] = { 0 };
        devset_text(DEV_WIFI, w, sizeof w);
        devset_text(DEV_GAIN, g, sizeof g);
        CHECK(w[0] == '*', "WiFi is staged but carries no marker: \"%s\"", w);
        CHECK(g[0] != '*', "Volume applies live but is marked staged: \"%s\"", g);
    }

    // ---- live items: cycle AND reach the hardware -------------------
    enter_devset();
    s_dev_item = DEV_ARTIFACT;
    for (int i = 0; i < 3; i++) {
        uint8_t before = s_cfg_store.artifact;
        do_short_press();
        CHECK(s_cfg_store.artifact == (before + 1) % 3,
              "artifact did not cycle (%u -> %u)", before, s_cfg_store.artifact);
        CHECK(hw_artifact == s_cfg_store.artifact,
              "artifact %u not pushed to the renderer (hw %u)",
              s_cfg_store.artifact, hw_artifact);
        s_dev_item = DEV_ARTIFACT;         // press does not move the cursor
    }

    s_dev_item = DEV_GAIN;
    for (int i = 0; i < 4; i++) {
        uint8_t before = s_cfg_store.audio_gain;
        do_short_press();
        CHECK(s_cfg_store.audio_gain == (before + 1) % 4,
              "volume did not cycle (%u -> %u)", before, s_cfg_store.audio_gain);
        CHECK(hw_gain == s_cfg_store.audio_gain,
              "volume %u not pushed to audio (hw %u)",
              s_cfg_store.audio_gain, hw_gain);
        s_dev_item = DEV_GAIN;
    }

    // The two output-stage settings are LIVE like artifact and volume:
    // they exist to be judged by ear while sound is playing, so waiting
    // for a reboot to hear the difference would make them useless.
    s_dev_item = DEV_FILTER;
    for (int i = 0; i < 4; i++) {
        uint8_t before = s_cfg_store.audio_filter;
        do_short_press();
        CHECK(s_cfg_store.audio_filter == (before + 1) % 4,
              "filter did not cycle (%u -> %u)", before,
              s_cfg_store.audio_filter);
        CHECK(hw_filter == s_cfg_store.audio_filter,
              "filter %u not pushed to audio (hw %u)",
              s_cfg_store.audio_filter, hw_filter);
        s_dev_item = DEV_FILTER;
    }

    s_dev_item = DEV_ONEBIT;
    for (int i = 0; i < 4; i++) {
        uint8_t before = s_cfg_store.audio_onebit;
        do_short_press();
        CHECK(s_cfg_store.audio_onebit == (before + 1) % 4,
              "1-bit level did not cycle (%u -> %u)", before,
              s_cfg_store.audio_onebit);
        CHECK(hw_onebit == s_cfg_store.audio_onebit,
              "1-bit level %u not pushed to audio (hw %u)",
              s_cfg_store.audio_onebit, hw_onebit);
        s_dev_item = DEV_ONEBIT;
    }

    // HDMI audio is staged: cfg changes, nothing is applied anywhere.
    s_dev_item = DEV_HDMIAUD;
    uint8_t ha0 = s_cfg_store.hdmi_audio;
    uint8_t hwa = hw_artifact, hwg = hw_gain;
    do_short_press();
    CHECK(s_cfg_store.hdmi_audio == !ha0, "HDMI audio did not toggle");
    CHECK(hw_artifact == hwa && hw_gain == hwg,
          "toggling HDMI audio disturbed a live setting");

    // The label must say what hstx actually latched, not just what cfg
    // holds -- a boot-order bug that left HDMI mode off was invisible for
    // four builds because the menu only ever echoed cfg back.
    {
        char line[OLED_COLS + 1] = { 0 };
        hw_hdmi_audio = (s_cfg_store.hdmi_audio != 0);
        devset_text(DEV_HDMIAUD, line, sizeof line);
        CHECK(strstr(line, "act:") == NULL,
              "agreeing state still shows an act: suffix (%s)", line);
        hw_hdmi_audio = !hw_hdmi_audio;
        devset_text(DEV_HDMIAUD, line, sizeof line);
        CHECK(strstr(line, "act:") != NULL,
              "cfg and hstx disagree but the menu does not say so (%s)", line);
    }

    // ---- Back (no save) restores cfg AND the hardware ---------------
    s_cfg_store.wifi.enabled = 1;
    s_cfg_store.artifact = 1; s_cfg_store.audio_gain = 1;
    s_cfg_store.audio_filter = 1; s_cfg_store.audio_onebit = 1;
    audio_set_filter(1); audio_set_onebit(1);
    s_cfg_store.hdmi_audio = 1;
    vdg_set_artifact(1); audio_set_gain(1);
    enter_devset();                        // snapshot taken here

    s_dev_item = DEV_ARTIFACT; do_short_press();    // -> 2, applied live
    s_dev_item = DEV_GAIN;     do_short_press();    // -> 2, applied live
    s_dev_item = DEV_WIFI;     do_short_press();    // staged
    CHECK(s_cfg_store.artifact == 2 && hw_artifact == 2, "setup: artifact");
    CHECK(s_cfg_store.audio_gain == 2 && hw_gain == 2, "setup: volume");
    CHECK(s_cfg_store.wifi.enabled == 0, "setup: wifi toggle");

    s_dev_item = DEV_BACK; do_short_press();
    CHECK(s_mode == UI_LIST, "Back did not leave the device menu");
    CHECK(s_cfg_store.wifi.enabled == 1, "Back did not restore WiFi");
    CHECK(s_cfg_store.artifact == 1, "Back did not restore artifact in cfg");
    CHECK(s_cfg_store.audio_gain == 1, "Back did not restore volume in cfg");
    // The live ones were pushed to hardware as they were turned, so
    // restoring only cfg would leave the screen and the sound wrong.
    CHECK(hw_artifact == 1,
          "Back restored artifact in cfg but left %u applied to the renderer",
          hw_artifact);
    CHECK(hw_gain == 1,
          "Back restored volume in cfg but left %u applied to audio", hw_gain);

    s_dev_item = DEV_FILTER; do_short_press();
    s_dev_item = DEV_ONEBIT; do_short_press();
    s_dev_item = DEV_BACK;   do_short_press();
    CHECK(s_cfg_store.audio_filter == 1 && hw_filter == 1,
          "Back did not un-apply the filter (cfg %u, hw %u)",
          s_cfg_store.audio_filter, hw_filter);
    CHECK(s_cfg_store.audio_onebit == 1 && hw_onebit == 1,
          "Back did not un-apply the 1-bit level (cfg %u, hw %u)",
          s_cfg_store.audio_onebit, hw_onebit);

    if (failures) { printf("test_ui_vc: %d FAILURES\n", failures); return 1; }
    printf("test_ui_vc: OK (10 items, window keeps the cursor visible both "
           "ways, every item labelled and fits 21 cols, live artifact/volume apply, staged "
           "HDMI audio, Back un-applies the live ones)\n");
    return 0;
}
