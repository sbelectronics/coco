#ifndef MULTICART_BUS_H
#define MULTICART_BUS_H

#include <stdint.h>
#include <stdbool.h>

// Bank-switching scheme of the image core 1 is serving.
typedef enum {
    BUS_SCHEME_FLAT = 0,    // <=16K, A0..A13 index the image directly
    BUS_SCHEME_BANK16K,     // write to $FF40..5F selects a 16K bank (data byte = bank #)
} bus_scheme_t;

// Called once from main() on core 0 before launching core 1: claims PIO,
// GPIOs, and initializes the strobe SM with the cfg'd delay.
void bus_init(uint32_t strobe_delay_ns);

// Core 1 entry point. Never returns; entirely RAM-resident after entry
// (no flash access, no interrupts), so core 0 may erase and program
// flash at any time without pausing it.
void bus_core1_main(void);

// -------- core-0 -> core-1 control (all safe while serving) ----------
//
// Two buffers with fixed roles (not a ping-pong):
//
//   main buffer (128 KB) — the ROM image being, or about to be, served.
//   stub buffer (16 KB)  — the cold-boot stub window.
//
// The safety invariant is structural: the STUB buffer is only written
// while !RESET is asserted (the CoCo cannot be fetching), and the MAIN
// buffer is only written while the stub (or nothing) is being served.
// select.c's sequencing satisfies both; keep it that way.

// The main image buffer. Core 0 fills it, then calls bus_serve().
// Flat images use the first 16 KB (mirrored by select.c's loader).
uint8_t *bus_staging(void);

// How many bytes of that buffer a cart image may occupy. NOT always the
// whole buffer: on the videocart the image buffer is also where the
// permuted serve tables live -- including the selection stub's own
// table, which the CoCo is fetching from at the exact moment select.c
// stages the next image. Writing past this bound scribbles over it, the
// stub never runs, and every selection falls through the WAIT_MAGIC
// timeout without its cold boot.
uint32_t bus_staging_bytes(void);

// Atomically direct core 1 at the main buffer. len is the image size in
// bytes (sets the bank count for BANK16K schemes).
void bus_serve(bus_scheme_t scheme, uint32_t len);

// Optional: do bus_serve()'s expensive setup NOW, while the caller still
// holds the CoCo in reset, so the later bus_serve() is just a pointer
// flip. On the videocart that setup is a multi-millisecond SRAM burst
// that otherwise starves core 1's capture loop and silently drops bus
// writes; see the commentary in videocart/bus.c. Must be called with the
// same (scheme, len) later handed to bus_serve(), which falls back to
// building inline if they do not match. A no-op where it buys nothing.
void bus_serve_prepare(bus_scheme_t scheme, uint32_t len);

// The stub image buffer (MC_BANK_BYTES). Write it only under reset.
uint8_t *bus_stub_staging(void);

// Atomically direct core 1 at the stub buffer (always FLAT, 16 KB).
void bus_serve_stub(void);

// Stop serving (data bus held as inputs; CoCo sees an empty slot).
//
// Idling does NOT invalidate a build prepared by bus_serve_prepare():
// the insertion-style path idles the slot on purpose so BASIC boots to
// READY with an empty cart, then serves the SAME image ~2 s later, and
// that serve has to stay a pointer flip. Rebuilding there would put
// milliseconds of SRAM traffic microseconds before the cart's one-shot
// FIRQ start-up writes.
void bus_idle(void);

// Cold-boot stub support: true once a strobe matching the stub magic
// has been captured since the last bus_stub_arm(). Disarm when giving
// up (timeout) — a stale armed state would swallow a real bank write.
void bus_stub_arm(void);
void bus_stub_disarm(void);
bool bus_stub_seen(void);

// Diagnostics.
uint32_t bus_strobe_count(void);
uint8_t  bus_current_bank(void);

