// ======================================================================
// test_audio.c — the HDMI audio path, proved on the host.
//
// The things that can silently destroy a picture or a sink lock are all
// arithmetic, so they are all checkable here:
//   - the BCH ECC byte table equals the bit-serial generator the vendored
//     RTL uses (packet_assembler.sv:33-52);
//   - the TERC4 table matches tmds_channel.sv:117-132, is DC-balanced,
//     and collides with no control code;
//   - an island line is EXACTLY 160 blanking clocks + 640 pixels, with
//     the packets where hdl-util puts them (hdmi.sv:263-300);
//   - hsync is right on an island line, where it exists ONLY inside the
//     ch0 TERC4 nibbles -- get this wrong and the display loses sync
//     rather than merely losing audio;
//   - a non-island HDMI line is still 160 + 640 after the preamble and
//     guard band eat 10 clocks of back porch;
//   - the synth reconstructs a known square wave at the right frequency,
//     honours the sound gate, and holds silence when the mux is not on
//     the DAC.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
} while (0)

// ---- stubs for the core-1 audio ring ---------------------------------
#define EVQ 4096
static uint32_t evq[EVQ];
static uint32_t ev_wr, ev_rd, fake_lap;

uint32_t bus_audio_take(uint32_t *ev) {
    if (ev_rd == ev_wr) return 0;
    *ev = evq[ev_rd++ % EVQ];
    return 1;
}
uint32_t bus_audio_depth(void)     { return ev_wr - ev_rd; }
uint32_t bus_audio_ring_size(void) { return EVQ; }
uint32_t bus_audio_lap(void)       { return fake_lap; }
void     bus_audio_enable(bool on) { (void)on; }

static void push(uint32_t lap, uint32_t reg, uint32_t data) {
    evq[ev_wr++ % EVQ] = ((lap & 0xFFFFFu) << 12) | (reg << 8) | data;
    if (lap > fake_lap) fake_lap = lap;
}

#include "audio.c"

// ---- HSTX command-stream walker --------------------------------------
// Returns clocks consumed; *tmds gets the pixel count of the trailing
// CMD_TMDS. Calls back per clock with the raw word so the caller can
// decode sync.
typedef void (*clk_fn)(uint32_t clk, uint32_t word, void *ud);

static uint32_t walk(const uint32_t *p, uint32_t nwords,
                     uint32_t *tmds, clk_fn cb, void *ud) {
    uint32_t clk = 0, i = 0;
    *tmds = 0;
    while (i < nwords) {
        uint32_t cmd = p[i] & 0xF000u, n = p[i] & 0x0FFFu;
        i++;
        if (cmd == CMD_RAW_REPEAT) {
            for (uint32_t k = 0; k < n; k++, clk++) if (cb) cb(clk, p[i], ud);
            i++;
        } else if (cmd == CMD_RAW) {
            for (uint32_t k = 0; k < n; k++, clk++) if (cb) cb(clk, p[i + k], ud);
            i += n;
        } else if (cmd == CMD_TMDS) {
            *tmds = n;
            break;                       // pixels live in a separate block
        } else if (cmd == 0xF000u) {
            /* NOP: no clocks */
        } else {
            printf("FAIL: unknown HSTX command 0x%08x\n", p[i - 1]);
            failures++;
            break;
        }
    }
    return clk;
}

// Decode hsync from a word's ch0 symbol. Control codes carry {v,h}
// directly; inside an island, ch0 is TERC4 of {1,hdr,v,h}.
static int hsync_of(uint32_t w, bool island) {
    uint32_t c0 = w & 0x3FFu;
    if (!island) {
        for (int i = 0; i < 4; i++) if (CTRL[i] == c0) return i & 1;
        return -1;
    }
    for (int i = 0; i < 16; i++) if (TERC4[i] == c0) return i & 1;
    return -1;
}

// TERC4 decoding applies ONLY inside the packet region. The video guard
// band's ch0 symbol is 0x2CC, which is also TERC4[8] -- decoding it as a
// nibble would read its 2 clocks as hsync-low and inflate the count. Both
// guards and every control period decode as control codes or not at all.
struct hs { int first_low, last_low, lows; uint32_t t4lo, t4hi; };
static void hs_cb(uint32_t clk, uint32_t w, void *ud) {
    struct hs *h = (struct hs *)ud;
    int s = hsync_of(w, clk >= h->t4lo && clk < h->t4hi);
    if (s == 0) {
        if (h->first_low < 0) h->first_low = (int)clk;
        h->last_low = (int)clk;
        h->lows++;
    }
}

// ---- bit-serial ECC reference (packet_assembler.sv:33-52) ------------
static uint8_t ecc_ref(const uint8_t *p, int nbits) {
    uint8_t e = 0;
    for (int k = 0; k < nbits; k++) {
        uint8_t bit = (uint8_t)((p[k >> 3] >> (k & 7)) & 1u);
        e = (uint8_t)((e >> 1) ^ (((e & 1u) ^ bit) ? 0x83u : 0u));
    }
    return e;
}

// Pull the four packets back out of a filled island: reverse the TERC4,
// lift the header bits out of ch0 and the subpacket bit-pairs out of
// ch1/ch2. Checking what the WIRE carries, rather than what the builder
// intended, is the only way this can catch a transposition bug.
static int terc4_rev[1024];

