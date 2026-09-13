// ======================================================================
// test_serve_pipeline.c — end-to-end proof of the HARDWARE serve path.
//
// The serve path has no CPU in it, so there is nothing to step through on
// the device and nothing a print statement can catch. This test stands in
// for that: it models every stage in the chain and asserts the two
// properties the design makes absolute.
//
//   board  -> the three '153 muxes as wired, phase by phase, with LIVE
//             GARBAGE on the HSTX pins (GP12-19) and SCL (GP21) that fall
//             inside the sampler's IN span
//   PIO    -> mux_sampler executed INSTRUCTION BY INSTRUCTION against a
//             modelled ISR/OSR/X/Y, including shift directions, the
//             A14/A15 gates and the non-blocking push
//   DMA    -> pushed word treated as a literal byte address:
//             entry = *(uint16_t *)word
//   SM     -> bus_serve's gates: E-high only, live R/W, dirs byte
//
// ASSERTED, for every cycle of a synthetic bus trace:
//   1. every cartridge-window READ is answered with exactly the right
//      ROM byte from the CURRENT bank                  (no dropped serve)
//   2. NOTHING is driven on writes, outside $C000-$FEFF, or in
//      $FF00-$FFFF                                     (no wrong drive)
//
// The permutation this depends on is production code (perm_idx/perm_build
// in videocart/sweep.h), so a divergence between the PIO program and the
// table generator fails here rather than on the bench.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "sweep.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        if (failures < 12) { printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
        failures++; \
    } \
} while (0)

// ---------------------------------------------------------------- board
// What the '153 outputs show during sweep phase p, as the sampler's IN
// span sees them (bit i = GP(9+i)). Garbage is injected on every pad the
// sampler must ignore; if the program ever indexes one, the address comes
// out wrong and the checks below fail.
static uint32_t board_pins(uint32_t p, uint32_t addr, uint32_t rw,
                           uint32_t cts, uint32_t scs, uint32_t garbage) {
    uint32_t m0 = (addr >> p)        & 1u;
    uint32_t m1 = (addr >> (4 + p))  & 1u;
    uint32_t m2 = (addr >> (8 + p))  & 1u;
    uint32_t m3 = (addr >> (12 + p)) & 1u;
    uint32_t m4 = (p == 0) ? rw : (p == 2) ? scs : cts;

    uint32_t pins = m0 << MC_MUXBIT_M0 | m1 << MC_MUXBIT_M1
                  | m2 << MC_MUXBIT_M2 | m3 << MC_MUXBIT_M3
                  | m4 << MC_MUXBIT_M4;
    pins |= garbage & ((0xFFu << 3) | (1u << 12));   // HSTX GP12-19, SCL
    return pins;
}

// ------------------------------------------------------- sampler model
// A faithful execution of mux_sampler. Kept structurally parallel to the
// .pio listing so the two can be diffed by eye:
//
//   mov isr, x        ISR = X, input shift counter reset
//   mov osr, pins     OSR = pin snapshot
//   in  osr, 3        ISR = (ISR << 3) | (OSR & 7)      -- OSR untouched
//   out null, 13      OSR >>= 13                        -- ISR untouched
//   in  osr, 1        ISR = (ISR << 1) | (OSR & 1)
//   out y, 1          Y = OSR & 1; OSR >>= 1
//   jmp !y, nopush    give up on this cycle, push nothing
//   in  null, 1       ISR <<= 1
//
// Returns 1 and sets *word if the SM pushed; 0 if it aborted.
static int sampler_run(uintptr_t table_base, uint32_t addr, uint32_t rw,
                       uint32_t cts, uint32_t scs, uint32_t seed,
                       uintptr_t *word) {
    uintptr_t isr = table_base >> 15;        // mov isr, x
    uint32_t osr, y;   // 32-bit on the device; the ISR chain is
                       // widened only so a 64-bit host pointer survives

    // ---- phase 0: A0,A4,A8 + A12
    osr = board_pins(0, addr, rw, cts, scs, seed);
    isr = (isr << 3) | (osr & 7u);           // in osr, 3
    osr >>= 13;                              // out null, 13
    isr = (isr << 1) | (osr & 1u);           // in osr, 1   -> A12

    // ---- phase 1: A1,A5,A9 + A13
    osr = board_pins(1, addr, rw, cts, scs, seed >> 4);
    isr = (isr << 3) | (osr & 7u);
    osr >>= 13;
    isr = (isr << 1) | (osr & 1u);           // A13

    // ---- phase 2: A2,A6,A10 + A14 gate
    osr = board_pins(2, addr, rw, cts, scs, seed >> 8);
    isr = (isr << 3) | (osr & 7u);
    osr >>= 13;
    y = osr & 1u;                            // out y, 1    -> A14

    // ---- phase 3: A3,A7,A11 + A15 gate
    osr = board_pins(3, addr, rw, cts, scs, seed >> 12);
    isr = (isr << 3) | (osr & 7u);
    if (!y) return 0;                        // jmp !y, nopush  (A14)
    osr >>= 13;
    y = osr & 1u;                            // out y, 1    -> A15
    if (!y) return 0;                        // jmp !y, nopush  (A15)
    isr <<= 1;                               // in null, 1  (x2)

    *word = isr;                             // push noblock
    return 1;
}

