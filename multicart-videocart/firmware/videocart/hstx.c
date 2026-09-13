// ======================================================================
// hstx.c — 640x480 DVI/HDMI out on the RP2350 HSTX, scanned by a
// self-chaining DMA pair with NO CPU and NO interrupts on the video path.
//
// Why not the pico-examples IRQ approach: core 0 disables interrupts for
// the length of a flash sector erase (~45 ms) on every ROM upload and
// every cfg save. An IRQ-fed scanout would lose sync for that whole
// window. Here the frame is a static descriptor list in SRAM, walked by
// a DMA control channel that reloads a DMA data channel forever, so a
// flash erase is invisible to the display.
//
//   CH_CTRL:  reads 4-word control blocks {read, write, count, ctrl}
//             and writes them into CH_DATA's alias-0 registers. Writing
//             al0_ctrl (the trigger) starts CH_DATA. A 16-byte write
//             ring keeps CH_CTRL hitting the same four registers when
//             it is re-triggered (the pico-examples control_blocks
//             pattern; transfer_count reloads on trigger).
//   CH_DATA:  pushes words into the HSTX FIFO paced by DREQ_HSTX and
//             chains back to CH_CTRL when done.
//   CH_RESET: the LAST control block gives CH_DATA a ctrl word whose
//             CHAIN_TO is this channel instead of CH_CTRL. It writes
//             the table base into CH_CTRL's al3_read_addr_trig, which
//             both rewinds and restarts the frame. One fixed word, no
//             increments, so it needs no ring of its own.
//
// Note a hardware constraint that shapes all of the above: a DMA channel
// has ONE ring, read or write, not both (channel_config_set_ring
// overwrites). CH_CTRL needs its ring on the write side, so the read
// rewind has to come from somewhere else — hence CH_RESET.
//
// Frame layout (525 lines total, VESA 640x480@60):
//   45 blanking lines (10 front porch, 2 vsync, 33 back porch) — runs of
//      a pre-built 45-line command array
//   48 black active lines (top border)  — one descriptor each
//  384 image lines = 192 rows x2       — one descriptor each
//   48 black active lines (bottom)     — one descriptor each
//
// Each image row carries its own command prefix inline, so a display line
// is normally a SINGLE descriptor covering commands + pixels (an HDMI
// data-island line is the exception and takes two).
// ======================================================================

#include "video.h"
#include "config.h"
#include "audio.h"
#include "cfg.h"

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

#include <string.h>

// ---- DVI/TMDS constants (from pico-examples dvi_out_hstx_encoder) ----
#define TMDS_CTRL_00 0x354u
#define TMDS_CTRL_01 0x0abu
#define TMDS_CTRL_10 0x154u
#define TMDS_CTRL_11 0x2abu

#define SYNC_V0_H0 (TMDS_CTRL_00 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V0_H1 (TMDS_CTRL_01 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H0 (TMDS_CTRL_10 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))
#define SYNC_V1_H1 (TMDS_CTRL_11 | (TMDS_CTRL_00 << 10) | (TMDS_CTRL_00 << 20))


#define HSTX_CMD_RAW         (0x0u << 12)
#define HSTX_CMD_RAW_REPEAT  (0x1u << 12)
#define HSTX_CMD_TMDS        (0x2u << 12)
#define HSTX_CMD_TMDS_REPEAT (0x3u << 12)
#define HSTX_CMD_NOP         (0xfu << 12)

// ---- geometry --------------------------------------------------------
// Command prefix per ACTIVE line. 11 words in both modes: HDMI needs the
// video preamble (8 clocks) and guard band (2) immediately before the
// pixels, and DVI pads to the same length with NOPs so the framebuffer
// layout -- and hstx_row() -- is identical either way.
#define ROW_CMD_WORDS   11
// Blanking lines never carry preamble or guard: those exist only to
// announce active video, and a vblank line is not followed by any within
// its own descriptor.
#define BLANK_CMD_WORDS 7
#define ROW_PIX_WORDS   (VID_W / 4)             // RGB332, 4 px per word
#define ROW_WORDS       (ROW_CMD_WORDS + ROW_PIX_WORDS)

