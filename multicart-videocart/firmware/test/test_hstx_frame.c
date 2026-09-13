// ======================================================================
// test_hstx_frame.c — walks the videocart's HSTX scanout exactly as the
// hardware would: follow the control-block table (in order, checking
// the chain fields), stream every word into a model of the HSTX
// command/expander front end, and slice the result into scanlines.
//
// Asserts, against VESA 640x480@60 (800 clocks/line, 525 lines):
//   - every line is exactly 800 pixel-clocks, no command straddles a
//     line boundary;
//   - hsync is 96 clocks starting at clock 16 of every line;
//   - exactly 2 vsync lines, in the right place (lines 10-11);
//   - 45 blank lines then 480 active lines of exactly 640 TMDS pixels;
//   - TMDS runs consume whole words (4 RGB332 pixels/word);
//   - image rows appear twice each (vertical doubling), borders point
//     at the shared black row;
//   - the last block chains to CH_RESET and CH_RESET rewinds CH_CTRL
//     to the table base (frame loops forever);
//   - crossbar: clock on HSTX bits 0/1 with INV on the '-' leg (GP12),
//     lanes 0/1/2 on bit pairs 2/4/6 with INV on the even ('-') bit.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"
#include "cfg.h"

// ---- shim state (one definition per binary) --------------------------
uint64_t shim_now_us;
shim_hstx_ctrl_t shim_hstx_ctrl;
shim_hstx_fifo_t shim_hstx_fifo;
shim_bus_ctrl_t  shim_bus_ctrl;
shim_dma_hw_t    shim_dma;
int  shim_dma_next_chan;
uint shim_dma_started;
uint32_t shim_dma_cfg_write[16], shim_dma_cfg_read[16];
uint32_t shim_dma_cfg_count[16], shim_dma_cfg_ctrl[16];
bool     shim_dma_cfg_trig[16];
uint32_t shim_gpio_funcsel[48];

// ---- audio/cfg stubs so hstx.c can be built here ---------------------
// The frame is walked in HDMI mode, the shipping default, so the data
// islands are exercised: 56 of the 480 active lines scan as TWO
// descriptors instead of one, and every one of them still has to come to
// exactly 800 pixel clocks. The packet CONTENT is proved by test_audio;
// what matters here is that the descriptor table stays a valid frame.
static cfg_t stub_cfg = { .hdmi_audio = 1, .audio_gain = 2 };
cfg_t *cfg_get(void) { return &stub_cfg; }

static uint32_t stub_lap;
uint32_t bus_audio_take(uint32_t *ev) { (void)ev; return 0; }
uint32_t bus_audio_depth(void)        { return 0; }
uint32_t bus_audio_ring_size(void)    { return 0; }
uint32_t bus_audio_lap(void)          { return stub_lap; }
void     bus_audio_enable(bool on)    { (void)on; }
bool     hstx_hdmi_audio(void);

#include "audio.c"          // island + prefix builders
#include "hstx.c"           // code under test (statics visible)

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
} while (0)

// ---- HSTX stream consumer -------------------------------------------
#define MAX_LINES 600
typedef struct {
    int  clocks;            // total pixel-clocks
    int  tmds_pixels;
    int  hsync_start, hsync_len;
    bool vsync;
    bool overrun;           // a command crossed the 800-clock boundary
} line_t;

static line_t lines[MAX_LINES];
static int    nlines;
static line_t cur;

