#ifndef VIDEOCART_SWEEP_H
#define VIDEOCART_SWEEP_H

#include <stdint.h>

// ======================================================================
// sweep.h — reassembly of the '153 mux sweep into {addr, rw, cts, scs}.
//
// Pure functions, no SDK dependencies: bus.c uses this on the device and
// firmware/test/test_sweep.c runs the SAME code on the host against an
// independent software model of the schematic's mux wiring.
//
// Input: the four tagged sweep words exactly as the PIO pushes them,
// already sorted by tag (w[tag] = word). Word format (shift-left ISR):
//   bits [15:2] = the 14 pads GP9..GP22 at sample time
//   bits [1:0]  = phase tag
// Phase p carries A(p), A(4+p), A(8+p), A(12+p) on M0..M3 and
// R/W (p=0) / CTS* (p=1,3) / SCS* (p=2) on M4.
// ======================================================================

// Compact tag-stripped phase word -> 4-bit index: M0..M2 sit at bits
// 0..2, M3 sits at bit MC_MUXBIT_M3 and lands at bit 3.
#define SWEEP_IDX(t)  (((t) & 7u) | (((t) >> (MC_MUXBIT_M3 - 3)) & 8u))

typedef struct {
    uint32_t addr;      // A0..A15
    uint32_t rw;        // 1 = read
    uint32_t cts;       // active low; OR of both sweep samples (see bus.c)
    uint32_t scs;       // active low
} sweep_view_t;

// ---------------------------------------------------------------------
// SNOOP path: byte-pair transpose LUTs.
//
// Used by core 1 to reassemble the FOLLOWER's four words into an address
// for the capture (write) pipeline. The serve path does not come through
// here at all -- it is PIO+DMA, see mux_sampler in bus_pio.pio.
//
// The address arrives as a 4x4 bit-matrix transpose: phase p's nibble
// holds A(p), A(4+p), A(8+p), A(12+p). Undoing that with the 16-entry
// spread LUT costs four LUT loads plus four shift/or chains -- ~55
// cycles, measured as the single largest block between the sweep landing
// and the serve push. Pairing two phase nibbles into one byte index cuts
// it to two loads and one OR:
//
//   idx_lo = nib(phase0) | nib(phase1) << 4   -> t_lo[idx_lo] scatters to
//            bits {0,4,8,12} and {1,5,9,13}
//   idx_hi = nib(phase2) | nib(phase3) << 4   -> t_hi[idx_hi] scatters to
//            bits {2,6,10,14} and {3,7,11,15}
//   addr   = t_lo[idx_lo] | t_hi[idx_hi]
//
// 2 x 256 x u16 = 1 KB, filled at init by sweep_luts_init() so the
// tables live in SRAM (never flash -- the bus loop must not XIP).
//
// PAIRING IS BY RING SLOT, which requires slot i to carry phase i. That
// held on hardware across millions of cycles (tag_slip == 0), and the
// loop keeps counting tag_slip so a future slip is seen, not suffered.
static inline void sweep_luts_init(uint16_t t_lo[256], uint16_t t_hi[256]) {
    static const uint8_t pos_lo[8] = {0, 4, 8, 12, 1, 5, 9, 13};
    static const uint8_t pos_hi[8] = {2, 6, 10, 14, 3, 7, 11, 15};
    for (uint32_t i = 0; i < 256; i++) {
        uint16_t a = 0, b = 0;
        for (uint32_t k = 0; k < 8; k++) {
            if (i & (1u << k)) {
                a |= (uint16_t)(1u << pos_lo[k]);
                b |= (uint16_t)(1u << pos_hi[k]);
            }
        }
        t_lo[i] = a;
        t_hi[i] = b;
    }
}

// One byte index from two consecutive-phase words (raw, tag included).
static inline __attribute__((always_inline))
uint32_t sweep_idx8(uint32_t wa, uint32_t wb) {
    return SWEEP_IDX(wa >> 2) | (SWEEP_IDX(wb >> 2) << 4);
}

// R/W rides M4 on phase 0 (slot 0). Set up with the address before
// E rises, so it is valid when the sweep samples it.
static inline __attribute__((always_inline))
uint32_t sweep_rw(uint32_t w0) {
    return (w0 >> (2 + MC_MUXBIT_M4)) & 1u;
}

