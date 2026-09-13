// ======================================================================
// bus.c — VIDEOCART core-1 bus service.
//
// The multicart sees A0-A13 directly; this board sees the address bus
// through three '153 muxes swept in four phases. Consequences:
//
//   * a free-running PIO sweep + DMA rings keep the address stream in
//     SRAM, so the snoop loop reads words instead of draining a FIFO;
//   * the serve path is entirely hardware (sampler SM -> DMA -> serve
//     SM) with a precomputed table per bank -- no CPU in the deadline;
//   * bank writes, the cold-boot stub signal and the 64K mirror are all
//     decoded in firmware from {address, R/W, data} -- there is no
//     strobe pin on this board.
//
// RAM RESIDENCY IS LOAD-BEARING, exactly as on the multicart: core 1
// runs with interrupts off and every instruction and datum in SRAM, so
// core 0 may erase/program flash without pausing it. `make audit`
// verifies the linked image after any edit here -- the attribute alone
// does not guarantee it (see ota.c's inlining note).
// ======================================================================

#include "bus.h"
#include "config.h"
#include "video.h"
#include "bus_pio.pio.h"

// Byte-pair transpose LUTs used by the snoop reassembly on core 1. Core 1
// reads both on EVERY lap, so they live in the scratch banks: in striped
// SRAM they would arbitrate against HSTX, the mirror stores and the serve
// DMA on every single cycle. 2 x 256 x u16 = 1 KB; filled before core 1
// launches.
static uint16_t s_tlo[256] __scratch_x("sweeplut");
static uint16_t s_thi[256] __scratch_y("sweeplut");
#include "sweep.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/systick.h"
#include "hardware/clocks.h"

#include <string.h>

// PIO placement. A block has 32 instruction words shared by ALL of its
// programs, and pio_add_program() PANICS at runtime (no video at all)
// when one does not fit -- the assembler only range-checks each program
// on its own. The preflight PIO gate fails the build if the budget below
// stops holding.
//
// pio0 MUST STAY FREE: cyw43_arch_init() claims the first free SM it
// finds, and keeping WiFi off our blocks keeps its DMA bursts off the
// same slave port as the bus SMs.
// pio1 holds the SAMPLER ALONE: it is the only SM that drives SEL and it
// must never contend for issue slots.
// pio2 holds follower + capture + serve.
#define SAMP_PIO        pio1
#define CAP_PIO         pio2
#define SM_SAMPLER      0
#define SM_CAPTURE      0
#define SM_SERVE        1
#define SM_FOLLOWER     2

// ---------------------------------------------------------------- state

// Serve tables. This buffer has three lives, in sequence, never at once:
//   1. raw image staging (low bus_staging_bytes(), written by romfs)
//   2. permuted serve tables, built in place (see tables_build())
//   3. the live table the serve DMA dereferences every window read
//
// 32 KB alignment is a HARD requirement: the sampler builds the DMA's
// read address as (base | index) rather than (base + index), so any set
// bit below bit 15 of the base would corrupt every lookup. The linker
// cannot be asked to prove placement, so bus_init() checks it at
// runtime and panics rather than serve garbage.
static uint8_t s_main[MC_IMAGE_BUF_BYTES] __attribute__((aligned(32768)));
static uint8_t s_stub[MC_BANK_BYTES]      __attribute__((aligned(4)));

// Image banks served: 2 (32 KB). The buffer is 96 KB = three 32 KB table
// slots, and the third is reserved for the selection stub. 32 KB covers
// every commercial CoCo cartridge; the only larger image in normal use is
// the synthetic 4-bank Bank Diagnostic, which loads truncated: banks_for()
// clamps, bank_mask aliases the upper banks onto the lower ones, and the
// diagnostic reports PASS for banks 0-1 and SWFAIL for 2-3 on this board.
// That is the intended difference from the multicart (128 KB, no tables).
#define MC_MAX_BANKS    2u

// The selection stub has its own permanent table slot above the image
// banks, so tables_build() can never touch it and every cart's tables
// can be built under reset while the stub is still being served.
#define STUB_TABLE_BANK 2u

_Static_assert(MC_IMAGE_BUF_BYTES >= (STUB_TABLE_BANK + 1u) * PERM_TABLE_BYTES,
               "image buffer must hold MC_MAX_BANKS tables plus the stub");
_Static_assert(PERM_TABLE_BYTES == 32768u, "sampler assumes 32 KB tables");
_Static_assert(MC_MAX_BANKS * MC_BANK_BYTES
                   <= STUB_TABLE_BANK * PERM_TABLE_BYTES,
               "raw staging must end below the stub's table slot -- "
               "select.c clears bus_staging_bytes() while it is served");
_Static_assert(MC_MAX_BANKS * PERM_TABLE_BYTES
                   <= STUB_TABLE_BANK * PERM_TABLE_BYTES,
               "image tables must not reach the stub's table slot");

static inline uint16_t *table_for_bank(uint32_t bank) {
    return (uint16_t *)(s_main + bank * PERM_TABLE_BYTES);
}

// 64K shadow of CoCo RAM. Written by core 1 from snooped writes, read by
// core 0's renderer. No locking: worst case the renderer catches a byte
// mid-frame, which is the tearing the design accepts.
static uint8_t s_mirror[MC_MIRROR_BYTES] __attribute__((aligned(4)));

// ---------------------------------------------------------------------
// SNOOP RINGS.
//
// Both snoop streams are buffered DEEP and paired BY INDEX:
//
//   capture SM pushes exactly 1 word  per bus cycle  -> s_capring[k]
//   follower SM pushes exactly 4 words per bus cycle -> s_folring[4k..]
//
// Both DMAs preserve order, so a consumer that anchors once to the
// follower DMA's live write pointer and then advances one group per
// capture stays paired exactly, no matter how far behind core 1 runs
// (see the pairing note in bus_loop -- the anchor is the load-bearing
// part). A delayed capture is harmless; only a dropped one is a defect.
// There is no per-cycle deadline in the snoop, only the throughput
// requirement that the average lap beat the 1117 ns bus cycle, which the
// lap-anatomy instrument reports.
//
// The rings live in striped SRAM rather than the 4 KB scratch banks
// because depth matters more than placement: scratch capped them at 64
// cycles, and a loop running ~25% slow for a few ms (a table build on
// core 0, say) filled that and let the DMA overwrite unconsumed history.
// 256 cycles is ~286 us of grace. The serve chain does not need physical
// separation from this traffic: its DMA channels run at high priority.
//
// Alignment is load-bearing: the DMA write rings wrap on the size.
#define FOL_RING_WORDS  1024             // 256 cycles x 4 phases, 4 KB
#define CAP_RING_WORDS  256              // 256 cycles, 1 KB
#define RING_CYCLES     256              // depth in bus cycles (both)
static volatile uint32_t s_folring[FOL_RING_WORDS]
    __attribute__((aligned(4096)));
static volatile uint32_t s_capring[CAP_RING_WORDS]
    __attribute__((aligned(1024)));

// Capture word format: (1 << 8) | data. The validity bit matters: a ring
// slot must distinguish "written" from "empty", and a $00 data byte is
// legal, so zero alone cannot mean empty. The consumer zeroes consumed
// slots.

// ---------------------------------------------------------------------
// Video-register ring: core 1 -> core 0.
//
// Core 1 keeps everything that must land in-cycle -- pairing, bank
// decode, the mirror -- and hands only the VIDEO REGISTER writes ($FF22
// and the SAM range, a few per mode change) to core 0 as packed words:
// [23:16] data, [15:0] address. The renderer's consumer lives on core 0,
// where a microsecond of jitter is irrelevant. At that rate 2048 slots
// is effectively unbounded; overflow drops the write and counts it in
// pair_drop rather than stalling core 1.
#define PAIR_RING  2048                  // power of two, 8 KB
static volatile uint32_t s_pair[PAIR_RING];
static volatile uint32_t s_pair_wr;      // producer: core 1 only
static volatile uint32_t s_pair_rd;      // consumer: core 0 only

// Audio event ring: core 1 -> the HSTX island IRQ on core 0. See the push
// site at the end of bus_loop() for the word layout and why it is separate
// from the video ring.
//
// 1024 words = 4 KB. The consumer is an interrupt, so the only long gap is
// a flash erase running with interrupts off: ~45 ms at the CoCo's worst
// audio write rate (~22 kHz) is ~990 events, which this just covers.
#define AUD_RING   1024
static volatile uint32_t s_aud[AUD_RING];
static volatile uint32_t s_aud_wr;       // producer: core 1 only
static volatile uint32_t s_aud_rd;       // consumer: core 0 IRQ only

