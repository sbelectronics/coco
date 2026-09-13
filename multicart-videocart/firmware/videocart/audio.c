// ======================================================================
// audio.c — CoCo internal sound over HDMI data islands.
//
// The CoCo 2 has no sound chip: audio is CPU-generated through PIA
// registers, and every one of those writes crosses the cartridge port.
// core 1 stamps them with its bus-cycle lap counter (bus.c, "AUDIO
// EVENTS"); this file turns that stream into 48 kHz PCM and encodes it
// as HDMI audio sample packets.
//
// PORTED FROM WORKING HARDWARE, not from the spec. coco-hdmi drives this
// same display through hdl-util/hdmi; the packet layer here is that RTL
// translated from per-clock logic into pre-computed RAW words. Cited
// per-item below as rtl/vendor/hdmi/<file>:<line>. Where the spec and the
// RTL could differ, the RTL wins -- it is the thing that is known to make
// this TV show "PCM 48 kHz".
//
// EVERYTHING HERE IS RAM-RESIDENT. audio_fill_island() runs from the HSTX
// DMA interrupt, and core 0 erases flash during ROM/firmware uploads with
// interrupts off; XIP is unavailable then, so a flash-resident handler
// would fault rather than merely glitch.
// ======================================================================

#include "audio.h"
#include "video.h"
#include "config.h"
#include "bus.h"

#include "pico/stdlib.h"

#include <string.h>

// ---- TMDS symbols ----------------------------------------------------
// Control codes: tmds_channel.sv:104-109. Already in hstx.c, repeated
// here so this file is self-contained for the host tests.
static const uint16_t CTRL[4] = { 0x354, 0x0AB, 0x154, 0x2AB };

// TERC4, tmds_channel.sv:117-132. Every entry has 4-6 ones (the DC
// balance the encoding exists to provide); test_terc4 asserts that and
// that none collides with a control or guard word.
static const uint16_t TERC4[16] = {
    0x29C, 0x263, 0x2E4, 0x2E2, 0x171, 0x11E, 0x18E, 0x13C,
    0x2CC, 0x139, 0x19C, 0x2C6, 0x28E, 0x271, 0x163, 0x2C3,
};

// Guard bands, tmds_channel.sv:140-153.
#define VGUARD_CH0  0x2CC       // 1011001100
#define VGUARD_CH1  0x133       // 0100110011
#define VGUARD_CH2  0x2CC
// The data guard's ch0 is TERC4 of {1,1,vsync,hsync} -- which is exactly
// TERC4[0xC..0xF], so it needs no separate table.
#define DGUARD_CH12 0x133

#define RAWWORD(c0, c1, c2)  ((uint32_t)(c0) | ((uint32_t)(c1) << 10) \
                                             | ((uint32_t)(c2) << 20))

// hdmi.sv:333 -- ch0 always carries {vsync,hsync}; ch1/ch2 carry the
// preamble type. Video preamble = ch1 01, ch2 00; island preamble = both
// 01. Sync levels are active-LOW here, matching hstx.c's SYNC_V*_H*.
#define VH(v, h)        CTRL[((v) << 1) | (h)]
#define W_CTRL(v, h)    RAWWORD(VH(v, h), CTRL[0], CTRL[0])
#define W_VPRE(v, h)    RAWWORD(VH(v, h), CTRL[1], CTRL[0])
#define W_IPRE(v, h)    RAWWORD(VH(v, h), CTRL[1], CTRL[1])
#define W_VGUARD        RAWWORD(VGUARD_CH0, VGUARD_CH1, VGUARD_CH2)
#define W_DGUARD(v, h)  RAWWORD(TERC4[0xC | ((v) << 1) | (h)], \
                                DGUARD_CH12, DGUARD_CH12)

// HSTX command words (as in hstx.c).
#define CMD_RAW          (0x0u << 12)
#define CMD_RAW_REPEAT   (0x1u << 12)
#define CMD_TMDS         (0x2u << 12)

// ---- ACR --------------------------------------------------------------
// audio_clock_regeneration_packet.sv:19,47. N = 6144 for 48 kHz; CTS =
// pixel_rate * N / (128 * audio_rate). Our pixel clock is exactly
// 25.000 MHz (clk_hstx 125 MHz / 5), so CTS = 25 000 comes out an
// integer -- no fractional-CTS error to chase.
#define ACR_N    6144u
#define ACR_CTS  25000u

// ---- audio format -----------------------------------------------------
#define AUDIO_RATE       48000u
// CoCo E clock = 3.579545 MHz / 4 = 894886.25 Hz. Laps per sample =
// 894886.25 / 48000 = 18.6434635, in 16.16 fixed point.
#define LAPS_PER_SAMPLE_Q16  1221825u

// Drift servo: the CoCo crystal and the RP2350 crystal are independent, so
// the event backlog walks. Snap when it exceeds this, which at ~50 ppm is
// roughly once a minute and moves a held level by <= 4 ms -- a shift, not
// a click. Do NOT servo ACR instead: the sink's PLL is far less tolerant
// of a moving CTS than the ear is of a 4 ms step.
#define DRIFT_SNAP_LAPS  (4u * 895u)      // ~4 ms of bus cycles
#define DRIFT_AIM_LAPS   (1u * 895u)      // snap to ~1 ms behind

// ---- decoded CoCo sound state ----------------------------------------
static uint8_t  s_ff20, s_ff21 = 0x04, s_ff22, s_ff23, s_ff01, s_ff03;
static uint8_t  s_dac;              // the ladder's held value, not $FF20
static uint8_t  s_pb;               // Port B OUTPUT register, not $FF22
static uint8_t  s_onebit = 3;       // single-bit level, 0..3
// Duty cycle of the mux being off the DAC, accumulated in Q8 laps so
// it is a fraction of TIME. Counting calls instead over-reports badly:
// current_pcm() is called once per EVENT inside a sample window, and
// events cluster exactly where the mux is being swept.
static bool     s_live, s_gate_off, s_mux_off;
static int32_t  s_ob_val;           // single-bit contribution, ungated
static uint32_t s_span, s_gate_ns, s_mux_ns;
static uint32_t s_peak, s_clips;    // output level, and clamp hits