#define VBLANK_LINES    (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH + MODE_V_BACK_PORCH)
#define IMAGE_LINES     (VID_ROWS * 2)
#define BORDER_LINES    ((MODE_V_ACTIVE_LINES - IMAGE_LINES) / 2)

// Control blocks: vblank runs + top border lines + IMAGE_LINES + bottom
// border lines. Border lines each get their own block, all pointing at
// ONE shared black row — that keeps every active line built from the
// exact command sequence the pico-examples encoder is known to work
// with (RAW_REPEAT syncs + a TMDS run of real pixel words; the
// datasheet's TMDS_REPEAT shortcut is deliberately not used, since the
// example never exercises it against the expander).
// No padding and no alignment requirement — CH_RESET rewinds the table,
// so it does not need to be a power of two.
// In HDMI mode each island-carrying line needs a SECOND descriptor (the
// slot, then the pixels), so the table is sized for the worst case and
// the DVI build simply leaves the tail unused: one descriptor per island,
// plus (worst case) one per vblank line that is split around an island,
// plus the active lines and their islands.
#define DESC_COUNT      (2 * VBLANK_LINES + 2 * BORDER_LINES \
                         + IMAGE_LINES + ISLAND_LINES + 4)

// ---- storage ---------------------------------------------------------

// Framebuffer: each row is [ROW_CMD_WORDS command words][160 pixel words].
// Commands live inline so one descriptor covers a whole display line.
static uint32_t s_fb[VID_ROWS][ROW_WORDS] __attribute__((aligned(4)));

static uint32_t s_vblank[VBLANK_LINES * BLANK_CMD_WORDS];
static uint32_t s_black[ROW_WORDS];     // shared all-black active line

// One control block per DMA transfer, in alias-0 register order:
//   +0 READ_ADDR  +4 WRITE_ADDR  +8 TRANS_COUNT  +C CTRL (trigger)
typedef struct {
    uint32_t read_addr;
    uint32_t write_addr;
    uint32_t count;
    uint32_t ctrl;
} ctrl_block_t;

static ctrl_block_t s_desc[DESC_COUNT];
static uint32_t     s_desc_base;        // = &s_desc[0], read by CH_RESET
static uint32_t     s_desc_used;        // descriptors actually built

static int s_ch_data, s_ch_ctrl, s_ch_reset;

// ---- helpers ---------------------------------------------------------

uint8_t *hstx_row(int y) {
    return (uint8_t *)&s_fb[y][ROW_CMD_WORDS];
}

void hstx_fill(uint8_t c) {
    for (int y = 0; y < VID_ROWS; y++)
        memset(hstx_row(y), c, VID_W);
}

// ---- HDMI mode (data islands) ----------------------------------------
// Staged at boot from cfg, because the descriptor table's SHAPE differs:
// in HDMI mode the island-carrying lines scan as two blocks instead of
// one. Nothing about the framebuffer or the renderer changes, so vdg.c
// never knows which mode is running.
#define ACTIVE_LINES   MODE_V_ACTIVE_LINES

static bool     s_hdmi;
static uint32_t s_island[AUD_SLOTS][AUD_ISLAND_WORDS];
static uint32_t s_island_count;
static uint32_t s_isl_seen;
// Which island positions sit on a blanking line. Fixed at build time, so
// the IRQ can fill a slot with the right shape for the position that will
// next be scanned out of it.
static uint8_t  s_isl_blank[ISLAND_LINES];
// Descriptor index of each island, so the refill handler can work out
// WHICH island just finished from the scanout position rather than
// trusting its own running count.
static uint16_t s_isl_desc[ISLAND_LINES];
static uint32_t s_isl_fix;      // times the running count had to be corrected



