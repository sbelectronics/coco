// ======================================================================
// select.c — ROM selection sequencer.
//
// Flow:
//
//   select_rom()      assert !RESET, build the stub in its dedicated
//                     buffer, serve it, CART gate ON
//   RESET_HOLD        load the real image into the MAIN buffer (safe by
//                     construction: the stub buffer is what's serving)
//                     while reset is held; after MC_RESET_PULSE_MS arm
//                     strobe capture and release !RESET
//   WAIT_MAGIC        the stub boots, clears $71, writes $FF5F ->
//                     bus_stub_seen(); swap to the real image and set
//                     the gate per the ROM's autostart flag. The stub
//                     then jumps through $FFFE for a true cold boot.
//   timeout           a stale RAM warm-start hook beat the stub:
//                     swap anyway and
//                     pulse reset a second time.
// ======================================================================

#include "select.h"
#include "config.h"
#include "bus.h"
#include "romfs.h"
#include "expander.h"
#include "cfg.h"
#include "stub6809.h"
#include "ota.h"

#include "pico/stdlib.h"

#include <stdio.h>
#include <string.h>

static select_state_t s_state;
static absolute_time_t s_t0;
static char s_pending[ROMFS_NAME_MAX];
static char s_active[ROMFS_NAME_MAX];
static char s_status[32] = "Ready";
static uint32_t s_pending_len;
static uint8_t  s_pending_scheme;
static uint8_t  s_pending_autostart;
static bool     s_loaded;
static bool     s_pending_basic;    // selection target is "no cart / BASIC"
static uint8_t  s_pending_insert;   // per-ROM: use insertion-style start
static bool     s_was_timeout;      // reached the serve via the stub-timeout
                                    // fallback; report TIMEOUT, not DONE

select_state_t select_state(void)  { return s_state; }
const char    *select_status(void) { return s_status; }
const char    *select_active(void) { return s_active; }
bool select_busy(void) {
    return s_state == SEL_RESET_HOLD || s_state == SEL_WAIT_MAGIC ||
           s_state == SEL_WAIT_BASIC || s_state == SEL_RESET2;
}

// Did the cold-boot stub actually RUN? A selection that falls through
// the WAIT_MAGIC timeout still reaches the cart, but without the
// guaranteed cold boot: a warm start leaves $71 set, BASIC skips its
// init, and a cart that expects a fresh BASIC (the insertion-style ones
// especially) loads and then dies. The panel shows both counts so that
// failure mode is visible rather than silent.
static uint32_t s_stub_ok;          // selections where the magic arrived
static uint32_t s_stub_to;          // selections that fell out on timeout
uint32_t select_stub_ok(void)       { return s_stub_ok; }
uint32_t select_stub_timeouts(void) { return s_stub_to; }

static void gate_autostart(bool on) {
    // P6 low = Q gated onto CART* (74HCT125 section 1 enabled).
    if (on) expander_assert(MC_XP_CART_EN);
    else    expander_release(MC_XP_CART_EN);
}

static void reset_assert(bool on) {
    // P5 low = '125 section 3 pulls !RESET low (open drain).
    if (on) expander_assert(MC_XP_RESET_EN);
    else    expander_release(MC_XP_RESET_EN);
}

// Load a ROM into the staging buffer with real-hardware semantics:
// unused space reads 0xFF (open bus is friendlier than stale garbage),
// and a power-of-two image smaller than the 16K window is mirrored the
// way a small ROM with unconnected upper address lines appears (an 8K
// cart shows up twice; 4K four times).
//
// The bound is bus_staging_bytes(), NOT the buffer size. On the
// videocart those differ: the buffer's upper part holds the serve
// tables, one of which is the stub table the CoCo is fetching from
// right now, and clearing past the bound would wipe it. An image larger
// than the bound is truncated (the board serves what it can address).
static int32_t load_image(const char *file) {
    uint8_t *stage = bus_staging();
    uint32_t cap   = bus_staging_bytes();
    memset(stage, 0xFF, cap);
    int32_t n = romfs_load(file, stage, cap);
    if (n <= 0) return n;

    if ((uint32_t)n < MC_BANK_BYTES &&
        ((uint32_t)n & ((uint32_t)n - 1)) == 0) {
        for (uint32_t off = n; off < MC_BANK_BYTES; off += n)
            memcpy(stage + off, stage, n);
    }
    return n;
}

bool select_boot_rom(const char *file) {
    const rom_entry_t *e = romfs_find(file);
    if (!e) return false;

    int32_t n = load_image(e->file);
    if (n <= 0) return false;

    bus_serve((bus_scheme_t)e->scheme, (uint32_t)n);
    gate_autostart(e->autostart != 0);
    snprintf(s_active, sizeof(s_active), "%s", e->file);
    snprintf(s_status, sizeof(s_status), "%s", e->title);
    s_state = SEL_IDLE;
    return true;
}