// ---- PCM capture ------------------------------------------------------
// The last PCMLOG samples actually sent to the sink, for looking at the
// spectrum instead of reasoning about it. 4096 samples is 85 ms at
// 48 kHz, about 12 Hz of resolution, and it also settles whether a
// setting reaches the synth at all: two filter settings that produce the
// same samples mean the control is not connected to anything.
#define PCMLOG 4096u
static int16_t  s_pcmlog[PCMLOG];
static uint32_t s_pcmlog_wr;
// Capture stops when the buffer is full and stays stopped until the dump
// re-arms it. Otherwise the reader races the interrupt: re-reading the
// live write pointer per sample slides the window forward while it
// prints, and the output carries single-sample skips that look like
// jumps the filter could never produce.
static bool     s_pcm_full;
static uint32_t s_gate_edges;       // transitions of the sound gate

// ---- event log -------------------------------------------------------
// A tee of the last EVLOG events, kept whether or not the synth needed
// them, so that exactly which registers a program wrote, and when, can be
// read back over the seconds a noise happens. Sound that a real CoCo does
// not make cannot be diagnosed by ear.
#define EVLOG 1024u
static uint32_t s_evlog[EVLOG];
// The DERIVED state right after each event, recorded rather than
// replayed. Rebuilding it from power-on defaults while reading the log
// is only right when every register involved was written inside the
// window; a program that set $FF23 once before the window opened and
// then only toggles $FF22 would read back as "gate off" throughout.
//   bit 15..10 dac[5:0]   bit 3 gate   bit 2..1 sel   bit 0 one-bit
static uint16_t s_evstate[EVLOG];
static uint32_t s_evlog_wr;

// A SECOND log, for video events only, because the first cannot hold them.
// A sound routine can write $FF22 thousands of times a second, so 1024
// entries is a fraction of a second of history and the SAM writes that
// move the display base are long gone before anyone can open the page.
// Video events are rare -- tens in a whole session -- so a small ring the
// sound flood cannot reach into covers an entire run.
//
// What qualifies: any SAM write, and a $FF22 write that actually moved the
// video bits. PB1 toggling by itself is sound, not video.
#define VLOG 256u
static uint32_t s_vlog[VLOG];
static uint32_t s_vlog_wr;

static uint32_t s_mux_edges;        // transitions of the select lines
static uint8_t  s_filt;             // analogue-path model: 0 = off, 1..3
static int32_t  s_dc_q12, s_dc2_q12, s_lp, s_lp2;  // filter states
static uint8_t  s_gain = 2;

// ---- synth state ------------------------------------------------------
static uint32_t s_lap;              // consumer's audio-time lap counter
static uint32_t s_lap_frac;         // 16.16 accumulator
static bool     s_primed;
static uint32_t s_ev_count, s_resyncs, s_late, s_samples;
// One-event lookahead for the drain loop. FILE scope, not function-static:
// audio_init() has to be able to clear it, or a re-init inherits an event
// from the previous run and silently blocks the queue until the lap
// counter catches up to a timestamp from a different era.
static uint32_t s_pending;
static bool     s_have_pending;

// ---- packet scheduling ------------------------------------------------
static uint32_t s_frame_ctr;        // IEC 60958 frame counter, 0..191
static uint32_t s_pkt_ctr;          // packets since the last ACR
static uint32_t s_last_island;      // for frame-start detection

// ---- BCH ECC ----------------------------------------------------------
// packet_assembler.sv:33-52. Bit-serial:
//     ecc = (ecc >> 1) ^ (((ecc & 1) ^ bit) ? 0x83 : 0)
// Since ecc is 8 bits, eight steps consume the whole byte and the initial
// value survives only through the feedback, so ecc' = T[ecc ^ byte].
// test_packet_ecc proves the table against the bit-serial reference.
static uint8_t s_ecc[256];

static void ecc_build(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t e = 0, b = (uint8_t)i;
        for (int k = 0; k < 8; k++) {
            uint8_t bit = (uint8_t)((b >> k) & 1u);
            e = (uint8_t)((e >> 1) ^ (((e & 1u) ^ bit) ? 0x83u : 0u));
        }
        s_ecc[i] = e;
    }
}

static inline uint8_t ecc_bytes(const uint8_t *p, int n) {
    uint8_t e = 0;
    for (int i = 0; i < n; i++) e = s_ecc[e ^ p[i]];
    return e;
}

// ---- IEC 60958 channel status ----------------------------------------
// audio_sample_packet.sv:47-52. Consumer, LPCM, copyright not asserted,
// category 0, 48 kHz (0010), 16-bit word length (0010).
static uint32_t chan_status_bit(uint32_t idx, bool right) {
    switch (idx) {
    case 2:  return 1;                       // copyright not asserted
    case 20: return right ? 0 : 1;           // channel number 1 / 2
    case 21: return right ? 1 : 0;
    case 26: return 1;                       // sampling frequency 0010
    case 34: return 1;                       // word length 0010
    default: return 0;
    }
}