// Exactly ROW_CMD_WORDS words in either mode. DVI is the three sync runs
// plus NOP padding (a NOP consumes no pixel clocks, so the line is still
// 800); HDMI hands off to audio.c, which shortens the back porch by the
// 10 clocks the preamble and guard band occupy.
static uint32_t *emit_active_line_cmds(uint32_t *p) {
    if (s_hdmi) return audio_emit_active_prefix(p, false);

    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = SYNC_V1_H1;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH;
    *p++ = SYNC_V1_H0;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_BACK_PORCH;
    *p++ = SYNC_V1_H1;
    // Padding goes BEFORE the TMDS command, never after: the 640 words
    // following CMD_TMDS are pixel data, so a NOP there would be drawn.
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_NOP;
    *p++ = HSTX_CMD_TMDS       | MODE_H_ACTIVE_PIXELS;
    return p;
}

static uint32_t *emit_blank_line_cmds(uint32_t *p, bool vsync) {
    uint32_t s1 = vsync ? SYNC_V0_H1 : SYNC_V1_H1;
    uint32_t s0 = vsync ? SYNC_V0_H0 : SYNC_V1_H0;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;
    *p++ = s1;
    *p++ = HSTX_CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH;
    *p++ = s0;
    *p++ = HSTX_CMD_RAW_REPEAT | (MODE_H_BACK_PORCH + MODE_H_ACTIVE_PIXELS);
    *p++ = s1;
    *p++ = HSTX_CMD_NOP;
    return p;
}

static void build_command_lists(void) {
    // Vertical blanking: front porch, sync, back porch — one array.
    uint32_t *p = s_vblank;
    for (int i = 0; i < MODE_V_FRONT_PORCH; i++) p = emit_blank_line_cmds(p, false);
    for (int i = 0; i < MODE_V_SYNC_WIDTH;  i++) p = emit_blank_line_cmds(p, true);
    for (int i = 0; i < MODE_V_BACK_PORCH;  i++) p = emit_blank_line_cmds(p, false);

    // The shared black active line (commands + 640 black pixels; the
    // pixel words are already zero as static storage).
    (void)emit_active_line_cmds(s_black);

    // Per-row command prefixes inside the framebuffer.
    for (int y = 0; y < VID_ROWS; y++)
        (void)emit_active_line_cmds(s_fb[y]);
}

// CH_DATA's ctrl word. Every block but the last chains back to CH_CTRL
// for the next line; the last chains to CH_RESET, which rewinds the
// table and starts the next frame.
static uint32_t data_ctrl_word(int chain_to) {
    dma_channel_config c = dma_channel_get_default_config(s_ch_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_HSTX);
    channel_config_set_chain_to(&c, chain_to);
    // QUIET, and this is load-bearing. The SDK's default config leaves
    // IRQ_QUIET *clear*, so without this every one of the ~480 blocks in
    // a frame raises DMA_IRQ_0 -- 31.5 kHz instead of the 3.3 kHz the
    // island refill wants. The handler then advances the island index
    // nine times too fast and rewrites slots while scanout is still
    // reading them: garbage islands (the sink never enters HDMI mode and
    // draws the guard bands as a bright column down the left edge),
    // audio drained nine times too fast, and core 0 starved badly enough
    // that the web server stops answering while lwIP still returns
    // pings.
    channel_config_set_irq_quiet(&c, true);
    return channel_config_get_ctrl_value(&c);
}

// Same as data_ctrl_word() but with IRQ_QUIET cleared, so THIS block --
// and only this block -- raises the channel interrupt when it completes.
// Every other descriptor stays quiet, which is what keeps the island IRQ
// at ~3.3 kHz instead of one per line at 31.5 kHz.
static uint32_t island_ctrl_word(void) {
    dma_channel_config c = dma_channel_get_default_config(s_ch_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_HSTX);
    channel_config_set_chain_to(&c, s_ch_ctrl);
    channel_config_set_irq_quiet(&c, false);
    return channel_config_get_ctrl_value(&c);
}

