// ======================================================================
// vdg.c — MC6847 model: turns the snooped 64K mirror + PIA/SAM register
// state into the RGB332 framebuffer.
//
// Register state arrives via video_note_reg() and is applied at once.
// Rendering happens on core 0 at ~60 Hz; tearing against the free-
// running DMA scanout is accepted, exactly like the design's other
// non-genlock compromises.
//
// Mode inputs:
//   $FF22 bits: b7 = A/G, b6 = GM0/INV-ish, b5..b3 = GM2..GM0 / CSS
//               (see mode decode below), b1 = single-bit sound
//   $FFC0..$FFC5 = SAM V0..V2, $FFC6..$FFD3 = SAM F0..F6 display offset.
//               Each SAM bit is a CLEAR/SET *address pair*: even address
//               clears, odd sets. The data byte is meaningless.
// ======================================================================

#include "video.h"
#include "config.h"
#include "bus.h"
#include "vdgfont.h"

#include "pico/stdlib.h"

#include <string.h>

// ---- VDG palette (RGB332 approximations of the MC6847 colours) -------
enum { C_GREEN, C_YELLOW, C_BLUE, C_RED, C_BUFF, C_CYAN, C_MAGENTA,
       C_ORANGE, C_BLACK, C_DKGREEN, C_DKORANGE };

static const uint8_t pal[] = {
    [C_GREEN]    = RGB332(0x00, 0xC0, 0x00),
    [C_YELLOW]   = RGB332(0xC0, 0xC0, 0x00),
    [C_BLUE]     = RGB332(0x00, 0x00, 0xC0),
    [C_RED]      = RGB332(0xC0, 0x00, 0x00),
    [C_BUFF]     = RGB332(0xE0, 0xE0, 0xE0),
    [C_CYAN]     = RGB332(0x00, 0xC0, 0xC0),
    [C_MAGENTA]  = RGB332(0xC0, 0x00, 0xC0),
    [C_ORANGE]   = RGB332(0xE0, 0x60, 0x00),
    [C_BLACK]    = RGB332(0x00, 0x00, 0x00),
    [C_DKGREEN]  = RGB332(0x00, 0x20, 0x00),
    [C_DKORANGE] = RGB332(0x20, 0x08, 0x00),
};

// ---- decoded state ---------------------------------------------------
static uint8_t  s_ff22;                  // last write to $FF22
static uint8_t  s_sam_v;                 // SAM V0..V2
static uint8_t  s_artifact = 1;          // 0 off, 1 phase A, 2 phase B
static uint16_t s_scr_base;              // geometry the last frame used
static uint16_t s_scr_bytes;
static uint32_t s_mode_chg;              // $FF22 value changes
static uint32_t s_mode_chg_last;         // ...at the last rate sample
static uint32_t s_frames_last;           // frames at the last rate sample
static uint16_t s_sam_f;                 // SAM F0..F6 (offset / 512)
static uint32_t s_frames;
static uint32_t s_reg_writes;            // register writes applied
static bool     s_dirty = true;
static absolute_time_t s_next_frame;

// Register writes are APPLIED IMMEDIATELY -- there is no queue between
// bus_pump() and here, and there must not be one. Both run on core 0, so
// a queue buys nothing, and any queue that can drop is a queue that will
// eventually drop a SAM page-flip: Dragon Fire's raster loop writes
// $FF22 ~6300 times a second, and a lost F2 event parks the renderer one
// third of a screen away from the page the game is drawing -- background
// visible, sprites not -- while composite shows the game running fine.
//
// Stays RAM-resident: it is called from the core-0 pump loop and a romfs
// erase must never collide with a register update. The audit enforces
// it.
void __not_in_flash_func(video_note_reg)(uint16_t addr, uint8_t data) {
    s_reg_writes++;
    if (addr == MC_REG_PIA1_DB) {
        if (data != s_ff22) { s_ff22 = data; s_dirty = true; s_mode_chg++; }
    } else if (addr >= MC_REG_SAM_BASE && addr <= MC_REG_SAM_END) {
        // SAM bits are selected by ADDRESS, not data: even clears, odd
        // sets, one address pair per bit.
        uint32_t idx = (addr - MC_REG_SAM_BASE) >> 1;    // which bit
        bool     set = (addr & 1u) != 0;                 // odd = set
        if (idx < 3) {                                   // V0..V2
            uint8_t m = (uint8_t)(1u << idx);
            uint8_t v = set ? (s_sam_v | m) : (s_sam_v & (uint8_t)~m);
            if (v != s_sam_v) { s_sam_v = v; s_dirty = true; }
        } else if (idx < 10) {                           // F0..F6
            uint16_t m = (uint16_t)(1u << (idx - 3));
            uint16_t f = set ? (s_sam_f | m) : (s_sam_f & (uint16_t)~m);
            if (f != s_sam_f) { s_sam_f = f; s_dirty = true; }
        }
    }
}