// ---- one packet, serialised to 32 TERC4 RAW words --------------------
// The layout is hdmi.sv:335-338 exactly: ch0 = {1, header bit k, V, H};
// ch1 = bit 2k of each of the four 64-bit subpacket+parity blocks; ch2 =
// bit 2k+1 of the same. Header is 24 bits + 8 parity = 32, one bit per
// clock.
//
// `clk0` is the island's first absolute clock in the line, so each word
// can carry the hsync level that belongs to ITS clock: on an island line
// the whole hsync pulse falls inside the island, and H travels only in
// the ch0 nibble. Getting this wrong costs sync, not just audio.
static void emit_packet(uint32_t *dst, const uint8_t hb[3],
                        const uint8_t sb[4][7], uint32_t clk0, bool vlow)
{
    uint8_t hdr[4], sub[4][8];
    hdr[0] = hb[0]; hdr[1] = hb[1]; hdr[2] = hb[2];
    hdr[3] = ecc_bytes(hdr, 3);
    for (int i = 0; i < 4; i++) {
        memcpy(sub[i], sb[i], 7);
        sub[i][7] = ecc_bytes(sub[i], 7);
    }

    // Transpose the four subpackets ONCE into per-clock nibbles, instead
    // of re-extracting four bits inside every clock. Extracting per clock
    // costs ~40 us per island (584 instructions with a 128-iteration
    // body); at 3.3 kHz that is ~13% of core 0 spent inside an
    // interrupt, which is enough to starve the CYW43 driver and take the
    // web server down while ICMP still answers.
    uint8_t n1[32] = { 0 }, n2[32] = { 0 };
    for (int i = 0; i < 4; i++) {
        const uint8_t *s = sub[i];
        for (uint32_t k = 0; k < 32; k++) {
            uint32_t b = (uint32_t)s[k >> 2] >> ((k & 3) * 2);
            n1[k] |= (uint8_t)((b & 1u) << i);
            n2[k] |= (uint8_t)(((b >> 1) & 1u) << i);
        }
    }

    uint32_t v = vlow ? 0u : 1u;
    for (uint32_t k = 0; k < 32; k++) {
        uint32_t clk = clk0 + k;
        // hsync is asserted (low) for clocks 16..111 of the blanking.
        uint32_t h = (clk >= MODE_H_FRONT_PORCH &&
                      clk <  MODE_H_FRONT_PORCH + MODE_H_SYNC_WIDTH) ? 0u : 1u;

        uint32_t hbit = (hdr[k >> 3] >> (k & 7)) & 1u;
        uint32_t n0 = 0x8u | (hbit << 2) | (v << 1) | h;

        uint32_t w = RAWWORD(TERC4[n0], TERC4[n1[k]], TERC4[n2[k]]);
        // RAW_REPEAT of one word, NOT a single long RAW run. Every sync
        // in hstx.c, and every line known to reach the display, is
        // RAW_REPEAT; a 128-word non-repeat RAW run would be the one
        // command shape nothing else exercises, and no host test can
        // settle whether the expander handles it. Spending one extra
        // word per clock buys the islands the same command shape as the
        // syncs.
        *dst++ = CMD_RAW_REPEAT | 1u;
        *dst++ = w;
    }
}

// ---- packet builders --------------------------------------------------
static void build_acr(uint8_t hb[3], uint8_t sb[4][7]) {
    hb[0] = 0x01; hb[1] = 0; hb[2] = 0;
    for (int i = 0; i < 4; i++) {
        sb[i][0] = 0;
        sb[i][1] = (uint8_t)(ACR_CTS >> 12);
        sb[i][2] = (uint8_t)(ACR_CTS >> 4);
        sb[i][3] = (uint8_t)(((ACR_CTS & 0xF) << 4) | (ACR_N >> 16));
        sb[i][4] = (uint8_t)(ACR_N >> 8);
        sb[i][5] = (uint8_t)(ACR_N);
        sb[i][6] = 0;
    }
}

// InfoFrame packets carry their payload in subpacket 0 (and 1 when
// longer). Checksum makes the byte sum, including the header, zero.
static void build_infoframe(uint8_t hb[3], uint8_t sb[4][7],
                            uint8_t type, uint8_t ver, uint8_t len,
                            const uint8_t *body, int nbody)
{
    hb[0] = type; hb[1] = ver; hb[2] = len;
    memset(sb, 0, 4 * 7);
    uint8_t sum = (uint8_t)(hb[0] + hb[1] + hb[2]);
    for (int i = 0; i < nbody; i++) sum = (uint8_t)(sum + body[i]);
    sb[0][0] = (uint8_t)(-(int)sum);                 // checksum, PB0
    for (int i = 0; i < nbody; i++) {
        int idx = i + 1;                             // PB1..
        sb[idx / 7][idx % 7] = body[i];
    }
}

static void build_avi(uint8_t hb[3], uint8_t sb[4][7]) {
    // VIC 1 = 640x480p60, RGB, no repetition.
    uint8_t body[13] = { 0 };
    body[0] = 0x00;         // Y=RGB, no active-format info
    body[1] = 0x08;         // R = same as coded frame
    body[2] = 0x00;
    body[3] = 0x01;         // VIC = 1
    body[4] = 0x00;
    build_infoframe(hb, sb, 0x82, 0x02, 13, body, 13);
}

// Source Product Description. Not required for a sink to enter HDMI
// mode, but packet_picker.sv:141 sends one every field and that is the
// configuration known to make THIS display accept coco-hdmi's stream;
// there is no reason to differ from the reference.
static void build_spd(uint8_t hb[3], uint8_t sb[4][7]) {
    uint8_t body[25] = { 0 };
    memcpy(&body[0],  "CoCoCart", 8);      // vendor, 8 bytes, space-padded
    memcpy(&body[8],  "Videocart HDMI  ", 16);  // product, 16 bytes
    body[24] = 0x09;                       // source device: PC general
    build_infoframe(hb, sb, 0x83, 0x01, 25, body, 25);
}

static void build_audio_if(uint8_t hb[3], uint8_t sb[4][7]) {
    // CT = refer to stream, CC = 2 channels, SF/SS = refer to stream.
    uint8_t body[10] = { 0 };
    body[0] = 0x01;         // CC = 2ch
    body[1] = 0x00;
    body[2] = 0x00;
    body[3] = 0x00;         // CA = FR,FL
    build_infoframe(hb, sb, 0x84, 0x01, 10, body, 10);
}

