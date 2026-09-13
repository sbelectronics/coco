#include "cfg.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include "pico/cyw43_arch.h"   // for CYW43_AUTH_* macros

#include <string.h>
#include <strings.h>
#include <stdio.h>

// ----------------------------------------------------------------------
// On-flash layout — last 4 KB sector of flash. Single-buffered: a save
// erases then writes; a power loss mid-save reverts to defaults on the
// next boot (acceptable for v1; reprovision via USB CDC).
//
//   offset 0:                   uint32_t magic
//   offset 4:                   uint32_t version
//   offset 8:                   cfg_t payload
//   offset PAYLOAD_END..page-4: padding (0xFF after erase)
//   offset PAGE-4:              uint32_t crc32 of bytes [0..page-4)
//
// One FLASH_PAGE_SIZE (256-byte) page is enough for everything we
// store today; we still reserve the full sector since erase granularity
// is 4 KB.
// ----------------------------------------------------------------------

#define CFG_MAGIC        0x434F434Fu    // "COCO"
// Version history:
//   1  base layout
//   2  adds hdmi_audio + audio_gain
//   3  adds audio_filter + audio_onebit
// Older pages are still ACCEPTED and migrated (see cfg_init) rather than
// rejected -- rejecting one would throw away the WiFi credentials, which
// is a far worse outcome than a setting reverting to its default.
#define CFG_VERSION      3u
#define CFG_VERSION_MIN  1u

#define CFG_FLASH_OFFSET (MC_CFG_FLASH_OFFSET)
#define CFG_FLASH_ADDR   ((const uint8_t *)(XIP_BASE + CFG_FLASH_OFFSET))

#define CFG_PAGE_BYTES   FLASH_PAGE_SIZE   // 256
#define CFG_CRC_OFFSET   (CFG_PAGE_BYTES - 4)

typedef struct {
    uint32_t magic;
    uint32_t version;
    cfg_t    payload;
} cfg_header_t;

_Static_assert(sizeof(cfg_header_t) <= CFG_CRC_OFFSET,
               "cfg payload + header must fit before CRC slot");

static cfg_t s_cfg;
static bool  s_loaded_from_flash;

// CRC32 (IEEE 802.3 polynomial, reflected). Standard zlib variant.
static uint32_t crc32(const uint8_t *data, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= data[i];
        for (int b = 0; b < 8; b++) {
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
        }
    }
    return ~c;
}

static bool flash_payload_valid(const uint8_t *page) {
    const cfg_header_t *h = (const cfg_header_t *)page;
    if (h->magic   != CFG_MAGIC)   return false;
    if (h->version <  CFG_VERSION_MIN) return false;
    if (h->version >  CFG_VERSION)     return false;
    uint32_t expect;
    memcpy(&expect, page + CFG_CRC_OFFSET, sizeof(expect));
    if (crc32(page, CFG_CRC_OFFSET) != expect) return false;
    return true;
}

void cfg_load_defaults(void) {
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.wifi.auth       = CFG_AUTH_WPA2;
    s_cfg.wifi.enabled    = 1;
    s_cfg.strobe_delay_ns = MC_STROBE_DELAY_NS_DEFAULT;
    // Artifact colour on by default: real composite hardware shows it
    // whether or not a game wants it, so mono is the deviation, not this.
    //
    // Phase B, established by rendering a real Zaxxon title dump through
    // the reference model and comparing both phases against a photograph
    // of the same screen on composite: phase B puts the ZAXXON logo and
    // the fuel arrows in blue, matching the display; phase A rendered
    // them orange. The phase is arbitrary in the hardware, so this is
    // only a default -- the web UI switches it live.
    s_cfg.artifact        = 2;
    // HDMI signalling with audio by default: a DVI sink still shows the
    // picture, and the CoCo has no other audio path off this board.
    s_cfg.hdmi_audio      = 1;
    s_cfg.audio_gain      = 2;
    // Filter ON by default. The CoCo's audio path band-limits what its
    // DAC produces, and reproducing the raw ladder is the less faithful
    // choice, not the neutral one.
    s_cfg.audio_filter    = 1;
    s_cfg.audio_onebit    = 3;

#if defined(MC_WIFI_SSID)
    strncpy(s_cfg.wifi.ssid, MC_WIFI_SSID, sizeof(s_cfg.wifi.ssid) - 1);
#endif
#if defined(MC_WIFI_PASSWORD)
    strncpy(s_cfg.wifi.psk,  MC_WIFI_PASSWORD, sizeof(s_cfg.wifi.psk) - 1);
#endif
    // MC_WIFI_AUTH from config_local.h is a CYW43_AUTH_* macro.
    // Fold it back into our enum where we recognise it.
#if defined(MC_WIFI_AUTH)
    switch (MC_WIFI_AUTH) {
        case CYW43_AUTH_OPEN:           s_cfg.wifi.auth = CFG_AUTH_OPEN;  break;
        case CYW43_AUTH_WPA2_AES_PSK:   s_cfg.wifi.auth = CFG_AUTH_WPA2;  break;
        case CYW43_AUTH_WPA2_MIXED_PSK: s_cfg.wifi.auth = CFG_AUTH_MIXED; break;
#ifdef CYW43_AUTH_WPA3_SAE_AES_PSK
        case CYW43_AUTH_WPA3_SAE_AES_PSK: s_cfg.wifi.auth = CFG_AUTH_WPA3; break;
#endif
        default: break;
    }
#endif
}