static void build_descriptors(void) {
    const uint32_t fifo = (uint32_t)(uintptr_t)&hstx_fifo_hw->fifo;
    const uint32_t ctrl_next   = data_ctrl_word(s_ch_ctrl);
    const uint32_t island_ctrl = island_ctrl_word();
    int i = 0;

    #define DESC(cnt, addr) do {                            \
        s_desc[i].read_addr  = (uint32_t)(uintptr_t)(addr); \
        s_desc[i].write_addr = fifo;                        \
        s_desc[i].count      = (cnt);                       \
        s_desc[i].ctrl       = ctrl_next;                   \
        i++;                                                \
    } while (0)

    // An active line normally scans as ONE descriptor: its inline command
    // prefix plus its pixels. A line carrying a data island scans as two
    // -- the island slot (which ends with the CMD_TMDS that starts the
    // pixels), then the pixels themselves -- because the island is
    // rewritten between frames and the pixels are not.
    //
    // Islands are spread by Bresenham over the WHOLE frame -- all 525
    // lines, blanking included -- so the gap between them is uniform and
    // the sink never has to coast. Confining them to the 480 active lines
    // puts a 1.44 ms hole in every frame and buzzes at the frame rate.
    uint32_t acc = 0;
    uint32_t isl = 0;

    // True when this line should carry an island. Called once per line,
    // in scan order, for blanking and active lines alike.
    #define WANT_ISLAND() \
        (s_hdmi && (acc += ISLAND_LINES) >= (uint32_t)MODE_V_TOTAL_LINES \
             ? (acc -= MODE_V_TOTAL_LINES, 1) : 0)

    #define EMIT_ISLAND(blank) do {                                     \
        s_desc[i].read_addr  = (uint32_t)(uintptr_t)s_island[isl % AUD_SLOTS]; \
        s_desc[i].write_addr = fifo;                                    \
        s_desc[i].count      = (blank) ? AUD_ISLAND_BLANK_WORDS         \
                                       : AUD_ISLAND_WORDS;              \
        /* IRQ on the island block: the handler refills this slot */    \
        s_desc[i].ctrl       = island_ctrl;                             \
        i++;                                                            \
        s_isl_blank[isl] = (blank);                                     \
        s_isl_desc[isl]  = (uint16_t)(i - 1);                           \
        isl++;                                                          \
    } while (0)

    #define ACTIVE(addr) do {                                           \
        if (WANT_ISLAND()) {                                            \
            EMIT_ISLAND(0);                                             \
            DESC(ROW_PIX_WORDS, &((const uint32_t *)(addr))[ROW_CMD_WORDS]); \
        } else {                                                        \
            DESC(ROW_WORDS, addr);                                      \
        }                                                               \
    } while (0)

    // ---- vertical blanking -------------------------------------------
    // Emitted as runs of plain lines separated by island lines: an island
    // line's whole 800 clocks live inside the island slot, so it replaces
    // that line's entry in s_vblank rather than adding to it.
    //
    // The two VSYNC lines are skipped deliberately. An island carries
    // {V,H} inside its TERC4 nibbles, and the slot content is rebuilt
    // per frame without knowing which line it landed on; keeping them off
    // the sync lines means every island in the frame has V inactive, and
    // the deficit just moves to the next line.
    {
        int run_start = 0;                      // first line of the pending run
        int pend = 0;                           // island owed to a sync line
        for (int ln = 0; ln < VBLANK_LINES; ln++) {
            bool is_sync = (ln >= MODE_V_FRONT_PORCH &&
                            ln <  MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH);
            // The accumulator advances on EVERY line, sync lines included,
            // so the frame always produces exactly ISLAND_LINES islands --
            // the slot mapping (position % AUD_SLOTS) depends on it. An
            // island that lands on a sync line is deferred, not dropped;
            // the 33-line back porch guarantees somewhere to put it.
            if (WANT_ISLAND()) pend = 1;
            if (!pend || is_sync) continue;
            pend = 0;
            if (ln > run_start)
                DESC((ln - run_start) * BLANK_CMD_WORDS,
                     &s_vblank[run_start * BLANK_CMD_WORDS]);
            EMIT_ISLAND(1);
            run_start = ln + 1;
        }
        if (VBLANK_LINES > run_start)
            DESC((VBLANK_LINES - run_start) * BLANK_CMD_WORDS,
                 &s_vblank[run_start * BLANK_CMD_WORDS]);
    }

    for (int j = 0; j < BORDER_LINES; j++)
        ACTIVE(s_black);                        // top border
    for (int y = 0; y < VID_ROWS; y++) {
        ACTIVE(s_fb[y]);                        // each row is scanned
        ACTIVE(s_fb[y]);                        // twice: 2x vertical
    }
    for (int j = 0; j < BORDER_LINES; j++)
        ACTIVE(s_black);                        // bottom border
    #undef ACTIVE
    #undef EMIT_ISLAND
    #undef WANT_ISLAND
    #undef DESC

    s_island_count = isl;
    // DESC_COUNT is a worst-case bound, not the exact figure: how many
    // descriptors the vblank needs depends on where its islands land.
    // Everything that walks the table must stop here.
    s_desc_used = (uint32_t)i;

    // Last block of the frame: hand off to the rewind channel.
    s_desc[i - 1].ctrl = data_ctrl_word(s_ch_reset);
    s_desc_base = (uint32_t)(uintptr_t)s_desc;
}