// ---- the synth --------------------------------------------------------
// Mux select {SEL1,SEL0}: 00 = DAC, 01 = cassette, 10 = cartridge SND,
// 11 = none. Only 00 is reproducible, and the others must be SILENT
// rather than "hold the last DAC value" -- BASIC's JOYIN sweeps the mux
// while doing successive approximation on $FF20, and holding would turn
// every joystick read into a buzz instead of the click a real CoCo makes.
//
// s_dac is the PIA's OUTPUT REGISTER, not the byte last written to the
// address. Two things follow:
//
//  - A write to $FF20 while CRA bit 2 is clear goes to the data DIRECTION
//    register. It does not reach the ladder, so it must not disturb the
//    held DAC value. Latching it injects a spurious level.
//  - Clearing CRA bit 2 does not zero the ladder either. It only changes
//    what the CPU sees at that address; the analogue output holds. Reading
//    the DAC as 0 there produces a full-scale NEGATIVE excursion, since 0
//    is the bottom of the range and 32 is the midpoint -- an audible spike
//    for a register access that is silent on real hardware.
//
// Returns the LADDER, ungated. The gate and the mux are applied by the
// caller, after the DC blocker.
//
// That order is the whole point. Zaxxon mutes the sound gate around its
// joystick read, sweeps the ladder, leaves it at 0 and unmutes -- then
// sits still for 66 ms. Blocking DC on the GATED output turns that into
// a 15 Hz square wave at half full scale, because gate-off reads as 0
// while gate-on reads as (0-32)*512. A real CoCo is silent through all
// of it: a motionless ladder is DC, and DC never reaches the speaker
// whatever the gate does. Blocking DC on the SOURCE reproduces that: a
// static level is already zero before the gate sees it, so switching
// the gate around it produces nothing at all.
static inline int32_t current_pcm(void) {
    uint32_t dac  = (uint32_t)(s_dac >> 2);
    bool     gate = (s_ff23 & 0x38u) == 0x38u;
    uint32_t sel  = (uint32_t)(((s_ff03 >> 3) & 1u) << 1 | ((s_ff01 >> 3) & 1u));
    bool     live = gate && sel == 0u;
    int32_t  pcm  = ((int32_t)dac - 32) * 512;

    // The single-bit output is NOT gated by the sound enable, and must be
    // carried separately rather than summed in here. On the CoCo the
    // enable switch sits in the DAC/mux path; PB1 is summed into the audio
    // node past it, which is why a diagnostic can drive PB1 with the gate
    // off and still be heard (the diagnostic cart's 1.3 s single-bit
    // sweep runs with the gate off throughout). It also needs its own DC
    // blocker: a parked PB1 is a DC level like a parked ladder, and only
    // its TRANSITIONS are sound.
    //
    // OB_LVL: 0 silences the single-bit output, 3 is a full ladder swing.
    // Real hardware mixes PB1 in through a resistor, so full swing is
    // almost certainly too loud when a program drives both at once.
    static const int32_t OB_LVL[4] = { 0, 2048, 4096, 8192 };
    s_ob_val = ((s_pb >> 1) & 1u)
                 ? (OB_LVL[s_onebit & 3u] >> (3 - s_gain)) : 0;
    // Split, because these are different faults with different fixes and
    // one number cannot tell them apart. The gate is the program
    // deliberately turning sound off between notes -- entirely normal.
    // The mux moving is the analogue selector being swept for a joystick
    // read, which chops the audio.
    s_live     = live;      // the caller weighs these by duration
    s_gate_off = !gate;
    s_mux_off  = gate && sel != 0u;

    return pcm >> (3 - s_gain);
}

// ---- diagnostic tone --------------------------------------------------
// Bypasses the CoCo entirely: a fixed-point phase accumulator stepped one
// sample at a time, so what comes out of the HDMI is a function of the
// SAMPLE STREAM ALONE. This is a bisection tool, not a feature. If the
// tone is clean the packet layer, the ACR rate, the island timing and the
// sink's own handling are all proven good, and any remaining noise is in
// the bus capture or the synth. If the tone is dirty, none of them are,
// and there is no point looking at the CoCo side at all.
//
// One cycle is 2^24 of phase, so the step is 2^24 / (48000 / f). 1 kHz is
// 48 samples a cycle, 500 Hz is 96. The lower tone is the more revealing
// probe: artefacts that hide under a 1 kHz fundamental stand out against
// a slower one. The top 6 bits of phase index the 64 steps of the
// quarter-wave table directly.
#define TONE_PH_BITS   24
#define TONE_STEP_1K   ((uint32_t)((1u << TONE_PH_BITS) / 48u))
#define TONE_STEP_500  ((uint32_t)((1u << TONE_PH_BITS) / 96u))
static uint8_t  s_test;             // 0 = off, 1 = 1 kHz, 2 = 500 Hz
static uint32_t s_nulls;            // island slots with nothing due

static uint32_t s_tone_ph;

// Quarter-wave sine, 16 entries, Q15. Small enough to stay in the icache
// next to the island filler.
static const int16_t TONE_Q[17] = {
        0,  3212,  6393,  9512, 12539, 15446, 18204, 20787,
    23170, 25330, 27245, 28898, 30273, 31357, 32138, 32610, 32767
};

static int32_t tone_sample(void) {
    uint32_t ph = s_tone_ph >> (TONE_PH_BITS - 6);   // 0..63 over a cycle
    s_tone_ph = (s_tone_ph + (s_test == 2u ? TONE_STEP_500 : TONE_STEP_1K))
                & ((1u << TONE_PH_BITS) - 1u);
    uint32_t q = (ph >> 4) & 3u, i = ph & 15u;
    int32_t v;
    switch (q) {
    case 0:  v =  TONE_Q[i];        break;
    case 1:  v =  TONE_Q[16 - i];   break;
    case 2:  v = -TONE_Q[i];        break;
    default: v = -TONE_Q[16 - i];   break;
    }
    return v >> 2;                          // -8191..8191, well clear of clip
}

void audio_set_test(uint8_t m) {
    s_test = (m > 2u) ? 0u : m;
    s_tone_ph = 0;
}
uint8_t audio_test(void) { return s_test; }