void cfg_init(void) {
    s_loaded_from_flash = false;
    if (flash_payload_valid(CFG_FLASH_ADDR)) {
        const cfg_header_t *h = (const cfg_header_t *)CFG_FLASH_ADDR;
        // Start from defaults, overlay what was stored, then put the
        // defaults back for every field the saved layout predates.
        // Overlaying wholesale would hand every existing user whatever
        // bytes happened to follow the old struct -- which silently
        // disables a feature rather than leaving it at its default.
        // test_cfg.c pins this for every version step.
        cfg_load_defaults();
        uint8_t af = s_cfg.audio_filter, ob = s_cfg.audio_onebit;
        uint8_t ha = s_cfg.hdmi_audio,   ag = s_cfg.audio_gain;
        s_cfg = h->payload;
        if (h->version < 3u) {
            s_cfg.audio_filter = af;
            s_cfg.audio_onebit = ob;
        }
        if (h->version < 2u) {
            s_cfg.hdmi_audio = ha;
            s_cfg.audio_gain = ag;
        }
        s_loaded_from_flash = true;
        printf("cfg: loaded from flash (ssid='%s', auth=%s)\n",
               s_cfg.wifi.ssid, cfg_auth_name(s_cfg.wifi.auth));
        return;
    }
    cfg_load_defaults();
    if (s_cfg.wifi.ssid[0]) {
        printf("cfg: flash empty — using config_local.h defaults "
               "(ssid='%s')\n", s_cfg.wifi.ssid);
    } else {
        printf("cfg: unconfigured — connect USB CDC and provision "
               "(any keystroke enters menu)\n");
    }
}

cfg_t *cfg_get(void) { return &s_cfg; }

bool cfg_is_usable(void) {
    return s_cfg.wifi.ssid[0] != 0;
}

bool cfg_loaded_from_flash(void) { return s_loaded_from_flash; }

uint32_t cfg_auth_to_cyw43(cfg_auth_t a) {
    switch (a) {
        case CFG_AUTH_OPEN:  return CYW43_AUTH_OPEN;
        case CFG_AUTH_WPA2:  return CYW43_AUTH_WPA2_AES_PSK;
        case CFG_AUTH_MIXED: return CYW43_AUTH_WPA2_MIXED_PSK;
#ifdef CYW43_AUTH_WPA3_SAE_AES_PSK
        case CFG_AUTH_WPA3:  return CYW43_AUTH_WPA3_SAE_AES_PSK;
#else
        case CFG_AUTH_WPA3:  return CYW43_AUTH_WPA2_AES_PSK;  // fallback
#endif
    }
    return CYW43_AUTH_WPA2_AES_PSK;
}

const char *cfg_auth_name(cfg_auth_t a) {
    switch (a) {
        case CFG_AUTH_OPEN:  return "OPEN";
        case CFG_AUTH_WPA2:  return "WPA2";
        case CFG_AUTH_WPA3:  return "WPA3";
        case CFG_AUTH_MIXED: return "MIXED";
    }
    return "?";
}

bool cfg_auth_parse(const char *s, cfg_auth_t *out) {
    if      (!strcasecmp(s, "OPEN"))  *out = CFG_AUTH_OPEN;
    else if (!strcasecmp(s, "WPA2"))  *out = CFG_AUTH_WPA2;
    else if (!strcasecmp(s, "WPA3"))  *out = CFG_AUTH_WPA3;
    else if (!strcasecmp(s, "MIXED")) *out = CFG_AUTH_MIXED;
    else return false;
    return true;
}

// ----------------------------------------------------------------------
// Save path.
//
// No flash_safe_execute() here: that parks core 1 through a lockout IRQ,
// and core 1 runs with interrupts disabled forever (bus.c), so it could
// never answer and every save would time out. The lockout is not needed
// at all: core 1 is provably XIP-free — its code is __not_in_flash_func
// and its data are SRAM arrays — so disabling interrupts on core 0 for
// the duration of one sector is sufficient and leaves the bus service
// running throughout.
// ----------------------------------------------------------------------

bool cfg_save(void) {
    uint8_t page[CFG_PAGE_BYTES];
    memset(page, 0xFF, sizeof(page));

    cfg_header_t *h = (cfg_header_t *)page;
    h->magic   = CFG_MAGIC;
    h->version = CFG_VERSION;
    h->payload = s_cfg;

    uint32_t crc = crc32(page, CFG_CRC_OFFSET);
    memcpy(page + CFG_CRC_OFFSET, &crc, sizeof(crc));

    uint32_t save = save_and_disable_interrupts();
    flash_range_erase  (CFG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CFG_FLASH_OFFSET, page, CFG_PAGE_BYTES);
    restore_interrupts(save);

    // flash_range_* cannot report failure, so read the sector back through
    // the no-cache alias and compare. Without this the bool return would be
    // a constant true and every "save failed" path in the UIs would be
    // unreachable decoration.
    const uint8_t *live = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE
                                            + CFG_FLASH_OFFSET);
    if (memcmp(live, page, CFG_PAGE_BYTES) != 0) return false;

    s_loaded_from_flash = true;
    return true;
}

// ----- wipe -----------------------------------------------------------

bool cfg_wipe(void) {
    uint32_t save = save_and_disable_interrupts();
    flash_range_erase(CFG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(save);

    s_loaded_from_flash = false;
    cfg_load_defaults();

    // Erased flash reads back 0xFF; anything else means the erase did not
    // take (see cfg_save).
    const uint8_t *live = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE
                                            + CFG_FLASH_OFFSET);
    for (uint32_t i = 0; i < CFG_PAGE_BYTES; i++)
        if (live[i] != 0xFF) return false;
    return true;
}
