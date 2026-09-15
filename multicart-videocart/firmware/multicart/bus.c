// ======================================================================
// bus.c — core-1 CoCo bus service.
//
// The hot loop is the PicoROM recipe adapted to direct (bufferless)
// drive: every lap it reads all GPIOs once, looks the address up in the
// active image, writes the byte to D0..D7, and sets the D pindirs from
// the live !CTS level. Against these bus deadlines:
//
//   - data valid deadline: ~500 ns after !CTS asserts (0.89 MHz bus;
//     the loop settles data+direction in <= 2 laps, and keeps refreshing
//     as the address lines settle);
//   - release deadline: next device drives >= ~500 ns after !CTS clears;
//     the loop tristates within one lap.
//
// LAP TIME, MEASURED: ~374 ns per lap at 150 MHz with instrumentation in
// place, call it ~300 ns bare. Instruction counting predicted 150-250 ns
// and was wrong; measure, do not estimate.
//
// That matters because !CTS is asserted for only ~636 ns. We drive
// ~160 ns after the sample that sees !CTS low, and the CoCo latches
// ~478 ns after !CTS asserts, so a sample has to land in roughly the
// first 318 ns of the window. At 374 ns spacing that is luck, ~85% of the
// time, and the misses read back as 0xFF because we simply have not
// driven yet. On a bare CoCo the remaining margin covers it. Behind a
// Multi-Pak, !CTS arrives ~40 ns late through a '367 and a '139, and
// there is no margin left: intermittent 0xFF reads, immune to drive
// strength, immune to WiFi, address-independent. main() therefore runs
// this board at 250 MHz, which brings the lap to ~224 ns -- SHORTER than
// the 318 ns window, so a sample landing in time is guaranteed on every
// access.
//
// RAM RESIDENCY IS LOAD-BEARING. Core 1 runs with interrupts off and
// every instruction + datum in SRAM (__not_in_flash_func + static
// arrays; the SDK GPIO/PIO helpers used below are static-inline), so
// core 0 may erase/program flash at any time without pausing core 1.
// `make audit` verifies this on the linked image.
// ======================================================================

#include "bus.h"
#include "config.h"
#include "bus_pio.pio.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "hardware/dma.h"
#include "hardware/structs/bus_ctrl.h"

#include <string.h>

#define BUS_PIO         pio0
#define BUS_SM_STROBE   0

// Fixed-role image buffers (see bus.h for the safety invariant): the
// 128 KB main buffer holds the ROM being served; the 16 KB stub buffer
// holds the cold-boot stub window. Alignment lets bank arithmetic use
// simple adds. Placed in the main SRAM striped region (plenty of
// bandwidth for one byte read per ~70 ns against a 32-bit-wide bus).
static uint8_t s_main[MC_IMAGE_BUF_BYTES] __attribute__((aligned(4)));
static uint8_t s_stub[MC_BANK_BYTES]      __attribute__((aligned(4)));

// Latest bank/stub strobe word (A0..13 | D<<14), kept fresh in SRAM by a
// free-running DMA that drains the PIO RX FIFO. The core-1 serve loop
// reads THIS (SRAM, covered by the PROC1 bus-fabric priority) instead of
// the PIO FIFO (an APB read the priority does NOT cover) — so the loop
// never touches the APB and stays WiFi-immune even for BANKED images.
// Polling the FIFO from the loop instead lets WiFi DMA stall the lap and
// the CoCo latches a corrupted byte.
static volatile uint32_t s_strobe __attribute__((aligned(4)));
static int      s_strobe_dma = -1;
static uint32_t s_delay_ns;

// Control block shared between cores. Core 0 writes fields then bumps
// `epoch` (release order); core 1 samples `epoch` each lap and re-reads
// the block when it changes. All fields fit in single aligned words, so
// each individual read/write is atomic on M33.
typedef struct {
    volatile uint32_t epoch;
    volatile uint32_t serve;        // 0 = idle (never drive the bus)
    volatile uint32_t scheme;       // bus_scheme_t
    volatile uint32_t use_stub;     // 1 = serve s_stub, 0 = serve s_main
    volatile uint32_t bank_mask;    // (#banks-1) for BANK16K, else 0
    volatile uint32_t stub_armed;
    volatile uint32_t stub_seen;
    volatile uint32_t strobe_count;
    volatile uint32_t current_bank;
} bus_ctl_t;

static bus_ctl_t s_ctl;