static void apply_event(uint32_t ev) {
    s_evlog[s_evlog_wr & (EVLOG - 1u)] = ev;

    {   // Video log: SAM writes always, $FF22 only when the mode moved.
        uint32_t r = (ev >> 8) & 0xFu;
        bool vid = (r == 6u);
        // $FF23 too: its bit 2 is what decides whether a $FF22 write is
        // a mode change at all, so a log without it cannot be read.
        if (r == 3u) vid = true;
        if (r == 2u && (s_ff23 & 0x04u) &&
            (((uint8_t)(ev & 0xFFu) ^ s_pb) & 0xF8u)) vid = true;
        if (vid) { s_vlog[s_vlog_wr & (VLOG - 1u)] = ev; s_vlog_wr++; }
    }


    uint8_t d = (uint8_t)(ev & 0xFFu);
    switch ((ev >> 8) & 0xFu) {
    case 0:
        // Only when CRA bit 2 selects the output register; otherwise this
        // write is going to the data direction register and the ladder
        // keeps whatever it was holding.
        s_ff20 = d;
        if (s_ff21 & 0x04u) s_dac = d;
        break;

    case 1: s_ff21 = d; break;
    case 2:
        // Same rule as the DAC and CRA bit 2: with CRB bit 2 clear this
        // write is going to the Port B DIRECTION register and never
        // reaches the pin, so the single-bit output must not follow it.
        s_ff22 = d;
        if (s_ff23 & 0x04u) s_pb = d;
        break;

    case 3:
        // The sound gate is CB2: bits 5:4 = 11 makes it an output and
        // bit 3 is its level, so 0x38 is "sound enabled".
        if (((s_ff23 & 0x38u) == 0x38u) != ((d & 0x38u) == 0x38u))
            s_gate_edges++;
        s_ff23 = d; break;

    // The select lines. Counting the TRANSITIONS separates "a game polls
    // the joystick once a frame" from "a game polls it continuously" --
    // the two produce quite different artefacts and want different fixes.
    case 4:
        if (((s_ff01 ^ d) >> 3) & 1u) s_mux_edges++;
        s_ff01 = d; break;
    case 5:
        if (((s_ff03 ^ d) >> 3) & 1u) s_mux_edges++;
        s_ff03 = d; break;
    default: break;
    }
    s_ev_count++;

    // Snapshot AFTER the write has landed, so the line reads as "this is
    // what the machine looked like once that write took effect".
    {
        uint32_t sel = (uint32_t)(((s_ff03 >> 3) & 1u) << 1 |
                                  ((s_ff01 >> 3) & 1u));
        uint16_t st = (uint16_t)(((uint32_t)(s_dac >> 2) << 10) |
                                 ((((s_ff23 & 0x38u) == 0x38u) ? 1u : 0u) << 3) |
                                 (sel << 1) |
                                 ((s_ff22 >> 1) & 1u));
        s_evstate[s_evlog_wr & (EVLOG - 1u)] = st;
    }
    s_evlog_wr++;
}

// Produce one sample covering the window [cursor, cursor + one sample).
//
// The sample is the AVERAGE of the DAC across that window, not a point
// reading at its start. That distinction is the difference between clean
// and scratchy, and it is why a synthetic tone always sounds better than
// the CoCo: the tone is generated in the sample domain and has no edges
// between samples, while a CoCo square wave has edges wherever the
// program put them.
//
// Point-sampling snaps every edge to the nearest 48 kHz boundary, so an
// edge can move by up to 20.8 us. Events arrive stamped to the 1.117 us
// bus cycle, so that resolution is thrown away -- and on a square wave a
// moving edge is period jitter: about 1% on a 1 kHz note, 4% at 4 kHz.
// It is heard as roughness on exactly the material the CoCo produces.
//
// Integrating instead puts a partially-covered sample at a proportional
// value, which recovers the edge position to a fraction of a sample.
// Arithmetic stays 32-bit: offsets are Q8 laps, and the widest possible
// term is 24064 * 4773 = 1.15e8.
#define LAPS_PER_SAMPLE_Q8  ((int32_t)(LAPS_PER_SAMPLE_Q16 >> 8))

