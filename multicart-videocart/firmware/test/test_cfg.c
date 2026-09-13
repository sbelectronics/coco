// ======================================================================
// test_cfg.c — config persistence across a VERSION BUMP.
//
// Adding a field to cfg_t without raising CFG_VERSION lets a stale page
// be accepted and copied over the defaults wholesale, so the new field
// reads back as 0 -- which for hdmi_audio silently selects DVI and leaves
// the entire audio path compiled in but never executed.
//
// The rule every version must keep: a page saved by an OLDER firmware
// keeps every value it stored, and every field that version predates
// comes back at its DEFAULT -- never zero by accident, never whatever
// bytes happened to follow the old struct.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "config.h"
#include "cfg.h"
#include "hardware/flash.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
} while (0)

#include "cfg.c"

// Write a page exactly as a firmware of that version would have: payload
// truncated to the size cfg_t was back then, everything after it zero.
static void write_old_page(uint32_t ver, const cfg_t *payload, size_t bytes) {
    uint8_t page[CFG_PAGE_BYTES];
    memset(page, 0xFF, sizeof page);
    cfg_header_t *h = (cfg_header_t *)page;
    h->magic   = CFG_MAGIC;
    h->version = ver;
    memset(&h->payload, 0, sizeof h->payload);
    memcpy(&h->payload, payload, bytes);
    uint32_t crc = crc32(page, CFG_CRC_OFFSET);
    memcpy(page + CFG_CRC_OFFSET, &crc, sizeof crc);
    flash_range_erase(CFG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CFG_FLASH_OFFSET, page, CFG_PAGE_BYTES);
}

int main(void) {
    cfg_load_defaults();
    cfg_t def = *cfg_get();
    CHECK(def.hdmi_audio == 1, "default hdmi_audio is %u, want 1",
          def.hdmi_audio);

    // ---- a version-2 page: no audio_filter, no audio_onebit ---------
    {
        cfg_t old = def;
        old.strobe_delay_ns = 555;
        old.artifact        = 1;
        old.hdmi_audio      = 0;      // deliberately NOT the default
        old.audio_gain      = 3;
        old.audio_filter    = 0xAA;   // garbage past the end of v2's struct
        old.audio_onebit    = 0xAA;
        write_old_page(2u, &old, offsetof(cfg_t, audio_filter));

        cfg_init();
        const cfg_t *c = cfg_get();
        CHECK(cfg_loaded_from_flash(), "v2 page was not accepted");
        CHECK(c->strobe_delay_ns == 555, "v2: strobe lost (%u)",
              (unsigned)c->strobe_delay_ns);
        CHECK(c->artifact == 1, "v2: artifact lost (%u)", c->artifact);
        CHECK(c->hdmi_audio == 0, "v2: hdmi_audio lost (%u)", c->hdmi_audio);
        CHECK(c->audio_gain == 3, "v2: audio_gain lost (%u)", c->audio_gain);
        // The fields v2 never had must come back as DEFAULTS, not as the
        // 0xAA sitting past the old struct. That is the exact bug shape.
        CHECK(c->audio_filter == def.audio_filter,
              "v2: audio_filter came back as %u, want the default %u",
              c->audio_filter, def.audio_filter);
        CHECK(c->audio_onebit == def.audio_onebit,
              "v2: audio_onebit came back as %u, want the default %u",
              c->audio_onebit, def.audio_onebit);
    }

    // ---- a version-1 page: no audio fields at all -------------------
    {
        cfg_t old = def;
        old.strobe_delay_ns = 777;
        old.hdmi_audio      = 0xAA;
        old.audio_gain      = 0xAA;
        old.audio_filter    = 0xAA;
        old.audio_onebit    = 0xAA;
        write_old_page(1u, &old, offsetof(cfg_t, hdmi_audio));

        cfg_init();
        const cfg_t *c = cfg_get();
        CHECK(c->strobe_delay_ns == 777, "v1: strobe lost (%u)",
              (unsigned)c->strobe_delay_ns);
        CHECK(c->hdmi_audio == def.hdmi_audio,
              "v1: hdmi_audio came back as %u, want the default %u -- this "
              "silently disables the whole audio path",
              c->hdmi_audio, def.hdmi_audio);
        CHECK(c->audio_gain   == def.audio_gain,   "v1: audio_gain default");
        CHECK(c->audio_filter == def.audio_filter, "v1: audio_filter default");
        CHECK(c->audio_onebit == def.audio_onebit, "v1: audio_onebit default");
    }

    // ---- a page from a FUTURE version must be refused ---------------
    {
        cfg_t old = def;
        old.strobe_delay_ns = 999;
        write_old_page(CFG_VERSION + 1u, &old, sizeof old);
        cfg_init();
        CHECK(!cfg_loaded_from_flash(),
              "a page newer than this firmware was accepted; its layout is "
              "unknown and reading it is guesswork");
        CHECK(cfg_get()->strobe_delay_ns == def.strobe_delay_ns,
              "a refused page still changed the config");
    }

    // ---- a round trip at the CURRENT version keeps everything -------
    {
        cfg_load_defaults();
        cfg_get()->audio_filter = 2;
        cfg_get()->audio_onebit = 1;
        cfg_get()->audio_gain   = 3;
        CHECK(cfg_save(), "save failed");
        cfg_init();
        const cfg_t *c = cfg_get();
        CHECK(c->audio_filter == 2 && c->audio_onebit == 1 &&
              c->audio_gain == 3,
              "round trip lost a value (filter %u, onebit %u, gain %u)",
              c->audio_filter, c->audio_onebit, c->audio_gain);
    }

    if (failures) { printf("test_cfg: %d FAILURES\n", failures); return 1; }
    printf("test_cfg: OK (v1 and v2 pages migrate, new fields get defaults "
           "not stale bytes, future versions refused, round trip)\n");
    return 0;
}