// Shared prefix: halt the CoCo and serve the cold-boot stub so it runs,
// clears the warm-start flag, and signals us to swap in the target. The
// stub buffer may only be written under reset (bus.h invariant), which
// asserting reset first guarantees.
static void serve_stub_and_reset(void) {
    reset_assert(true);

    // Stub image: a full 16K window of NOPs with the stub at $C000, so
    // stray reads above the stub are harmless. It lives in its own
    // dedicated buffer, leaving the main buffer free for the real image
    // to load into while the stub is being served.
    uint8_t *stage = bus_stub_staging();
    memset(stage, 0x12, MC_BANK_BYTES);              // 6809 NOP
    memcpy(stage, stub6809, STUB6809_LEN);
    bus_serve_stub();

    gate_autostart(true);                            // the stub must launch

    s_loaded = false;
    s_state  = SEL_RESET_HOLD;
    s_t0     = get_absolute_time();
}

bool select_rom(const char *file) {
    if (select_busy()) return false;
    // A firmware upload's flash erases can stall the pump loop past the
    // stub's pre-jump delay; don't start a selection under it.
    if (ota_active()) return false;

    const rom_entry_t *e = romfs_find(file);
    if (!e) {
        s_state = SEL_ERROR;
        s_t0    = get_absolute_time();
        snprintf(s_status, sizeof(s_status), "Not found");
        return false;
    }

    s_pending_basic     = false;
    s_was_timeout       = false;
    s_pending_insert    = e->insert;
    s_pending_scheme    = e->scheme;
    s_pending_autostart = e->autostart;
    snprintf(s_pending, sizeof(s_pending), "%s", e->file);

    serve_stub_and_reset();
    snprintf(s_status, sizeof(s_status), "Loading...");
    return true;
}

// Boot the bare machine to Extended Color BASIC: run the cold-boot stub
// (for a clean cold start), then leave the slot IDLE with the autostart
// gate off instead of serving a ROM. The stub's JMP [$FFFE] then lands
// in BASIC with no cartridge present.
bool select_basic(void) {
    if (select_busy() || ota_active()) return false;

    s_pending_basic  = true;
    s_pending_insert = 0;
    s_pending[0]     = 0;

    serve_stub_and_reset();
    snprintf(s_status, sizeof(s_status), "-> BASIC");
    return true;
}

// Insertion-style autostart.
//
// There are TWO ways a CoCo starts a "DK" cartridge, and they are NOT
// equivalent:
//
//   reset path      BASIC's cold-init checks $C000 for "DK" and jumps to
//                   $C002 from PARTWAY THROUGH its own initialisation.
//   insertion path  BASIC finishes booting to READY; asserting CART*
//                   raises FIRQ, and the ROM handler at $A0F6 does
//                   CLR $71 then JMP $C000 -- the "DK" bytes ($44 = LSRA,
//                   $4B) execute harmlessly and fall through to $C002.
//
// Cassette games converted to carts are written for a fully-initialised
// BASIC, and several of them (Seadragon, Klendathu) run ONLY via the
// insertion path: they work when physically inserted into a running
// BASIC and fail on every reset-path start.
//
// So we reproduce insertion: hold the slot EMPTY across the cold boot so
// the "DK" check finds nothing and BASIC runs all the way to READY, then
// serve the image and raise CART* to fire the FIRQ. Same end state, with
// the machine BASIC actually finished setting up.
static void serve_and_autostart(void) {
    bus_serve((bus_scheme_t)s_pending_scheme, s_pending_len);
    gate_autostart(s_pending_autostart != 0);
}

static void swap_to_real(void) {
    cfg_t *c = cfg_get();

    if (s_pending_basic) {
        // No cartridge: idle the slot and drop the gate so the stub's
        // JMP [$FFFE] cold-boots into Extended Color BASIC. Empty
        // last_rom means the next power-on also comes up in BASIC.
        bus_idle();
        gate_autostart(false);
        s_active[0] = 0;
        snprintf(s_status, sizeof(s_status), "Extended BASIC");
        if (c->last_rom[0]) { c->last_rom[0] = 0; cfg_save(); }
        return;
    }

    if (s_pending_insert) {
        // Insertion-style: slot stays EMPTY and CART* low for now. The
        // stub is about to JMP [$FFFE], and BASIC must complete its whole
        // cold init without seeing a cartridge; SEL_WAIT_BASIC serves it
        // and raises CART* once BASIC has reached READY.
        bus_idle();
        gate_autostart(false);
    } else {
        // Default: serve immediately and let BASIC's cold-init "DK"
        // check start the cart.
        serve_and_autostart();
    }

    snprintf(s_active, sizeof(s_active), "%s", s_pending);
    const rom_entry_t *e = romfs_find(s_pending);
    snprintf(s_status, sizeof(s_status), "%s", e ? e->title : s_pending);

    // Remember for the next power-on.
    if (strcmp(c->last_rom, s_pending)) {
        snprintf(c->last_rom, sizeof(c->last_rom), "%s", s_pending);
        cfg_save();
    }
}