static void emit_clocks(int n, uint32_t sym, bool is_tmds) {
    if (cur.clocks + n > 800) cur.overrun = true;
    if (is_tmds) {
        cur.tmds_pixels += n;
    } else {
        // Sync decode: TMDS control symbols on lane 0.
        uint32_t l0 = sym & 0x3FFu;
        // Inside a data island there are no control codes: H and V ride
        // in the ch0 TERC4 nibble {1, header bit, vsync, hsync}. Islands
        // only sit on active lines, so vsync is always inactive and the
        // nibble is one of A/B/E/F -- which is what makes this decode
        // unambiguous. TERC4[8] is 0x2CC, the same word as the video
        // guard band, and since nibble 8 would mean vsync active it can
        // only ever BE the guard band here, carrying no sync information.
        for (int t = 0; t < 16; t++) {
            if (TERC4[t] != l0) continue;
            if (t == 0xA || t == 0xB || t == 0xE || t == 0xF) {
                if (!(t & 1)) {                       // hsync asserted (low)
                    if (cur.hsync_len == 0) cur.hsync_start = cur.clocks;
                    cur.hsync_len += n;
                }
                cur.clocks += n;
                if (cur.clocks >= 800) {
                    if (nlines < MAX_LINES) lines[nlines++] = cur;
                    memset(&cur, 0, sizeof(cur));
                }
                return;
            }
            break;                                    // guard band: no sync
        }
        bool hs = (l0 == TMDS_CTRL_01) || (l0 == TMDS_CTRL_11);  // H=0? no:
        // TMDS_CTRL_xy encodes {vsync,hsync}: 00=V1H1? Map from hstx.c:
        // SYNC_V1_H1 uses TMDS_CTRL_11, V1_H0 uses TMDS_CTRL_10,
        // V0_H1 uses TMDS_CTRL_01, V0_H0 uses TMDS_CTRL_00.
        hs = (l0 == TMDS_CTRL_10) || (l0 == TMDS_CTRL_00);       // hsync ON
        bool vs = (l0 == TMDS_CTRL_01) || (l0 == TMDS_CTRL_00);  // vsync ON
        if (hs) {
            if (cur.hsync_len == 0) cur.hsync_start = cur.clocks;
            cur.hsync_len += n;
        }
        if (vs) cur.vsync = true;
    }
    cur.clocks += n;
    if (cur.clocks >= 800) {
        if (nlines < MAX_LINES) lines[nlines++] = cur;
        memset(&cur, 0, sizeof(cur));
    }
}

// The control blocks store 32-bit device addresses; on a 64-bit host
// the upper pointer bits are gone. Every block points at one of a known
// set of buffers, so recover the real pointer by matching low words.
static const uint32_t *real_ptr(uint32_t lo) {
    // The vblank array is not covered by a single descriptor: islands
    // split it into runs, so a block may begin at ANY line boundary
    // inside it.
    {
        uint32_t base = (uint32_t)(uintptr_t)s_vblank;
        uint32_t span = (uint32_t)sizeof s_vblank;
        if (lo >= base && lo < base + span &&
            ((lo - base) % (BLANK_CMD_WORDS * 4u)) == 0)
            return &s_vblank[(lo - base) / 4u];
    }

    if (lo == (uint32_t)(uintptr_t)s_black)  return s_black;
    for (int y = 0; y < VID_ROWS; y++)
        if (lo == (uint32_t)(uintptr_t)s_fb[y]) return s_fb[y];
    // HDMI mode: an island line scans as the island slot followed by a
    // pixels-only block, so both of those are legitimate read addresses.
    for (int k = 0; k < AUD_SLOTS; k++)
        if (lo == (uint32_t)(uintptr_t)s_island[k]) return s_island[k];
    if (lo == (uint32_t)(uintptr_t)&s_black[ROW_CMD_WORDS])
        return &s_black[ROW_CMD_WORDS];
    for (int y = 0; y < VID_ROWS; y++)
        if (lo == (uint32_t)(uintptr_t)&s_fb[y][ROW_CMD_WORDS])
            return &s_fb[y][ROW_CMD_WORDS];
    return NULL;
}

// Which framebuffer row a block draws, for the doubling and border
// checks -- a pixels-only block belongs to the same row as its prefix.
static int row_of(uint32_t lo) {
    for (int y = 0; y < VID_ROWS; y++)
        if (lo == (uint32_t)(uintptr_t)s_fb[y] ||
            lo == (uint32_t)(uintptr_t)&s_fb[y][ROW_CMD_WORDS]) return y;
    return -1;
}
static bool is_island_slot(uint32_t lo) {
    for (int k = 0; k < AUD_SLOTS; k++)
        if (lo == (uint32_t)(uintptr_t)s_island[k]) return true;
    return false;
}
static int isl_pos;
static int isl_line[ISLAND_LINES];   // scanline each island landed on

static bool is_black_row(uint32_t lo) {

    return lo == (uint32_t)(uintptr_t)s_black ||
           lo == (uint32_t)(uintptr_t)&s_black[ROW_CMD_WORDS];
}

// Feed one control block's words through the command parser.
// A TMDS run may START in one block and have its pixel words delivered by
// the NEXT one -- that is exactly what an HDMI island line does, and the
// HSTX FIFO has no notion of block boundaries. Carry the outstanding
// pixel-word count across calls rather than demanding each run fit.
static uint32_t pending_pix_words;

