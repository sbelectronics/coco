// ======================================================================
// test_select.c — drives the real selection state machine (select.c)
// with fake bus/expander/romfs/cfg modules and a controllable clock.
//
// Verifies the exact hardware choreography the CoCo will see:
//   happy path: reset LOW -> stub built+served -> gate ON -> image
//   loaded under reset -> arm -> reset HIGH -> magic -> swap+gate(flag)
//   -> last_rom persisted;
//   timeout path: disarm -> swap -> second (non-blocking) reset pulse;
//   load-failure path: bus idled, gate off, no stub boot-loop;
//   image semantics: 0xFF fill, power-of-two mirroring, banked sizing;
//   guards: busy re-entry and OTA-active rejection.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

#include "pico/stdlib.h"

uint64_t shim_now_us;

#include "config.h"
#include "bus.h"
#include "romfs.h"
#include "expander.h"
#include "cfg.h"
#include "ota.h"
#include "stub6809.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
} while (0)

// ---- event journal ---------------------------------------------------
static char events[64][48];
static int  nev;
static void ev(const char *fmt, ...) {
    if (nev >= 64) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(events[nev++], sizeof(events[0]), fmt, ap);
    va_end(ap);
}
static int ev_index(const char *needle) {
    for (int i = 0; i < nev; i++)
        if (strstr(events[i], needle)) return i;
    return -1;
}

// ---- fake bus --------------------------------------------------------
static uint8_t fake_main[MC_IMAGE_BUF_BYTES];
static uint8_t fake_stub[MC_BANK_BYTES];
static bool    fk_armed, fk_seen;

uint8_t *bus_staging(void)      { return fake_main; }
// The staging CAP is deliberately smaller than the buffer here, the way
// the videocart's is: above it sits the serve table the CoCo is reading
// from while this runs. fk_canary_ok checks nothing above it moved.
static uint32_t fk_stage_cap = MC_IMAGE_BUF_BYTES;
uint32_t bus_staging_bytes(void) { return fk_stage_cap; }
static bool fk_canary_ok(void) {
    for (uint32_t i = fk_stage_cap; i < MC_IMAGE_BUF_BYTES; i++)
        if (fake_main[i] != 0x5A) return false;
    return true;
}
static void fk_canary_set(uint32_t cap) {
    fk_stage_cap = cap;
    memset(fake_main + cap, 0x5A, MC_IMAGE_BUF_BYTES - cap);
}
uint8_t *bus_stub_staging(void) { return fake_stub; }
// Model the prepare -> serve contract the real bus keeps (bus.h): a
// bus_serve() whose (scheme,len) matches a still-valid prepared build is
// a pointer flip; anything else rebuilds the tables INLINE, which on the
// videocart is milliseconds of SRAM hammering on a live bus. bus_idle()
// deliberately does NOT invalidate a prepared build -- the insertion path
// idles the slot on purpose and serves the same image minutes later.
static int32_t fk_prep_len = -1;
static int     fk_prep_scheme = -1;
static int     fk_inline_builds;
void bus_serve(bus_scheme_t s, uint32_t len) {
    if (fk_prep_len != (int32_t)len || fk_prep_scheme != (int)s) {
        fk_inline_builds++;
        ev("inline_build len=%u", len);
    }
    fk_prep_len = -1;                       // consumed
    ev("serve s=%d len=%u", s, len);
}
void bus_serve_prepare(bus_scheme_t s, uint32_t len) {
    fk_prep_len = (int32_t)len; fk_prep_scheme = (int)s;
    ev("serve_prep s=%d len=%u", s, len);
}
void bus_serve_stub(void)  { fk_prep_len = -1; ev("serve_stub"); }
void bus_idle(void)        { ev("bus_idle"); }
void bus_stub_arm(void)    { fk_armed = true; fk_seen = false; ev("arm"); }
void bus_stub_disarm(void) { fk_armed = false; ev("disarm"); }
bool bus_stub_seen(void)   { return fk_seen; }
void bus_init(uint32_t d)  { (void)d; }
void bus_core1_main(void)  {}
uint32_t bus_strobe_count(void) { return 0; }
uint8_t  bus_current_bank(void) { return 0; }
const uint8_t *bus_mirror(void) { return NULL; }
uint32_t bus_write_count(void)  { return 0; }
uint32_t bus_capture_behind(void) { return 0; }
uint32_t bus_scs_disagree(void) { return 0; }