// One-glance snapshot of the strobe pipeline (multicart; other boards
// zero-fill). strobe_raw holds A0..13 in bits 0-13, D0..7 in 14-21, and
// in bit 22 a sequence bit the PIO flips on every capture — so a raw
// value that differs only in bit 22 means "same write, captured again",
// not a new one.
typedef struct {
    uint32_t strobe_raw;
    uint32_t strobe_count;
    uint32_t current_bank;
    uint32_t fifo_level;        // should always read 0
    uint32_t strobe_delay_ns;
    uint32_t dma_ctrl, dma_read, dma_write, dma_count;
    uint32_t lap_count;         // serve-loop laps (videocart; 0 elsewhere)
    uint32_t drive_count;       // laps where the data bus was driven
    uint32_t serving;           // 1 = an image is mounted and being served
    // Videocart zero-drop proof counters (0 elsewhere). ALL THREE MUST
    // READ ZERO in steady state -- each one counts a byte that was lost
    // on the serve or the capture pipeline, which the design forbids.
    uint32_t serve_stall;       // serve word lost: sampler RX FIFO full
    uint32_t pair_drop;         // captured write lost: core 0 pump stalled
    uint32_t aud_drop;          // audio event lost (ring full; videocart only)
    uint32_t tag_slip;          // follower ring handed phases out of order
    uint32_t reset_vecs;        // $FFFE fetches: one per 6809 reset
    uint32_t tag_guard;         // ...of which: groups whose tags were wrong
    uint32_t tag_retry;         // ...and laps spent re-anchoring after one
    // Videocart lap anatomy + serve-chain readouts (0 elsewhere). The
    // loop measures its own wait/body split because instruction counting
    // consistently underestimates a core-1 loop; the serve-chain fields
    // read back what the hardware actually did.
    uint32_t lap_wait_ns;       // 4096-lap avg: blocked on capture
    uint32_t lap_body_ns;       // 4096-lap avg: the part we pay for
    uint32_t lap_bmax_ns;       // worst body in the window
    uint32_t serve_look;        // last DMA lookup, as table byte offset
    uint32_t tbl_ok;            // 1 = tables verified on device
    uint32_t snoop_lag;         // deepest snoop-ring lag ever, in cycles
    // snoop_lag is a high-water mark that is never reset, so a single
    // spike at cart start pins it at "nearly full" for the rest of the
    // session and it cannot tell a transient from a ring that is sitting
    // saturated and dropping continuously. These two can: lag_now is the
    // most recent sample, lag_hi counts how many samples came in at 3/4
    // depth or worse.
    uint32_t lag_now;           // most recent snoop-ring lag, in cycles
    uint32_t lag_hi;            // samples at >= 3/4 ring depth
    uint32_t pair_hi;           // high-water occupancy of the write ring
    uint32_t serve_fifo;        // worst serve TX FIFO level; 2+ = latched
    uint32_t tag_diag;          // last slip: [9:8] ring phase, [7:0] tags
    // Serve echo: bus read-back vs the table entry owed. echo_bad > 0 is
    // DIRECT proof serving is wrong; echo_diag names the case.
    uint32_t echo_ok;
    uint32_t echo_bad;
    uint32_t echo_diag;         // [31:16] addr, [15:8] owed, [7:0] got
    uint32_t anchor_fix;        // anchor re-takes forced by verification
    uint32_t anchor_dist;       // observed follower distance at last check
    uint32_t sam_ty;            // 1 = SAM all-RAM mode (observed only)
    uint32_t ty_flips;          // SAM map-type transitions seen
} bus_debug_t;
void bus_debug(bus_debug_t *d);

// ---- optional, boards that snoop the whole bus (videocart) ----------
// Weakly-linked so cdcmenu/web can report them on any board; the
// multicart's bus.c leaves them at their default zero/NULL.
const uint8_t *bus_mirror(void);        // 64K RAM shadow, or NULL

// Videocart only: core-0 half of the snoop. Core 1 pairs each bus write
// with its address and hands it over through a ring; this drains it --
// mirror, write counter, video-register forwarding. Call from the main
// pump loop. Not defined (and not needed) on the multicart.
void bus_pump(void);
uint32_t bus_write_count(void);
uint32_t bus_capture_behind(void);
uint32_t bus_scs_disagree(void);

#endif