void bus_init(uint32_t strobe_delay_ns) {
    s_delay_ns = strobe_delay_ns;
    // Address + !CTS as plain SIO inputs. No internal pull-downs ever
    // (RP2350 erratum E9). gpio_init() does NOT touch the pad
    // pulls, and the RP2350 RESETS with pull-down enabled on every pad
    // (PADS_BANK0 reset = PDE set) — so each pull must be cleared
    // explicitly or the E9 latch-up condition is live on every bus pin.
    for (int i = 0; i < MC_ADDR_BITS; i++) {
        gpio_init(MC_PIN_A0 + i);
        gpio_set_dir(MC_PIN_A0 + i, GPIO_IN);
        gpio_disable_pulls(MC_PIN_A0 + i);
    }
    gpio_init(MC_PIN_CTS);
    gpio_set_dir(MC_PIN_CTS, GPIO_IN);
    gpio_disable_pulls(MC_PIN_CTS);
    gpio_init(MC_PIN_STROBE);
    gpio_set_dir(MC_PIN_STROBE, GPIO_IN);
    gpio_pull_up(MC_PIN_STROBE);      // belt+suspenders atop external 4.7k
                                      // (pull-UP; also clears the PDE)

    // Data pins: SIO-owned, input until the hot loop drives them.
    for (int i = 0; i < 8; i++) {
        gpio_init(MC_PIN_D0 + i);
        gpio_set_dir(MC_PIN_D0 + i, GPIO_IN);
        gpio_disable_pulls(MC_PIN_D0 + i);
        gpio_set_slew_rate(MC_PIN_D0 + i, GPIO_SLEW_RATE_FAST);
        gpio_set_drive_strength(MC_PIN_D0 + i, GPIO_DRIVE_STRENGTH_12MA);
    }

    // Claim the SM before use. Without this the CYW43 WiFi driver —
    // which picks any FREE state machine at cyw43_arch_init() time via
    // pio_claim_free_sm_and_add_program_for_gpio_range() — could grab
    // this very SM and silently destroy the strobe capture.
    pio_sm_claim(BUS_PIO, BUS_SM_STROBE);
    uint off = pio_add_program(BUS_PIO, &bank_strobe_program);
    bank_strobe_program_init(BUS_PIO, BUS_SM_STROBE, off, strobe_delay_ns);
    pio_sm_set_enabled(BUS_PIO, BUS_SM_STROBE, true);

    // Free-running DMA: drain the strobe RX FIFO into s_strobe forever,
    // paced by the PIO RX DREQ. No read/write increment (fixed FIFO src,
    // fixed SRAM dst) so s_strobe always holds the LATEST strobe word.
    // The serve loop then reads SRAM, never the APB FIFO. Claim the
    // channel here (before WiFi init) for the same reason we claim the SM:
    // CYW43 grabs free DMA channels at cyw43_arch_init().
    s_strobe = 0;
    s_strobe_dma = dma_claim_unused_channel(true);
    dma_channel_config dc = dma_channel_get_default_config(s_strobe_dma);
    channel_config_set_read_increment(&dc, false);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_dreq(&dc, pio_get_dreq(BUS_PIO, BUS_SM_STROBE, false));
    dma_channel_configure(s_strobe_dma, &dc,
                          (void *)&s_strobe,               // dst (SRAM)
                          &BUS_PIO->rxf[BUS_SM_STROBE],     // src (RX FIFO)
                          0xFFFFFFFFu,                      // ~unbounded
                          true);                            // start now

    // Core 1 (PROC1) runs the cycle-tight bus-serve loop; give it top
    // priority on the SRAM/bus fabric so the CYW43 WiFi DMA can't stall a
    // serve read past the CoCo's ~500 ns data deadline. Without this,
    // enabling WiFi injects DMA bursts that occasionally delay the read
    // and the CoCo latches a stale byte -> ROM crashes, only with WiFi
    // on. DMA still gets every cycle core 1 isn't using, so WiFi
    // throughput is unaffected in practice.
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC1_BITS;

    memset((void *)&s_ctl, 0, sizeof(s_ctl));
}

uint8_t *bus_staging(void)      { return s_main; }
uint32_t bus_staging_bytes(void) { return MC_IMAGE_BUF_BYTES; }
uint8_t *bus_stub_staging(void) { return s_stub; }

// Nothing to pre-build: bus_serve() below is already just a pointer flip,
// and this board's serve path reads the image directly rather than from a
// permuted table. Present only so the shared selection code can call it.
void bus_serve_prepare(bus_scheme_t scheme, uint32_t len) {
    (void)scheme; (void)len;
}