// ---- fake expander ---------------------------------------------------
void expander_init(void) {}
bool expander_ok(void) { return true; }
uint8_t expander_read(void) { return 0xFF; }
void expander_assert(uint8_t bit) {
    ev(bit == MC_XP_RESET_EN ? "reset ON" : "gate ON");
}
void expander_release(uint8_t bit) {
    ev(bit == MC_XP_RESET_EN ? "reset OFF" : "gate OFF");
}

// ---- fake romfs ------------------------------------------------------
static rom_entry_t fk_roms[4];
static int fk_nroms;
static const uint8_t *fk_data;
static int32_t fk_len;                  // <0 = load fails

const rom_entry_t *romfs_find(const char *f) {
    for (int i = 0; i < fk_nroms; i++)
        if (!strcmp(fk_roms[i].file, f)) return &fk_roms[i];
    return NULL;
}
int32_t romfs_load(const char *f, uint8_t *dst, uint32_t max) {
    (void)f;
    if (fk_len < 0) return -1;
    uint32_t n = ((uint32_t)fk_len > max) ? max : (uint32_t)fk_len;
    memcpy(dst, fk_data, n);
    return (int32_t)n;
}

// ---- fake cfg / ota --------------------------------------------------
static cfg_t fk_cfg;
static int   fk_saves;
static bool  fk_ota_active;
cfg_t *cfg_get(void) { return &fk_cfg; }
bool cfg_save(void)  { fk_saves++; ev("cfg_save"); return true; }
bool ota_active(void) { return fk_ota_active; }

#include "select.c"          // code under test

// ---- scenarios -------------------------------------------------------

static void pump_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) { shim_advance_us(1000); select_pump(); }
}

static void reset_journal(void) { nev = 0; }