static int16_t next_sample(void) {
    int32_t pos = 0;                       // Q8 laps into this window
    int32_t acc = 0;                       // sum of ladder * duration
    int32_t live_acc = 0;                  // ...of which the gate let out
    int32_t ob_acc = 0;                    // the single-bit output, ungated
    const int32_t W = LAPS_PER_SAMPLE_Q8;

    for (;;) {
        if (!s_have_pending) {
            uint32_t ev;
            if (!bus_audio_take(&ev)) break;
            s_pending = ev; s_have_pending = true;
        }
        // 20-bit lap, unwrapped against our own counter.
        uint32_t elap = (s_pending >> 12) & 0xFFFFFu;
        int32_t  d    = (int32_t)((elap - s_lap) & 0xFFFFFu);
        if (d >= 0x80000) d -= 0x100000;          // signed 20-bit distance

        // Where the event falls inside this window, in Q8 laps. s_lap_frac
        // is how far into the current lap the window actually started.
        // d * 256, not d << 8: d is signed and often negative (an event
        // that was already due), and shifting a negative value is UB.
        int32_t off = d * 256 - (int32_t)(s_lap_frac >> 8);
        if (off >= W) break;                      // belongs to a later one
        if (off < pos) off = pos;                 // already due: zero width

        {
            int32_t v = current_pcm(), w = off - pos;
            acc += v * w;
            ob_acc += s_ob_val * w;
            if (s_live) live_acc += w;   // how much of the window was audible
            s_span += (uint32_t)w;
            if (s_gate_off) s_gate_ns += (uint32_t)w;
            if (s_mux_off)  s_mux_ns  += (uint32_t)w;
        }
        pos  = off;
        apply_event(s_pending);
        s_have_pending = false;
    }
    {
        int32_t v = current_pcm(), w = W - pos;
        acc += v * w;                              // the rest of the window
        ob_acc += s_ob_val * w;
        if (s_live) live_acc += w;
        s_span += (uint32_t)w;
        if (s_gate_off) s_gate_ns += (uint32_t)w;
        if (s_mux_off)  s_mux_ns  += (uint32_t)w;
    }


    // These accumulate ~4772 per sample, so a minute of audio overflows a
    // uint32 and the duty cycles come out as nonsense. Halving all three
    // together keeps the ratios exact and turns them into a rolling
    // estimate rather than a total, which is what a duty cycle wants to
    // be anyway.
    if (s_span > (1u << 30)) {
        s_span    >>= 1;
        s_gate_ns >>= 1;
        s_mux_ns  >>= 1;
    }

    s_lap_frac += LAPS_PER_SAMPLE_Q16;
    s_lap      += s_lap_frac >> 16;
    s_lap_frac &= 0xFFFFu;
    s_samples++;

    // The tone is generated in the sample domain, so there is nothing to
    // integrate. Events were still drained above, which keeps the capture
    // side exercised and stops the ring backing up while it runs.
    int32_t pcm = s_test ? tone_sample() : (acc / W);

    // ---- the analogue output stage -------------------------------------
    // What leaves the CoCo is not the ladder voltage. It goes through a
    // coupling capacitor and an amplifier of finite bandwidth. Emitting
    // the raw ladder -- DC and all, with edges as fast as the arithmetic
    // allows -- is audibly wrong in two ways: the ladder's resting level
    // is a DC offset that has no business in the stream, and a joystick
    // poll's successive-approximation sweep of the ladder, which is a
    // soft click on real hardware, becomes a burst of wideband noise
    // sixty times a second.
    //
    // The DC blocker is not optional: real hardware cannot pass DC. The
    // low-pass corner is a judgement call about the analogue path, so it
    // is selectable rather than guessed at -- 1 is gentle, 3 is heavy.
    if (!s_test) {
        // ORDER MATTERS: block DC on the ladder, THEN apply the gate.
        // The other way round, a program that mutes, parks the ladder at
        // one end and unmutes -- which is exactly what Zaxxon does around
        // every joystick read -- gets a half-scale step at 15 Hz out of a
        // machine that is sitting still. Blocked here, a motionless
        // ladder is already zero and the gate has nothing to chop.
        //
        // Corner ~7.5 Hz. At ~15 Hz every plateau of a 0.69 ms PCM step
        // (Clowns & Balloons) droops 6.1% -- a sagging baseline on every
        // note that a real coupling capacitor does not produce; halving
        // the corner halves the droop. It cannot go much lower: Zaxxon
        // parks the ladder and toggles the gate 66 ms later, and whatever
        // has not decayed by then gets chopped -- 4.5% remains at this
        // corner, but 21% at 3.7 Hz and 46% at 1.9 Hz, which brings the
        // tick back.
        s_dc_q12 += ((pcm * 4096) - s_dc_q12) >> 10;

        // Round to nearest, not toward zero: truncating here leaves the
        // estimate one LSB short and the blocker settles at 1 instead of
        // 0, putting a permanent unit offset on everything.
        pcm -= (s_dc_q12 + 2048) >> 12;

        // Now the gate and the mux, weighted by how much of this sample
        // they actually let through, so a mid-window transition lands
        // proportionally instead of snapping to a boundary.
        pcm = (int32_t)((pcm * live_acc) / W);

        // The single-bit output joins AFTER the gate, because the enable
        // switch is not in front of it, and carries its own blocker so a
        // parked PB1 is silent the same way a parked ladder is.
        {
            int32_t ob = ob_acc / W;
            s_dc2_q12 += ((ob * 4096) - s_dc2_q12) >> 9;
            pcm += ob - ((s_dc2_q12 + 2048) >> 12);
        }
        if (s_filt) {
            // TWO cascaded poles, 12 dB/octave. A single pole rolls off
            // at only 6 dB/octave, so no corner can separate the buzz
            // from the notes: by the time the harmonics are down the
            // fundamentals are too. Two poles let the corner sit up
            // where the music actually is and still put the harmonics
            // down. Coefficients are Q14 of 1 - exp(-2*pi*fc/fs) at
            // 48 kHz: 6 kHz, 4 kHz, 2.5 kHz. Worst-case term is
            // 65534 * 8915, comfortably inside int32.
            static const int32_t A_Q14[4] = { 0, 8915, 6678, 4571 };
            int32_t a = A_Q14[s_filt & 3u];
            s_lp  += ((pcm   - s_lp)  * a) >> 14;
            s_lp2 += ((s_lp  - s_lp2) * a) >> 14;
            pcm = s_lp2;
        }
    }
    // Measure, do not assert. If the clamp fires, the arithmetic was
    // wrong. If it never fires and the peak sits well below full scale,
    // then what is being heard as "clipping" is the CoCo's own square
    // waves or the balance between the ladder and the single-bit output
    // -- a different problem needing a different fix.
    {
        int32_t mag = (pcm < 0) ? -pcm : pcm;
        if (mag > (int32_t)s_peak) s_peak = (uint32_t)mag;
    }
    if (pcm >  32767) { pcm =  32767; s_clips++; }
    if (pcm < -32768) { pcm = -32768; s_clips++; }
    if (!s_pcm_full) {
        s_pcmlog[s_pcmlog_wr] = (int16_t)pcm;
        if (++s_pcmlog_wr >= PCMLOG) s_pcm_full = true;
    }
    return (int16_t)pcm;

}

static void servo(void) {
    uint32_t prod = bus_audio_lap();
    if (!s_primed) { s_lap = prod; s_primed = true; return; }
    int32_t back = (int32_t)(prod - s_lap);
    if (back > (int32_t)DRIFT_SNAP_LAPS || back < -(int32_t)DRIFT_SNAP_LAPS) {
        s_lap = prod - DRIFT_AIM_LAPS;
        s_resyncs++;
    }
}

