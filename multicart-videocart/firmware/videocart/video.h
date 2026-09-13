#ifndef VIDEOCART_VIDEO_H
#define VIDEOCART_VIDEO_H

#include <stdint.h>
#include <stdbool.h>

// ======================================================================
// Video subsystem: HSTX scanout (hstx.c) + VDG renderer (vdg.c).
//
// Display: 640x480@60 DVI/HDMI. The CoCo's 256x192 VDG image is drawn 2x
// horizontally into a 640-wide framebuffer (with black side borders
// baked in) and 2x vertically by pointing two DMA descriptors at the
// same row, so only 192 rows are stored.
// ======================================================================

#define VID_W           640         // active pixels per line
#define VID_ROWS        192         // stored rows (each scanned twice)
#define VID_IMG_W       512         // 256 VDG pixels x2
#define VID_X_MARGIN    ((VID_W - VID_IMG_W) / 2)   // 64px side borders

// VESA 640x480@60 timing. Lives here rather than in hstx.c because audio.c
// builds line prefixes too (the HDMI preamble/guard and data islands).
#define MODE_H_FRONT_PORCH   16
#define MODE_H_SYNC_WIDTH    96
#define MODE_H_BACK_PORCH    48
#define MODE_H_ACTIVE_PIXELS 640

#define MODE_V_FRONT_PORCH   10
#define MODE_V_SYNC_WIDTH    2
#define MODE_V_BACK_PORCH    33
#define MODE_V_ACTIVE_LINES  480
// Whole frame including blanking -- data islands are spread over this,
// not just the active lines.
#define MODE_V_TOTAL_LINES   (MODE_V_FRONT_PORCH + MODE_V_SYNC_WIDTH \
                              + MODE_V_BACK_PORCH + MODE_V_ACTIVE_LINES)

// Islands are spread over ALL 525 lines, vertical blanking included --
// NOT just the 480 active ones. Confining them to active lines leaves a
// 1.44 ms hole in every frame with no audio packets in it: the sink has
// to coast ~69 samples on its own buffer, once per frame, and it buzzes
// at exactly the 59.52 Hz frame rate. A conforming HDMI source uses every
// blanking interval, and the vertical one is the largest.
//
// Must stay a multiple of AUD_SLOTS: island position p is always served
// by slot p % AUD_SLOTS, and the descriptor for that position carries a
// fixed word count. If the mapping rotated between frames, a blanking
// island (268 words) could land in a descriptor sized for an active one.
#define ISLAND_LINES    80      // 4 packets each: 320/frame against the
                                // 806.4 samples a frame owes. The headroom
                                // matters: with fewer islands, one that
                                // gives up a slot to an InfoFrame comes up
                                // short and has to be made back later.


// RGB332 helper — standard RRRGGGBB, which is what the HSTX expander
// configuration expects: lane 2 (red) ROT=0 takes bits [7:5], lane 1
// (green) ROT=29 takes [4:2], lane 0 (blue) ROT=26 takes [1:0]. Packing
// red into the LOW bits swaps the red and blue channels on the physical
// display. test_vdg's PPM decode is keyed to this exact layout; keep them
// in sync.
#define RGB332(r, g, b) ((uint8_t)(((r) & 0xE0) | (((g) & 0xE0) >> 3) | (((b) & 0xC0) >> 6)))

// ---- hstx.c ----------------------------------------------------------
// Brings up HSTX, builds the frame program and starts the self-chaining
// DMA loop. After this returns the display scans out of SRAM forever
// with zero CPU involvement — which is what makes it immune to the
// multi-millisecond interrupt-off windows of a flash erase.
void hstx_init(bool hdmi_audio);

// Pixel row `y` (0..VID_ROWS-1), VID_W bytes of RGB332.
uint8_t *hstx_row(int y);

// Fill every row with one colour (borders included).
void hstx_fill(uint8_t rgb332);

// Bring-up aid: a static pattern proving scanout works with no CoCo
// attached. Splits "renderer wrong" from "capture wrong" later.
void hstx_test_pattern(void);

// ---- vdg.c -----------------------------------------------------------
void vdg_init(void);

// Called from bus_pump() on core 0 for every snooped write to a video
// register ($FF22 and the SAM range). Applied immediately; RAM-resident
// so a flash erase can never collide with it.
void video_note_reg(uint16_t addr, uint8_t data);

// Core-0 pump: re-renders the frame when due (~60 Hz).
void vdg_pump(void);

// Diagnostics for DIAG / the web API.
uint32_t vdg_frames(void);

// HSTX: which signalling is running, and how many island lines the frame
// carries (0 in DVI mode).
uint32_t hstx_island_lines(void);
// Times the island refill handler found its running count disagreed with
// where scanout actually was. Must be 0 after the first frame.
uint32_t hstx_island_fixes(void);
bool     hstx_hdmi_audio(void);
// Total register writes applied. They are applied on arrival, never
// queued, so this can never fall behind what the bus delivered.
uint32_t vdg_reg_writes(void);
// $FF22 changes per rendered frame since the previous call. Many per
// frame means the cart is doing a per-scanline effect that a whole-frame
// renderer cannot represent. Consumes the interval; call it once per poll.
uint32_t vdg_mode_rate(void);

// PMODE 4 artifact colour: 0 = off (mono), 1 = phase A, 2 = phase B.
// The phase is arbitrary on real hardware, so this is switchable live.
void    vdg_set_artifact(uint8_t mode);
uint8_t vdg_artifact(void);

// The buffer the LAST rendered frame actually read, for /api/screen. Not
// recomputed: the dump must be the bytes that produced what is on screen.
uint16_t vdg_screen_base(void);
uint16_t vdg_screen_bytes(void);
uint16_t vdg_mode_word(void);
uint16_t vdg_sam_offset(void);

#endif