uint32_t vdg_reg_writes(void) { return s_reg_writes; }

// ---- NTSC artifact decoder -------------------------------------------
// Implements the NTSC decode model from the coco-hdmi project. The 6847
// never makes a colour in PMODE 4: it emits dots at exactly two per NTSC
// subcarrier cycle, and the MONITOR's decoder invents the colour.
//
//   chroma C[n] = boxcar4(s[n] * (-1)^n)     narrow band, demodulated
//   luma   Y[n] = s[n]                       wide band, "kernel B"
//   RGB    = (1-a)*Y*white + a*axis,  a = |C|, sign of C picks the axis
//
// Kernel B (dot-sharp, no notch) rather than the composite-faithful
// [1,2,1]/4, because the reference display comb-filters: it suppresses
// chroma on static vertical detail and keeps text crisp. That is THE
// reason composite shows Zaxxon's title legibly -- luma keeps the full
// 256-dot resolution while only chroma is narrowband. Colouring each
// pixel PAIR instead quantises luma to 128 cells and turns glyph stems
// into fringed blocks.
//
// The model's output depends only on the 4-dot window s[n-1..n+2] and
// the phase -- 32 combinations -- so this table IS the continuous model,
// evaluated exactly, at one lookup per dot.
static uint8_t s_art_lut[32];

static void artifact_build(void) {
    // The +C and -C axis ends, from the reference model.
    static const int ORANGE[3] = { 232, 72,  24 };
    static const int BLUE[3]   = {  48, 64, 240 };
    const int *pos = (s_artifact == 2) ? BLUE   : ORANGE;
    const int *neg = (s_artifact == 2) ? ORANGE : BLUE;

    for (int p = 0; p < 2; p++) {
        for (int idx = 0; idx < 16; idx++) {
            int sm1 = (idx >> 3) & 1, s0 = (idx >> 2) & 1;
            int sp1 = (idx >> 1) & 1, sp2 = idx & 1;
            // 2*C from the demodulated 4-dot boxcar; phase flips the sign.
            int c = (s0 + sp2) - (sm1 + sp1);          // -2..+2
            if (p) c = -c;

            // a = |C| in {0, 1/2, 1}. DE-EMPHASISED to a^2: on a
            // comb-filtered display an isolated transient renders nearly
            // luma-pure and only a sustained pattern saturates. That is
            // what keeps single-dot glyph stems white instead of smearing
            // them into colour.
            int a2 = (c < 0 ? -c : c);                 // 0,1,2  = 2a
            int an = a2 * a2;                          // 0,1,4  = 4a^2
            const int *ax = (c > 0) ? pos : neg;
            int yy = s0 ? 255 : 0;

            int r = ((4 - an) * yy + an * ax[0]) / 4;
            int g = ((4 - an) * yy + an * ax[1]) / 4;
            int b = ((4 - an) * yy + an * ax[2]) / 4;
            s_art_lut[(p << 4) | idx] = RGB332(r, g, b);
        }
    }
}

// Artifact phase is a display preference, not bus state: it is settable
// at runtime so the two phases can be compared without a reflash, which
// matters because only looking at the screen can say which is right.
void vdg_set_artifact(uint8_t mode) {
    s_artifact = (mode > 2) ? 0 : mode;
    artifact_build();
    s_dirty    = true;
}
uint8_t vdg_artifact(void) { return s_artifact; }

uint16_t vdg_screen_base(void)  { return s_scr_base; }
uint16_t vdg_screen_bytes(void) { return s_scr_bytes; }

// $FF22 CHANGES PER RENDERED FRAME. A cart doing a per-scanline effect
// changes the mode register many times WITHIN one frame; we sample it
// once per frame and land on an arbitrary one of those values, so the
// whole screen alternates between palettes. A high value here says the
// flicker is that renderer limitation (Dragon Fire's raster loop runs
// ~105 iterations per frame); ~0 says the fault is upstream.
uint32_t vdg_mode_rate(void) {
    uint32_t n = s_mode_chg - s_mode_chg_last;
    s_mode_chg_last = s_mode_chg;
    uint32_t f = s_frames - s_frames_last;
    s_frames_last = s_frames;
    return f ? (n / f) : n;
}