// ---- island assembly --------------------------------------------------
// `blanking` says whether the line this island sits on is followed by
// active pixels. On an active line the island hands off to the video
// preamble, guard band and TMDS run; on a vertical-blanking line nothing
// active follows, so the 640 clocks after it are control periods and
// there is no preamble or guard band. Either way the line is exactly 800
// clocks.
void __not_in_flash_func(audio_fill_island)(uint32_t *dst, uint32_t island_idx,
                                            bool blanking)
{
    // Frame boundary: resynchronise the drift servo. The InfoFrames key
    // off island_idx directly (0, 1 and 2), which is once per frame by
    // construction and comfortably inside "at least once per two fields".
    bool frame_start = (island_idx < s_last_island);
    s_last_island = island_idx;
    if (frame_start) servo();

    uint32_t *p = dst;
    bool vlow = false;                 // islands never sit on vsync lines

    *p++ = CMD_RAW_REPEAT | 4u;   *p++ = W_CTRL(1, 1);
    *p++ = CMD_RAW_REPEAT | 8u;   *p++ = W_IPRE(1, 1);
    *p++ = CMD_RAW_REPEAT | 2u;   *p++ = W_DGUARD(1, 1);

    for (uint32_t k = 0; k < AUD_PACKETS; k++) {
        uint8_t hb[3], sb[4][7];
        uint32_t clk0 = 14u + k * 32u;

        // One InfoFrame per island, on three CONSECUTIVE islands -- not
        // three in the first island of the frame. Stacking them steals
        // three of that island's four slots, so it carries at most 4
        // samples where ~13 are due: a ~9-sample hole once per frame that
        // takes seven islands to make back, and crackles at the frame
        // rate. Spread out, each of the three islands gives up ONE slot
        // and still carries 12.
        if (island_idx == 0u && k == 0u)       build_avi(hb, sb);
        else if (island_idx == 1u && k == 0u)  build_audio_if(hb, sb);
        else if (island_idx == 2u && k == 0u)  build_spd(hb, sb);
        else if (s_pkt_ctr >= 12u)     { build_acr(hb, sb); s_pkt_ctr = 0; }
        else {
            // Audio sample packet. sample_present lets a packet carry
            // 0..4 samples, so the packet rate does not have to divide
            // the sample rate: emit exactly what audio time says is DUE
            // and let the header report how many arrived.
            //
            // Filling all four slots every time is NOT a harmless
            // simplification. A frame has ISLAND_LINES x AUD_PACKETS
            // packet slots carrying up to four samples each, and there is
            // deliberately far more room there than the 806.4 samples a
            // 59.524 Hz frame owes at 48 kHz -- the headroom is what
            // absorbs an island giving a slot up to an InfoFrame. Filling
            // every slot regardless would emit samples the sink never
            // asked for: the ACR (N=6144, CTS=25000) tells it to
            // regenerate exactly 48.000 kHz, so its FIFO overruns, while
            // on this side s_lap outruns the event stream until the servo
            // snaps it back -- a discontinuity several times a second.
            s_pkt_ctr++;
            memset(sb, 0, sizeof sb);
            uint32_t present = 0, bflag = 0;
            uint32_t prod = bus_audio_lap();
            uint32_t n = 0;
            for (int i = 0; i < 4; i++) {
                // Audio time targets DRIFT_AIM_LAPS behind the producer --
                // the same point the servo snaps to -- so a sample is due
                // only while the synth is still behind it. This is what
                // makes the long-run rate exactly one sample per
                // LAPS_PER_SAMPLE bus cycles: 48 kHz in CoCo time.
                if ((int32_t)(prod - DRIFT_AIM_LAPS - s_lap) <= 0) break;
                int16_t s = next_sample();
                uint32_t l = ((uint32_t)(uint16_t)s) << 8;   // left-align 24b
                uint32_t r = l;
                sb[i][0] = (uint8_t)(l);
                sb[i][1] = (uint8_t)(l >> 8);
                sb[i][2] = (uint8_t)(l >> 16);
                sb[i][3] = (uint8_t)(r);
                sb[i][4] = (uint8_t)(r >> 8);
                sb[i][5] = (uint8_t)(r >> 16);

                uint32_t fc = (s_frame_ctr + (uint32_t)i) % 192u;
                uint32_t cl = chan_status_bit(fc, false);
                uint32_t cr = chan_status_bit(fc, true);
                uint32_t pl = __builtin_parity(l) ^ cl;
                uint32_t pr = __builtin_parity(r) ^ cr;
                sb[i][6] = (uint8_t)((pr << 7) | (cr << 6) |
                                     (pl << 3) | (cl << 2));
                present |= 1u << i;
                if (fc == 0) bflag |= 1u << i;
                n++;
            }
            if (n == 0) {
                // Nothing due yet. A Null packet is the defined filler
                // for an island slot with nothing to say; the island is a
                // fixed 271 words, so a slot cannot simply be skipped.
                hb[0] = 0; hb[1] = 0; hb[2] = 0;   // sb is already zeroed
                s_nulls++;
            } else {
                hb[0] = 0x02;
                hb[1] = (uint8_t)present;          // layout 0, present[3:0]
                hb[2] = (uint8_t)(bflag << 4);
            }
            s_frame_ctr = (s_frame_ctr + n) % 192u;
        }
        emit_packet(p, hb, sb, clk0, vlow);
        p += 64;                  // 32 clocks, two words each
    }

    *p++ = CMD_RAW_REPEAT | 2u;   *p++ = W_DGUARD(1, 1);
    if (blanking) {
        // 6 + 8 + 2 clocks of what would have been preamble and guard,
        // then the 640 that would have been pixels: all control.
        *p++ = CMD_RAW_REPEAT | 16u;                   *p++ = W_CTRL(1, 1);
        *p++ = CMD_RAW_REPEAT | MODE_H_ACTIVE_PIXELS;  *p++ = W_CTRL(1, 1);
    } else {
        *p++ = CMD_RAW_REPEAT | 6u;   *p++ = W_CTRL(1, 1);
        *p++ = CMD_RAW_REPEAT | 8u;   *p++ = W_VPRE(1, 1);
        *p++ = CMD_RAW_REPEAT | 2u;   *p++ = W_VGUARD;
        *p++ = CMD_TMDS | MODE_H_ACTIVE_PIXELS;
    }
}