static void consume_block(const uint32_t *w, uint32_t nwords) {
    uint32_t i = 0;
    if (pending_pix_words) {
        uint32_t take = pending_pix_words < nwords ? pending_pix_words : nwords;
        i += take;
        pending_pix_words -= take;
    }
    while (i < nwords) {
        uint32_t cmd   = w[i] & 0xF000u;
        uint32_t count = w[i] & 0x0FFFu;
        i++;
        if (cmd == HSTX_CMD_NOP) continue;
        if (cmd == HSTX_CMD_RAW_REPEAT) {
            CHECK(i < nwords, "RAW_REPEAT data word missing");
            emit_clocks((int)count, w[i], false);
            i++;
        } else if (cmd == HSTX_CMD_TMDS) {
            CHECK(count % 4 == 0, "TMDS count %u not word-aligned", count);
            uint32_t words = count / 4;
            emit_clocks((int)count, 0, true);
            uint32_t here = (i + words <= nwords) ? words : (nwords - i);
            pending_pix_words = words - here;   // rest comes from the next block
            i += here;
        } else if (cmd == HSTX_CMD_RAW) {
            // Non-repeat: every word is its own clock, and in an island
            // they all differ -- feeding only the first would decode the
            // whole run as one symbol and hide the hsync inside it.
            if (i + count > nwords) {
                CHECK(0, "RAW run of %u overruns a %u-word block", count, nwords);
                return;                 // reading on is a real overrun
            }
            for (uint32_t k = 0; k < count; k++)
                emit_clocks(1, w[i + k], false);
            i += count;
        } else {
            CHECK(0, "unknown command %04X", cmd);
            return;
        }
    }
}