uint32_t vdg_frames(void)     { return s_frames; }
uint16_t vdg_mode_word(void)  { return (uint16_t)s_ff22 | ((uint16_t)s_sam_v << 8); }
uint16_t vdg_sam_offset(void) { return (uint16_t)(s_sam_f * 512u); }

void vdg_init(void) {
    // SAM/PIA registers are write-only and the CoCo programs them once
    // at ITS reset. If this firmware starts (or OTA-restarts) after the
    // CoCo has already booted, those writes are never seen. Default the
    // display base to the text screen at $0400 (F=2) rather than $0000,
    // which would render the direct page as characters.
    s_ff22 = 0; s_sam_v = 0; s_sam_f = 2;
    artifact_build();                   // before any frame can be drawn
    s_next_frame = get_absolute_time();
    s_dirty = true;
}

// ---- pixel helpers ---------------------------------------------------
// One VDG pixel = 2 framebuffer pixels horizontally (256 -> 512).

static inline void put2(uint8_t *row, int vx, uint8_t c) {
    int x = VID_X_MARGIN + vx * 2;
    row[x] = c;
    row[x + 1] = c;
}

static void fill_borders(uint8_t *row, uint8_t c) {
    memset(row, c, VID_X_MARGIN);
    memset(row + VID_W - VID_X_MARGIN, c, VID_X_MARGIN);
}

// ---- renderers -------------------------------------------------------

// Alphanumeric / semigraphics-4: 32x16 cells of 8x12 pixels. The VDG
// cell is 12 rows tall; we store 192 rows, so a cell maps 1:1.
static void render_alpha(const uint8_t *mem, uint16_t base, bool css) {
    uint8_t fg = css ? pal[C_ORANGE] : pal[C_GREEN];
    uint8_t bg = css ? pal[C_DKORANGE] : pal[C_DKGREEN];

    for (int cy = 0; cy < 16; cy++) {
        for (int sub = 0; sub < 12; sub++) {
            int y = cy * 12 + sub;
            uint8_t *row = hstx_row(y);
            fill_borders(row, pal[C_BLACK]);

            for (int cx = 0; cx < 32; cx++) {
                uint8_t ch = mem[(uint16_t)(base + cy * 32 + cx)];

                if (ch & 0x80) {
                    // Semigraphics 4: 2x2 blocks, colour in bits 6..4.
                    uint8_t colour = pal[(ch >> 4) & 7];
                    int half = (sub < 6) ? 0 : 1;
                    for (int px = 0; px < 8; px++) {
                        int quad = (half ? 0 : 2) + (px < 4 ? 1 : 0);
                        bool on = (ch >> quad) & 1u;
                        put2(row, cx * 8 + px, on ? colour : pal[C_BLACK]);
                    }
                } else {
                    // Alphanumeric. D6 drives the 6847's INV directly:
                    // bit 6 SET means inverse, i.e. a lit background with
                    // the glyph knocked out of it.
                    //
                    // Anchored to the real machine, not to a datasheet
                    // reading: BASIC clears the screen to $60 (verified in
                    // an XRoar snapshot — every blank cell is $60), $60 is
                    // a SPACE ($20) with bit 6 set, and a blank CoCo screen
                    // is bright green. A space has no lit glyph pixels, so
                    // the only way it comes out green is with the
                    // background lit. Hence bit 6 set => INV asserted, and
                    // 'A' ($41) is a dark glyph on green — the familiar
                    // black-on-green CoCo text. Inverting this test renders
                    // the entire BASIC screen as a photographic negative.
                    bool inv = (ch & 0x40) != 0;
                    uint8_t code = ch & 0x3F;
                    uint8_t ascii = (code < 0x20) ? (uint8_t)(code + 0x40) : code;
                    const uint8_t *g = vdgfont[ascii - 0x20];

                    for (int px = 0; px < 8; px++) {
                        bool on = false;
                        // Glyph is 5 wide, centred in 8; rows 1..7 of 12.
                        if (px >= 1 && px <= 5 && sub >= 2 && sub <= 8)
                            on = (g[px - 1] >> (sub - 2)) & 1u;
                        if (inv) on = !on;
                        put2(row, cx * 8 + px, on ? fg : bg);
                    }
                }
            }
        }
    }
}