typedef struct {
    volatile uint32_t epoch;
    volatile uint32_t serve;
    volatile uint32_t scheme;
    volatile uint32_t use_stub;
    volatile uint32_t bank_mask;
    volatile uint32_t stub_armed;
    volatile uint32_t stub_seen;
    volatile uint32_t strobe_count;      // bank-register writes seen
    volatile uint32_t current_bank;
    // diagnostics
    volatile uint32_t write_count;       // total writes mirrored
    volatile uint32_t capture_behind;    // captures discarded by a runtime
                                         // RESYNC (never boot) -- real
                                         // losses, must stay 0
    volatile uint32_t snoop_lag;         // deepest ring lag ever, cycles
    volatile uint32_t lag_now;           // most recent lag sample
    volatile uint32_t lag_hi;            // samples at >= 3/4 ring depth
    volatile uint32_t pair_hi;           // video-ring high-water occupancy
    // Serve TX FIFO occupancy, worst seen. THE test for serve latch-up:
    // the serve SM does a blocking pull per window read, so if one word
    // is ever delivered after its cycle ended, the SM drives it in the
    // NEXT cycle and stays permanently one behind -- serving stale bytes
    // forever with no way to recover. That state shows up here as a
    // persistently non-empty FIFO. 0-1 = healthy; 2+ = latched behind.
    volatile uint32_t serve_fifo_max;
    // Last tag-guard failure: [9:8] follower pointer word phase, [7:0]
    // the tag byte read. Distinguishes ring word-slip from anchor error.
    volatile uint32_t tag_diag;
    // SERVE ECHO: read-back of the byte the bus actually carried during
    // our own drive, versus the table entry the DMA looked up. The only
    // direct measurement of serve correctness.
    volatile uint32_t echo_ok;
    volatile uint32_t echo_bad;
    volatile uint32_t echo_diag;   // [31:16] addr, [15:8] owed, [7:0] got
    // Anchor re-takes forced by the periodic verification. A tick per
    // cart change or RESET press is the mechanism working; steady growth
    // means the streams slip continuously.
    volatile uint32_t anchor_fix;
    // Observed follower-pointer distance at the last verification: the
    // ground truth for the anchor geometry.
    volatile uint32_t anchor_dist;
    // SAM map type mirror + how often it toggled (observed only).
    volatile uint32_t sam_ty;
    volatile uint32_t ty_flips;
    volatile uint32_t scs_disagree;      // swept SCS* vs address decode
    volatile uint32_t lap_count;         // snoop laps == bus cycles
    // ---- zero-drop counters. All of these must read 0 in steady state;
    // a nonzero value is a dropped byte on one pipeline or the other.
    volatile uint32_t pair_drop;         // video-register write lost
    volatile uint32_t aud_drop;          // audio event lost (ring full)
    volatile uint32_t aud_on;            // 1 = HDMI audio consumer exists
    volatile uint32_t tag_slip;          // follower ring lost slot order
    // Split of tag_slip. A GUARD is a group whose four tags did not read
    // 0,1,2,3 -- the streams genuinely slipped. A RETRY is a lap spent
    // waiting to re-anchor afterwards, which costs a cycle but diagnoses
    // nothing on its own. One guard followed by many retries is a
    // different fault from many guards.
    volatile uint32_t reset_vecs;        // $FFFE/$FFFF pairs: one per reset
    volatile uint32_t tag_guard;
    volatile uint32_t tag_retry;
    // Serve words the sampler could not push because its RX FIFO was
    // full, i.e. the serve DMA was not draining. Read from PIO FDEBUG.
    volatile uint32_t serve_stall;
    // Window reads observed by core 1 = serves the hardware was asked
    // for. Not a fault counter; it shows serving is being exercised.
    volatile uint32_t window_reads;
    // ---- lap anatomy (4096-lap averages, ns). See bus_loop.
    volatile uint32_t lap_wait_ns;       // blocked on capture arrival
    volatile uint32_t lap_body_ns;       // everything else -- the cost
    volatile uint32_t lap_bmax_ns;       // worst body in the window
    // Set by tables_build: every bank's table spot-checked against its
    // source ON DEVICE right after building. 1 = verified, 0 = MISMATCH
    // (a build/permute bug that host tests cannot see on real memory).
    volatile uint32_t tbl_ok;
} bus_ctl_t;

static bus_ctl_t s_ctl __scratch_y("busring");

const uint8_t *bus_mirror(void) { return s_mirror; }

// ------------------------------------------------------------------ init

// Channel ids of each chained pair, kept so the loop's index pairing can
// read the live write pointer (whichever half is busy).
static int s_fol_dma_a = -1, s_fol_dma_b = -1;
static int s_cap_dma_a = -1, s_cap_dma_b = -1;

// Priority is deliberately DEFAULT on both snoop pairs -- measured, not
// assumed. High round-robin priority here made capture-arrival wait
// explode and drops quadruple. With index pairing, delivery jitter is
// absorbed by the ring depth anyway, so there is nothing for priority
// to buy.
static void snoop_pair_init(int *ida, int *idb, uint sm,
                            volatile uint32_t *ring, uint ring_bits) {
    int a = dma_claim_unused_channel(true);
    int b = dma_claim_unused_channel(true);
    *ida = a; *idb = b;

    for (int pass = 0; pass < 2; pass++) {
        int ch    = pass ? b : a;
        int other = pass ? a : b;
        dma_channel_config c = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, true);
        channel_config_set_ring(&c, true, ring_bits);
        channel_config_set_dreq(&c, pio_get_dreq(CAP_PIO, sm, false));
        channel_config_set_chain_to(&c, other);
        // 0x10000 transfers per half, then the twin takes over. Both ring
        // sizes divide 0x10000, so the write pointer is at slot 0 at
        // every handover -- the ring position is seamless across swaps.
        dma_channel_configure(ch, &c, (void *)ring,
                              &CAP_PIO->rxf[sm], 0x10000, false);
    }
    dma_channel_start(a);
}

// Live write position of a chained pair, as a WORD index into its ring.
//
// Checks BOTH busy bits and retries the brief instant where neither is
// set (the every-18 ms half-to-half handover). Falling back to channel
// B's write_addr when A is idle is wrong: during handover B is not
// started yet and its write_addr still reads the CONFIGURED RING BASE,
// so an anchor taken at that instant aims at garbage. The retry is
// bounded by the handover completing in a few bus-fabric cycles.
static inline __attribute__((always_inline))
uint32_t snoop_pair_pos(int a, int b, volatile const uint32_t *ring) {
    uint32_t wa;
    for (;;) {
        if (dma_hw->ch[a].al1_ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
            wa = dma_hw->ch[a].write_addr;
            break;
        }
        if (dma_hw->ch[b].al1_ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS) {
            wa = dma_hw->ch[b].write_addr;
            break;
        }
    }
    return (wa - (uint32_t)(uintptr_t)ring) >> 2;
}

// ---- the serve chain: address -> table entry -> serve FIFO, no CPU ----
//
// CH_ADDR is ENDLESS and DREQ-paced by the sampler's RX FIFO: one
// transfer per pushed word, forever, never chaining or interrupting.
// Each transfer copies the pushed word -- which IS the byte address of a
// uint16_t table entry -- into CH_DATA's read-address TRIGGER register,
// starting CH_DATA.
//
// CH_DATA then moves that one entry into the serve SM's TX FIFO. Its
// TRANS_COUNT of 1 is reloaded by the hardware on every trigger, so it
// stays armed without any CPU help.
//
// 16-bit transfers: the entry is {dirs,data} and the serve SM consumes
// only the low half of the FIFO word, so whatever the bus fabric leaves
// in the upper half is never looked at.
static int s_dma_addr = -1, s_dma_data = -1;
static uint s_off_serve;                 // serve program origin, for restart