// Non-island active line: the three sync runs, with the back porch
// shortened by the 10 clocks the video preamble and guard band need.
uint32_t *audio_emit_active_prefix(uint32_t *p, bool vsync_low) {
    uint32_t v = vsync_low ? 0u : 1u;
    *p++ = CMD_RAW_REPEAT | MODE_H_FRONT_PORCH;  *p++ = W_CTRL(v, 1);
    *p++ = CMD_RAW_REPEAT | MODE_H_SYNC_WIDTH;   *p++ = W_CTRL(v, 0);
    *p++ = CMD_RAW_REPEAT | (MODE_H_BACK_PORCH - 10u);
    *p++ = W_CTRL(v, 1);
    *p++ = CMD_RAW_REPEAT | 8u;                  *p++ = W_VPRE(v, 1);
    *p++ = CMD_RAW_REPEAT | 2u;                  *p++ = W_VGUARD;
    *p++ = CMD_TMDS | MODE_H_ACTIVE_PIXELS;
    return p;
}

void audio_init(void) {
    ecc_build();
    s_lap = 0; s_lap_frac = 0; s_primed = false;
    s_pending = 0; s_have_pending = false;
    s_frame_ctr = 0; s_pkt_ctr = 0; s_last_island = 0;
    s_ev_count = s_resyncs = s_late = s_samples = 0;
    s_nulls = 0; s_tone_ph = 0;
    // The captured PIA shadows too: audio_init means "start clean", and
    // leaving a stale mux selection behind makes the synth silent for
    // reasons nothing in its own state explains.
    s_ff20 = s_ff21 = s_ff22 = s_ff23 = s_ff01 = s_ff03 = 0;
    s_pb = 0;
    s_dac = 0; s_span = s_gate_ns = s_mux_ns = 0;
    s_peak = 0; s_clips = 0;
    s_mux_edges = s_gate_edges = 0;
    s_evlog_wr = 0; s_vlog_wr = 0;
    s_dc_q12 = 0; s_dc2_q12 = 0; s_lp = 0; s_lp2 = 0; s_ob_val = 0;
}

void    audio_set_gain(uint8_t g) { s_gain = (g > 3) ? 3 : g; }
void    audio_set_onebit(uint8_t n) { s_onebit = (n > 3) ? 3 : n; }
uint8_t audio_onebit(void) { return s_onebit; }
uint8_t audio_gain(void)          { return s_gain; }

uint32_t audio_events(void)  { return s_ev_count; }
uint32_t audio_resyncs(void) { return s_resyncs; }
uint32_t audio_late(void)    { return s_late; }
uint32_t audio_samples(void) { return s_samples; }
uint32_t audio_nulls(void)   { return s_nulls; }
// Permille of sampled instants where the mux or the sound gate had the
// DAC disconnected. Steady tens of permille means something is polling.
uint32_t audio_gate_off(void) {
    return s_span ? (uint32_t)(((uint64_t)s_gate_ns * 1000u) / s_span) : 0u;
}
uint32_t audio_mux_off(void) {
    return s_span ? (uint32_t)(((uint64_t)s_mux_ns * 1000u) / s_span) : 0u;
}
uint32_t audio_mux_edges(void)  { return s_mux_edges; }
uint32_t audio_gate_edges(void) { return s_gate_edges; }
uint32_t audio_peak(void)  { return s_peak; }
uint32_t audio_clips(void) { return s_clips; }

uint32_t audio_pcm_count(void) { return s_pcmlog_wr; }
int16_t audio_pcm_at(uint32_t i) {
    return (i < s_pcmlog_wr) ? s_pcmlog[i] : 0;
}
// Start a fresh capture. Called AFTER a dump has been read, so the
// samples that were printed are a single coherent window rather than a
// sliding one.
void audio_pcm_rearm(void) { s_pcmlog_wr = 0; s_pcm_full = false; }
bool audio_pcm_ready(void) { return s_pcm_full; }

// Oldest-first walk of the event log. Returns how many are available;
// index 0 is the oldest still held.
uint32_t audio_log_count(void) {
    return (s_evlog_wr < EVLOG) ? s_evlog_wr : EVLOG;
}
uint32_t audio_log_at(uint32_t i) {
    uint32_t n = audio_log_count();
    if (i >= n) return 0u;
    return s_evlog[(s_evlog_wr - n + i) & (EVLOG - 1u)];
}
uint32_t audio_vlog_count(void) {
    return (s_vlog_wr < VLOG) ? s_vlog_wr : VLOG;
}
uint32_t audio_vlog_at(uint32_t i) {
    uint32_t n = audio_vlog_count();
    if (i >= n) return 0u;
    return s_vlog[(s_vlog_wr - n + i) & (VLOG - 1u)];
}
uint32_t audio_log_state(uint32_t i) {

    uint32_t n = audio_log_count();
    if (i >= n) return 0u;
    return s_evstate[(s_evlog_wr - n + i) & (EVLOG - 1u)];
}
void     audio_set_filter(uint8_t n) { s_filt = (n > 3u) ? 0u : n; }
uint8_t  audio_filter(void) { return s_filt; }

void audio_note_late(void) { s_late++; }

uint16_t audio_state(void) {
    uint32_t dac  = (s_ff21 & 0x04u) ? (uint32_t)(s_ff20 >> 2) : 0u;
    uint32_t gate = ((s_ff23 & 0x38u) == 0x38u) ? 1u : 0u;
    uint32_t sel  = (uint32_t)(((s_ff03 >> 3) & 1u) << 1 | ((s_ff01 >> 3) & 1u));
    return (uint16_t)((dac << 8) | (gate << 3) | (sel << 1) |
                      ((s_ff22 >> 1) & 1u));
}
