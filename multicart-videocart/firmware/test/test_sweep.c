// ======================================================================
// test_sweep.c — exhaustive check of the videocart sweep reassembly.
//
// A software model of the BOARD (the three '153s as wired in
// the board wiring) generates the exact words the PIO would push,
// including garbage on the pad bits that sit inside the 14-bit IN span
// but carry HSTX/SCL signals. The firmware's own sweep_assemble()
// (videocart/sweep.h — the code the bus loop runs) must reconstruct
// every address and flag combination bit-perfectly.
//
// 65536 addresses x 8 rw/cts/scs combinations x random garbage.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "config.h"     // videocart board config
#include "sweep.h"      // code under test

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
} while (0)

// The board model: what the '153 outputs show during sweep phase p.
// Board mux arrangement:
//   phase | M0   M1     M2     M3      M4
//     0   | A0   A4     A8     A12     R/W
//     1   | A1   A5     A9     A13     CTS*
//     2   | A2   A6     A10    A14     SCS*
//     3   | A3   A7     A11    A15     CTS*
static uint32_t board_phase_word(uint32_t p, uint32_t addr, uint32_t rw,
                                 uint32_t cts, uint32_t scs,
                                 uint32_t garbage) {
    uint32_t m0 = (addr >> p)        & 1u;
    uint32_t m1 = (addr >> (4 + p))  & 1u;
    uint32_t m2 = (addr >> (8 + p))  & 1u;
    uint32_t m3 = (addr >> (12 + p)) & 1u;
    uint32_t m4 = (p == 0) ? rw : (p == 2) ? scs : cts;

    uint32_t pins = m0 << MC_MUXBIT_M0
                  | m1 << MC_MUXBIT_M1
                  | m2 << MC_MUXBIT_M2
                  | m3 << MC_MUXBIT_M3
                  | m4 << MC_MUXBIT_M4;

    // Garbage on every pad the sweep must IGNORE: the HSTX pins
    // (GP12..19 = span bits 3..10) and SCL (GP21 = span bit 12).
    uint32_t junk_mask = (0xFFu << 3) | (1u << 12);
    pins |= garbage & junk_mask;

    return (pins << 2) | p;     // exactly what the PIO pushes
}

int main(void) {
    uint32_t seed = 12345;

    for (uint32_t addr = 0; addr <= 0xFFFF; addr++) {
        for (uint32_t f = 0; f < 8; f++) {
            uint32_t rw  = (f >> 0) & 1u;
            uint32_t cts = (f >> 1) & 1u;
            uint32_t scs = (f >> 2) & 1u;

            seed = seed * 1103515245u + 12345u;     // junk generator

            uint32_t w[4];
            for (uint32_t p = 0; p < 4; p++)
                w[p] = board_phase_word(p, addr, rw, cts, scs,
                                        seed >> (p * 4));

            sweep_view_t v;
            sweep_assemble(w, &v);

            CHECK(v.addr == addr, "addr %04X -> %04X (flags %u)",
                  addr, v.addr, f);
            CHECK(v.rw  == rw,  "addr %04X rw %u -> %u",  addr, rw,  v.rw);
            CHECK(v.cts == cts, "addr %04X cts %u -> %u", addr, cts, v.cts);
            CHECK(v.scs == scs, "addr %04X scs %u -> %u", addr, scs, v.scs);

            if (failures > 20) {
                printf("too many failures, aborting\n");
                return 1;
            }
        }
    }

    // CTS is sampled on phases 1 AND 3; the firmware ORs them so a
    // deasserting edge caught by either sample releases the bus. Model
    // the mid-edge case: phase 1 still shows asserted (0), phase 3
    // already shows deasserted (1) -> view must be deasserted.
    {
        uint32_t w[4];
        for (uint32_t p = 0; p < 4; p++)
            w[p] = board_phase_word(p, 0xC123, 1, 0, 1, 0);
        w[3] |= 1u << (2 + MC_MUXBIT_M4);           // phase-3 CTS = 1
        sweep_view_t v;
        sweep_assemble(w, &v);
        CHECK(v.cts == 1, "mid-edge CTS OR: got %u, want 1 (release)", v.cts);
    }

    if (failures) { printf("test_sweep: %d FAILURES\n", failures); return 1; }
    printf("test_sweep: OK (65536 addresses x 8 flag combos, "
           "garbage-injected)\n");
    return 0;
}