int main(void) {
    static uint8_t img[40000];
    for (unsigned i = 0; i < sizeof(img); i++) img[i] = (uint8_t)(i * 13);

    fk_nroms = 2;
    snprintf(fk_roms[0].file, sizeof(fk_roms[0].file), "game.rom");
    snprintf(fk_roms[0].title, sizeof(fk_roms[0].title), "GAME");
    fk_roms[0].autostart = 1; fk_roms[0].scheme = 0;
    snprintf(fk_roms[1].file, sizeof(fk_roms[1].file), "mega.ccc");
    snprintf(fk_roms[1].title, sizeof(fk_roms[1].title), "MEGA");
    fk_roms[1].autostart = 0; fk_roms[1].scheme = 1;

    // ================= happy path ====================================
    fk_data = img; fk_len = 8192;        // 8K image: mirroring expected
    CHECK(select_rom("game.rom"), "select_rom");
    CHECK(select_busy(), "not busy after select");

    // Stub buffer must hold the stub + NOP fill (built before pump).
    CHECK(!memcmp(fake_stub, stub6809, STUB6809_LEN), "stub bytes wrong");
    CHECK(fake_stub[STUB6809_LEN] == 0x12 &&
          fake_stub[MC_BANK_BYTES - 1] == 0x12, "stub NOP fill wrong");

    // Ordering so far: reset ON before serve_stub before gate ON.
    CHECK(ev_index("reset ON") >= 0 &&
          ev_index("reset ON") < ev_index("serve_stub") &&
          ev_index("serve_stub") < ev_index("gate ON"),
          "stub setup order wrong");

    pump_ms(1);                          // load happens on first pump
    // Image loaded into MAIN buffer with 0xFF fill + 8K mirrored twice.
    CHECK(!memcmp(fake_main, img, 8192), "image content");
    CHECK(!memcmp(fake_main + 8192, img, 8192), "8K image not mirrored");
    CHECK(fake_main[MC_BANK_BYTES] == 0xFF, "fill above 16K not 0xFF");

    CHECK(ev_index("reset OFF") < 0, "reset released before pulse time");
    pump_ms(MC_RESET_PULSE_MS + 5);
    CHECK(ev_index("arm") >= 0 && ev_index("reset OFF") > ev_index("arm"),
          "arm must precede reset release");

    // Stub signals.
    fk_seen = true;
    pump_ms(1);
    // insert=0 (default): FAST path -- serve straight away, exactly as
    // before the insertion-style option existed. No 2 s penalty.
    int i_swap = ev_index("serve s=0 len=8192");
    CHECK(i_swap >= 0, "no swap to real image");
    CHECK(ev_index("gate ON") >= 0, "autostart gate not set");
    CHECK(!strcmp(select_active(), "game.rom"), "active '%s'",
          select_active());
    CHECK(fk_saves == 1 && !strcmp(fk_cfg.last_rom, "game.rom"),
          "last_rom not persisted");
    CHECK(select_state() == SEL_DONE, "state %d != DONE", select_state());
    pump_ms(2100);

    // ---- insert=1: insertion-style start -----------------------------
    // Slot must stay EMPTY across BASIC's cold boot, then serve and only
    // THEN raise CART* so the ROM handler's JMP $C000 finds the image.
    reset_journal();
    fk_roms[0].insert = 1;
    fk_seen = false;
    CHECK(select_rom("game.rom"), "select insert-style");
    pump_ms(MC_RESET_PULSE_MS + 5);
    fk_seen = true;
    pump_ms(1);
    CHECK(ev_index("serve s=0 len=8192") < 0, "served before BASIC booted");
    CHECK(select_state() == SEL_WAIT_BASIC, "state %d != WAIT_BASIC",
          select_state());
    pump_ms(MC_BASIC_BOOT_MS + 10);
    int i_ins = ev_index("serve s=0 len=8192");
    CHECK(i_ins >= 0, "insert-style never served");
    int last_on = -1;
    for (int i = 0; i < nev; i++) if (strstr(events[i], "gate ON")) last_on = i;
    CHECK(last_on > i_ins, "CART* must rise AFTER the serve");
    // The serve at the end of the insertion wait must be a POINTER FLIP,
    // not a rebuild. bus_serve_prepare() ran under reset for a reason: on
    // the videocart the build is milliseconds of 32K-strided SRAM traffic,
    // and here it would land microseconds before the FIRQ that starts the
    // game -- starving the snoop across exactly the writes ($FF22, SAM
    // V/F) a cart makes once and never repeats. bus_idle() in the swap
    // must therefore leave the prepared build standing.
    CHECK(fk_inline_builds == 0,
          "insertion-style serve rebuilt the tables on a LIVE bus (%d)",
          fk_inline_builds);
    CHECK(select_state() == SEL_DONE, "state %d != DONE", select_state());
    fk_roms[0].insert = 0;               // back to the default fast path
    pump_ms(2100);
    CHECK(select_state() == SEL_IDLE, "no return to IDLE");

    // Re-select same ROM: cfg unchanged -> no extra save.
    reset_journal();
    fk_seen = false;
    CHECK(select_rom("game.rom"), "re-select");
    pump_ms(MC_RESET_PULSE_MS + 5);
    fk_seen = true;
    pump_ms(1);
    CHECK(fk_saves == 1, "redundant cfg_save (flash wear)");
    pump_ms(2100);

    // ================= banked ROM ====================================
    reset_journal();
    fk_len = 40000; fk_seen = false;
    CHECK(select_rom("mega.ccc"), "select banked");
    pump_ms(MC_RESET_PULSE_MS + 5);
    fk_seen = true;
    pump_ms(1);
    CHECK(ev_index("serve s=1 len=40000") >= 0, "banked serve missing");
    // autostart=0 -> gate OFF after swap.
    int last_gate_on = -1, last_gate_off = -1;
    for (int i = 0; i < nev; i++) {
        if (strstr(events[i], "gate ON"))  last_gate_on = i;
        if (strstr(events[i], "gate OFF")) last_gate_off = i;
    }
    CHECK(last_gate_off > last_gate_on, "gate not dropped for manual ROM");
    pump_ms(2100);

    // ================= timeout path ==================================
    reset_journal();
    fk_len = 8192; fk_seen = false;      // stub never signals
    CHECK(select_rom("game.rom"), "select for timeout");
    pump_ms(MC_RESET_PULSE_MS + 5);
    pump_ms(MC_STUB_TIMEOUT_MS + 10);
    CHECK(ev_index("disarm") >= 0, "no disarm on timeout");
    CHECK(select_state() == SEL_RESET2, "no second reset pulse state");
    int resets = 0;
    for (int i = 0; i < nev; i++) if (strstr(events[i], "reset ON")) resets++;
    CHECK(resets == 2, "%d reset pulses, want 2", resets);
    pump_ms(MC_RESET_PULSE_MS + 5);
    CHECK(select_state() == SEL_TIMEOUT, "RESET2 didn't complete");
    CHECK(events[nev - 1] && ev_index("reset OFF") >= 0, "reset stuck low");
    CHECK(ev_index("serve s=0 len=8192") >= 0, "timeout path never served");
    pump_ms(2100);
    CHECK(select_state() == SEL_IDLE, "timeout never returns to IDLE");

    // ================= load-failure path =============================
    reset_journal();
    fk_len = -1;                         // romfs_load fails
    CHECK(select_rom("game.rom"), "select for load-fail");
    pump_ms(1);
    CHECK(ev_index("bus_idle") >= 0, "bus not idled on load failure");
    CHECK(ev_index("gate OFF") >= 0, "gate left on -> stub boot-loop!");
    CHECK(ev_index("reset OFF") >= 0, "reset stuck asserted");
    CHECK(select_state() == SEL_ERROR, "state %d != ERROR", select_state());
    CHECK(select_active()[0] == 0, "active not cleared on failure");
    pump_ms(2100);

    // ================= guards ========================================
    fk_len = 8192;
    CHECK(!select_rom("nope.rom"), "missing ROM accepted");
    pump_ms(2100);
    CHECK(select_rom("game.rom"), "select for busy test");
    CHECK(!select_rom("mega.ccc"), "re-entry while busy accepted");
    pump_ms(MC_RESET_PULSE_MS + 5);
    fk_seen = true;
    pump_ms(1); pump_ms(2100);

    fk_ota_active = true;
    CHECK(!select_rom("game.rom"), "selection during OTA accepted");
    fk_ota_active = false;

    // ================= boot path =====================================
    reset_journal();
    CHECK(select_boot_rom("game.rom"), "boot rom");
    CHECK(ev_index("serve s=0 len=8192") >= 0, "boot serve missing");
    CHECK(ev_index("reset ON") < 0 && ev_index("serve_stub") < 0,
          "boot path must not pulse reset or serve the stub");


    // ========== staging must stay inside bus_staging_bytes() =========
    // The videocart's image buffer doubles as its serve-table space, and
    // the slot above the images holds the STUB table -- which the CoCo is
    // fetching instructions from for the whole of RESET_HOLD. A loader
    // that clears the entire buffer leaves the stub reading $FF by the
    // time reset is released: the stub never runs, the magic strobe never
    // arrives, and every selection falls out of WAIT_MAGIC on the 1.5 s
    // timeout instead of getting its guaranteed cold boot.
    reset_journal();
    fk_canary_set(32768);                       // videocart's real cap
    fk_len = 8192;
    CHECK(select_rom("game.rom"), "select for staging-bound test");
    pump_ms(1);
    CHECK(fk_canary_ok(),
          "load_image() wrote past bus_staging_bytes() -- on the videocart "
          "that is the live stub serve table");
    CHECK(!memcmp(fake_main, img, 8192), "image content under a cap");
    pump_ms(MC_RESET_PULSE_MS + 5);
    fk_seen = true;
    pump_ms(1);
    CHECK(fk_canary_ok(), "staging spilled later in the selection");

    // An image LARGER than the cap is truncated, not refused, and still
    // must not spill: the board serves the banks it can address.
    reset_journal();
    fk_len = (int32_t)sizeof(img);              // 40000 > 32768
    CHECK(select_rom("mega.ccc"), "select oversize");
    pump_ms(1);
    CHECK(fk_canary_ok(), "oversize image spilled past the cap");
    CHECK(ev_index("serve_prep s=1 len=32768") >= 0,
          "oversize image not truncated to the cap");
    pump_ms(MC_RESET_PULSE_MS + 5);
    fk_seen = true;
    pump_ms(2200);
    fk_stage_cap = MC_IMAGE_BUF_BYTES;          // restore for later cases
    if (failures) { printf("test_select: %d FAILURES\n", failures); return 1; }
    printf("test_select: OK (happy, banked, timeout+RESET2, load-fail, "
           "guards, boot path)\n");
    return 0;
}
