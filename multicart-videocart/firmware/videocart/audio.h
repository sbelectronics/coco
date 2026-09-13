#ifndef VIDEOCART_AUDIO_H
#define VIDEOCART_AUDIO_H

// ======================================================================
// audio.h — CoCo internal sound reconstructed from the bus, shipped as
// HDMI audio data islands.
// ======================================================================

#include <stdint.h>
#include <stdbool.h>

// One HDMI data island carries 4 packets of 32 TMDS clocks. The command
// stream for a whole island line (blanking + the TMDS command that starts
// the pixels) is this many 32-bit words; hstx.c keeps a small ring of
// these and audio_fill_island() writes one.
// 8 preamble/guard/control pairs + 4 packets x 32 clocks x 2 words
// (RAW_REPEAT 1 + the word) + the trailing CMD_TMDS.
// Active-line island: 6 lead-in + 4x64 packet + 9 hand-off words.
// A blanking-line island ends in two control runs instead of the
// preamble/guard/TMDS hand-off, so it is 3 words shorter. The slot
// buffers are sized for the larger; each descriptor carries its own
// count.
#define AUD_ISLAND_WORDS   271
#define AUD_ISLAND_BLANK_WORDS 268
#define AUD_PACKETS        4          // packets per island line
#define AUD_SLOTS          4          // island lines of lead time

// Audio event ring, produced by core 1 and owned by bus.c (declared here
// rather than in the shared common/bus.h because it is videocart-only).
// bus_audio_take() pops one event word, returning 0 when empty;
// bus_audio_lap() is core 1's live bus-cycle count, the audio timebase.
uint32_t bus_audio_take(uint32_t *ev);
uint32_t bus_audio_depth(void);
uint32_t bus_audio_ring_size(void);
uint32_t bus_audio_lap(void);

void audio_init(void);

// Tell core 1 whether to stamp audio events at all (bus.c). Off in DVI
// mode, where nothing would ever drain them.
void bus_audio_enable(bool on);

// Called by the HSTX IRQ when a slot was filled after its line had
// already been scanned out -- the one number that says the just-in-time
// scheme is not keeping up. Must stay 0 in steady state.
void audio_note_late(void);

// Fill one island slot with the packets due at `island_idx` of the frame.
// Called from the HSTX DMA IRQ on core 0 — RAM-resident, no flash, no
// blocking. `dst` is AUD_ISLAND_WORDS words.
void audio_fill_island(uint32_t *dst, uint32_t island_idx, bool blanking);

// Emit a line prefix with no data island (video preamble + guard only).
// `vsync_low` selects the vsync level carried in the control words.
uint32_t *audio_emit_active_prefix(uint32_t *p, bool vsync_low);

// Live gain, 0..3 (PCM >> (3 - gain)). Applied on the next sample.
// Diagnostic tone: replaces the CoCo-derived PCM with a synthetic 1 kHz
// sine, to separate "the HDMI audio transport is dirty" from "what we
// captured off the bus is dirty". 1 = 1 kHz, 2 = 500 Hz. Runtime only,
// never persisted.
void    audio_set_test(uint8_t mode);
uint8_t audio_test(void);

void    audio_set_gain(uint8_t gain);
uint8_t audio_gain(void);

// ---- diagnostics (see /api/status) ----
uint32_t audio_events(void);        // register writes consumed
uint32_t audio_resyncs(void);       // drift snaps
uint32_t audio_late(void);          // islands filled after being scanned
uint32_t audio_samples(void);       // PCM samples generated
uint32_t audio_nulls(void);          // island slots that had nothing due
uint32_t audio_gate_off(void);       // permille of TIME the sound gate is off
uint32_t audio_mux_off(void);        // ...and the mux is away from the DAC
uint32_t audio_mux_edges(void);      // select-line transitions seen
uint32_t audio_gate_edges(void);     // sound-gate transitions seen
uint32_t audio_peak(void);           // largest sample magnitude seen
uint32_t audio_clips(void);          // samples that actually hit the clamp

// The last few thousand samples actually sent to the sink, for looking at
// the spectrum instead of reasoning about it.
uint32_t audio_pcm_count(void);
int16_t  audio_pcm_at(uint32_t i);
void     audio_pcm_rearm(void);      // start a fresh coherent window
bool     audio_pcm_ready(void);      // the window is complete

// Tee of the last register writes the CoCo made to the audio registers,
// oldest first. Event word: lap[19:0]<<12 | reg[3:0]<<8 | data[7:0].
uint32_t audio_log_count(void);
uint32_t audio_log_at(uint32_t i);
// Derived state recorded WITH each event, not replayed from defaults:
// dac[15:10], gate[3], sel[2:1], one-bit[0].
uint32_t audio_log_state(uint32_t i);
// A separate, unflooded log of VIDEO events only: SAM writes, and $FF22
// writes that actually moved the mode bits.
uint32_t audio_vlog_count(void);
uint32_t audio_vlog_at(uint32_t i);

// Analogue output stage: 0 = raw ladder, 1..3 = increasing low-pass. The
// DC blocker is always on -- real hardware cannot pass DC. Runtime only.
void    audio_set_filter(uint8_t n);

// Single-bit output level against the ladder: 0 off, 1 quarter, 2 half,
// 3 full. Real hardware mixes PB1 in through a resistor, so "full" is
// almost certainly too loud -- but the ratio is for the ear to settle.
void    audio_set_onebit(uint8_t n);
uint8_t audio_onebit(void);
uint8_t audio_filter(void);
uint16_t audio_state(void);         // [15:8] dac, [3] gate, [2:1] mux, [0] 1-bit

#endif