// Repoint the sampler's X at a new bank table -- ONLY while the SM is
// parked. The sampler parks at `wait 0 gpio 8` for all of E-high; the
// two exec'd instructions cost ~130 ns against >=550 ns of park.
//
// Exec'ing into a RUNNING sweep instead clobbers OSR -- the live pin
// snapshot -- mid-phase, corrupting that cycle's address and its A14/A15
// gates: worst case a spurious drive into a RAM fetch. Core 1 reaches
// the bank branch mid-sweep of the following cycle, so an unguarded exec
// would corrupt one sweep on EVERY bank strobe.
//
// Waiting for a FRESH rising edge -- not just "E is high", which could
// be the tail of the window -- guarantees the full park time.
//
// Two forms: bus_loop uses the ALWAYS-INLINE one so the loop stays
// call-free (a call is the thin end of hidden work and hidden flash
// veneers); core 0's mount path uses the out-of-line wrapper below.
//
// The E waits are BOUNDED. bus_serve() calls this from core 0 at mount,
// and E is dead whenever the CoCo is unpowered -- the normal state while
// flashing on the bench. An unbounded spin there hangs core 0 before the
// web UI or OLED ever come up, i.e. the board looks bricked. ~2 ms is
// thousands of bus cycles if E is alive and a blink if it is not.
//
// Giving up and writing anyway is safe at mount (the serve SM is being
// restarted regardless, so a corrupted sweep is discarded). On core 1's
// bank-switch path E is provably alive -- a capture just arrived, which
// requires E edges -- so the bound is never reached there.
#define E_PARK_SPINS  100000u

static inline __attribute__((always_inline))
void sampler_retable_parked_inline(uint32_t base) {
    uint32_t g = E_PARK_SPINS;
    while (gpio_get(MC_PIN_E)  && --g) { }   // wait out the current E-high
    g = g ? E_PARK_SPINS : 0;
    while (!gpio_get(MC_PIN_E) && --g) { }   // fresh rise: sampler parked
    SAMP_PIO->txf[SM_SAMPLER]      = base >> 15;
    SAMP_PIO->sm[SM_SAMPLER].instr = pio_encode_pull(false, true);
    SAMP_PIO->sm[SM_SAMPLER].instr = pio_encode_out(pio_x, 32);
}

static void __no_inline_not_in_flash_func(sampler_retable_parked)(uint32_t base) {
    sampler_retable_parked_inline(base);
}

static void serve_dma_init(void) {
    s_dma_addr = dma_claim_unused_channel(true);
    s_dma_data = dma_claim_unused_channel(true);

    // HIGH PRIORITY, and only here. This chain is the one thing on the
    // board with a hard deadline: the byte must be on the pins before
    // the CoCo latches (~fall+1037 ns). Everything else -- the snoop
    // rings, HSTX scanout -- tolerates delay by design. A late drive
    // shows up on the serve echo as a single-bit error (the bus still
    // holding a neighbouring value), and lateness comes from lookup
    // jitter, which comes from arbitrating against bulk traffic. These
    // two channels move 6 bytes per window read, so elevating them costs
    // the bulk streams nothing measurable.
    dma_channel_config d = dma_channel_get_default_config(s_dma_data);
    channel_config_set_transfer_data_size(&d, DMA_SIZE_16);
    channel_config_set_read_increment(&d, false);
    channel_config_set_write_increment(&d, false);
    channel_config_set_high_priority(&d, true);
    dma_channel_configure(s_dma_data, &d,
                          &CAP_PIO->txf[SM_SERVE],
                          s_main,               // replaced on every trigger
                          1, false);

    dma_channel_config a = dma_channel_get_default_config(s_dma_addr);
    channel_config_set_transfer_data_size(&a, DMA_SIZE_32);
    channel_config_set_read_increment(&a, false);
    channel_config_set_write_increment(&a, false);
    channel_config_set_high_priority(&a, true);   // see CH_DATA above
    channel_config_set_dreq(&a, pio_get_dreq(SAMP_PIO, SM_SAMPLER, false));
    dma_channel_configure(s_dma_addr, &a,
                          &dma_hw->ch[s_dma_data].al3_read_addr_trig,
                          &SAMP_PIO->rxf[SM_SAMPLER],
                          1, false);
    // ENDLESS: never decrements, never completes, never chains. Paced
    // purely by the sampler's DREQ.
    dma_hw->ch[s_dma_addr].transfer_count =
        (DMA_CH0_TRANS_COUNT_MODE_VALUE_ENDLESS << DMA_CH0_TRANS_COUNT_MODE_LSB) | 1u;
    dma_channel_start(s_dma_addr);
}

static uint32_t s_delay_ns;
// Nanoseconds per SysTick tick, worked out once in bus_init. The hot loop
// is RAM-resident and must not call into flash, so clock_get_hz() cannot
// be called from inside it.
static uint32_t s_ns_per_tick = 8u;