// ---------------------------------------------------- serve SM model
// bus_serve's gates. `e_high` is false during E-low, when the SM parks on
// `wait 1 gpio 8` and physically cannot drive.
typedef struct { int driving; uint8_t data; } drive_t;

static drive_t serve_sm(int have_word, uint16_t entry, uint32_t live_rw,
                        int e_high) {
    drive_t d = { 0, 0 };
    if (!have_word) return d;                // parked on `pull block`
    if (!e_high)    return d;                // parked on `wait 1 gpio 8`
    if (!live_rw)   return d;                // `jmp pin` says write
    if ((entry >> 8) == 0)  return d;        // dirs = 0x00 -> stay Hi-Z
    d.driving = 1;
    d.data    = (uint8_t)(entry & 0xFFu);
    return d;
}

// ---------------------------------------------------------------- main
#define BANKS 4
static uint8_t  rom[BANKS][MC_BANK_BYTES];
// The device requires 32 KB alignment because the sampler ORs the base
// with the index. Model that here too: a misaligned base would silently
// corrupt every lookup, and this test must not accidentally pass with one.
static uint16_t tables[BANKS][PERM_TABLE_ENTRIES] __attribute__((aligned(32768)));

int main(void) {
    uint32_t seed = 0xC0C0FEEDu;

    for (uint32_t b = 0; b < BANKS; b++)
        for (uint32_t i = 0; i < MC_BANK_BYTES; i++)
            rom[b][i] = (uint8_t)(b * 37u + i * 5u + (i >> 7));

    for (uint32_t b = 0; b < BANKS; b++)
        perm_build(tables[b], rom[b], 0xC000u);

    if (((uintptr_t)tables[0] & (PERM_TABLE_BYTES - 1u)) != 0) {
        printf("test_serve_pipeline: table base not 32 KB aligned\n");
        return 1;
    }

    // ---- 1. exhaustive over the whole address space, both directions --
    // Every address, as a read and as a write, with fresh garbage each
    // time. This is the no-dropped-serve and no-wrong-drive proof.
    uint32_t served = 0, refused = 0;
    for (uint32_t bank = 0; bank < BANKS; bank++) {
        uintptr_t base = (uintptr_t)tables[bank];
        for (uint32_t a = 0; a <= 0xFFFFu; a++) {
            for (uint32_t rw = 0; rw <= 1; rw++) {
                seed = seed * 1103515245u + 12345u;
                uint32_t cts = (seed >> 3) & 1u, scs = (seed >> 5) & 1u;

                uintptr_t word = 0;
                int pushed = sampler_run(base, a, rw, cts, scs, seed, &word);

                uint16_t entry = 0;
                if (pushed) {
                    // The DMA does exactly this: the pushed word IS the
                    // byte address of a uint16_t entry.
                    CHECK(word >= base && word < base + PERM_TABLE_BYTES,
                          "bank %u addr %04X rw%u: word %08X out of table",
                          bank, a, rw, (unsigned)(word & 0xFFFFFFFFu));
                    if (word < base || word >= base + PERM_TABLE_BYTES)
                        continue;
                    CHECK((word & 1u) == 0, "odd entry address %08X",
                          (unsigned)(word & 0xFFFFFFFFu));
                    entry = *(const uint16_t *)word;
                }

                // The serve SM only ever drives during E-high, and R/W on
                // the pin is the same R/W the cycle carries.
                drive_t d = serve_sm(pushed, entry, rw, 1);

                int in_window = (a >= 0xC000u && a <= 0xFEFFu);
                if (rw && in_window) {
                    // REQUIREMENT 1: this read MUST be answered, with the
                    // right byte, from the right bank.
                    CHECK(d.driving,
                          "DROPPED SERVE: bank %u addr %04X not driven",
                          bank, a);
                    CHECK(d.data == rom[bank][a - 0xC000u],
                          "WRONG BYTE: bank %u addr %04X got %02X want %02X",
                          bank, a, d.data, rom[bank][a - 0xC000u]);
                    served++;
                } else {
                    // REQUIREMENT 2: everything else must stay off the bus
                    // -- writes, sub-$C000, and the $FF00-$FFFF I/O and
                    // vector page.
                    CHECK(!d.driving,
                          "WRONG DRIVE: bank %u addr %04X rw%u drove %02X",
                          bank, a, rw, d.data);
                    refused++;
                }

                // And nothing may reach the bus during E-low, whatever the
                // sampler decided: that half-cycle belongs to the SAM's
                // VDG fetch.
                drive_t low = serve_sm(pushed, entry, rw, 0);
                CHECK(!low.driving,
                      "DROVE DURING E-LOW: bank %u addr %04X", bank, a);
            }
        }
    }

    // ---- 2. bank switching selects the right image ---------------------
    // Same address, every bank: the byte must follow the table the
    // sampler's X points at.
    for (uint32_t a = 0xC000u; a < 0xC100u; a++) {
        for (uint32_t bank = 0; bank < BANKS; bank++) {
            seed = seed * 1103515245u + 12345u;
            uintptr_t word = 0;
            uintptr_t base = (uintptr_t)tables[bank];
            int pushed = sampler_run(base, a, 1, 0, 1, seed, &word);
            CHECK(pushed, "bank %u addr %04X: no push", bank, a);
            if (!pushed) continue;
            uint16_t entry = *(const uint16_t *)word;
            drive_t d = serve_sm(1, entry, 1, 1);
            CHECK(d.driving && d.data == rom[bank][a - 0xC000u],
                  "bank switch: bank %u addr %04X got %02X want %02X",
                  bank, a, d.data, rom[bank][a - 0xC000u]);
        }
    }

    // ---- 3. the vector page is specifically refused ---------------------
    // The 6809 fetches RESET/IRQ/FIRQ/NMI/SWI here on every interrupt and
    // the SAM answers them. Driving would be contention on a hot path, so
    // check the exact vectors as well as the range.
    static const uint32_t vec[] = { 0xFFF0u, 0xFFF2u, 0xFFF4u, 0xFFF6u,
                                    0xFFF8u, 0xFFFAu, 0xFFFCu, 0xFFFEu };
    for (unsigned i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        seed = seed * 1103515245u + 12345u;
        uintptr_t word = 0;
        uintptr_t base = (uintptr_t)tables[0];
        int pushed = sampler_run(base, vec[i], 1, 0, 1, seed, &word);
        uint16_t entry = pushed ? *(const uint16_t *)word : 0;
        drive_t d = serve_sm(pushed, entry, 1, 1);
        CHECK(!d.driving, "DROVE A VECTOR FETCH at %04X", vec[i]);
    }

    if (failures) {
        printf("test_serve_pipeline: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_serve_pipeline: OK (%u served, %u correctly refused, "
           "4 banks x 64K x rw, garbage-injected)\n", served, refused);
    return 0;
}