int main(void) {
    hstx_init(stub_cfg.hdmi_audio != 0);

    // ---- crossbar ----
    CHECK(shim_hstx_ctrl.bit[0] ==
              (HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS),
          "bit0 (GP12=TXC-) must be inverted clock");
    CHECK(shim_hstx_ctrl.bit[1] == HSTX_CTRL_BIT0_CLK_BITS,
          "bit1 (GP13=TXC+) must be clock");
    for (uint lane = 0; lane < 3; lane++) {
        uint32_t sel = (lane * 10) << HSTX_CTRL_BIT0_SEL_P_LSB
                     | (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        uint bit = 2 + lane * 2;
        CHECK(shim_hstx_ctrl.bit[bit] == (sel | HSTX_CTRL_BIT0_INV_BITS),
              "lane %u '-' leg (bit %u) wrong", lane, bit);
        CHECK(shim_hstx_ctrl.bit[bit + 1] == sel,
              "lane %u '+' leg (bit %u) wrong", lane, bit + 1);
    }
    for (int i = 12; i <= 19; i++)
        CHECK(shim_gpio_funcsel[i] == 0, "GP%d not set to HSTX funcsel", i);

    // ---- RGB332 layout vs the expander rotations ----
    // Lane2=red ROT=0 reads bits [7:5], lane1=green ROT=29 reads [4:2],
    // lane0=blue ROT=26 reads [1:0]. The RGB332() macro must agree or
    // red/blue land on the wrong TMDS lanes.
    CHECK(shim_hstx_ctrl.expand_tmds ==
              (2u  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
               0u  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
               2u  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
               29u << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
               1u  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
               26u << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB),
          "expander TMDS rotations changed — re-derive RGB332()");
    CHECK(RGB332(0xFF, 0, 0) == 0xE0, "red not in bits 7:5");
    CHECK(RGB332(0, 0xFF, 0) == 0x1C, "green not in bits 4:2");
    CHECK(RGB332(0, 0, 0xFF) == 0x03, "blue not in bits 1:0");

    // ---- DMA topology ----
    CHECK(shim_dma_next_chan == 3, "expected 3 DMA channels, claimed %d",
          shim_dma_next_chan);
    CHECK(shim_dma_cfg_write[s_ch_ctrl] ==
              (uint32_t)(uintptr_t)&dma_hw->ch[s_ch_data].read_addr,
          "CH_CTRL must target CH_DATA's alias-0 block");
    CHECK(shim_dma_cfg_count[s_ch_ctrl] == 4, "CH_CTRL count != 4");
    uint32_t cc = shim_dma_cfg_ctrl[s_ch_ctrl];
    CHECK((cc & SHIM_CTRL_RING_SEL) &&
          (((cc >> SHIM_CTRL_RING_LSB) & 0x1F) == 4),
          "CH_CTRL needs a 16-byte WRITE ring");
    CHECK(shim_dma_cfg_trig[s_ch_ctrl], "CH_CTRL must start triggered");
    CHECK(shim_dma_cfg_write[s_ch_reset] ==
              (uint32_t)(uintptr_t)&dma_hw->ch[s_ch_ctrl].al3_read_addr_trig,
          "CH_RESET must rewind CH_CTRL via al3_read_addr_trig");
    CHECK(shim_dma_cfg_read[s_ch_reset] == (uint32_t)(uintptr_t)&s_desc_base,
          "CH_RESET must read the table base");
    CHECK(*(uint32_t *)&s_desc_base == (uint32_t)(uintptr_t)s_desc,
          "s_desc_base doesn't point at the table");

    // ---- walk the frame ----
    hstx_test_pattern();    // realistic pixel content

    for (int i = 0; i < (int)s_desc_used; i++) {
        const ctrl_block_t *b = &s_desc[i];
        CHECK(b->write_addr == (uint32_t)(uintptr_t)&hstx_fifo_hw->fifo,
              "block %d writes somewhere other than the HSTX FIFO", i);
        uint chain = SHIM_CTRL_CHAIN(b->ctrl);
        if (i == (int)s_desc_used - 1)
            CHECK(chain == (uint)s_ch_reset, "last block must chain to RESET");
        else
            CHECK(chain == (uint)s_ch_ctrl, "block %d must chain to CTRL", i);
        CHECK(((b->ctrl >> SHIM_CTRL_DREQ_LSB) & 0xFF) == DREQ_HSTX + 1,
              "block %d not paced by DREQ_HSTX", i);
        const uint32_t *src = real_ptr(b->read_addr);
        CHECK(src != NULL, "block %d points at no known buffer", i);

        // Island slots rotate: position p is served by slot p % AUD_SLOTS,
        // and on hardware the refill IRQ has already written the shape for
        // position p into that slot before it is scanned. Do the same here,
        // otherwise the walk reads whichever position last used the slot --
        // and if the two disagree about blanking vs active, the word count
        // in the descriptor is wrong and the whole frame desynchronises.
        // That disagreement is precisely what this is here to catch.
        if (src && is_island_slot(b->read_addr)) {
            CHECK(isl_pos < ISLAND_LINES, "more island blocks than positions");
            if (isl_pos < ISLAND_LINES) {
                audio_fill_island(s_island[isl_pos % AUD_SLOTS], isl_pos,
                                  s_isl_blank[isl_pos] != 0);
                CHECK(b->read_addr ==
                      (uint32_t)(uintptr_t)s_island[isl_pos % AUD_SLOTS],
                      "island %d is not in slot %d", isl_pos,
                      isl_pos % AUD_SLOTS);
                CHECK(b->count == (s_isl_blank[isl_pos]
                                      ? AUD_ISLAND_BLANK_WORDS
                                      : AUD_ISLAND_WORDS),
                      "island %d descriptor is %u words, wrong for its shape",
                      isl_pos, b->count);
                isl_line[isl_pos] = nlines;   // line this island starts
                isl_pos++;
            }
        }
        if (src) consume_block(src, b->count);
    }
    CHECK(cur.clocks == 0, "frame ended mid-line (%d clocks over)", cur.clocks);

    // ---- descriptor sources: borders and doubled rows ----
    // Counted, not indexed: in HDMI mode an island line scans as two
    // descriptors, so fixed positions do not identify a line. Counting
    // is also the stronger claim -- it catches a row drawn three times or
    // a border line pointing at the wrong buffer anywhere in the table.
    int black_scans = 0, isl_blocks = 0;
    int per_row[VID_ROWS];
    memset(per_row, 0, sizeof per_row);
    for (int i = 0; i < (int)s_desc_used; i++) {
        uint32_t ra = s_desc[i].read_addr;
        if (is_black_row(ra)) black_scans++;
        int y = row_of(ra);
        if (y >= 0) per_row[y]++;
        for (int k = 0; k < AUD_SLOTS; k++)
            if (ra == (uint32_t)(uintptr_t)s_island[k]) isl_blocks++;
    }
    CHECK(black_scans == 2 * BORDER_LINES,
          "border scanned %d times, want %d", black_scans, 2 * BORDER_LINES);

    // EXACTLY the island blocks may raise the DMA interrupt. The SDK's
    // default config leaves IRQ_QUIET clear, so forgetting to set it on
    // the ordinary line blocks turns a 3.3 kHz refill into a 31.5 kHz
    // one that rewrites island slots mid-scan and takes the picture, the
    // web server and all the audio down with it.
    int noisy = 0;
    for (int i = 0; i < (int)s_desc_used; i++) {
        bool quiet = (s_desc[i].ctrl & SHIM_CTRL_IRQ_QUIET) != 0;
        bool isl = false;
        for (int k = 0; k < AUD_SLOTS; k++)
            if (s_desc[i].read_addr == (uint32_t)(uintptr_t)s_island[k]) isl = true;
        if (!quiet) {
            noisy++;
            CHECK(isl, "block %d raises an IRQ but is not an island block", i);
        } else {
            CHECK(!isl, "island block %d is quiet and will never be refilled", i);
        }
    }
    CHECK(noisy == (stub_cfg.hdmi_audio ? ISLAND_LINES : 0),
          "%d blocks raise the refill IRQ, want %d", noisy,
          stub_cfg.hdmi_audio ? ISLAND_LINES : 0);
    CHECK(isl_blocks == (stub_cfg.hdmi_audio ? ISLAND_LINES : 0),
          "%d island blocks in the table, want %d", isl_blocks,
          stub_cfg.hdmi_audio ? ISLAND_LINES : 0);
    CHECK(s_island_count == (uint32_t)(stub_cfg.hdmi_audio ? ISLAND_LINES : 0),
          "hstx reports %u island lines, want %d", s_island_count,
          stub_cfg.hdmi_audio ? ISLAND_LINES : 0);
    for (int y = 0; y < VID_ROWS; y++) {
        CHECK(per_row[y] == 2,
              "row %d scanned %d times, want exactly 2", y, per_row[y]);
    }

    // ---- scanline structure ----
    CHECK(nlines == 525, "frame has %d lines, want 525", nlines);
    int vsync_lines = 0, active_lines = 0;
    for (int i = 0; i < nlines; i++) {
        line_t *l = &lines[i];
        CHECK(!l->overrun, "line %d: command crossed the line boundary", i);
        CHECK(l->clocks == 800, "line %d is %d clocks", i, l->clocks);
        CHECK(l->hsync_start == 16 && l->hsync_len == 96,
              "line %d hsync at %d len %d (want 16/96)",
              i, l->hsync_start, l->hsync_len);
        if (l->vsync) vsync_lines++;
        if (l->tmds_pixels) {
            active_lines++;
            CHECK(l->tmds_pixels == 640, "line %d has %d pixels",
                  i, l->tmds_pixels);
            CHECK(i >= 45, "active pixels inside vertical blanking (line %d)", i);
        }
    }
    CHECK(vsync_lines == 2, "%d vsync lines, want 2", vsync_lines);
    CHECK(lines[10].vsync && lines[11].vsync,
          "vsync not at lines 10-11 (front porch 10)");
    CHECK(active_lines == 480, "%d active lines, want 480", active_lines);

    // ---- island spread ----
    // This is the check that matters for audio. Islands confined to the
    // 480 ACTIVE lines leave a 1.44 ms hole across vertical blanking with
    // no audio packets in it: the sink has to coast ~69 samples once per
    // frame and buzzes at the 59.52 Hz frame rate. Every packet-shape
    // test passes regardless -- the packets are immaculate, there is
    // just a hole between them. Only a check on the GAPS can see that.
    if (stub_cfg.hdmi_audio) {
        CHECK(isl_pos == ISLAND_LINES, "%d islands in the table, want %d",
              isl_pos, ISLAND_LINES);
        int in_vblank = 0, worst = 0, worst_at = 0;
        for (int k = 0; k < isl_pos; k++) {
            if (isl_line[k] < 45) in_vblank++;
            int gap = (k == 0)
                ? isl_line[0] + (525 - isl_line[isl_pos - 1])  // across the wrap
                : isl_line[k] - isl_line[k - 1];
            if (gap > worst) { worst = gap; worst_at = isl_line[k]; }
        }
        CHECK(in_vblank > 0,
              "no islands in vertical blanking -- the frame has a silent hole");
        // Nominal spacing is 525/80 = 6.6 lines. Deferring an island off
        // a vsync line can stretch one gap, but nothing should approach
        // the 45-line hole that active-only placement leaves.
        CHECK(worst <= 14,
              "widest gap between islands is %d lines (at line %d), want <= 14",
              worst, worst_at);
    }

    if (failures) { printf("test_hstx_frame: %d FAILURES\n", failures); return 1; }
    printf("test_hstx_frame: OK (525 lines x 800 clocks, hsync 16+96, "
           "vsync @10-11, 480x640px, doubled rows, ring rewind, islands spread over the whole frame with no gap > 14 lines)\n");
    return 0;
}