void bus_serve(bus_scheme_t scheme, uint32_t len) {
    uint32_t banks = (len + MC_BANK_BYTES - 1) / MC_BANK_BYTES;
    if (banks < 1) banks = 1;
    // bank_mask wants a power of two count; round up. Non-power-of-two
    // images just alias their top banks, which is what real latch
    // hardware did too.
    uint32_t p2 = 1;
    while (p2 < banks) p2 <<= 1;

    s_ctl.scheme       = scheme;
    s_ctl.use_stub     = 0;
    s_ctl.bank_mask    = (scheme == BUS_SCHEME_BANK16K) ? (p2 - 1) : 0;
    s_ctl.current_bank = 0;
    s_ctl.serve        = 1;
    __dmb();
    s_ctl.epoch++;
}

void bus_serve_stub(void) {
    s_ctl.scheme       = BUS_SCHEME_FLAT;
    s_ctl.use_stub     = 1;
    s_ctl.bank_mask    = 0;
    s_ctl.current_bank = 0;
    s_ctl.serve        = 1;
    __dmb();
    s_ctl.epoch++;
}

void bus_idle(void) {
    s_ctl.serve = 0;
    __dmb();
    s_ctl.epoch++;
}

// Bump epoch so bus_loop re-reads the control block (arm is not
// otherwise an epoch-changing event).
void bus_stub_arm(void)    { s_ctl.stub_seen = 0; s_ctl.stub_armed = 1;
                             __dmb(); s_ctl.epoch++; }
void bus_stub_disarm(void) { s_ctl.stub_armed = 0; __dmb(); s_ctl.epoch++; }
bool bus_stub_seen(void)   { return s_ctl.stub_seen != 0; }
uint32_t bus_strobe_count(void) { return s_ctl.strobe_count; }
uint8_t  bus_current_bank(void) { return (uint8_t)s_ctl.current_bank; }

// NOTE: never add a CPU-side drain of the strobe RX FIFO alongside the
// DMA. The DMA is DREQ-credit paced: a CPU pop steals a word the DMA
// holds a credit for, the DMA then reads the EMPTY FIFO (undefined
// data) and latches garbage into s_strobe — which the every-lap bank
// decode holds as the current bank. One steal = wrong bank until the
// next real write, which garbles every banked cart. The FIFO has exactly
// one consumer: the DMA.

// Snapshot of the whole strobe pipeline for /api/status and the CDC
// menu: distinguishes "no strobes captured" (PIO/hardware), "captured
// but not moved" (DMA, fifo_level > 0), and "moved but mis-decoded"
// (strobe_raw's data bits vs current_bank) at a glance.
void bus_debug(bus_debug_t *d) {
    d->strobe_raw      = s_strobe;
    d->strobe_count    = s_ctl.strobe_count;
    d->current_bank    = s_ctl.current_bank;
    d->fifo_level      = pio_sm_get_rx_fifo_level(BUS_PIO, BUS_SM_STROBE);
    d->strobe_delay_ns = s_delay_ns;
    d->lap_count       = 0;     // not counted here: the loop is proven
                                // and an extra store would cost margin
    d->drive_count     = 0;
    d->serving         = 0;
    d->serve_stall     = 0;     // videocart-only zero-drop counters
    d->pair_drop       = 0;
    d->aud_drop        = 0;     // videocart-only; must not read as garbage
    d->tag_slip        = 0;
    d->reset_vecs      = 0;
    d->tag_guard       = 0;
    d->tag_retry       = 0;
    d->lap_wait_ns     = 0;     // videocart-only lap anatomy
    d->lap_body_ns     = 0;
    d->lap_bmax_ns     = 0;
    d->serve_look      = 0;
    d->snoop_lag       = 0;
    d->serve_fifo      = 0;
    d->tag_diag        = 0;
    d->echo_ok         = 0;
    d->echo_bad        = 0;
    d->echo_diag       = 0;
    d->anchor_fix      = 0;
    d->anchor_dist     = 0;
    d->sam_ty          = 0;
    d->ty_flips        = 0;
    d->tbl_ok          = 0;
    if (s_strobe_dma >= 0) {
        d->dma_ctrl  = dma_hw->ch[s_strobe_dma].al1_ctrl;
        d->dma_read  = dma_hw->ch[s_strobe_dma].read_addr;
        d->dma_write = dma_hw->ch[s_strobe_dma].write_addr;
        d->dma_count = dma_hw->ch[s_strobe_dma].transfer_count;
    } else {
        d->dma_ctrl = d->dma_read = d->dma_write = d->dma_count = 0;
    }
}