// Which island position the descriptor at `d` belongs to: the last island
// at or before it. Seven comparisons over 80 entries, inside an interrupt
// that already costs tens of microseconds.
static uint32_t __not_in_flash_func(isl_at_desc)(uint32_t d) {
    uint32_t lo = 0, hi = ISLAND_LINES - 1u, best = 0;
    while (lo <= hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (s_isl_desc[mid] <= d) { best = mid; lo = mid + 1u; }
        else { if (mid == 0u) break; hi = mid - 1u; }
    }
    return best;
}

// Fires when an island block has finished scanning, so that slot is free.
// Refill it for the island AUD_SLOTS ahead -- that lead time is the whole
// reason a frame of islands never has to exist in RAM at once.
static void __not_in_flash_func(hstx_island_irq)(void) {
    dma_hw->ints0 = 1u << s_ch_data;

    // Anchor to where scanout ACTUALLY is, instead of a free-running
    // count. A free-running count desynchronises permanently on a single
    // missed interrupt, and since each position carries its own island
    // SHAPE, being off by one writes a 268-word blanking island into a
    // descriptor that reads 271 -- the line's trailing TMDS command comes
    // out stale and the frame breaks. The control channel's read address
    // names the descriptor it is about to fetch; islands are ~13
    // descriptors apart, so a couple of descriptors of interrupt latency
    // cannot push this into the wrong one.
    uint32_t idx  = s_isl_seen;
    {
        uint32_t cra = dma_hw->ch[s_ch_ctrl].read_addr;
        uint32_t base = s_desc_base;
        if (cra > base) {
            uint32_t d = ((cra - base) / sizeof(ctrl_block_t));
            uint32_t at = isl_at_desc(d ? d - 1u : 0u);
            if (at != idx) { s_isl_fix++; idx = at; }
        }
    }
    s_isl_seen    = (idx + 1u) % ISLAND_LINES;
    uint32_t slot = idx % AUD_SLOTS;

    // If scanout has already come back around to this slot, the handler
    // ran too late and the sink got a stale island. Count it; aud_late
    // must be 0 in steady state.
    uint32_t ra = dma_hw->ch[s_ch_data].read_addr;
    uint32_t lo = (uint32_t)(uintptr_t)s_island[slot];
    if (ra >= lo && ra < lo + sizeof s_island[0]) audio_note_late();

    uint32_t next = (idx + AUD_SLOTS) % ISLAND_LINES;
    audio_fill_island(s_island[slot], next, s_isl_blank[next] != 0);
}