// Colour graphics: 2 bits per pixel, 4 colours from one of two sets.
static void render_cg(const uint8_t *mem, uint16_t base, bool css,
                      int px_w, int rows, int bytes_per_row) {
    const uint8_t *set = css ? (const uint8_t[]){ pal[C_BUFF], pal[C_CYAN],
                                                  pal[C_MAGENTA], pal[C_ORANGE] }
                             : (const uint8_t[]){ pal[C_GREEN], pal[C_YELLOW],
                                                  pal[C_BLUE], pal[C_RED] };
    int yscale = 192 / rows;
    int xscale = 256 / px_w;
    // MC6847 border: black ONLY in the alphanumeric modes. In every
    // graphics mode it takes the colour-set colour -- green (CSS=0) or
    // buff (CSS=1). Zaxxon runs RG6 with CSS=1, and its white surround
    // is border, not screen content; a black border here is wrong.
    uint8_t bord = css ? pal[C_BUFF] : pal[C_GREEN];

    for (int y = 0; y < 192; y++) {
        uint8_t *row = hstx_row(y);
        fill_borders(row, bord);
        int sy = y / yscale;
        for (int x = 0; x < px_w; x++) {
            uint8_t b = mem[(uint16_t)(base + sy * bytes_per_row + (x >> 2))];
            uint8_t c = set[(b >> (6 - 2 * (x & 3))) & 3];
            for (int r = 0; r < xscale; r++)
                put2(row, x * xscale + r, c);
        }
    }
}

// Resolution graphics: 1 bit per pixel, on/off in one of two colours.
static void render_rg(const uint8_t *mem, uint16_t base, bool css,
                      int px_w, int rows, int bytes_per_row) {
    uint8_t on  = css ? pal[C_BUFF] : pal[C_GREEN];
    uint8_t off = pal[C_BLACK];
    int yscale = 192 / rows;
    int xscale = 256 / px_w;
    uint8_t bord = css ? pal[C_BUFF] : pal[C_GREEN];   // see render_cg

    // NTSC ARTIFACT COLOUR. In PMODE 4 the VDG clocks 256 pixels per line
    // at twice the colour subcarrier, so on composite every PAIR of
    // pixels resolves to a single colour sample: 00 black, 11 white, and
    // the two mixed pairs come out as the artifact colours. Games are
    // designed around it -- Zaxxon and Donkey King both run RG6 with
    // CSS=1 ($F8), and drawn a pixel at a time they are pure black and
    // white.
    //
    // Only the 256-wide mode artifacts. The narrower RG modes clock each
    // pixel twice, so pairs are uniform and there is no colour to recover
    // -- rendering them through this path would invent colour that real
    // hardware does not show.
    //
    // The phase is NOT derivable from anything we can see. On real
    // hardware it settles arbitrarily at power-on, which is why so many
    // CoCo games ask you to reset until the colours come out right. So it
    // is a runtime setting, not a constant, and CSS flips it -- carts
    // toggle CSS to swap the pair, which is why Zaxxon writes both $F0
    // and $F8.
    if (s_artifact && px_w == 256) {
        // Phase lives entirely in the LUT's axis assignment (sign of C
        // picks the hue); do NOT also flip it here or the two cancel.
        for (int y = 0; y < 192; y++) {
            uint8_t *row = hstx_row(y);
            fill_borders(row, bord);
            int sy = y / yscale;
            uint16_t rb = (uint16_t)(base + sy * bytes_per_row);

            // Unpack the row to one dot per byte, with two zero dots of
            // guard at each end so the 4-tap window needs no bounds test.
            // d[i + 1] is dot i.
            uint8_t d[260];
            d[0] = 0;
            for (int i = 0; i < 256; i++)
                d[i + 1] = (uint8_t)((mem[(uint16_t)(rb + (i >> 3))]
                                      >> (7 - (i & 7))) & 1u);
            d[257] = d[258] = d[259] = 0;

            for (int x = 0; x < 256; x++) {
                uint32_t idx = ((uint32_t)d[x]     << 3)
                             | ((uint32_t)d[x + 1] << 2)
                             | ((uint32_t)d[x + 2] << 1)
                             |  (uint32_t)d[x + 3];
                put2(row, x, s_art_lut[((x & 1u) << 4) | idx]);
            }
        }
        return;
    }

    for (int y = 0; y < 192; y++) {
        uint8_t *row = hstx_row(y);
        fill_borders(row, bord);
        int sy = y / yscale;
        for (int x = 0; x < px_w; x++) {
            uint8_t b = mem[(uint16_t)(base + sy * bytes_per_row + (x >> 3))];
            uint8_t c = ((b >> (7 - (x & 7))) & 1u) ? on : off;
            for (int r = 0; r < xscale; r++)
                put2(row, x * xscale + r, c);
        }
    }
}

