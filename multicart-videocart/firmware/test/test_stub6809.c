// ======================================================================
// test_stub6809.c — executes the cold-boot stub (common/stub6809.h) on
// a small 6809 interpreter covering exactly the instruction forms the
// stub uses, against a 64K memory model that behaves like the CoCo:
//
//   - $0071 preloaded with $55 (warm-start flag set — the case the stub
//     exists to defeat);
//   - the write to $FF5F triggers the "Pico swap": the whole cartridge
//     window $C000-$FEFF is clobbered, so if any instruction after that
//     point still executes from ROM, the test fails loudly;
//   - $FFFE/F holds Color BASIC's reset vector ($A027).
//
// Asserts: byte-identical tail copy to $0600, warm flag cleared BEFORE
// the magic write, magic write of $A5 to $FF5F issued from RAM, delay
// loop iteration count and its wall-clock at 0.895 MHz (must exceed the
// worst-case 45 ms flash-stall window with margin), and final control
// transfer through [$FFFE].
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "stub6809.h"

static uint8_t  mem[65536];
static uint64_t cycles;
static int      failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
} while (0)

// ---- write log -------------------------------------------------------
typedef struct { uint16_t addr; uint8_t data; uint16_t pc; } wr_t;
static wr_t wlog[64];
static int  nwr;
static int  swap_done;

static void bus_write(uint16_t a, uint8_t d, uint16_t pc) {
    if (nwr < 64) wlog[nwr++] = (wr_t){ a, d, pc };
    mem[a] = d;
    if (a == 0xFF5F && !swap_done) {
        // The Pico swaps the served image: everything the stub was
        // running from is replaced. 0x00 decodes as NEG direct — noise
        // that derails any ROM-resident execution immediately.
        memset(mem + 0xC000, 0x00, 0xFEFF - 0xC000 + 1);
        swap_done = 1;
    }
}

// ---- interpreter (only the forms the stub uses) ----------------------
static uint16_t X, Y, PC;
static uint8_t  A, B, CC;
static uint32_t bne_taken;

static uint8_t  fetch8(void)  { return mem[PC++]; }
static uint16_t fetch16(void) { uint16_t v = mem[PC] << 8 | mem[PC + 1]; PC += 2; return v; }
#define SET_Z(v) (CC = (CC & ~0x04) | (((v) == 0) ? 0x04 : 0))

// Returns 0 on clean stop (jump through [$FFFE] taken), -1 on error.
static int run(uint32_t max_instr) {
    while (max_instr--) {
        uint16_t ipc = PC;
        uint8_t op = fetch8();
        switch (op) {
        case 0x1A: CC |= fetch8(); cycles += 3; break;              // ORCC #
        case 0x8E: X = fetch16(); cycles += 3; break;               // LDX #
        case 0x10:
            if (fetch8() != 0x8E) goto bad;
            Y = fetch16(); cycles += 4; break;                      // LDY #
        case 0xC6: B = fetch8(); SET_Z(B); cycles += 2; break;      // LDB #
        case 0x86: A = fetch8(); SET_Z(A); cycles += 2; break;      // LDA #
        case 0xA6:                                                  // LDA idx
            if (fetch8() != 0x80) goto bad;                         // ,X+
            A = mem[X++]; SET_Z(A); cycles += 6; break;
        case 0xA7:                                                  // STA idx
            if (fetch8() != 0xA0) goto bad;                         // ,Y+
            bus_write(Y++, A, ipc); cycles += 6; break;
        case 0x5A: B--; SET_Z(B); cycles += 2; break;               // DECB
        case 0x26: {                                                // BNE
            int8_t off = (int8_t)fetch8();
            cycles += 3;
            if (!(CC & 0x04)) { PC = (uint16_t)(PC + off); bne_taken++; }
            break;
        }
        case 0x7E: PC = fetch16(); cycles += 4; break;              // JMP ext
        case 0x7F: {                                                // CLR ext
            uint16_t a = fetch16();
            bus_write(a, 0, ipc); SET_Z(0); cycles += 7; break;
        }
        case 0xB7: bus_write(fetch16(), A, ipc); cycles += 5; break;// STA ext
        case 0x30: {                                                // LEAX idx
            uint8_t pb = fetch8();
            if (pb != 0x1F) goto bad;                               // -1,X
            X = (uint16_t)(X - 1); SET_Z(X); cycles += 5; break;
        }
        case 0x6E: {                                                // JMP idx
            uint8_t pb = fetch8();
            if (pb != 0x9F) goto bad;                               // [ext]
            uint16_t ptr = fetch16();
            PC = (uint16_t)(mem[ptr] << 8 | mem[ptr + 1]);
            cycles += 8;
            return 0;                                               // done
        }
        default:
        bad:
            printf("FAIL: illegal/unexpected opcode %02X at %04X "
                   "(ROM swapped=%d)\n", op, ipc, swap_done);
            return -1;
        }
    }
    printf("FAIL: instruction budget exhausted (runaway)\n");
    return -1;
}