static void decode_island(const uint32_t *isl, uint8_t hb[][4],
                          uint8_t sb[][4][8])
{
    memset(hb, 0, AUD_PACKETS * 4);
    memset(sb, 0, AUD_PACKETS * 4 * 8);
    for (uint32_t k = 0; k < AUD_PACKETS; k++) {
        // 3 lead-in pairs, then 64 words per packet: [cmd, data] per clock.
        for (uint32_t c = 0; c < 32; c++) {
            uint32_t w = isl[6 + 64 * k + 2 * c + 1];
            int n0 = terc4_rev[ w        & 0x3FFu];
            int n1 = terc4_rev[(w >> 10) & 0x3FFu];
            int n2 = terc4_rev[(w >> 20) & 0x3FFu];
            if (n0 < 0 || n1 < 0 || n2 < 0) {
                CHECK(0, "packet %u clock %u is not TERC4", k, c);
                return;
            }
            CHECK((n0 & 0x8) != 0,
                  "packet %u clock %u: ch0 bit3 must be 1 in an island", k, c);
            hb[k][c >> 3] |= (uint8_t)(((n0 >> 2) & 1u) << (c & 7));
            for (int i = 0; i < 4; i++) {
                uint32_t b = (uint32_t)((n1 >> i) & 1) |
                             (uint32_t)(((n2 >> i) & 1) << 1);
                sb[k][i][c >> 2] |= (uint8_t)(b << ((c & 3) * 2));
            }
        }
    }
}