void select_pump(void) {
    switch (s_state) {

    case SEL_RESET_HOLD: {
        // Load the real image into staging while reset is held — it must
        // be resident before the stub can possibly signal. The BASIC
        // target has nothing to load (it goes idle on the swap).
        if (!s_loaded && s_pending_basic) {
            s_pending_len = 0;
            s_loaded = true;
        }
        if (!s_loaded) {
            int32_t n = load_image(s_pending);
            if (n <= 0) {
                // Abort cleanly: the STUB is being served right now. If
                // we just released reset, the CoCo would boot-loop it
                // (stub -> reset vector -> stub ...) forever. Idle the
                // slot and drop the gate so it boots plain BASIC.
                bus_idle();
                gate_autostart(false);
                s_active[0] = 0;
                reset_assert(false);
                s_state = SEL_ERROR;
                s_t0    = get_absolute_time();
                snprintf(s_status, sizeof(s_status), "Load failed");
                return;
            }
            s_pending_len = (uint32_t)n;
            s_loaded = true;
            // Do the serve-side setup HERE, with reset still asserted and
            // the CoCo off the bus. On the videocart this is a several-
            // millisecond SRAM burst; left until swap_to_real() it runs
            // while the freshly released cart is executing its startup
            // code, starves the capture loop, and eats exactly the kind of
            // one-shot register writes a cart never repeats.
            bus_serve_prepare((bus_scheme_t)s_pending_scheme, s_pending_len);
        }
        if (absolute_time_diff_us(s_t0, get_absolute_time())
                >= MC_RESET_PULSE_MS * 1000) {
            bus_stub_arm();
            reset_assert(false);
            s_state = SEL_WAIT_MAGIC;
            s_t0    = get_absolute_time();
        }
        break;
    }

    case SEL_WAIT_BASIC: {
        // The slot has been empty across BASIC's cold boot. Once it has
        // had time to reach READY, serve the image and raise CART* --
        // FIRQ then autostarts the cart exactly as a physical insertion
        // does, with BASIC fully initialised.
        if (absolute_time_diff_us(s_t0, get_absolute_time())
                >= MC_BASIC_BOOT_MS * 1000) {
            serve_and_autostart();
            s_state = s_was_timeout ? SEL_TIMEOUT : SEL_DONE;
            s_t0    = get_absolute_time();
        }
        break;
    }

    case SEL_WAIT_MAGIC: {
        if (bus_stub_seen()) {
            s_stub_ok++;
            swap_to_real();
            // Only an insertion-style ROM waits out BASIC's boot; the
            // BASIC target and the fast path are already finished here.
            s_state = (!s_pending_basic && s_pending_insert)
                          ? SEL_WAIT_BASIC : SEL_DONE;
            s_t0    = get_absolute_time();
        } else if (absolute_time_diff_us(s_t0, get_absolute_time())
                       >= MC_STUB_TIMEOUT_MS * 1000) {
            // The stub never ran (stale RAM warm-start hook). Swap and
            // reset again — the user may need to power-cycle. Disarm
            // first: a stale armed state would swallow the new game's
            // first bank write if it happened to hit $FF5F. The second
            // pulse is a state, not a sleep — a blocking 120 ms here
            // would stall the OLED, lwIP and any in-flight upload.
            s_stub_to++;
            bus_stub_disarm();
            s_was_timeout = true;
            swap_to_real();
            reset_assert(true);
            s_state = SEL_RESET2;
            s_t0    = get_absolute_time();
            printf("select: stub timeout on '%s'\n", s_pending);
        }
        break;
    }

    case SEL_RESET2: {
        if (absolute_time_diff_us(s_t0, get_absolute_time())
                >= MC_RESET_PULSE_MS * 1000) {
            reset_assert(false);
            // An insertion-style ROM still has to wait out this boot and
            // then autostart by CART*; everything else is already served.
            s_state = (!s_pending_basic && s_pending_insert)
                          ? SEL_WAIT_BASIC : SEL_TIMEOUT;
            s_t0    = get_absolute_time();
        }
        break;
    }

    case SEL_DONE:
    case SEL_TIMEOUT:
    case SEL_ERROR:
        if (absolute_time_diff_us(s_t0, get_absolute_time()) >= 2000000)
            s_state = SEL_IDLE;
        break;

    case SEL_IDLE:
    default:
        break;
    }
}