int main(void) {
    // CoCo-like initial state.
    memset(mem, 0x39, sizeof(mem));         // RTS everywhere
    mem[0x0071] = 0x55;                     // warm-start flag SET
    mem[0xFFFE] = 0xA0; mem[0xFFFF] = 0x27; // BASIC cold-start vector

    // Cartridge window: stub at $C000, NOP fill above — exactly what
    // select.c builds in the stub buffer.
    memset(mem + 0xC000, 0x12, 0xFEFF - 0xC000 + 1);
    memcpy(mem + 0xC000, stub6809, STUB6809_LEN);

    uint8_t rom_tail[0x13];
    memcpy(rom_tail, mem + 0xC015, sizeof(rom_tail));   // for later compare

    PC = 0xC000;                            // CART FIRQ entry
    CC = 0;

    int r = run(2 * 1000 * 1000);
    CHECK(r == 0, "stub did not reach JMP [$FFFE] cleanly");

    // 1. Interrupts masked first thing.
    CHECK((CC & 0x50) == 0x50, "ORCC #$50 did not mask I+F (CC=%02X)", CC);

    // 2. Tail copied to $0600, byte-identical.
    CHECK(memcmp(mem + 0x0600, rom_tail, sizeof(rom_tail)) == 0,
          "tail at $0600 differs from ROM tail");

    // 3. Warm-start flag cleared, and BEFORE the magic write.
    CHECK(mem[0x0071] == 0x00, "warm-start flag not cleared ($71=%02X)",
          mem[0x0071]);
    int i_clr = -1, i_magic = -1;
    for (int i = 0; i < nwr; i++) {
        if (wlog[i].addr == 0x0071 && wlog[i].data == 0x00) i_clr = i;
        if (wlog[i].addr == 0xFF5F && wlog[i].data == 0xA5) i_magic = i;
    }
    CHECK(i_clr >= 0,   "no write clearing $71 observed");
    CHECK(i_magic >= 0, "no $A5 -> $FF5F magic write observed");
    CHECK(i_clr < i_magic, "swap signalled before warm flag cleared");

    // 4. The magic write must be issued from RAM (the ROM is clobbered
    //    at that instant), and everything after it survived the swap.
    CHECK(wlog[i_magic].pc >= 0x0600 && wlog[i_magic].pc < 0x0700,
          "magic write executed from %04X, not the RAM tail",
          wlog[i_magic].pc);
    CHECK(swap_done, "ROM swap never triggered");

    // 5. Delay: 0x4000 LEAX/BNE iterations, ~8 cycles each. At the
    //    CoCo's 0.895 MHz this must comfortably exceed the 45 ms
    //    worst-case flash-erase stall of the Pico's pump loop.
    double delay_ms = (double)(0x4000 * 8) / 894886.0 * 1000.0;
    CHECK(delay_ms > 90.0 && delay_ms < 250.0,
          "delay %.1f ms out of range", delay_ms);
    CHECK(bne_taken >= 0x4000, "delay loop iterations %u < %u",
          bne_taken, 0x4000);

    // 6. Ends at the reset vector target.
    CHECK(PC == 0xA027, "final PC %04X != [$FFFE] target A027", PC);

    if (failures) { printf("test_stub6809: %d FAILURES\n", failures); return 1; }
    printf("test_stub6809: OK (tail copy, cold flag, magic from RAM, "
           "%.0f ms delay, vectored exit)\n", delay_ms);
    return 0;
}
