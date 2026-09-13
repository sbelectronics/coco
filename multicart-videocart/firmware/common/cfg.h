#ifndef MULTICART_CFG_H
#define MULTICART_CFG_H

// Persistent configuration — piconet's cfg module, payload extended for
// the multicart. Stored in the last 4 KB flash sector with magic,
// version and CRC32; config_local.h supplies build-time WiFi fallbacks
// used only when the sector has never been provisioned.

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    CFG_AUTH_OPEN = 0, CFG_AUTH_WPA2, CFG_AUTH_WPA3, CFG_AUTH_MIXED,
} cfg_auth_t;

typedef struct {
    struct {
        char       ssid[33];
        char       psk[64];
        cfg_auth_t auth;
        uint8_t    enabled;          // radio on at boot
    } wifi;
    char     last_rom[64];           // auto-served at power-on; "" = none
    uint32_t strobe_delay_ns;        // bank-write sample delay (scope knob)
    uint8_t  artifact;               // PMODE 4 colour: 0 off, 1 phase A, 2 B
    uint8_t  hdmi_audio;             // 1 = HDMI w/ audio islands, 0 = plain DVI
    uint8_t  audio_gain;             // 0..3, PCM >> (3 - gain)
    // The analogue output stage. A CoCo playing a six-step waveform puts
    // out mostly harmonics, and its own audio path removes them before
    // they reach a speaker; emitting them at full bandwidth is what makes
    // this sound harsher than the real machine. Where to put the corner is
    // a judgement for the ear, so it is a setting, and it has to survive a
    // reboot or it gets re-chosen every session.
    uint8_t  audio_filter;           // 0 = raw ladder, 1..3 = more low-pass
    // Level of the single-bit output (PIA1 PB1) against the 6-bit ladder.
    // On real hardware PB1 joins the summing node through a resistor and
    // is quieter than a full ladder swing; the right ratio is for the ear.
    uint8_t  audio_onebit;           // 0 = off, 1 = 1/4, 2 = 1/2, 3 = full
} cfg_t;

void     cfg_init(void);
void     cfg_load_defaults(void);
cfg_t   *cfg_get(void);
bool     cfg_save(void);
bool     cfg_wipe(void);
bool     cfg_is_usable(void);        // wifi ssid present
bool     cfg_loaded_from_flash(void);

uint32_t    cfg_auth_to_cyw43(cfg_auth_t a);
const char *cfg_auth_name(cfg_auth_t a);
bool        cfg_auth_parse(const char *s, cfg_auth_t *out);

#endif