// CTS*/SCS*: diagnostics only, so these run in the bookkeeping pass where
// there is no deadline. M4 carries CTS* on phases 1 and 3, SCS* on 2.
static inline __attribute__((always_inline))
void sweep_flags4(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3,
                  uint32_t *cts, uint32_t *scs) {
    uint32_t t0 = w0 >> 2, t1 = w1 >> 2, t2 = w2 >> 2, t3 = w3 >> 2;
    uint32_t s = (w0 & 3u) == 2 ? t0 : (w1 & 3u) == 2 ? t1
               : (w2 & 3u) == 2 ? t2 : t3;
    *scs = (s >> MC_MUXBIT_M4) & 1u;
    // Tags 1 and 3 are exactly the odd ones, so bit 0 of the tag selects
    // the two words that carry CTS*.
    uint32_t c = 0;
    if (w0 & 1u) c |= t0;
    if (w1 & 1u) c |= t1;
    if (w2 & 1u) c |= t2;
    if (w3 & 1u) c |= t3;
    *cts = (c >> MC_MUXBIT_M4) & 1u;
}

// Full assembly, THROUGH THE SAME LUT PATH THE BUS LOOP RUNS -- this is
// the entry point the host test drives, so the tables and index code
// above are what 65536x8 garbage-injected combinations actually verify.
// Words must be in slot order (slot i = phase i), as on the ring.
static inline void sweep_assemble(const uint32_t w[4], sweep_view_t *v) {
    static uint16_t t_lo[256], t_hi[256];
    static int init;
    if (!init) { sweep_luts_init(t_lo, t_hi); init = 1; }
    v->addr = (uint32_t)t_lo[sweep_idx8(w[0], w[1])]
            | (uint32_t)t_hi[sweep_idx8(w[2], w[3])];
    v->rw   = sweep_rw(w[0]);
    sweep_flags4(w[0], w[1], w[2], w[3], &v->cts, &v->scs);
}

// ======================================================================
// SERVE path: the ROM table permutation.
//
// The sampler SM (mux_sampler, bus_pio.pio) shifts address bits into the
// ISR in the order the '153 phases deliver them and NEVER un-scrambles
// them -- un-scrambling per cycle is what costs a CPU its deadline. The
// scramble is absorbed here instead, once per mount, by storing each ROM
// byte at the index the hardware will naturally compute.
//
// Index bit order, MSB..LSB, exactly as the ISR accumulates it:
//
//   A8 A4 A0 | A12 | A9 A5 A1 | A13 | A10 A6 A2 | A11 A7 A3
//
// (the sampler appends one more zero bit, the *2 for 16-bit entries).
//
// THIS FUNCTION AND THE SAMPLER PROGRAM MUST AGREE BIT FOR BIT. They are
// proven equal by test_serve_pipeline.c, which models the PIO program
// instruction by instruction and compares against this. Change one and
// the test fails -- do not "fix" the test.
static inline uint32_t perm_idx(uint32_t a) {          // a = A0..A13
    return ((a >>  8 & 1u) << 13) | ((a >>  4 & 1u) << 12)
         | ((a >>  0 & 1u) << 11)                          // A8 A4 A0
         | ((a >> 12 & 1u) << 10)                          // A12
         | ((a >>  9 & 1u) <<  9) | ((a >>  5 & 1u) <<  8)
         | ((a >>  1 & 1u) <<  7)                          // A9 A5 A1
         | ((a >> 13 & 1u) <<  6)                          // A13
         | ((a >> 10 & 1u) <<  5) | ((a >>  6 & 1u) <<  4)
         | ((a >>  2 & 1u) <<  3)                          // A10 A6 A2
         | ((a >> 11 & 1u) <<  2) | ((a >>  7 & 1u) <<  1)
         | ((a >>  3 & 1u) <<  0);                         // A11 A7 A3
}

// One 16-bit table entry: dirs in [15:8], data in [7:0]. The serve SM
// consumes data with `out pins,8` then dirs with `out pindirs,8`.
//
// dirs = 0x00 makes the SM leave the bus tristated. That is how
// $FF00-$FFFF is excluded: those addresses pass the sampler's A14/A15
// gate, but the 6809 fetches its vectors there and the SAM answers them,
// so driving would be contention. Excluded in DATA, not in logic.
#define PERM_ENTRY(dirs, data)  ((uint16_t)(((dirs) << 8) | (uint8_t)(data)))
#define PERM_TABLE_ENTRIES      16384u                 // 2^14 (A0..A13)
#define PERM_TABLE_BYTES        (PERM_TABLE_ENTRIES * 2u)   // 32 KB

// Build one bank's table. `rom` is 16 KB of raw cart image; `win_base` is
// the CPU address the bank's offset 0 maps to ($C000). Kept here, beside
// perm_idx, so the host tests exercise the production generator.
static inline void perm_build(uint16_t *table, const uint8_t *rom,
                              uint32_t win_base) {
    for (uint32_t a = 0; a < PERM_TABLE_ENTRIES; a++) {
        uint32_t cpu = win_base + a;
        uint32_t dirs = (cpu >= 0xC000u && cpu <= 0xFEFFu) ? 0xFFu : 0x00u;
        table[perm_idx(a)] = PERM_ENTRY(dirs, rom[a]);
    }
}

#endif