static void render_frame(void) {
    const uint8_t *mem = bus_mirror();
    if (!mem) return;

    uint16_t base = (uint16_t)(s_sam_f * 512u);
    bool ag  = (s_ff22 & 0x80) != 0;      // alpha(0) / graphics(1)
    bool css = (s_ff22 & 0x08) != 0;      // colour set select
    uint8_t gm = (uint8_t)((s_ff22 >> 4) & 7);   // GM2..GM0 (+ bit for RG)

    if (!ag) {
        s_scr_base = base; s_scr_bytes = 32 * 16;   // 32x16 character cells
        render_alpha(mem, base, css);
    } else {
        // GM encoding: bit0 selects RG (1bpp) vs CG (2bpp); bits 2..1
        // select the resolution tier. Matches the VDG's GM0..GM2.
        bool rg = (gm & 1) != 0;
        int tier = (gm >> 1) & 3;
        static const int cg_w[4]    = {  64,  128, 128, 128 };
        static const int cg_rows[4] = {  64,   64,  96, 192 };
        static const int rg_w[4]    = { 128,  128, 128, 256 };
        static const int rg_rows[4] = {  64,   96, 192, 192 };
        // The SAM's video counter -- not the VDG's mode -- decides how
        // many DISTINCT rows get fetched. When it scans fewer than 192
        // the VDG repeats each row to fill the screen, so a cart can run
        // a full-height display out of half the memory. Monster Maze
        // does exactly that: RG6 (gm=7, 256x192) fed by SAM V=4, which
        // is 32 bytes x 96 rows = 3072 bytes, not 6144. Rendered as 192
        // distinct rows the picture lands in the top half and the bottom
        // half shows whatever follows the buffer.
        //
        // GUARDED HARD, and the guard is the important part. s_sam_v is
        // accumulated from CAPTURED BUS WRITES; a stale or missed V bit
        // would otherwise silently rewrite the geometry for EVERY cart --
        // and because the HDMI panel draws the mirror, that looks exactly
        // like "capture is broken". So require the SAM and the VDG to
        // AGREE before the SAM is allowed a say:
        //   * same bytes per row -- if they disagree on the row width,
        //     this V is not describing this display at all; and
        //   * the row count is the mode's own, or exactly half of it --
        //     the documented row-doubling trick and nothing else.
        // A stale V=2 (32 bytes but 64 rows) fails the second test, a
        // stale V=1/3/5 (16 bytes) fails the first, V=0 is alphanumeric
        // and never applies. Only the deliberate half-height case
        // survives, which is the entire intended blast radius.
        static const int sam_rows[8] = { 0, 64, 64, 96, 96, 192, 192, 192 };
        static const int sam_bpr[8]  = { 0, 16, 32, 16, 32, 16, 32, 32 };
        int v     = s_sam_v & 7;
        int w     = rg ? rg_w[tier] : cg_w[tier];
        int r     = rg ? rg_rows[tier] : cg_rows[tier];
        int bpr   = rg ? (w / 8) : (w / 4);
        int r_sam = sam_rows[v];
        if (r_sam && sam_bpr[v] == bpr && (r_sam == r || r_sam * 2 == r))
            r = r_sam;

        // Publish what was ACTUALLY used, so /api/screen dumps exactly the
        // bytes that produced the frame on screen. Recomputing the
        // geometry in the dump path would let the two drift apart, and the
        // whole point of the dump is to be ground truth.
        s_scr_base  = base;
        s_scr_bytes = (uint16_t)(r * bpr);

        if (rg) render_rg(mem, base, css, w, r, bpr);
        else    render_cg(mem, base, css, w, r, bpr);
    }
    s_frames++;
}

void vdg_pump(void) {
    // Cap at ~60 Hz; re-render on a timer as well as on dirty state, so
    // plain RAM writes (which do not touch a register) still show up.
    if (absolute_time_diff_us(get_absolute_time(), s_next_frame) > 0)
        return;
    s_next_frame = delayed_by_us(get_absolute_time(), 16667);
    s_dirty = false;
    render_frame();
}