// hdmi_audio is a PARAMETER, not something read from cfg in here: this
// runs early in boot, before cfg is necessarily loaded, and reading the
// config directly silently selects DVI when it is not. Making the caller
// supply it puts the dependency where it can be seen.
void hstx_init(bool hdmi_audio) {
    s_hdmi = hdmi_audio;
    audio_init();

    build_command_lists();
    hstx_fill(0x00);

    // ---- HSTX: RGB332 TMDS encoder ----
    hstx_ctrl_hw->expand_tmds =
        2  << HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB |
        0  << HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB   |
        2  << HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB |
        29 << HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB   |
        1  << HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB |
        26 << HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB;

    // Pixels arrive as 4 8-bit chunks per word; control symbols are a
    // whole 32-bit word.
    hstx_ctrl_hw->expand_shift =
        4 << HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB |
        8 << HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB |
        1 << HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB |
        0 << HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB;

    hstx_ctrl_hw->csr = 0;
    hstx_ctrl_hw->csr =
        HSTX_CTRL_CSR_EXPAND_EN_BITS |
        5u << HSTX_CTRL_CSR_CLKDIV_LSB |
        5u << HSTX_CTRL_CSR_N_SHIFTS_LSB |
        2u << HSTX_CTRL_CSR_SHIFT_LSB |
        HSTX_CTRL_CSR_EN_BITS;

    // ---- output crossbar for THIS board's J4 wiring ----
    // Connector J4 order: negative leg first, clock pair first.
    //   GP12 TXC-  GP13 TXC+
    //   GP14 TX0-  GP15 TX0+
    //   GP16 TX1-  GP17 TX1+
    //   GP18 TX2-  GP19 TX2+
    // (The Pico DVI Sock used by pico-examples is a different order —
    // do not copy its lane_to_output_bit table.)
    hstx_ctrl_hw->bit[0] = HSTX_CTRL_BIT0_CLK_BITS | HSTX_CTRL_BIT0_INV_BITS;
    hstx_ctrl_hw->bit[1] = HSTX_CTRL_BIT0_CLK_BITS;
    for (uint lane = 0; lane < 3; ++lane) {
        uint32_t sel =
            (lane * 10    ) << HSTX_CTRL_BIT0_SEL_P_LSB |
            (lane * 10 + 1) << HSTX_CTRL_BIT0_SEL_N_LSB;
        uint bit = 2 + lane * 2;                 // GP14/16/18 pairs
        hstx_ctrl_hw->bit[bit    ] = sel | HSTX_CTRL_BIT0_INV_BITS;  // -
        hstx_ctrl_hw->bit[bit + 1] = sel;                            // +
    }

    for (int i = MC_PIN_HSTX_BASE; i < MC_PIN_HSTX_BASE + 8; ++i)
        gpio_set_function(i, 0);                 // FUNCSEL 0 = HSTX

    // DMA wins bus arbitration over core 0: the scanout FIFO must never
    // underrun because a core was hammering the same SRAM bank.
    //
    // OR, never assign. bus_init() elevates PROC1 for the bus-serve loop
    // and runs AFTER this, so a plain `=` in either place silently drops
    // the other's bits. Elevating DMA_R/DMA_W and PROC1 together is the
    // intent: those three round-robin among themselves and core 0 (WiFi,
    // lwIP, UI, flash) yields to all of them.
    bus_ctrl_hw->priority |= BUSCTRL_BUS_PRIORITY_DMA_W_BITS |
                             BUSCTRL_BUS_PRIORITY_DMA_R_BITS;

    // ---- DMA: data channel fed forever by a control channel ----
    s_ch_data  = dma_claim_unused_channel(true);
    s_ch_ctrl  = dma_claim_unused_channel(true);
    s_ch_reset = dma_claim_unused_channel(true);

    build_descriptors();          // needs the channel numbers above

    // Prime every slot before scanout starts, so the first frame is never
    // reading uninitialised words out of the FIFO.
    //
    // AFTER build_descriptors, never before: it is what fills s_isl_blank,
    // and a slot primed with the wrong shape writes an active island's 271
    // words into a descriptor sized for a blanking island's 268. The frame
    // then desynchronises from the very first line.
    if (s_hdmi)
        for (uint32_t s = 0; s < AUD_SLOTS; s++)
            audio_fill_island(s_island[s], s, s_isl_blank[s] != 0);

    // CH_DATA is fully reprogrammed by every control block; configure it
    // only enough to exist.
    dma_channel_config c = dma_channel_get_default_config(s_ch_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, DREQ_HSTX);
    channel_config_set_chain_to(&c, s_ch_ctrl);
    dma_channel_configure(s_ch_data, &c, &hstx_fifo_hw->fifo,
                          s_desc, 0, false);

    // CH_RESET: one fixed word (the table base) into CH_CTRL's alias-3
    // read-address trigger, which rewinds AND restarts the control
    // channel. No increments, so no ring is needed.
    c = dma_channel_get_default_config(s_ch_reset);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_chain_to(&c, s_ch_reset);   // == self: no chain
    dma_channel_configure(s_ch_reset, &c,
                          &dma_hw->ch[s_ch_ctrl].al3_read_addr_trig,
                          &s_desc_base, 1, false);

    // CH_CTRL: four words per block into CH_DATA's alias-0 registers.
    // The 16-byte write ring keeps it on those four registers forever.
    c = dma_channel_get_default_config(s_ch_ctrl);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, 4);          // 16-byte write ring
    channel_config_set_chain_to(&c, s_ch_ctrl);    // == self: no chain

    // Arm the island refill interrupt BEFORE scanout starts, so the first
    // island block to complete already has a handler. Only the island
    // descriptors clear IRQ_QUIET, so this fires ~3.3 kHz, not per line.
    if (s_hdmi) {
        dma_channel_set_irq0_enabled(s_ch_data, true);
        irq_set_exclusive_handler(DMA_IRQ_0, hstx_island_irq);
        // LOW priority, deliberately. The CYW43 WiFi driver also uses
        // interrupts on core 0, and a handler that blocks it for tens of
        // microseconds at 3.3 kHz takes the web server down while ICMP
        // still answers from a path that does not need it. The refill
        // has ~1.1 ms of lead time, so being preempted costs nothing;
        // `alate` would say if it ever did.
        irq_set_priority(DMA_IRQ_0, 0xC0);
        irq_set_enabled(DMA_IRQ_0, true);
    }

    // Alias 0 is the base register set (read_addr, write_addr,
    // transfer_count, ctrl_trig) — it has no "al0_" prefix.
    dma_channel_configure(s_ch_ctrl, &c,
                          &dma_hw->ch[s_ch_data].read_addr,
                          s_desc, 4, true);        // start scanning out
}

uint32_t hstx_island_lines(void) { return s_island_count; }
// Times the refill handler found its running island count disagreed with
// where scanout actually was. Must be 0 after the first frame: anything
// else means interrupts are being missed.
uint32_t hstx_island_fixes(void) { return s_isl_fix; }
bool     hstx_hdmi_audio(void)   { return s_hdmi; }

void hstx_test_pattern(void) {
    for (int y = 0; y < VID_ROWS; y++) {
        uint8_t *row = hstx_row(y);
        for (int x = 0; x < VID_W; x++) {
            if (x < VID_X_MARGIN || x >= VID_W - VID_X_MARGIN) {
                row[x] = 0x00;                   // side borders
            } else {
                int ix = (x - VID_X_MARGIN) / 2; // 0..255 VDG pixels
                // Colour bars over the image area, with a white frame so
                // the image extents are unambiguous on a real monitor.
                bool edge = (y < 2 || y >= VID_ROWS - 2 || ix < 2 || ix >= 254);
                row[x] = edge ? 0xFF : (uint8_t)((ix / 32) * 36 + (y / 24));
            }
        }
    }
}