void bus_init(uint32_t strobe_delay_ns) {
    s_delay_ns = strobe_delay_ns;
    {   // Resolved here, not in the loop: see s_ns_per_tick.
        uint32_t hz = clock_get_hz(clk_sys);
        s_ns_per_tick = hz ? (1000000000u / hz) : 8u;
    }
    // Data bus: SIO-owned, input until the serve SM drives it.
    // gpio_init() does NOT clear the pad pulls, and the RP2350 resets
    // with pull-down enabled on every pad — clear each one explicitly
    // (erratum E9: enabled pull-downs latch externally-driven-high pads).
    for (int i = 0; i < 8; i++) {
        gpio_init(MC_PIN_D0 + i);
        gpio_set_dir(MC_PIN_D0 + i, GPIO_IN);
        gpio_disable_pulls(MC_PIN_D0 + i);
        gpio_set_slew_rate(MC_PIN_D0 + i, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(MC_PIN_D0 + i, GPIO_DRIVE_STRENGTH_8MA);
    }
    // E and the mux outputs are inputs; the '153s and the bus drive them.
    static const uint8_t ins[] = { MC_PIN_E, MC_PIN_M0, MC_PIN_M1,
                                   MC_PIN_M2, MC_PIN_M3, MC_PIN_M4 };
    for (unsigned i = 0; i < sizeof(ins); i++) {
        gpio_init(ins[i]);
        gpio_set_dir(ins[i], GPIO_IN);
        gpio_disable_pulls(ins[i]);
    }

    // The sampler builds DMA read addresses as (base | index), so a base
    // with any bit set below bit 15 would silently corrupt every lookup.
    // The linker cannot be asked to prove placement, so check it here.
    if (((uintptr_t)s_main & (PERM_TABLE_BYTES - 1u)) != 0)
        panic("serve table buffer is not 32 KB aligned");

    // Claim every SM before use: the CYW43 WiFi driver picks any FREE
    // state machine at cyw43_arch_init() time and would silently take
    // an unclaimed one out from under us.
    pio_sm_claim(SAMP_PIO, SM_SAMPLER);
    pio_sm_claim(CAP_PIO, SM_CAPTURE);
    pio_sm_claim(CAP_PIO, SM_SERVE);
    pio_sm_claim(CAP_PIO, SM_FOLLOWER);

    // pio_add_program() PANICS if the program does not fit the block's
    // 32 instruction words. Check first and say so, rather than hanging
    // with no video.
    if (!pio_can_add_program(SAMP_PIO, &mux_sampler_program))
        panic("sampler program does not fit its PIO block");
    uint off_samp = pio_add_program(SAMP_PIO, &mux_sampler_program);
    mux_sampler_program_init(SAMP_PIO, SM_SAMPLER, off_samp,
                             MC_MUX_IN_BASE, MC_PIN_SEL0,
                             (uint32_t)(uintptr_t)s_main);

    if (!pio_can_add_program(CAP_PIO, &sweep_follower_program))
        panic("follower program does not fit its PIO block");
    uint off_fol = pio_add_program(CAP_PIO, &sweep_follower_program);
    sweep_follower_program_init(CAP_PIO, SM_FOLLOWER, off_fol,
                                MC_MUX_IN_BASE);

    if (!pio_can_add_program(CAP_PIO, &wr_capture_program))
        panic("capture program does not fit its PIO block");
    uint off_cap = pio_add_program(CAP_PIO, &wr_capture_program);
    wr_capture_program_init(CAP_PIO, SM_CAPTURE, off_cap, strobe_delay_ns);

    // The serve SM owns D0..D7 from here on. JMP_PIN is M4, which carries
    // live R/W while SEL rests at 0 -- i.e. for all of E-high, which is
    // the only time this SM can drive.
    if (!pio_can_add_program(CAP_PIO, &bus_serve_program))
        panic("serve program does not fit its PIO block");
    s_off_serve = pio_add_program(CAP_PIO, &bus_serve_program);
    // Pad drive for D0..D7, applied AFTER pio_gpio_init() has taken the
    // pins, so nothing in that path can leave them at a weaker default.
    // 8 mA and fast slew -- identical to the multicart, which drives this
    // same connector into this same CoCo. Raising drive on an unbuffered,
    // unterminated cartridge bus trades hypothetical setup margin for
    // real reflections and ground bounce; keep it at the proven value.
    bus_serve_program_init(CAP_PIO, SM_SERVE, s_off_serve,
                           MC_PIN_D0, MC_PIN_M4);
    for (int i = 0; i < 8; i++) {
        gpio_set_slew_rate(MC_PIN_D0 + i, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(MC_PIN_D0 + i, GPIO_DRIVE_STRENGTH_8MA);
    }

    // NOTE: this clears aud_on too, so bus_audio_enable() must be called
    // AFTER bus_init(), not before.
    memset((void *)&s_ctl, 0, sizeof(s_ctl));
    memset(s_mirror, 0, sizeof(s_mirror));

    sweep_luts_init(s_tlo, s_thi);     // before core 1 ever reads them

    snoop_pair_init(&s_fol_dma_a, &s_fol_dma_b, SM_FOLLOWER,
                    s_folring, 12);            // 4 KB write ring
    snoop_pair_init(&s_cap_dma_a, &s_cap_dma_b, SM_CAPTURE,
                    s_capring, 10);            // 1 KB write ring
    serve_dma_init();
    // Sampler and follower both anchor on the same E edges, so enabling
    // them together lands them on the same cycle from the first sweep.
    pio_sm_set_enabled(SAMP_PIO, SM_SAMPLER, true);
    pio_set_sm_mask_enabled(CAP_PIO,
                            (1u << SM_CAPTURE) | (1u << SM_FOLLOWER), true);
    // The serve SM stays DISABLED until an image is mounted: with it off
    // the data pins keep the Hi-Z pindirs set at init, so nothing can
    // reach the bus before there is a table to serve from.

    // Core 1 (PROC1) gets top priority on the SRAM/bus fabric so CYW43
    // WiFi DMA cannot stall its SRAM reads. This covers SRAM/fabric
    // accesses, NOT APB reads -- which is why the loop touches no PIO
    // register per lap.
    //
    // OR, never assign: hstx_init() runs first and elevates DMA_R/DMA_W
    // for the scanout FIFO. A plain `=` here wipes those, and the symptom
    // is intermittent HDMI sparkle that worsens when the CoCo is active —
    // i.e. it looks exactly like a TMDS routing or connector fault.
    bus_ctrl_hw->priority |= BUSCTRL_BUS_PRIORITY_PROC1_BITS;
}

// -------------------------------------------------- core-0 control API

uint8_t *bus_staging(void)      { return s_main; }

// The staging area is the RAW-IMAGE part of s_main only: banks 0..N-1 at
// MC_BANK_BYTES each. Everything above it is table territory, and slot
// STUB_TABLE_BANK up there is being SERVED while select.c stages the
// next image. See bus.h.
uint32_t bus_staging_bytes(void) { return MC_MAX_BANKS * MC_BANK_BYTES; }
uint8_t *bus_stub_staging(void) { return s_stub; }

// Stop the serve SM and leave the bus tristated. Also drains both FIFOs
// in the chain: a word left in the sampler's RX or the serve TX would be
// applied to a LATER cycle's address when serving resumes. Never
// reconfigure a live chain -- always quiesce through here first.
static void serve_stop(void) {
    pio_sm_set_enabled(CAP_PIO, SM_SERVE, false);
    // Force the drivers off regardless of where the SM was killed.
    pio_sm_exec(CAP_PIO, SM_SERVE, pio_encode_mov(pio_osr, pio_null));
    pio_sm_exec(CAP_PIO, SM_SERVE, pio_encode_out(pio_pindirs, 8));
    pio_sm_clear_fifos(CAP_PIO, SM_SERVE);
    pio_sm_clear_fifos(SAMP_PIO, SM_SAMPLER);
}

// Point the sampler at a table and restart the serve SM cleanly.
static void serve_start(const uint16_t *table) {
    pio_sm_clear_fifos(SAMP_PIO, SM_SAMPLER);
    pio_sm_clear_fifos(CAP_PIO, SM_SERVE);
    s_ctl.serve_fifo_max = 0;          // pre-mount idle level is not data
    sampler_retable_parked((uint32_t)(uintptr_t)table);
    pio_sm_restart(CAP_PIO, SM_SERVE);          // may have died mid-pull
    pio_sm_exec(CAP_PIO, SM_SERVE, pio_encode_jmp(s_off_serve));
    pio_sm_set_enabled(CAP_PIO, SM_SERVE, true);
}

// Spot-check a freshly built table against its still-intact source: 32
// offsets spread across the bank, verified through the same perm_idx the
// sampler's index order must match. This runs ON DEVICE against the real
// memory, so it catches what no host test can: a build that mis-ran here
// (alignment, overlap, a bounce bug). Result accumulates into tbl_ok.
static void table_verify(const uint16_t *table, const uint8_t *src) {
    for (uint32_t i = 0; i < 32; i++) {
        uint32_t a = (i * 509u + 7u) & (MC_BANK_BYTES - 1u);
        uint32_t cpu  = 0xC000u + a;
        uint32_t dirs = (cpu <= 0xFEFFu) ? 0xFFu : 0x00u;
        if (table[perm_idx(a)] != PERM_ENTRY(dirs, src[a]))
            s_ctl.tbl_ok = 0;
    }
}

// Build the permuted tables for `banks` banks IN PLACE over the raw image
// that romfs staged into s_main.
//
// Backward, because each 16 KB bank expands to a 32 KB table and would
// otherwise overwrite raw bytes it has not read yet:
//
//   bank 1: raw [16K,32K) -> table [32K, 64K)   disjoint
//   bank 0: raw [ 0,16K)  -> table [ 0, 32K)    OVERLAPS ITSELF
//
// so bank 0 is bounced through s_stub. s_stub is free at this point: the
// stub TABLE has already been built from it into its own slot.
static void tables_build(uint32_t banks) {
    if (banks < 1) banks = 1;
    if (banks > MC_MAX_BANKS) banks = MC_MAX_BANKS;

    s_ctl.tbl_ok = 1;
    for (uint32_t b = banks; b-- > 1; ) {
        perm_build(table_for_bank(b), s_main + b * MC_BANK_BYTES, 0xC000u);
        table_verify(table_for_bank(b), s_main + b * MC_BANK_BYTES);
    }

    memcpy(s_stub, s_main, MC_BANK_BYTES);         // bounce bank 0
    perm_build(table_for_bank(0), s_stub, 0xC000u);
    table_verify(table_for_bank(0), s_stub);
}

// tables_build() is a multi-millisecond, 32K-strided hammering of striped
// SRAM. Run on core 0 while the CoCo is live, it starves core 1's snoop
// loop: a 1-bank build is ~2.5-3 ms, which at 1117 ns/cycle spans ~2300
// bus cycles, and core 1 only has to run ~11% slow across that window to
// fill the 256-cycle ring. The next capture is then dropped with no
// error anywhere.
//
// A dropped write is *permanent* for a cart that programs its display
// registers once and never again. Monster Maze walks V2+F0..F6 in a single
// 8-write STA ,X++ burst at startup and never touches the SAM again, so
// losing one bit of that burst shifts the screen by the same amount on
// every run. Dragon Fire rewrites $FF22 6300x/sec, so the same loss reads
// as flicker rather than a fixed offset -- two symptoms, one defect.
//
// So build under reset, when the CoCo is off the bus entirely:
// select.c calls bus_serve_prepare() while it holds !RESET, and the later
// bus_serve() is then just a pointer flip.
static uint32_t     s_prep_len    = 0;         // 0 = nothing prepared
static bus_scheme_t s_prep_scheme = BUS_SCHEME_FLAT;

static uint32_t banks_for(uint32_t len) {
    uint32_t banks = (len + MC_BANK_BYTES - 1) / MC_BANK_BYTES;
    if (banks < 1) banks = 1;
    if (banks > MC_MAX_BANKS) banks = MC_MAX_BANKS;
    return banks;
}

void bus_serve_prepare(bus_scheme_t scheme, uint32_t len) {
    s_prep_len = 0;                                // any stale prep is void
    if (len == 0) return;
    // The stub owns slot STUB_TABLE_BANK, which tables_build() never
    // writes, so every cart can be prepared while the stub is served.
    tables_build(banks_for(len));
    s_prep_scheme = scheme;
    s_prep_len    = len;
}

void bus_serve(bus_scheme_t scheme, uint32_t len) {
    uint32_t banks = banks_for(len);
    uint32_t p2 = 1;
    while (p2 < banks) p2 <<= 1;
    if (p2 > MC_MAX_BANKS) p2 = MC_MAX_BANKS;

    serve_stop();
    // Skip the build only on an exact match with what was prepared under
    // reset; anything else (power-on fast path, a re-mount) builds here.
    if (s_prep_len == 0 || s_prep_len != len || s_prep_scheme != scheme)
        tables_build(banks);
    s_prep_len = 0;                                // consumed

    s_ctl.scheme       = scheme;
    s_ctl.use_stub     = 0;
    s_ctl.bank_mask    = (scheme == BUS_SCHEME_BANK16K) ? (p2 - 1) : 0;
    s_ctl.current_bank = 0;
    s_ctl.serve        = 1;
    __dmb();
    s_ctl.epoch++;
    serve_start(table_for_bank(0));
}

// The stub's own table slot is never written by tables_build(); the
// s_stub IMAGE buffer is reused as the bank-0 bounce, which is safe
// because the stub table has already been built from it by then.
void bus_serve_stub(void) {
    serve_stop();
    s_prep_len = 0;              // a new selection voids any prepared build
    s_ctl.tbl_ok = 1;
    perm_build(table_for_bank(STUB_TABLE_BANK), s_stub, 0xC000u);
    table_verify(table_for_bank(STUB_TABLE_BANK), s_stub);

    s_ctl.scheme       = BUS_SCHEME_FLAT;
    s_ctl.use_stub     = 1;
    s_ctl.bank_mask    = 0;
    s_ctl.current_bank = 0;
    s_ctl.serve        = 1;
    __dmb();
    s_ctl.epoch++;
    serve_start(table_for_bank(STUB_TABLE_BANK));
}

// Going idle does NOT void a prepared build. An insertion-style cart is
// idled on purpose: the slot has to look empty while BASIC boots to
// READY, and only then does select.c serve it and raise CART*. If idling
// voided the prep, that serve would rebuild the tables inline --
// milliseconds of SRAM hammering starting microseconds before the FIRQ
// that starts the game, starving the snoop across exactly the window in
// which the cart writes $FF22 and the SAM registers once and never
// again. The renderer would then never learn the mode (a green screen
// while the CoCo runs the game fine).
//
// Nothing can touch s_main between the prepare and the serve: select.c
// is busy for the whole window, so no other selection can start, and
// bus_serve_prepare()/bus_serve_stub() both reset s_prep_len themselves.
void bus_idle(void) {
    serve_stop();
    s_ctl.serve = 0;
    __dmb();
    s_ctl.epoch++;
}

void bus_stub_arm(void)    { s_ctl.stub_seen = 0; s_ctl.stub_armed = 1; }
void bus_stub_disarm(void) { s_ctl.stub_armed = 0; }
bool bus_stub_seen(void)   { return s_ctl.stub_seen != 0; }
uint32_t bus_strobe_count(void) { return s_ctl.strobe_count; }
uint8_t  bus_current_bank(void) { return (uint8_t)s_ctl.current_bank; }
uint32_t bus_write_count(void)  { return s_ctl.write_count; }
uint32_t bus_capture_behind(void) { return s_ctl.capture_behind; }
uint32_t bus_scs_disagree(void) { return s_ctl.scs_disagree; }

// Audio event ring accessors, for the HSTX island IRQ (audio.c). Exposed
// rather than shared as globals so the ring stays owned by bus.c, which is
// where the producer contract is documented.
uint32_t __not_in_flash_func(bus_audio_take)(uint32_t *ev) {
    uint32_t rd = s_aud_rd;
    if (rd == s_aud_wr) return 0;
    *ev = s_aud[rd & (AUD_RING - 1)];
    s_aud_rd = rd + 1;
    return 1;
}
uint32_t bus_audio_depth(void)   { return s_aud_wr - s_aud_rd; }
uint32_t bus_audio_ring_size(void) { return AUD_RING; }
uint32_t __not_in_flash_func(bus_audio_lap)(void) { return s_ctl.lap_count; }

// Core 1 only stamps audio events when something is going to consume
// them. In DVI mode the island IRQ does not exist, so without this the
// hot loop would pay for every DAC write and fill a ring nobody drains.
void bus_audio_enable(bool on) { s_ctl.aud_on = on ? 1u : 0u; }

// The multicart-only strobe-pipeline fields are reported as zero; this
// board snoops the full bus instead.
void bus_debug(bus_debug_t *d) {
    d->strobe_raw = d->fifo_level = 0;
    d->dma_ctrl = d->dma_read = d->dma_write = d->dma_count = 0;
    d->strobe_count    = s_ctl.strobe_count;
    d->current_bank    = s_ctl.current_bank;
    // The delay actually programmed into the capture SM, so /api/status
    // agrees with the OLED during scope tuning.
    d->strobe_delay_ns = s_delay_ns;
    // Snoop laps. One per bus cycle by construction (the capture SM paces
    // them), so sampling this twice a second apart must give ~1117 ns.
    // Anything else means core 1 is missing cycles.
    d->lap_count    = s_ctl.lap_count;
    // Window reads core 1 OBSERVED, i.e. the number of serves the
    // hardware chain was asked for. There is no CPU in the serve path to
    // count what it actually delivered, so this is the demand side; the
    // supply side is proven by serve_stall staying zero.
    d->drive_count  = s_ctl.window_reads;
    d->serving      = s_ctl.serve;

    // The sampler's RX FIFO overflowing is the ONLY way a serve word can
    // be lost (the push is non-blocking so it cannot freeze SEL and take
    // the snoop down with it). PIO latches that in FDEBUG_RXSTALL; latch
    // it into a counter here and clear, so the panel shows a running
    // total rather than a level.
    uint32_t fdebug_bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + SM_SAMPLER);
    if (SAMP_PIO->fdebug & fdebug_bit) {
        SAMP_PIO->fdebug = fdebug_bit;          // write-1-to-clear
        s_ctl.serve_stall++;
    }
    d->serve_stall  = s_ctl.serve_stall;
    d->pair_drop    = s_ctl.pair_drop;
    d->aud_drop     = s_ctl.aud_drop;
    d->tag_slip     = s_ctl.tag_slip;
    d->reset_vecs   = s_ctl.reset_vecs;
    d->tag_guard    = s_ctl.tag_guard;
    d->tag_retry    = s_ctl.tag_retry;

    // Lap anatomy + on-device table verification (see bus_ctl_t).
    d->lap_wait_ns  = s_ctl.lap_wait_ns;
    d->lap_body_ns  = s_ctl.lap_body_ns;
    d->lap_bmax_ns  = s_ctl.lap_bmax_ns;
    d->tbl_ok       = s_ctl.tbl_ok;
    d->snoop_lag    = s_ctl.snoop_lag;
    d->lag_now      = s_ctl.lag_now;
    d->lag_hi       = s_ctl.lag_hi;
    d->pair_hi      = s_ctl.pair_hi;
    d->serve_fifo   = s_ctl.serve_fifo_max;
    d->tag_diag     = s_ctl.tag_diag;
    d->echo_ok      = s_ctl.echo_ok;
    d->echo_bad     = s_ctl.echo_bad;
    d->echo_diag    = s_ctl.echo_diag;
    d->anchor_fix   = s_ctl.anchor_fix;
    d->anchor_dist  = s_ctl.anchor_dist;
    d->sam_ty       = s_ctl.sam_ty;
    d->ty_flips     = s_ctl.ty_flips;

    // What the serve chain ACTUALLY looked up last: CH_DATA's read
    // address post-increments, so this is (last entry address + 2).
    // Reported as the byte offset into the table buffer. An offset
    // beyond the table region means the sampler's pushed word is wrong
    // ON HARDWARE -- the one fault the host simulation cannot see.
    d->serve_look   = (uint32_t)(dma_hw->ch[s_dma_data].read_addr
                                 - (uint32_t)(uintptr_t)s_main);
}

// Core-0 half of the snoop: drain the video-register ring. Called from
// the main pump loop. The bound exists purely to guarantee termination
// against a producer running flat out; draining a full ring costs ~40 us
// on core 0, which has no deadline.
void bus_pump(void) {
    uint32_t rd = s_pair_rd;
    // Occupancy BEFORE draining: if the ring is the thing filling up,
    // this creeps toward PAIR_RING even while pair_drop stays 0.
    uint32_t occ = s_pair_wr - rd;
    if (occ > s_ctl.pair_hi) s_ctl.pair_hi = occ;
    for (uint32_t i = 0; i < PAIR_RING && rd != s_pair_wr; i++) {
        uint32_t p = s_pair[rd & (PAIR_RING - 1)];
        rd++;
        video_note_reg((uint16_t)(p & 0xFFFFu), (uint8_t)(p >> 16));
    }
    s_pair_rd = rd;
}

// ------------------------------------------------------------- core 1

// Core 1: the SNOOP pairing loop. It has NO serve duty at all -- the
// serve path is sampler SM -> DMA -> serve SM, entirely in hardware.
//
// PACED BY THE CAPTURE SM, which pushes exactly one word per bus cycle.
// There is no E spin: a software loop cannot be phase-locked to E when
// its body length sits inside the model error of the 1117 ns period
// (it locks to 2x or free-runs). The hardware sets the cadence and the
// loop simply follows it.
//
// INDEX pairing against the deep rings (see their comment): capture k
// pairs with follower group k regardless of how far behind this loop
// runs. There is no deadline in here -- only the throughput requirement
// that the average lap beat 1117 ns, and the lap-anatomy instrument
// publishes the actual margin every 4096 laps.
static void __no_inline_not_in_flash_func(bus_loop)(void) {
    uint32_t epoch = ~0u;
    uint32_t serve = 0, scheme = 0, bank_mask = 0;
    uint32_t cur_bank = 0;             // local copy for the serve echo
    uint32_t sam_ty   = 0;             // SAM map type: 1 = all-RAM
    uint32_t cap_rd = 0;               // capture ring consumer index
    uint32_t laps = 0;
    // Hoisted out of the hot loop: aud_on is set once, before core 1 is
    // launched, and the setting that drives it is staged behind a reboot.
    const uint32_t aud_on = s_ctl.aud_on;
    // PIA1 CRB bit 2: 1 = $FF22 is the output register, 0 = it is the
    // data direction register. Reset leaves it clear.
    uint32_t pb_is_data = 0;
    // Previous read was of $FFFE: half of a reset vector fetch.
    uint32_t saw_fffe = 0;

    // ---- lap anatomy instrumentation --------------------------------
    // Instruction counting has been wrong on every core-1 loop this
    // board has had, so the loop measures ITSELF: each lap is split into
    // WAIT (blocked on the capture arriving -- the idle part) and BODY
    // (everything else -- the part we pay for), accumulated and
    // published every 4096 laps.
    //
    //   body >> wait  -> the loop is compute/contention-bound: the work
    //                    or the SRAM arbitration is the problem.
    //   wait dominant, lap still > 1117 -> captures are NOT arriving
    //                    once per cycle: the capture SM or its DMA is
    //                    the problem, and no loop tuning will help.
    //
    // SysTick counts DOWN at clk_sys, core-local, ~2 cycles to read.
    // Cost: three reads + four adds per lap, and the cost is itself
    // inside the measurement.
    systick_hw->csr = 0;
    systick_hw->rvr = 0x00FFFFFFu;
    systick_hw->cvr = 0;
    systick_hw->csr = 5;               // enable, CPU clock, no interrupt
    uint32_t acc_wait = 0, acc_body = 0, body_max = 0;
    uint32_t t_prev = systick_hw->cvr;

    // ---- PAIRING: pointer-anchored, phase-agnostic --------------------
    // fol_rd walks the follower ring += 4 per capture, anchored to the
    // DMA's LIVE write pointer and inheriting whatever word phase that
    // pointer actually has. Computing the group's slot arithmetically
    // from a cycle count instead would silently assume group boundaries
    // sit at word-phase 0 forever; one word-level stream slip shifts the
    // ring's phase and such arithmetic can never land on a group again.
    // The pointer anchor is immune by construction: it follows the ring
    // wherever the ring actually is.
    //
    // ANCHOR RULE: anchor ONLY at a fresh blocking arrival with zero
    // backlog. At that instant (~fall+990 ns) the follower finished this
    // cycle's group at ~fall+520 and starts the next at ~nextfall+170,
    // so the write pointer sits just past the group that pairs with the
    // capture in hand: fol_rd = pos - 4. In any other phase the pointer
    // can be mid-sweep or one group ahead, and both errors are
    // undetectable downstream.
    //
    // RE-ANCHORING: on a tag failure the backlog is consumed WITHOUT
    // mirroring (each skipped write counted in tag_slip) until a lag-0
    // arrival re-anchors -- bounded loss of the 1-2 entries actually in
    // flight, never a drained burst.
    uint32_t fol_rd   = 0;             // follower ring consumer, in WORDS
    uint32_t anchored = 0;             // 0 = consuming toward a re-anchor
    uint32_t lag_max  = 0;
    uint32_t fresh    = 0;             // last arrival was blocked-for; set
                                       // per lap, read by the publish
                                       // block when closing that lap

    // Discard whatever the capture DMA banked while core 1 was being
    // launched: those cycles predate this loop and their addresses were
    // never held anywhere, so they are not losable data.
    while (s_capring[cap_rd & (CAP_RING_WORDS - 1)]) {
        s_capring[cap_rd & (CAP_RING_WORDS - 1)] = 0;
        cap_rd++;
    }

    for (;;) {
        // Close the PREVIOUS lap's body here -- the loop's `continue`
        // paths all come back through this point, so every path is
        // measured. Publish as 4096-lap averages.
        {
            uint32_t t_now = systick_hw->cvr;
            uint32_t body  = (t_prev - t_now) & 0x00FFFFFFu;
            acc_body += body;
            if (body > body_max) body_max = body;
            t_prev = t_now;
            if ((laps & 0xFFFu) == 0u) {
                // SysTick counts at clk_sys, so nanoseconds per tick is
                // not a constant (8 at 125 MHz, 4 at 250).
                s_ctl.lap_wait_ns = (acc_wait >> 12) * s_ns_per_tick;
                s_ctl.lap_body_ns = (acc_body >> 12) * s_ns_per_tick;
                s_ctl.lap_bmax_ns = body_max * s_ns_per_tick;
                // Snoop-ring lag and serve-chain health, sampled once per
                // 4096 laps so these APB reads never sit in the per-lap
                // path (they cost ~200 ns/lap there).
                {
                    uint32_t lag = (snoop_pair_pos(s_cap_dma_a, s_cap_dma_b,
                                                   s_capring) - cap_rd)
                                   & (CAP_RING_WORDS - 1u);
                    if (lag > lag_max) lag_max = lag;
                    s_ctl.snoop_lag = lag_max;
                    // Is the ring saturated NOW, or was it once? A cart
                    // that runs clean for seconds and then degrades is
                    // the signature of something filling rather than of
                    // a per-event fault, and the high-water mark alone
                    // cannot tell those apart.
                    s_ctl.lag_now = lag;
                    if (lag >= (CAP_RING_WORDS - (CAP_RING_WORDS >> 2)))
                        s_ctl.lag_hi++;

                    // Only meaningful while a cart is mounted: before
                    // that the serve SM is disabled and the DMA fills its
                    // FIFO to 4 unread, which would report as "running
                    // behind" when it is just idle.
                    if (s_ctl.serve) {
                        uint32_t lv = (CAP_PIO->flevel >> (SM_SERVE * 8))
                                      & 0xFu;
                        if (lv > s_ctl.serve_fifo_max)
                            s_ctl.serve_fifo_max = lv;
                    }
                }
                acc_wait = acc_body = body_max = 0;
            }
        }

        // ---- pace: block until capture k lands -----------------------
        // ONE capture per lap, nothing dropped: any backlog just means
        // the next laps run with zero wait until it clears.
        // `fresh` records whether the slot was EMPTY when we got here --
        // i.e. whether the arrival we are about to consume is one we
        // actually BLOCKED for. Only such an arrival is observed within
        // spin granularity (~50 ns) of the DMA write, which is the sole
        // condition under which the follower pointer provably sits just
        // past this capture's own group. An arrival found already
        // waiting proves nothing about phase: consuming it may happen at
        // any point in the bus cycle, including mid-sweep, and anchoring
        // there lands words off a group boundary.
        uint32_t cw;
        fresh = (s_capring[cap_rd & (CAP_RING_WORDS - 1)] == 0u);
        while ((cw = s_capring[cap_rd & (CAP_RING_WORDS - 1)]) == 0) { }

        // The anchor's pointer read happens HERE, at the instant of spin
        // exit -- not after the timing block and consume below. Those
        // cost 200-240 ns, which would land the read at ~fall+1280
        // against the next follower group's first DMA write at
        // ~fall+1297: a straddle. And because this loop is phase-locked
        // to the bus, a read that lands late once lands late on EVERY
        // retry -- a permanent failure to anchor on unlucky phase
        // relationships. Read at spin exit the margin is ~200 ns.
        //
        // The same phase-pinned read serves the periodic anchor
        // VERIFICATION below.
        uint32_t fpos_now = 0;
        uint32_t checking = (!anchored | ((laps & 0xFFu) == 0u)) & fresh;
        if (checking)
            fpos_now = snoop_pair_pos(s_fol_dma_a, s_fol_dma_b, s_folring);

        // Lap anatomy: everything since t_prev was WAIT, everything until
        // the bottom of the loop is BODY.
        {
            uint32_t t_now = systick_hw->cvr;
            acc_wait += (t_prev - t_now) & 0x00FFFFFFu;
            t_prev = t_now;
        }
        s_capring[cap_rd & (CAP_RING_WORDS - 1)] = 0;
        cap_rd++;

        // ---- (re-)anchor when needed, at a lag-0 arrival only --------
        if (!anchored) {
            if (fresh) {
                // fpos_now was captured at spin exit, inside the provable
                // window: `- 4` names this capture's own group, at the
                // ring's true word phase.
                fol_rd = (fpos_now - 4u) & (FOL_RING_WORDS - 1u);
                anchored = 1;
            } else {
                // Found waiting: arrival time unknown, phase unprovable.
                // Skip its mirror, count the loss, and come back -- a
                // skip lap is ~100 ns, so the loop returns to the spin
                // long before the next arrival and the next lap is
                // fresh. Convergence in one lap, guaranteed by rates.
                s_ctl.tag_slip++;
                s_ctl.tag_retry++;
                laps++;
                s_ctl.lap_count = laps;
                continue;
            }
        }

        // Follower group for THIS capture, at whatever phase the ring
        // really has; advance by one group per capture.
        uint32_t fi = fol_rd;
        uint32_t w0 = s_folring[fi & (FOL_RING_WORDS - 1)];
        uint32_t w1 = s_folring[(fi + 1) & (FOL_RING_WORDS - 1)];
        uint32_t w2 = s_folring[(fi + 2) & (FOL_RING_WORDS - 1)];
        uint32_t w3 = s_folring[(fi + 3) & (FOL_RING_WORDS - 1)];
        fol_rd = (fi + 4u) & (FOL_RING_WORDS - 1u);

        // ---- periodic anchor verification, phase-pinned --------------
        // Catches the slip the tag guard structurally cannot: a whole
        // group added or lost leaves tags reading 0,1,2,3 and mirrors
        // one-cycle-stale addresses forever. The physical RESET button
        // can cause exactly that, and unlike a cart change it bumps no
        // epoch, so nothing else would ever notice.
        //
        // At a fresh blocking arrival the follower has finished this
        // capture's group and cannot start the next until the following
        // cycle, so the write pointer nominally sits 4 words past the
        // group just read. The OBSERVED distance is published rather than
        // only a verdict, and only unambiguous values act: d == 0 means
        // we are about to read a group the DMA has not written yet
        // (consumer ahead); large d means we lag far enough that the
        // group may already have been overwritten -- half the ring is a
        // generous line. Between those, leave the anchor alone and let
        // the tag guard police word alignment. A check that fires on
        // every small phase excursion re-anchors constantly, and each
        // re-anchor skips a capture.
        if (checking && anchored) {
            uint32_t d = (fpos_now - fi) & (FOL_RING_WORDS - 1u);
            s_ctl.anchor_dist = d;
            if (d == 0u || d > (FOL_RING_WORDS / 2u)) {
                anchored = 0;
                s_ctl.anchor_fix++;
            }
        }

        laps++;
        s_ctl.lap_count = laps;

        // Tag guard: a paired group must read tags 0,1,2,3. A failure
        // means the streams slipped relative to each other; drop the
        // anchor and re-establish it at the next lag-0 arrival. The
        // capture in hand is lost (counted -- the acceptance bar is that
        // this stays zero), but the loss is bounded to the entries in
        // flight, never a drained backlog.
        if (((w0 & 3u) | ((w1 & 3u) << 2) | ((w2 & 3u) << 4) |
             ((w3 & 3u) << 6)) != 0xE4u) {
            s_ctl.tag_slip++;
            s_ctl.tag_guard++;
            // Diagnosis for the panel: the tag byte actually read and
            // the follower pointer's word phase at failure. This says
            // whether the ring lost word alignment (phase != 0) or the
            // anchor mis-aimed (phase 0, tags rotated).
            s_ctl.tag_diag =
                ((snoop_pair_pos(s_fol_dma_a, s_fol_dma_b, s_folring) & 3u)
                 << 8) |
                ((w0 & 3u) | ((w1 & 3u) << 2) | ((w2 & 3u) << 4) |
                 ((w3 & 3u) << 6));
            anchored = 0;
            continue;
        }

        uint32_t addr = (uint32_t)s_tlo[sweep_idx8(w0, w1)]
                      | (uint32_t)s_thi[sweep_idx8(w2, w3)];

        uint32_t e = s_ctl.epoch;
        if (e != epoch) {
            epoch     = e;
            serve     = s_ctl.serve;
            scheme    = s_ctl.scheme;
            bank_mask = s_ctl.bank_mask;
            cur_bank  = 0;             // epoch change resets to bank 0
            // DROP THE ANCHOR on every selection. A cart change pulses
            // the CoCo's RESET line, and E glitches across that pulse
            // can add or lose a sweep -- a WHOLE-GROUP slip, which the
            // tag guard cannot see because the group still reads tags
            // 0,1,2,3. The pairing would then stay silently off by a
            // cycle for good, and since the stub's $FF5F magic is decoded
            // FROM the snoop, select.c would miss the handshake too.
            anchored = 0;
        }

        if (sweep_rw(w0)) {                // a read: nothing to snoop
            // A 6809 reset fetches its vector from $FFFE then $FFFF on
            // CONSECUTIVE cycles, and nothing else reads those addresses
            // in normal operation. So one compare separates two faults
            // that both look like "it crashed": the machine being RESET
            // versus the program returning to its own menu. It is the
            // PAIR that identifies a reset, not $FFFE alone: the 6809
            // drives high addresses during its internal dead cycles, so
            // counting bare $FFFE reads reports thousands of "resets" a
            // second.
            if (saw_fffe && addr == 0xFFFFu) s_ctl.reset_vecs++;
            saw_fffe = (addr == 0xFFFEu);
            if (addr >= 0xC000u && addr <= 0xFEFFu) {
                s_ctl.window_reads++;

                // ---- SERVE ECHO: what the bus ACTUALLY carried -------
                // The capture SM samples D0-D7 at E-rise+400 ns, which is
                // INSIDE the window the serve SM drives (~fall+610 to
                // E-fall). So on a served read, this captured byte IS the
                // byte that reached the CoCo. Compare it against the very
                // table entry the DMA looked up:
                //   match  -> the serve path is correct end to end; look
                //             at the cart/select flow instead.
                //   differ -> serving is wrong, and echo_diag names the
                //             address, the byte owed and the byte seen.
                // (A mis-paired capture also reports here as a wrong
                // byte, so read it alongside tagslip.)
                //
                // Sampled 1-in-64, because the check is not free: it
                // reads the SAME table the serve DMA is reading, so
                // measuring every read adds contention to the very path
                // under test. 1-in-64 still yields thousands of samples
                // per refresh -- ample for a rate.
                if (serve && (s_ctl.window_reads & 63u) == 0u) {
                    uint16_t ent = table_for_bank(cur_bank)
                                       [perm_idx(addr & MC_ADDR_MASK)];
                    if ((ent >> 8) == 0xFFu) {      // we owed a drive
                        uint8_t want = (uint8_t)ent;
                        uint8_t got  = (uint8_t)cw;
                        if (want == got) {
                            s_ctl.echo_ok++;
                        } else {
                            s_ctl.echo_bad++;
                            s_ctl.echo_diag = (addr << 16)
                                            | ((uint32_t)want << 8) | got;
                        }
                    }
                }
            }
            continue;
        }

        uint32_t data = cw & 0xFFu;

        // ---- SAM map type, OBSERVED ONLY --------------------------
        // A real cartridge is gated by CTS*, which the SAM asserts only
        // for $C000-$FEFF in map type 0. This board approximates CTS*
        // with A14 & A15 (CTS* is E-qualified and the sampler runs
        // during E-low), and that approximation is wrong in map type 1,
        // where $C000-$FEFF becomes RAM. The TY bit is set by any access
        // to $FFDF and cleared by $FFDE; the counters let the panel say
        // whether a given cart uses 64K mode at all.
        //
        // The serve path is deliberately NOT muted from here. TY is
        // sticky -- one write to $FFDF with no matching $FFDE, entirely
        // plausible during BASIC's RAM sizing at boot, would mute serving
        // permanently -- and PIO enable/disable/restart sequences are
        // long APB operations that stall the lap and back the snoop
        // ring up. Any future muting must be cheap and self-recovering.
        if (addr >= 0xFFDEu) {
            uint32_t want_ty = (addr == 0xFFDFu);
            if (want_ty != sam_ty) {
                sam_ty       = want_ty;
                s_ctl.sam_ty = want_ty;
                s_ctl.ty_flips++;
            }
        }

        // ---- bank register / stub magic -----------------------------
        // The repoint may ONLY happen while the sampler is parked (see
        // sampler_retable_parked). The helper waits for the next fresh
        // E-rise, so the new bank is live from the SECOND cycle after
        // the strobe -- the following cycle's sweep has already read X.
        // That latency is fine for the RAM-loader convention every
        // banked cart here uses (bank-switch code runs from RAM, so the
        // fetches in those two cycles are not served by us). Native
        // banked ROMs that switch under their own feet would need
        // next-cycle effect, which this design does not provide.
        //
        // The wait also delays this loop up to ~2 bus cycles; the
        // capture ring absorbs that. Bank strobes are a handful per load.
        if (addr >= MC_REG_BANK_BASE && addr <= MC_REG_BANK_END) {
            if (s_ctl.stub_armed && addr == MC_STUB_MAGIC_ADDR) {
                s_ctl.stub_seen  = 1;
                s_ctl.stub_armed = 0;
            } else if (serve && scheme == BUS_SCHEME_BANK16K) {
                uint32_t bank = data & bank_mask;
                cur_bank = bank;               // serve echo reads this
                s_ctl.current_bank = bank;
                sampler_retable_parked_inline(
                        (uint32_t)(uintptr_t)table_for_bank(bank));
            }
            s_ctl.strobe_count++;
        }

        // MIRROR ON CORE 1. One store costs tens of ns on a quarter of
        // laps and cannot be stalled by anything core 0 does. Done on
        // core 0 instead, the mirror inherits every core-0 stall -- an
        // OLED refresh over 100 kHz I2C blocks ~90 ms -- and real screen
        // bytes are lost to a display update.
        s_mirror[addr] = (uint8_t)data;
        s_ctl.write_count++;

        // SAM writes go in the audio EVENT LOG too, not just the video
        // ring: a cart that moves the display base or flips the map type
        // mid-run (the diagnostic cart does both while testing memory)
        // is otherwise impossible to reconstruct from the log.
        if (aud_on && addr >= MC_REG_SAM_BASE && addr <= MC_REG_SAM_END) {
            uint32_t wr2 = s_aud_wr;
            if ((wr2 - s_aud_rd) < AUD_RING) {
                s_aud[wr2 & (AUD_RING - 1)] =
                    (laps << 12) | (6u << 8) | (addr - MC_REG_SAM_BASE);
                s_aud_wr = wr2 + 1;
            }
        }

        // $FF23 bit 2 decides what $FF22 IS. Track it here so the video
        // push below can tell a mode write from a direction-register
        // write; PIA1 is mirrored through $FF20-$FF3F, so match the range.
        if ((addr & 0xFFE0u) == 0xFF20u && (addr & 3u) == 3u)
            pb_is_data = (data & 0x04u) != 0u;

        // A write to $FF22 while CRB bit 2 is CLEAR goes to the Port B
        // DATA DIRECTION register. It does not touch the output register,
        // and the VDG reads the output register -- so it is not a mode
        // change and must not be forwarded as one. The diagnostic cart's
        // sound test writes the DDR thousands of times a second; applied
        // as modes those drop the machine into alphanumeric and back and
        // leave whatever the last one said on screen. Same rule as the
        // DAC latch and CRA bit 2 in audio.c.
        if ((addr == MC_REG_PIA1_DB && pb_is_data) ||
            (addr >= MC_REG_SAM_BASE && addr <= MC_REG_SAM_END)) {

        uint32_t wr = s_pair_wr;
        if ((wr - s_pair_rd) < PAIR_RING) {
            s_pair[wr & (PAIR_RING - 1)] = addr | (data << 16);
            s_pair_wr = wr + 1;            // entry before index: both
                                           // volatile, so stores stay
                                           // ordered
        } else {
            // A video-register write was lost. With the ring carrying
            // only mode/SAM writes this should be impossible; nonzero
            // means a latched display state may be stale (a permanent
            // wrong mode), so it stays a hard-fail counter.
            s_ctl.pair_drop++;
        }
        }

        // ---- AUDIO EVENTS ------------------------------------------
        // Separate ring, deliberately. Audio is a much higher event rate
        // than mode changes (a DAC playback loop runs 8-22 k writes/s
        // against a handful per mode change), and it must never be able
        // to evict a $FF22/SAM write -- those latch display state, and a
        // lost one is a permanently wrong screen. Sharing one ring would
        // couple them; two rings cannot.
        //
        // Event word: lap[19:0]<<12 | reg[3:0]<<8 | data[7:0]. The lap
        // counter is core 1's own bus-cycle count -- already locked to
        // the CoCo's 1.117 us cycle, so it IS the audio timebase and no
        // separate timestamp source is needed. 20 bits wraps every 1.17 s;
        // the consumer unwraps against its own running count and drains
        // every island line (~33 us), so it can never alias.
        //
        // ONE range compare rejects every RAM write before any of the
        // register tests run; a body that overruns the bus cycle loses
        // the whole lap and whatever register write it carried, and a
        // lost $FF20 write is audible on any DAC audio.
        //
        // The PIAs are MIRRORED: PIA0 repeats every 4 bytes through
        // $FF00-$FF1F and PIA1 through $FF20-$FF3F, because only the low
        // two address lines reach them. Plenty of programs use a mirror
        // ($FF2x especially), so match the ranges, not the canonical
        // addresses. $FF01 and $FF03 differ in one bit, so a single
        // masked compare covers both, and reg falls out of the address
        // without a branch. $FF22 goes to BOTH rings: the video ring
        // needs its mode bits, audio needs its bit 1.
        if (aud_on && addr >= 0xFF00u && addr < 0xFF40u &&
            ((addr & 0x20u) || (addr & 1u))) {
            uint32_t r    = addr & 3u;
            uint32_t reg  = (addr & 0x20u) ? r            // PIA1: PA,CRA,PB,CRB
                                           : (4u + (r >> 1)); // PIA0: CRA,CRB
            uint32_t wr = s_aud_wr;
            if ((wr - s_aud_rd) < AUD_RING) {
                s_aud[wr & (AUD_RING - 1)] =
                    (laps << 12) | (reg << 8) | data;
                s_aud_wr = wr + 1;              // entry before index
            } else {
                // Audio only. Counted, never fatal, and it NEVER stalls
                // core 1 -- a dropped sample is a click, a stalled lap is
                // a dropped screen byte. Expected to move only during a
                // flash erase, which holds off the consumer IRQ for tens
                // of ms.
                s_ctl.aud_drop++;
            }
        }
    }
}

void __not_in_flash_func(bus_core1_main)(void) {
    (void)save_and_disable_interrupts();
    bus_loop();
}
