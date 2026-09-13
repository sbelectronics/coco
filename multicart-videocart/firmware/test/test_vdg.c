// ======================================================================
// test_vdg.c — renders every VDG mode through the real vdg.c against a
// synthetic CoCo memory image and checks pixels, then writes PPM frames
// for human eyeballs.
//
// Covers: register-event ring (SAM clear/set pairs, $FF22), alpha mode
// glyphs + inverse video, semigraphics-4 quadrants/colours, all four CG
// and all four RG tiers' geometry, colour-set select, display-offset
// relocation, borders, and (via ASan) that no mode writes outside the
// framebuffer rows.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"

uint64_t shim_now_us;

// ---- fakes vdg.c links against --------------------------------------
#include "video.h"

static uint8_t fb[VID_ROWS][VID_W];
uint8_t *hstx_row(int y) { return fb[y]; }
void hstx_fill(uint8_t c) { memset(fb, c, sizeof(fb)); }
void hstx_init(bool a) { (void)a; }
void hstx_test_pattern(void) {}

static uint8_t coco_mem[65536];
const uint8_t *bus_mirror(void) { return coco_mem; }

#include "vdg.c"            // code under test

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
} while (0)

// ---- helpers ---------------------------------------------------------

// Push a register write the way core 1 would, then let core 0 render.
static void reg(uint16_t addr, uint8_t data) { video_note_reg(addr, data); }

static void render_now(void) {
    shim_advance_us(20000);         // past the 60 Hz cap
    vdg_pump();
}

// Set the SAM display offset F (base = F * 512) via clear/set pairs.
static void sam_set_offset(uint16_t f) {
    for (int b = 0; b < 7; b++)
        reg((uint16_t)(0xFFC6 + 2 * b + ((f >> b) & 1)), 0);
}

// Set the SAM V bits (scan geometry) the same way: $FFC0/1 = V0 ... etc.
static void sam_set_v(uint8_t v) {
    for (int b = 0; b < 3; b++)
        reg((uint16_t)(0xFFC0 + 2 * b + ((v >> b) & 1)), 0);
}

static void ppm_dump(const char *name) {
    char path[128];
    snprintf(path, sizeof(path), "out_%s.ppm", name);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fprintf(fp, "P6\n%d %d\n255\n", VID_W, VID_ROWS);
    for (int y = 0; y < VID_ROWS; y++)
        for (int x = 0; x < VID_W; x++) {
            uint8_t c = fb[y][x];
            uint8_t rgb[3] = {                           // RRRGGGBB
                (uint8_t)(((c >> 5) & 7) * 36),          // RRR
                (uint8_t)(((c >> 2) & 7) * 36),          // GGG
                (uint8_t)(((c >> 0) & 3) * 85),          // BB
            };
            fwrite(rgb, 1, 3, fp);
        }
    fclose(fp);
}

// Count pixels of a colour inside the image area of one row.
static int row_count(int y, uint8_t c) {
    int n = 0;
    for (int x = VID_X_MARGIN; x < VID_W - VID_X_MARGIN; x++)
        if (fb[y][x] == c) n++;
    return n;
}