// This board sees only A0-A13 and a hardware write strobe — it has no
// RAM mirror and no full-bus counters. Constant stubs so the shared
// diagnostic code compiles and reports "not available".
const uint8_t *bus_mirror(void)   { return NULL; }
uint32_t bus_write_count(void)    { return 0; }
uint32_t bus_capture_behind(void) { return 0; }
uint32_t bus_scs_disagree(void)   { return 0; }

// ----------------------------------------------------------------------
// Core 1. The loop must never fetch through XIP — core 0 erases and
// programs flash underneath it.
//
// __no_inline_not_in_flash_func, not the plain variant: GCC will happily
// inline a static __not_in_flash_func into a flash-resident caller and
// silently put the whole thing back in flash (see ota.c's copier for
// the case where that actually happened). Here the caller is itself
// RAM-resident so inlining would be harmless, but the invariant should
// hold by construction rather than by luck.
// ----------------------------------------------------------------------

static void __no_inline_not_in_flash_func(bus_loop)(void) {
    uint32_t epoch      = ~0u;
    const uint8_t *base = s_main;
    uint32_t serve = 0, bank_mask = 0, scheme = 0;
    uint32_t bank_base = 0;
    uint32_t last_strobe = 0;

    for (;;) {
        // -- control refresh (one load + compare per lap) --
        uint32_t e = s_ctl.epoch;
        if (e != epoch) {
            epoch     = e;
            serve     = s_ctl.serve;
            scheme    = s_ctl.scheme;
            bank_mask = s_ctl.bank_mask;
            bank_base = 0;
            s_ctl.current_bank = 0;
            base      = s_ctl.use_stub ? s_stub : s_main;
            // Adopt whatever strobe is currently latched as "already seen"
            // so a stale word (e.g. the cold-boot stub's $FF5F write) can't
            // be mis-decoded as a bank select on the new image — start at
            // bank 0 until a fresh write arrives. Do NOT zero the latch
            // here instead: that re-decodes the stale word on the next
            // lap and breaks banked serving.
            last_strobe = s_strobe;
            gpio_set_dir_masked(MC_DATA_MASK, 0);   // safe through a swap
        }

        uint32_t in = gpio_get_all();

        // -- bank / stub strobe capture --
        // Read the latest strobe from SRAM (kept fresh by the free-running
        // DMA), NOT the PIO FIFO. This keeps the serve loop entirely off
        // the APB, so WiFi DMA can never stall it -- banked images are
        // as WiFi-immune as flat ones.
        //
        // Act on CHANGE only. Decoding every lap adds ~7 cycles and an
        // SRAM store to each BANKED lap, which is enough to garble banked
        // serving on real hardware. Do not add work here.
        //
        // Change-detection is sound here ONLY because the PIO stamps a
        // sequence bit (bit 22) that flips on every capture, so no two
        // captures are ever byte-identical and a repeated bus write can
        // never masquerade as "nothing happened". Without it, a banked
        // cart re-selecting the bank it already asked for is invisible
        // and the bank never changes (Donkey King retried forever with
        // the bank-1 window reading back bank 0's bytes).
        //
        // The bank/address decoders below mask bit 22 away, so it costs
        // nothing but must not be reused for anything else.
        uint32_t sc = s_strobe;
        if (sc != last_strobe) {
            last_strobe = sc;
            s_ctl.strobe_count++;
            if (s_ctl.stub_armed) {
                // Match the ADDRESS ($FF5F), not the data byte: address
                // lines are valid at any sample delay (data may not be).
                if ((sc & MC_ADDR_MASK) == MC_STUB_MAGIC_ADDR) {
                    s_ctl.stub_seen  = 1;
                    s_ctl.stub_armed = 0;
                }
            } else if (scheme == BUS_SCHEME_BANK16K) {
                uint32_t bank = ((sc >> MC_PIN_D0) & 0xFFu) & bank_mask;
                bank_base = bank * MC_BANK_BYTES;
                s_ctl.current_bank = bank;
            }
        }

        // -- serve --
        if (!serve) {
            gpio_set_dir_masked(MC_DATA_MASK, 0);
            continue;
        }

        uint32_t d = base[bank_base + (in & MC_ADDR_MASK)];
        gpio_put_masked(MC_DATA_MASK, d << MC_PIN_D0);

        // Drive only while !CTS is asserted; released within one lap.
        gpio_set_dir_masked(MC_DATA_MASK,
                            (in & (1u << MC_PIN_CTS)) ? 0u : MC_DATA_MASK);
    }
}

void __not_in_flash_func(bus_core1_main)(void) {
    // No interrupts on this core, ever: the loop's timing and its
    // flash-independence both depend on it.
    (void)save_and_disable_interrupts();
    bus_loop();
}