int main(void) {
    for (int i = 0; i < 1024; i++) terc4_rev[i] = -1;
    for (int i = 0; i < 16; i++) terc4_rev[TERC4[i]] = i;

    audio_init();

    // ================= TERC4 and control codes =======================
    for (int i = 0; i < 16; i++) {
        int ones = __builtin_popcount(TERC4[i]);
        CHECK(ones >= 4 && ones <= 6,
              "TERC4[%d]=0x%03x has %d ones (want 4-6, DC balance)",
              i, TERC4[i], ones);
        for (int j = 0; j < 4; j++)
            CHECK(TERC4[i] != CTRL[j],
                  "TERC4[%d] collides with control code %d", i, j);
        for (int j = i + 1; j < 16; j++)
            CHECK(TERC4[i] != TERC4[j], "TERC4 %d and %d are equal", i, j);
    }

    // ================= BCH ECC =======================================
    {
        uint8_t hdr[3] = { 0x02, 0x31, 0x00 };
        CHECK(ecc_bytes(hdr, 3) == ecc_ref(hdr, 24),
              "header ECC table != bit-serial reference");
        uint32_t st = 12345;
        for (int t = 0; t < 10000; t++) {
            uint8_t sub[7];
            for (int i = 0; i < 7; i++) {
                st = st * 1103515245u + 12345u;
                sub[i] = (uint8_t)(st >> 16);
            }
            if (ecc_bytes(sub, 7) != ecc_ref(sub, 56)) {
                CHECK(0, "subpacket ECC mismatch at trial %d", t);
                break;
            }
        }
    }

    // ================= non-island HDMI line ==========================
    {
        uint32_t buf[16], tmds;
        uint32_t *end = audio_emit_active_prefix(buf, false);
        CHECK((uint32_t)(end - buf) == 11,
              "active prefix is %u words, framebuffer layout wants 11",
              (unsigned)(end - buf));
        struct hs h = { -1, -1, 0, 0, 0 };          // no island: control only
        uint32_t clks = walk(buf, (uint32_t)(end - buf), &tmds, hs_cb, &h);
        CHECK(clks == 160, "HDMI active line blanking = %u clocks, want 160",
              clks);
        CHECK(tmds == 640, "active line TMDS run = %u, want 640", tmds);
        CHECK(h.first_low == 16 && h.lows == 96,
              "hsync starts at %d for %d clocks, want 16 for 96",
              h.first_low, h.lows);
    }

    // ================= island line ===================================
    {
        static uint32_t isl[AUD_ISLAND_WORDS];
        uint32_t tmds;
        audio_fill_island(isl, 5, false);
        struct hs h = { -1, -1, 0, 14, 142 };       // packets at clocks 14..141
        uint32_t clks = walk(isl, AUD_ISLAND_WORDS, &tmds, hs_cb, &h);
        CHECK(clks == 160, "island line blanking = %u clocks, want 160", clks);
        CHECK(tmds == 640, "island line TMDS run = %u, want 640", tmds);
        // hsync lives only in the ch0 nibbles here. The leading guard at
        // clocks 12-13 also decodes as TERC4 with h=1, so the run is
        // exactly the 96 clocks from 16.
        CHECK(h.first_low == 16 && h.lows == 96,
              "island hsync starts at %d for %d clocks, want 16 for 96",
              h.first_low, h.lows);
    }

    // ============ island on a BLANKING line ==========================
    // Same 800 clocks, but nothing active follows it, so there is no
    // video preamble, no guard band and no TMDS run at all -- the 640
    // clocks that would have been pixels are control periods. Getting
    // this wrong puts data symbols where the sink expects control and
    // takes the whole picture down, so it is worth its own check.
    {
        static uint32_t isl[AUD_ISLAND_WORDS];
        uint32_t tmds;
        audio_fill_island(isl, 5, true);
        struct hs h = { -1, -1, 0, 14, 142 };
        uint32_t clks = walk(isl, AUD_ISLAND_BLANK_WORDS, &tmds, hs_cb, &h);
        CHECK(clks == 800, "blanking island line = %u clocks, want 800", clks);
        CHECK(tmds == 0, "blanking island emitted %u TMDS pixel clocks, "
              "want none", tmds);
        CHECK(h.first_low == 16 && h.lows == 96,
              "blanking island hsync at %d for %d, want 16 for 96",
              h.first_low, h.lows);
    }

    // Every island index must produce the same shape, including the
    // frame-start one that swaps two audio packets for InfoFrames.
    for (uint32_t idx = 0; idx < 60; idx++) {
        static uint32_t isl[AUD_ISLAND_WORDS];
        uint32_t tmds;
        audio_fill_island(isl, idx, false);
        uint32_t clks = walk(isl, AUD_ISLAND_WORDS, &tmds, NULL, NULL);
        if (clks != 160 || tmds != 640) {
            CHECK(0, "island %u: %u blanking clocks, TMDS %u", idx, clks, tmds);
            break;
        }
    }

    // ================= synth ==========================================
    // BASIC-style square wave: flip the DAC between 0 and 63 every
    // half-period. 1 kHz at 894886 laps/s is ~447 laps per half-cycle.
    {
        audio_init();
        ev_wr = ev_rd = fake_lap = 0;
        push(0, 3, 0x3C);           // $FF23 sound enable on
        push(0, 1, 0x04);           // $FF21 data register selected
        push(0, 4, 0x00);           // mux sel = 00 (DAC)
        push(0, 5, 0x00);
        uint32_t lap = 10;
        for (int i = 0; i < 400; i++) {
            push(lap, 0, (i & 1) ? 0xFC : 0x00);
            lap += 447;
        }
        int zc = 0; int16_t prev = 0;
        for (int i = 0; i < 4000; i++) {
            int16_t s = next_sample();
            if (i > 20 && ((prev <= 0) != (s <= 0))) zc++;
            prev = s;
        }
        // 4000 samples at 48 kHz = 83.3 ms; 1 kHz gives ~166 crossings.
        CHECK(zc > 150 && zc < 182,
              "square wave reconstructed with %d zero crossings, want ~166",
              zc);
    }

    // Gate off must silence within one sample, and a non-DAC mux too.
    {
        audio_init();
        ev_wr = ev_rd = fake_lap = 0;
        push(0, 3, 0x3C); push(0, 1, 0x04); push(0, 4, 0); push(0, 5, 0);
        push(0, 0, 0xFC);
        for (int i = 0; i < 10; i++) next_sample();
        CHECK(next_sample() != 0, "gate on + DAC max should not be silent");

        push(fake_lap + 1, 3, 0x34);             // CB2 low: sound disabled
        fake_lap += 100;
        // Settling, not instantly: the output stage blocks DC, so a level
        // change decays toward zero over the blocker's time constant
        // rather than snapping. Real hardware does exactly this.
        for (int i = 0; i < 4000; i++) next_sample();
        CHECK(next_sample() == 0, "sound gate off must settle to silence");

        push(fake_lap + 1, 3, 0x3C);             // back on
        push(fake_lap + 2, 4, 0x08);             // mux sel = 01 (cassette)
        fake_lap += 100;
        for (int i = 0; i < 4000; i++) next_sample();
        CHECK(next_sample() == 0,
              "mux away from the DAC must settle to silence, not hold");
    }

    // ================= InfoFrames ====================================
    // A sink that dislikes the AVI InfoFrame will still show a picture
    // and silently refuse the audio, which is indistinguishable from
    // every other failure mode this feature has had. Decode the packets
    // back out of the island TERC4 and check them against the spec
    // rather than against the builder that wrote them.
    //
    // The three InfoFrames sit in slot 0 of islands 0, 1 and 2 -- one
    // each, NOT stacked into the first island. Stacking them cost that
    // island three of its four sample slots and crackled at the frame
    // rate, so their placement is checked as carefully as their content.
    {
        static uint32_t isl[AUD_ISLAND_WORDS];
        static uint8_t hb[3][AUD_PACKETS][4], sb[3][AUD_PACKETS][4][8];

        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 100000;
        // Reach steady state first. Straight after audio_init() the servo
        // is unprimed and nothing is due, so every slot would fill with a
        // Null packet and the decode below would prove nothing.
        for (int f = 0; f < 2; f++)
            for (uint32_t idx = 0; idx < ISLAND_LINES; idx++) {
                fake_lap += 187 + (idx & 1u);     // ~187.5 laps per island
                audio_fill_island(isl, idx, false);
            }

        for (uint32_t n = 0; n < 3; n++) {
            fake_lap += 1000;               // guarantee samples are due too
            audio_fill_island(isl, n, false);
            decode_island(isl, hb[n], sb[n]);
        }

        // ---- one InfoFrame per island, all in slot 0 ----
        CHECK(hb[0][0][0] == 0x82, "island 0 slot 0 is %02X, want AVI 0x82",
              hb[0][0][0]);
        CHECK(hb[1][0][0] == 0x84,
              "island 1 slot 0 is %02X, want Audio InfoFrame 0x84",
              hb[1][0][0]);
        CHECK(hb[2][0][0] == 0x83, "island 2 slot 0 is %02X, want SPD 0x83",
              hb[2][0][0]);
        // Slots 1..3 of those islands must be carrying AUDIO, not more
        // InfoFrames -- that is the whole point of spreading them.
        for (uint32_t n = 0; n < 3; n++)
            for (uint32_t k = 1; k < AUD_PACKETS; k++)
                CHECK(hb[n][k][0] == 0x02 || hb[n][k][0] == 0x01 ||
                      hb[n][k][0] == 0x00,
                      "island %u slot %u is %02X -- InfoFrames are stacked",
                      n, k, hb[n][k][0]);

        // ---- ECC survives the round trip on every packet ----
        for (uint32_t n = 0; n < 3; n++)
            for (uint32_t k = 0; k < AUD_PACKETS; k++) {
                CHECK(hb[n][k][3] == ecc_bytes(hb[n][k], 3),
                      "island %u packet %u header ECC is wrong", n, k);
                for (int i = 0; i < 4; i++)
                    CHECK(sb[n][k][i][7] == ecc_bytes(sb[n][k][i], 7),
                          "island %u packet %u subpacket %d ECC is wrong",
                          n, k, i);
            }

        // ---- AVI: version 2, length 13, VIC 1 (640x480p60), RGB ----
        CHECK(hb[0][0][1] == 0x02, "AVI version %u, want 2", hb[0][0][1]);
        CHECK(hb[0][0][2] == 13,   "AVI length %u, want 13", hb[0][0][2]);
        // PB1..PB13 are subpacket 0 bytes 1..6 then subpacket 1 bytes 0..6.
        uint8_t pb[32] = { 0 };
        for (int n = 1; n <= 13; n++) pb[n] = sb[0][0][n / 7][n % 7];
        CHECK((pb[1] & 0x60) == 0x00, "AVI Y must be RGB, PB1 = %02X", pb[1]);
        CHECK(pb[4] == 1, "AVI VIC = %u, want 1 (640x480p60)", pb[4]);
        CHECK(pb[5] == 0, "AVI pixel repetition %u, want 0", pb[5]);
        // Checksum: HB0+HB1+HB2 + PB0..PB13 == 0 mod 256. PB0 IS the
        // checksum, so a builder that forgot it still looks plausible.
        {
            uint32_t sum = hb[0][0][0] + hb[0][0][1] + hb[0][0][2];
            for (int n = 0; n <= 13; n++)
                sum += (n == 0) ? sb[0][0][0][0] : pb[n];
            CHECK((sum & 0xFFu) == 0, "AVI checksum does not zero (%02X)",
                  (unsigned)(sum & 0xFFu));
            CHECK(sb[0][0][0][0] != 0, "AVI checksum byte is 0 -- not written?");
        }

        // ---- Audio InfoFrame: version 1, length 10, CC = 2 channels ----
        CHECK(hb[1][0][1] == 0x01, "Audio IF version %u, want 1", hb[1][0][1]);
        CHECK(hb[1][0][2] == 10,   "Audio IF length %u, want 10", hb[1][0][2]);
        {
            uint8_t a[16] = { 0 };
            for (int n = 1; n <= 10; n++) a[n] = sb[1][0][n / 7][n % 7];
            CHECK((a[1] & 0x07) == 0x01, "Audio IF CC = %u, want 2ch (1)",
                  a[1] & 0x07);
            uint32_t sum = hb[1][0][0] + hb[1][0][1] + hb[1][0][2]
                         + sb[1][0][0][0];
            for (int n = 1; n <= 10; n++) sum += a[n];
            CHECK((sum & 0xFFu) == 0, "Audio IF checksum does not zero");
        }

        // ---- SPD spills into all four subpackets; a length or index
        // slip shows up as a checksum failure here. ----
        CHECK(hb[2][0][1] == 0x01, "SPD version %u, want 1", hb[2][0][1]);
        CHECK(hb[2][0][2] == 25,   "SPD length %u, want 25", hb[2][0][2]);
        {
            uint32_t sum = hb[2][0][0] + hb[2][0][1] + hb[2][0][2]
                         + sb[2][0][0][0];
            for (int n = 1; n <= 25; n++) sum += sb[2][0][n / 7][n % 7];
            CHECK((sum & 0xFFu) == 0, "SPD checksum does not zero");
        }

        // ---- an island past the third carries no InfoFrame at all ----
        fake_lap += 1000;
        audio_fill_island(isl, 3, false);
        decode_island(isl, hb[0], sb[0]);
        for (uint32_t k = 0; k < AUD_PACKETS; k++)
            CHECK(hb[0][k][0] == 0x02 || hb[0][k][0] == 0x01 ||
                  hb[0][k][0] == 0x00,
                  "mid-frame island packet %u is type %02X, want audio/ACR",
                  k, hb[0][k][0]);
    }

    // ================= sample rate ====================================
    // A frame carries 204 audio packets x 4 subpacket slots = 816 sample
    // slots, but only 806.4 samples are DUE per 59.524 Hz frame at 48 kHz.
    // Filling every slot regardless is a 1.2% overproduction: the sink
    // regenerates 48.000 kHz from the ACR (N=6144, CTS=25000) and its FIFO
    // gains 571 samples/s until it overruns, while on this side audio time
    // outruns the event stream until the servo snaps it back. Both are
    // audible as periodic static, and neither shows up in a packet-shape
    // test -- the packets are perfectly well formed, there are just too
    // many samples in them.
    {
        static uint32_t isl[AUD_ISLAND_WORDS];
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 100000;

        #define ISL_LAPS(i) (268u + ((i) & 1u))   // 894886/59.524/56 = 268.5

        for (int f = 0; f < 3; f++)               // prime + settle
            for (uint32_t idx = 0; idx < 56; idx++) {
                fake_lap += ISL_LAPS(idx);
                audio_fill_island(isl, idx, false);
            }

        uint32_t base = audio_samples(), rs = audio_resyncs();
        const int FRAMES = 30;
        for (int f = 0; f < FRAMES; f++)
            for (uint32_t idx = 0; idx < 56; idx++) {
                fake_lap += ISL_LAPS(idx);
                audio_fill_island(isl, idx, false);
            }
        #undef ISL_LAPS

        // 15036 laps per frame / 18.6435 laps per sample = 806.5.
        double per = (double)(audio_samples() - base) / FRAMES;
        CHECK(per > 800.0 && per < 812.0,
              "%.1f samples per frame, want ~806.5 (816 means every slot "
              "was filled regardless of what was due)", per);

        // In steady state audio time tracks the producer, so the servo
        // must never need to snap. A snap is a discontinuity in the PCM.
        CHECK(audio_resyncs() == rs,
              "servo snapped %u times over %d steady frames",
              audio_resyncs() - rs, FRAMES);
    }

    // ================= diagnostic tone ===============================
    // The tone exists to exonerate (or convict) the HDMI side, so it has
    // to be trustworthy on its own: bounded well clear of clipping, and
    // periodic at 1 kHz. A tone that is itself distorted would send the
    // next round of debugging in exactly the wrong direction.
    {
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 0;
        // Both pitches: 1 kHz is 2000 zero crossings a second, 500 Hz is
        // 1000. A tone that is not the pitch it claims is useless as a
        // probe -- the whole point is judging artefacts against it.
        const int want_zc[3] = { 0, 2000, 1000 };
        for (uint8_t mode = 1; mode <= 2; mode++) {
            audio_set_test(mode);
            CHECK(audio_test() == mode, "tone mode %u did not take", mode);

            int32_t mn = 32767, mx = -32768;
            int zc = 0; int16_t prev = 0;
            for (int i = 0; i < 48000; i++) {     // one second at 48 kHz
                int16_t s = next_sample();
                if (s < mn) mn = s;
                if (s > mx) mx = s;
                if ((prev <= 0 && s > 0) || (prev >= 0 && s < 0)) zc++;
                prev = s;
            }
            CHECK(zc >= want_zc[mode] - 2 && zc <= want_zc[mode] + 2,
                  "tone mode %u has %d zero crossings/s, want %d",
                  mode, zc, want_zc[mode]);
            CHECK(mx > 7000 && mx < 8300 && mn < -7000 && mn > -8300,
                  "tone mode %u spans %d..%d, want about +/-8191",
                  mode, (int)mn, (int)mx);
        }
        // Out of range must fall back to OFF, not to a silent "on".
        audio_set_test(7);
        CHECK(audio_test() == 0, "an unknown tone mode did not turn it off");
        audio_set_test(2);

        // Turning it off must hand the DAC path straight back, with no
        // residual tone: the probe has to leave no trace.
        audio_set_test(0);
        CHECK(audio_test() == 0, "tone did not turn off");
        for (int i = 0; i < 8; i++) next_sample();
        CHECK(next_sample() == 0,
              "tone still audible after being switched off");
    }

    // ================= delivery uniformity ===========================
    // The packets can be immaculate and the average rate exact while the
    // samples still arrive in lumps: any gap where a frame's worth of
    // islands cannot keep up with what is due comes out of the SINK's
    // buffer, and when the gap repeats every frame it is audible at
    // 59.52 Hz. Islands confined to active lines (a 1.44 ms hole across
    // vertical blanking) and all three InfoFrames stacked in one island
    // (a ~9-sample hole) are both of this kind, and neither moves a
    // single packet-shape assertion.
    //
    // So: walk a whole frame island by island, advancing the producer
    // clock as scanout would, and track how far DELIVERED ever falls
    // behind DUE.
    {
        static uint32_t isl[AUD_ISLAND_WORDS];
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 100000;

        // 894886 laps/s / 59.524 frames/s / ISLAND_LINES islands.
        const uint32_t LAPS_PER_ISLAND = 15036u / ISLAND_LINES;

        for (int f = 0; f < 3; f++)               // prime + settle
            for (uint32_t idx = 0; idx < ISLAND_LINES; idx++) {
                fake_lap += LAPS_PER_ISLAND;
                audio_fill_island(isl, idx, false);
            }

        uint32_t worst = 0, worst_at = 0;
        for (int f = 0; f < 10; f++) {
            uint32_t base_lap = fake_lap, base_smp = audio_samples();
            for (uint32_t idx = 0; idx < ISLAND_LINES; idx++) {
                fake_lap += LAPS_PER_ISLAND;
                audio_fill_island(isl, idx, false);

                // Samples the sink has been owed since the frame started,
                // against the number actually handed over.
                uint32_t due  = ((fake_lap - base_lap) << 16) /
                                LAPS_PER_SAMPLE_Q16;
                uint32_t got  = audio_samples() - base_smp;
                uint32_t lag  = (due > got) ? due - got : 0;
                if (lag > worst) { worst = lag; worst_at = idx; }
            }
        }

        // A 1.44 ms vblank hole is 69 samples and buzzes; three stacked
        // InfoFrames were ~9 and crackled. Hold the line well under that.
        CHECK(worst <= 5,
              "delivery falls %u samples behind at island %u -- a hole this "
              "size repeats every frame and is audible", worst, worst_at);
    }

    // ================= box filtering =================================
    // Each sample must be the AVERAGE of the DAC over its window, not a
    // point reading at the window's start. Point-sampling snaps every
    // edge to a 48 kHz boundary, throwing away the 1.117 us resolution
    // the event timestamps carry; on a CoCo square wave that is period
    // jitter, and it is heard as scratchiness.
    {
        // ---- a steady DAC must come out perfectly steady ----
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 0;
        push(0, 4, 0x00);            // mux SEL = 00: the DAC
        push(0, 5, 0x00);
        push(0, 1, 0x3C);            // CRA bit 2: $FF20 is the DAC
        push(0, 3, 0x3C);            // sound enabled
        push(0, 0, 0xFC);            // DAC at full scale
        // A steady DAC must DECAY to zero, monotonically and without
        // ripple: that is the coupling capacitor, and it is why the CoCo
        // cannot put a DC offset on the wire. What must not appear is
        // wobble on the way down -- that would be the sampler, not the
        // filter.
        for (int i = 0; i < 16; i++) next_sample();
        int16_t prev_d = next_sample();
        CHECK(prev_d != 0, "steady DAC sampled as silence");
        bool falling = prev_d > 0;
        // The blocker is a 1024-sample one-pole (~7.5 Hz), so a full decay
        // from the top of the range needs tens of thousands of samples.
        for (int i = 0; i < 24000; i++) {
            int16_t s = next_sample();
            CHECK(falling ? (s <= prev_d) : (s >= prev_d),
                  "a steady DAC does not decay monotonically (ripple)");
            prev_d = s;
        }
        CHECK(prev_d == 0, "a steady DAC never reached zero");

        // ---- an edge inside a window must land between the two levels ----
        // One sample is 18.64 laps. Put a full-scale transition right in
        // the middle of one and the sample covering it must be roughly
        // halfway -- a point sampler can only ever return one end or the
        // other, which is exactly the resolution being lost.
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 0;
        push(0, 4, 0x00);
        push(0, 5, 0x00);
        push(0, 1, 0x3C);
        push(0, 3, 0x3C);
        push(0, 0, 0x00);            // start at the bottom of the range
        for (int i = 0; i < 4; i++) next_sample();
        int16_t lo = next_sample();

        // s_lap is now 5 samples in. Step to full scale nine laps into
        // the NEXT sample window.
        push(s_lap + 9, 0, 0xFC);
        int16_t mid = next_sample();
        int16_t hi  = next_sample();

        CHECK(hi != lo, "the step never arrived");
        int32_t half = ((int32_t)lo + (int32_t)hi) / 2;
        int32_t tol  = (hi > lo ? hi - lo : lo - hi) / 4;
        CHECK(mid > half - tol && mid < half + tol,
              "edge sample is %d; want near the midpoint %d of %d..%d "
              "(a point sampler returns one end or the other)",
              mid, half, lo, hi);
        CHECK(mid != lo && mid != hi,
              "edge sample sits exactly on a level -- still point-sampling");
    }

    // ================= the DAC latch =================================
    // s_ff20 is the PIA's OUTPUT REGISTER, not "the last byte written to
    // $FF20". When CRA bit 2 is clear that address is the data direction
    // register, and a write there never reaches the resistor ladder.
    // Treating every write as a DAC value injected a spurious level, and
    // reading the DAC as 0 while bit 2 was clear produced a full-scale
    // NEGATIVE spike -- 0 is the bottom of the range, 32 is the midpoint.
    {
        // Levels decay now (the output stage blocks DC), so this looks for
        // STEPS instead: a write that reaches the ladder moves the output
        // sharply, a write that does not leaves the decay undisturbed.
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 0;
        push(0, 4, 0x00);            // mux SEL = 00: the DAC
        push(0, 5, 0x00);
        push(0, 1, 0x3C);            // CRA bit 2 set: $FF20 is the DAC
        push(0, 3, 0x3C);            // sound enabled
        push(0, 0, 0xFC);            // a real DAC write
        for (int i = 0; i < 8; i++) next_sample();

        // Biggest sample-to-sample move over a short window.
        int16_t a = next_sample();
        int32_t quiet = 0;
        for (int i = 0; i < 8; i++) {
            int16_t b = next_sample();
            int32_t d = b > a ? b - a : a - b;
            if (d > quiet) quiet = d;
            a = b;
        }

        #define STEP_AFTER(reg, val) ({                       \
            push(s_lap + 1, (reg), (val));                    \
            fake_lap += 200;                                  \
            int16_t p = next_sample(); int32_t big = 0;       \
            for (int i = 0; i < 8; i++) {                     \
                int16_t q = next_sample();                    \
                int32_t dd = q > p ? q - p : p - q;           \
                if (dd > big) big = dd;                       \
                p = q;                                        \
            }                                                 \
            big; })

        // CRA bit 2 clear: $FF20 becomes the DDR. No step.
        CHECK(STEP_AFTER(1, 0x38) <= quiet * 4 + 8,
              "clearing CRA bit 2 disturbed the analogue output");
        // A write to $FF20 in that state is a DDR write. Still no step.
        CHECK(STEP_AFTER(0, 0x00) <= quiet * 4 + 8,
              "a DDR write to $FF20 reached the ladder");
        // Point bit 2 back at the output register: nothing new was
        // written, so the ladder still holds -- no step.
        CHECK(STEP_AFTER(1, 0x3C) <= quiet * 4 + 8,
              "restoring CRA bit 2 disturbed the held value");
        // And now a real write must move it, unmistakably.
        CHECK(STEP_AFTER(0, 0x00) > quiet * 4 + 8,
              "a real DAC write was ignored");
        #undef STEP_AFTER
    }

    // ================= the Zaxxon tick ===============================
    // Straight from a capture of the real thing. Every 4 frames Zaxxon
    // mutes the sound gate, reads the joystick, leaves the ladder at 0,
    // unmutes, and then sits still for 66 ms. A real CoCo is SILENT
    // throughout -- a motionless ladder is DC, and DC never reaches the
    // speaker whatever the gate is doing.
    //
    // Blocking DC on the gated output instead of the source turns that
    // into a 15 Hz square wave at half full scale: gate-off reads as 0,
    // gate-on reads as (0-32)*512 = -16384. It is plainly audible as
    // ticking and no packet, timing or rate test can see it.
    {
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 0;
        push(0, 4, 0x00);            // mux SEL = 00: the DAC
        push(0, 5, 0x00);
        push(0, 1, 0x3C);            // CRA bit 2: $FF20 is the DAC
        push(0, 3, 0xBC);            // gate ON, as Zaxxon leaves it
        push(0, 0, 0x03);            // ladder parked at 0 -- the bottom
        for (int i = 0; i < 20000; i++) next_sample();   // let DC settle

        // Four rounds of the real sequence, checking the output never
        // moves. The tolerance is tight on purpose: at -16384 the old
        // behaviour missed it by three orders of magnitude.
        int32_t worst = 0;
        for (int round = 0; round < 4; round++) {
            static const struct { uint32_t reg, val; } SEQ[] = {
                { 3, 0xB4 },             // gate OFF
                { 4, 0xBC }, { 5, 0x35 },// sel sweep
                { 0, 0x12 }, { 0, 0xEA },
                { 4, 0x34 },
                { 0, 0x12 }, { 0, 0xEA },
                { 5, 0x35 },
                { 0, 0x03 },             // ladder left at 0
                { 3, 0xBC },             // gate back ON
            };
            for (unsigned k = 0; k < sizeof SEQ / sizeof SEQ[0]; k++) {
                push(s_lap + 1 + k * 16u, SEQ[k].reg, SEQ[k].val);
            }
            fake_lap += 59730;            // 4 CoCo frames, as captured
            for (int i = 0; i < 3200; i++) {   // 66 ms of samples
                int32_t s = next_sample();
                if (s > worst)  worst = s;
                if (-s > worst) worst = -s;
            }
        }
        // A joystick sweep under a closed gate has to stay inaudible, and
        // the quiet stretch after it has to be actually quiet.
        CHECK(worst < 600,
              "Zaxxon's mute/read/unmute cycle peaks at %d; a real CoCo is "
              "silent here (DC blocked after the gate hits 16384)", (int)worst);
    }

    // ---- a static ladder is inaudible whatever the gate does ----
    {
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 0;
        push(0, 4, 0x00); push(0, 5, 0x00);
        push(0, 1, 0x3C); push(0, 3, 0x3C);
        push(0, 0, 0xFC);                 // parked at the TOP this time
        for (int i = 0; i < 20000; i++) next_sample();
        CHECK(next_sample() == 0, "a parked ladder is not silent");

        for (int round = 0; round < 3; round++) {
            push(s_lap + 1, 3, 0x34);     // gate off
            fake_lap += 200;
            for (int i = 0; i < 400; i++)
                CHECK(next_sample() == 0, "muting a parked ladder made a step");
            push(s_lap + 1, 3, 0x3C);     // gate on
            fake_lap += 200;
            for (int i = 0; i < 400; i++)
                CHECK(next_sample() == 0,
                      "unmuting a parked ladder made a step");
        }
    }

    // ================= the single-bit output =========================
    // Captured from the diagnostic cart: 1.3 seconds of $FF22 bit 1
    // toggling with the sound gate OFF the whole time, audible on a real
    // CoCo. The enable switch sits in the DAC/mux path; PB1 is summed
    // into the audio node past it, so gating PB1 behind `live` deletes an
    // entire section of the test.
    {
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 0;
        push(0, 1, 0x3C);
        push(0, 3, 0x34);            // sound gate OFF, and left off
        push(0, 0, 0x58);            // ladder parked, as captured
        for (int i = 0; i < 20000; i++) next_sample();

        // Toggle PB1 at roughly the captured rate and listen for it.
        int32_t peak = 0;
        for (int i = 0; i < 400; i++) {
            push(s_lap + 1, 2, (i & 1) ? 0x07u : 0x05u);
            fake_lap += 300;
            for (int k = 0; k < 16; k++) {
                int32_t s = next_sample();
                if (s > peak)  peak = s;
                if (-s > peak) peak = -s;
            }
        }
        CHECK(peak > 500,
              "single-bit output is silent with the gate off (peak %d); the "
              "enable switch is not in front of PB1", (int)peak);

        // ...and it must still obey DC: parked high is not a tone.
        push(s_lap + 1, 2, 0x07);
        fake_lap += 200;
        for (int i = 0; i < 24000; i++) next_sample();   // DC blocker is ~7.5 Hz
        CHECK(next_sample() == 0, "a parked single-bit output is not silent");
    }

    // ---- $FF22 obeys CRB bit 2, same as the DAC obeys CRA bit 2 ----
    // With CRB bit 2 clear, $FF22 is the Port B DIRECTION register. It
    // never reaches the pin, so the single-bit output must not follow it.
    // The diagnostic cart is the proof: its EORA-based sound routine can
    // only ever flip PB1, yet thousands of $05/$07 writes appear on the
    // bus -- values that instruction cannot produce, because they are
    // direction writes.
    {
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 0;
        push(0, 1, 0x3C);
        push(0, 3, 0x3C);            // CRB bit 2 SET: $FF22 is the data reg
        push(0, 0, 0x58);
        for (int i = 0; i < 20000; i++) next_sample();

        int32_t peak = 0;
        for (int i = 0; i < 200; i++) {          // toggling: audible
            push(s_lap + 1, 2, (i & 1) ? 0x07u : 0x05u);
            fake_lap += 300;
            for (int k = 0; k < 16; k++) {
                int32_t s = next_sample();
                if (s > peak) peak = s; if (-s > peak) peak = -s;
            }
        }
        CHECK(peak > 500, "single-bit silent while CRB bit 2 is set");

        // Now point $FF22 at the DIRECTION register and toggle the same
        // bit: it must make no sound at all.
        push(s_lap + 1, 3, 0x38);    // CRB bit 2 CLEAR
        fake_lap += 200;
        for (int i = 0; i < 24000; i++) next_sample();   // DC blocker is ~7.5 Hz
        int32_t ddr_peak = 0;
        for (int i = 0; i < 200; i++) {
            push(s_lap + 1, 2, (i & 1) ? 0x07u : 0x05u);
            fake_lap += 300;
            for (int k = 0; k < 16; k++) {
                int32_t s = next_sample();
                if (s > ddr_peak) ddr_peak = s;
                if (-s > ddr_peak) ddr_peak = -s;
            }
        }
        CHECK(ddr_peak < 200,
              "direction-register writes to $FF22 reached the single-bit "
              "output (peak %d); they never touch the pin", (int)ddr_peak);
    }

    // ================= filter order ==================================
    // What matters is SLOPE. One pole rolls off at 6 dB/octave, so every
    // corner that removes the buzz removes the notes with it.
    //
    // Measured on the STEP RESPONSE, not on square-wave levels: the mean
    // level of a square barely moves with frequency because it plateaus
    // at the rails either way, so a level-ratio test cannot tell the
    // orders apart.
    //
    // The shape is unambiguous. A single pole's largest jump is its FIRST
    // sample and every later one is smaller. Two poles start slowly and
    // accelerate -- the second increment exceeds the first. That tells
    // the orders apart without depending on any corner frequency.
    {
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 0;
        audio_set_filter(1);
        push(0, 4, 0x00); push(0, 5, 0x00);
        push(0, 1, 0x3C); push(0, 3, 0x3C);
        push(0, 0, 0x00);                      // ladder parked low
        for (int i = 0; i < 200; i++) next_sample();

        push(s_lap + 1, 0, 0xFC);              // full-scale step
        fake_lap += 400;
        int32_t y[4];
        for (int i = 0; i < 4; i++) y[i] = next_sample();

        // Let it settle. The low-pass is done inside ~10 samples while the
        // DC blocker's 512-sample tail has barely started, so this is the
        // filter's own final value.
        int32_t settled = y[3];
        for (int i = 0; i < 24; i++) {
            int32_t s = next_sample();
            if (s > settled) settled = s;
        }
        CHECK(settled > 2000, "the step never arrived (settled at %d)",
              (int)settled);
        // How much of the step lands in the very FIRST sample identifies
        // the order. The bound comes from measurement, not theory: the
        // textbook figures are 0.30 for two poles and 0.54 for one, but
        // the DC blocker and the box filter shift both, and the real
        // numbers here are about -0.08 and 0.30. A threshold taken from
        // the theory sits above BOTH and passes a one-pole build.
        if (settled > 0) {
            double first = (double)y[0] / (double)settled;
            CHECK(first < 0.15,
                  "the first sample after a step is %.2f of the settled "
                  "value; two poles measure about -0.08 here and one pole "
                  "about 0.30", first);
        }

        // ...and with the filter off the response must be immediate, so
        // the check above is testing the filter and not the sampler.
        audio_init();
        ev_wr = ev_rd = 0;
        fake_lap = 0;
        audio_set_filter(0);
        push(0, 4, 0x00); push(0, 5, 0x00);
        push(0, 1, 0x3C); push(0, 3, 0x3C);
        push(0, 0, 0x00);
        for (int i = 0; i < 200; i++) next_sample();
        int32_t before = next_sample();
        push(s_lap + 1, 0, 0xFC);
        fake_lap += 400;
        int32_t after = next_sample();
        CHECK(after - before > 2000,
              "unfiltered step only moved %d; the filter is not what the "
              "test above measured", (int)(after - before));
        audio_set_filter(0);
    }

    if (failures) { printf("test_audio: %d FAILURES\n", failures); return 1; }
    printf("test_audio: OK (TERC4, BCH ECC vs bit-serial, island + active "
           "line = 160+640 clocks, blanking island = 800 all-control, island hsync, AVI/audio/SPD InfoFrames decoded back out of the island, 806.5 samples/frame with no servo snap, 1 kHz + 500 Hz diagnostic tones, delivery never lags, box-filtered edges, DC blocked before the gate (Zaxxon tick), single-bit ungated, DAC latch honours CRA bit 2, square wave, 2-pole filter order, gate/mux)\n");
    return 0;
}