int main(void) {
    vdg_init();

    const uint8_t GREEN   = pal[C_GREEN],  BLACK  = pal[C_BLACK];
    const uint8_t DKGREEN = pal[C_DKGREEN], BUFF  = pal[C_BUFF];
    const uint8_t YELLOW  = pal[C_YELLOW], RED    = pal[C_RED];
    const uint8_t BLUE    = pal[C_BLUE],   CYAN   = pal[C_CYAN];

    // ================= alpha mode ====================================
    memset(coco_mem, 0, sizeof(coco_mem));
    memset(coco_mem + 0x0400, 0x60, 512);      // normal spaces (as BASIC's
                                               // cleared screen stores them)
    coco_mem[0x0400] = 0x41;                   // 'A' as BASIC stores it
    coco_mem[0x0401] = 0x01;                   // 'A' with bit6 clear
    sam_set_offset(2);                         // base $0400 (F=2)
    reg(0xFF22, 0x00);                         // alpha, css=0
    render_now();

    CHECK(vdg_sam_offset() == 0x0400, "SAM offset %04X != 0400",
          vdg_sam_offset());

    // Borders are black.
    CHECK(fb[0][0] == BLACK && fb[100][VID_W - 1] == BLACK,
          "side borders not black");

    // Anchor the INV polarity to the REAL MACHINE rather than to whatever
    // the renderer happens to do. BASIC clears the screen to $60 (a SPACE
    // with bit 6 set) and a blank CoCo screen is BRIGHT GREEN, so a $60
    // cell must come out fully lit. Checking only that the two cells are
    // complements of each other passes just as happily with the polarity
    // inverted, i.e. with the whole screen rendered as a photographic
    // negative.
    int blank = 0;
    for (int y = 0; y < 12; y++)
        for (int px = 0; px < 8; px++)
            if (fb[y][VID_X_MARGIN + (2 * 8 + px) * 2] == GREEN) blank++;
    CHECK(blank == 96, "$60 blank cell not fully lit (%d/96 green) -- "
                       "INV polarity inverted?", blank);

    // 'A' ($41, bit 6 set) is therefore a DARK glyph knocked out of a lit
    // cell, and the bit6-clear form is its complement.
    int fg0 = 0, fg1 = 0;
    for (int y = 0; y < 12; y++)
        for (int px = 0; px < 8; px++) {
            if (fb[y][VID_X_MARGIN + px * 2] == GREEN)            fg0++;
            if (fb[y][VID_X_MARGIN + (8 + px) * 2] == GREEN)      fg1++;
        }
    CHECK(fg0 > 96 - 40, "'A' should be a dark glyph on a lit cell "
                         "(only %d/96 green)", fg0);
    CHECK(fg1 > 5 && fg1 < 40, "bit6-clear 'A' should be a lit glyph on a "
                               "dark cell (%d/96 green)", fg1);
    CHECK(fg1 + fg0 == 96, "cells are not complements (%d + %d)", fg0, fg1);

    // Row 0 of the image area is entirely fg/bg (no stray colours).
    CHECK(row_count(0, DKGREEN) + row_count(0, GREEN) == 512,
          "alpha row contains colours outside the fg/bg pair");
    (void)DKGREEN;
    ppm_dump("alpha");

    // ================= semigraphics-4 ================================
    // 0x80|colour<<4|quads: yellow (colour 1), top-left quad only (bit3).
    memset(coco_mem + 0x0400, 0x80, 512);      // SG4 all-off cells
    coco_mem[0x0400] = 0x80 | (1 << 4) | 0x08;
    render_now();
    CHECK(fb[0][VID_X_MARGIN] == YELLOW, "SG4 top-left quad not yellow");
    CHECK(fb[0][VID_X_MARGIN + 4 * 2] == BLACK, "SG4 top-right quad lit");
    CHECK(fb[6][VID_X_MARGIN] == BLACK, "SG4 bottom-left quad lit");
    ppm_dump("sg4");

    // ================= CG tiers ======================================
    // CG1 64x64: 2bpp, colour set 0: 00=green 01=yellow 10=blue 11=red.
    memset(coco_mem, 0, sizeof(coco_mem));
    sam_set_offset(4);                         // base $0800
    coco_mem[0x0800] = 0x1B;                   // pixels 0,1,2,3 = 00 01 10 11
    reg(0xFF22, 0x80);                         // graphics, GM=000, css=0
    render_now();
    // CG1: 64 px wide -> xscale 4 -> each VDG pixel is 8 fb pixels.
    CHECK(fb[0][VID_X_MARGIN + 0]  == GREEN,  "CG1 px0 not green");
    CHECK(fb[0][VID_X_MARGIN + 8]  == YELLOW, "CG1 px1 not yellow");
    CHECK(fb[0][VID_X_MARGIN + 16] == BLUE,   "CG1 px2 not blue");
    CHECK(fb[0][VID_X_MARGIN + 24] == RED,    "CG1 px3 not red");
    // 64 rows -> each VDG row is 3 fb rows: row 2 same, row 3 next line.
    CHECK(fb[2][VID_X_MARGIN] == GREEN && fb[3][VID_X_MARGIN] == GREEN,
          "CG1 yscale wrong");   // next VDG row is byte 0x0810 (=0) -> green
    ppm_dump("cg1");

    // CG6 128x192 css=1: buff/cyan/magenta/orange.
    memset(coco_mem + 0x0800, 0x1B, 0x1800);
    reg(0xFF22, 0x80 | (3 << 5) | 0x08);       // GM=110 -> gm=6, css=1
    render_now();
    CHECK(vdg_mode_word() != 0, "");
    CHECK(fb[0][VID_X_MARGIN + 0] == BUFF, "CG6 px0 not buff");
    CHECK(fb[0][VID_X_MARGIN + 4] == CYAN, "CG6 px1 not cyan");
    ppm_dump("cg6");

    // ================= RG tiers ======================================
    // These verify the 1bpp bit-to-pixel mapping, so run them with
    // artifact colour off. With it on, alternating stripes like 0xAA are
    // a solid artifact colour on real composite -- correct, but it would
    // make these assertions about something they are not testing.
    vdg_set_artifact(0);
    // RG6 256x192 css=0: green on black, 1bpp.
    memset(coco_mem, 0, sizeof(coco_mem));
    sam_set_offset(4);
    for (int i = 0; i < 0x1800; i++) coco_mem[0x0800 + i] = 0xAA; // stripes
    reg(0xFF22, 0x80 | (7 << 4));              // GM=111, css=0
    render_now();
    CHECK(fb[0][VID_X_MARGIN + 0] == GREEN && fb[0][VID_X_MARGIN + 2] == BLACK,
          "RG6 stripe pattern wrong at left edge");
    CHECK(row_count(0, GREEN) == 256, "RG6 row0 green count %d != 256",
          row_count(0, GREEN));
    CHECK(row_count(191, GREEN) == 256, "RG6 last row wrong");
    ppm_dump("rg6");

    // RG1 128x64: xscale 2 (2 fb px per VDG px x2 doubling = 4), yscale 3.
    reg(0xFF22, 0x80 | (1 << 4));              // GM=001
    render_now();
    CHECK(fb[0][VID_X_MARGIN + 0] == GREEN && fb[0][VID_X_MARGIN + 4] == BLACK,
          "RG1 stripe scale wrong");
    ppm_dump("rg1");

    // ========== SAM V half-height scan (the Monster Maze case) ========
    // RG6 (gm=7, 256x192) driven by SAM V=4: the SAM fetches 32 bytes x
    // 96 rows = 3072 bytes and the VDG displays each row TWICE. Reading
    // all 6144 bytes as distinct rows instead puts the game's picture in
    // the top half of the screen with junk below.
    //
    // Light exactly one buffer row (the last the SAM scans, 95) and
    // poison everything past the 3072-byte buffer, so any over-read is
    // loudly visible rather than subtly wrong.
    memset(coco_mem, 0, sizeof(coco_mem));
    sam_set_offset(2);                             // base $0400, as the cart
    for (int i = 0; i < 32; i++)
        coco_mem[0x0400 + 95 * 32 + i] = 0xFF;
    for (int i = 0x0400 + 3072; i < 0x0400 + 6144; i++)
        coco_mem[i] = 0xFF;                        // must never be displayed
    sam_set_v(4);                                  // V2 set, V1/V0 clear
    reg(0xFF22, 0xFF);                             // RG6, css=1 -> BUFF
    render_now();
    CHECK((vdg_mode_word() >> 8) == 4, "SAM V decoded as %u, want 4",
          (unsigned)(vdg_mode_word() >> 8));
    CHECK(vdg_sam_offset() == 0x0400, "base %04X != 0400", vdg_sam_offset());
    // Buffer row 95 doubles onto scanlines 190 and 191 -- and, because it
    // is the only lit row in the buffer, those must be the ONLY two lit
    // scanlines. Reading 6144 bytes linearly instead lights scanline 95
    // plus every scanline from 96 up (all poisoned): 97 of them.
    int lit = 0;
    for (int y = 0; y < 192; y++) if (row_count(y, BUFF)) lit++;
    CHECK(lit == 2, "V=4: want exactly 2 lit scanlines (row 95, doubled), "
                    "got %d -- half-height SAM scan not honoured", lit);
    // A fully lit row is 512 fb pixels: 256 VDG px x2 horizontal doubling.
    CHECK(row_count(190, BUFF) == 512 && row_count(191, BUFF) == 512,
          "V=4: row 95 should fill scanlines 190-191 (%d/%d)",
          row_count(190, BUFF), row_count(191, BUFF));
    ppm_dump("rg6_samv4");

    // A self-consistent pairing must be left completely alone: RG6 with
    // V=6 is the full 6144-byte screen, 192 distinct rows, yscale 1.
    memset(coco_mem, 0, sizeof(coco_mem));
    for (int i = 0; i < 32; i++)
        coco_mem[0x0400 + 191 * 32 + i] = 0xFF;
    sam_set_v(6);
    render_now();
    CHECK(row_count(191, BUFF) == 512 && row_count(190, BUFF) == 0,
          "V=6: full-height scan altered (%d/%d)",
          row_count(191, BUFF), row_count(190, BUFF));

    // A STALE OR MIS-CAPTURED V MUST NEVER MOVE THE PICTURE. s_sam_v
    // comes from captured writes, capture can drop them, and an unguarded
    // override then rewrites the geometry for every cart. Each V below
    // disagrees with RG6 and must be ignored, leaving the full-height
    // render byte-identical.
    memset(coco_mem, 0, sizeof(coco_mem));
    for (int i = 0; i < 32; i++)
        coco_mem[0x0400 + 191 * 32 + i] = 0xFF;    // only the last row lit
    static const uint8_t stale_v[] = { 1, 2, 3, 5, 7 };
    for (unsigned i = 0; i < sizeof stale_v; i++) {
        sam_set_v(stale_v[i]);
        render_now();
        CHECK(row_count(191, BUFF) == 512 && row_count(190, BUFF) == 0,
              "stale SAM V=%u altered RG6 geometry (%d/%d) -- the SAM may "
              "only override a mode it agrees with", stale_v[i],
              row_count(191, BUFF), row_count(190, BUFF));
    }

    sam_set_v(0);                                  // leave state neutral

    // ============ PMODE 4 artifact colour (Zaxxon, Donkey King) =======
    // Both carts write $F8 = RG6 + CSS=1. The 6847 makes no colour at
    // all here; it emits dots at two per NTSC subcarrier cycle and the
    // MONITOR invents the colour (see the NTSC decode notes in vdg.c).
    //
    // These assert the MODEL's properties, not a pair-quantised palette.
    // Colouring pixel pairs destroys luma resolution and turns glyph
    // stems into fringed blocks.
    memset(coco_mem, 0, sizeof(coco_mem));
    sam_set_offset(2);                             // base $0400
    vdg_set_artifact(1);
    reg(0xFF22, 0xF8);                             // RG6, CSS=1

    // Border follows the colour set in EVERY graphics mode -- buff for
    // CSS=1, not black. Zaxxon's white surround is border, not content.
    render_now();
    CHECK(fb[0][0] == BUFF, "RG6 CSS=1 border should be buff, got %u",
          fb[0][0]);

    // A solid run carries no chroma: the demodulated boxcar cancels, so
    // 0xFF is white and 0x00 is black however long the run.
    for (int i = 0; i < 32; i++) coco_mem[0x0400 + i] = 0xFF;
    render_now();
    CHECK(fb[0][VID_X_MARGIN + 16] == BUFF,
          "solid 1s must stay white (no chroma), got %u",
          fb[0][VID_X_MARGIN + 16]);

    // A SUSTAINED alternating pattern IS a 3.58 MHz signal and must
    // saturate to a colour -- this is the whole artifact effect.
    for (int i = 0; i < 32; i++) coco_mem[0x0400 + i] = 0xAA;
    render_now();
    uint8_t sat_a = fb[0][VID_X_MARGIN + 16];
    CHECK(sat_a != BLACK && sat_a != BUFF,
          "sustained 1010 must saturate to a colour, got %u", sat_a);
    // ...and the opposite pattern gives the OTHER axis.
    for (int i = 0; i < 32; i++) coco_mem[0x0400 + i] = 0x55;
    render_now();
    uint8_t sat_b = fb[0][VID_X_MARGIN + 16];
    CHECK(sat_b != sat_a, "0x55 and 0xAA must land on opposite axes");

    // THE CRISPNESS TEST. An isolated dot is a transient, not a
    // sustained subcarrier, so a comb-filtered display renders it nearly
    // luma-pure. A pair quantiser turns it into a saturated colour block,
    // which makes text unreadable. Assert the lone dot stays much closer
    // to white than to either artifact axis.
    memset(coco_mem, 0, sizeof(coco_mem));
    coco_mem[0x0400] = 0x40;                       // one dot, isolated
    render_now();
    {
        uint8_t px = fb[0][VID_X_MARGIN + 2];      // dot 1, doubled
        int r = ((px >> 5) & 7) * 255 / 7;
        int g = ((px >> 2) & 7) * 255 / 7;
        int b = (px & 3) * 255 / 3;
        CHECK(r > 128 && g > 96 && b > 96,
              "isolated dot must render near luma-pure (got r%d g%d b%d) -- "
              "colouring it is what made text a fringed block", r, g, b);
    }

    // Phase B swaps which axis a sustained pattern lands on.
    for (int i = 0; i < 32; i++) coco_mem[0x0400 + i] = 0xAA;
    vdg_set_artifact(2);
    render_now();
    CHECK(fb[0][VID_X_MARGIN + 16] == sat_b,
          "phase B should swap the artifact axes");

    // Off restores plain 1bpp mono.
    vdg_set_artifact(0);
    render_now();
    CHECK(fb[0][VID_X_MARGIN + 0] == BUFF && fb[0][VID_X_MARGIN + 2] == BLACK,
          "artifact off must render 1bpp mono");

    // Narrower RG modes clock each pixel twice; no colour to recover.
    vdg_set_artifact(1);
    reg(0xFF22, 0x80 | (1 << 4) | 0x08);           // RG1 128 wide, CSS=1
    render_now();
    for (int x = 0; x < 256; x++)
        CHECK(fb[0][VID_X_MARGIN + x] == BLACK || fb[0][VID_X_MARGIN + x] == BUFF,
              "RG1 must stay mono under artifact (px %d = %u)",
              x, fb[0][VID_X_MARGIN + x]);
    vdg_set_artifact(0);          // later mono assertions depend on this

    // ================= display offset relocation =====================
    memset(coco_mem, 0, sizeof(coco_mem));
    coco_mem[0x3000] = 0xFF;                   // 8 lit pixels
    sam_set_offset(0x18);                      // 0x18*512 = 0x3000
    reg(0xFF22, 0x80 | (7 << 4));
    render_now();
    CHECK(vdg_sam_offset() == 0x3000, "offset reloc %04X", vdg_sam_offset());
    // 8 lit VDG pixels, x2 horizontal doubling = 16 framebuffer pixels.
    CHECK(row_count(0, GREEN) == 16, "reloc: %d green px, want 16",
          row_count(0, GREEN));

    // ============ register floods must not lose a page flip ==========
    // Dragon Fire writes $FF22 ~105 times per FRAME from a per-scanline
    // raster loop. A queue between the bus and the renderer overflows on
    // that, and the dropped events take SAM page-flip writes with them:
    // the renderer stays on the wrong display page, showing background
    // with no sprites. Writes are applied on arrival, so the flood must
    // be survivable AND must not swallow a following flip.
    uint32_t before = vdg_reg_writes();
    for (int i = 0; i < 5000; i++) reg(0xFF22, (uint8_t)i);   // raster storm
    CHECK(vdg_reg_writes() - before == 5000, "every write applied");

    sam_set_offset(0x30);                        // 0x30*512 = 0x6000
    CHECK(vdg_sam_offset() == 0x6000,
          "page flip after a flood: got %04X", vdg_sam_offset());
    for (int i = 0; i < 5000; i++) reg(0xFF22, (uint8_t)i);   // and again
    sam_set_offset(0x18);
    CHECK(vdg_sam_offset() == 0x3000,
          "flip back after a flood: got %04X", vdg_sam_offset());
    render_now();                                // must not wedge or crash

    // ================= 60 Hz cap =====================================
    uint32_t f0 = vdg_frames();
    vdg_pump(); vdg_pump(); vdg_pump();         // no time passes
    CHECK(vdg_frames() == f0, "renders without the 16.7 ms tick");
    shim_advance_us(17000);
    vdg_pump();
    CHECK(vdg_frames() == f0 + 1, "missed the tick render");

    if (failures) { printf("test_vdg: %d FAILURES\n", failures); return 1; }
    printf("test_vdg: OK (alpha/inverse, SG4, CG1/CG6, RG1/RG6, "
           "SAM offsets, flood-proof reg writes, 60 Hz cap; PPMs written)\n");
    return 0;
}
