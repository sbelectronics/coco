// ======================================================================
// ota.c — staged firmware update (see ota.h for the scheme and its one
// real risk).
// ======================================================================

#include "ota.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"

#include <stdio.h>
#include <string.h>

#define OTA_MAGIC       0x4F544143u          // "CATO" LE — CoCo cART OTA

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
    uint32_t reserved;
} ota_hdr_t;

#define HDR_OFFSET  (MC_FW_SLOT_B + MC_FW_SLOT_BYTES - FLASH_SECTOR_SIZE)
#define IMAGE_MAX   (MC_FW_SLOT_BYTES - FLASH_SECTOR_SIZE)

static bool     s_active;
static uint32_t s_expect, s_written;
static uint32_t s_crc;
static uint8_t  s_page[FLASH_PAGE_SIZE];
static uint32_t s_page_used;
static uint32_t s_erased;        // bytes of slot B erased so far
static bool     s_commit_pending;
static absolute_time_t s_commit_at;

static uint32_t s_last_rb;          // crc of the image as READ BACK
static uint32_t s_wire_crc;         // crc of the same bytes off the wire

static uint32_t crc32_up(uint32_t c, const uint8_t *d, uint32_t n) {
    c = ~c;
    for (uint32_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
    }
    return ~c;
}

static void erase_sector(uint32_t off) {
    uint32_t save = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    restore_interrupts(save);
}

static void prog_page(uint32_t off, const uint8_t *p) {
    uint32_t save = save_and_disable_interrupts();
    flash_range_program(off, p, FLASH_PAGE_SIZE);
    restore_interrupts(save);
}

// Erase just enough of slot B to cover `need` bytes of image.
static void erase_upto(uint32_t need) {
    while (s_erased < need && s_erased < MC_FW_SLOT_BYTES) {
        erase_sector(MC_FW_SLOT_B + s_erased);
        s_erased += FLASH_SECTOR_SIZE;
    }
}

// "Active" includes the post-200 commit window: a selection or a second
// OTA started in those 700 ms would either race the reboot or (worse)
// ota_begin's header erase would silently cancel the already-confirmed
// update.
bool ota_active(void) { return s_active || s_commit_pending; }

bool ota_begin(uint32_t len) {
    if (s_active || s_commit_pending || len == 0 || len > IMAGE_MAX)
        return false;

    // Invalidate any previously staged image, then erase lazily as the
    // upload advances — erasing all 768 KB here would stall the HTTP
    // connection for the better part of ten seconds.
    erase_sector(HDR_OFFSET);

    s_erased    = 0;
    s_expect    = len;
    s_written   = 0;
    s_crc       = 0;
    s_page_used = 0;
    s_active    = true;
    printf("ota: staging %u bytes\n", (unsigned)len);
    return true;
}

bool ota_chunk(const void *data, uint32_t len) {
    if (!s_active) return false;
    if (s_written + len > s_expect) return false;

    const uint8_t *p = (const uint8_t *)data;
    s_crc = crc32_up(s_crc, p, len);

    while (len) {
        uint32_t n = FLASH_PAGE_SIZE - s_page_used;
        if (n > len) n = len;
        memcpy(s_page + s_page_used, p, n);
        s_page_used += n;
        p += n; len -= n;

        if (s_page_used == FLASH_PAGE_SIZE) {
            erase_upto(s_written + FLASH_PAGE_SIZE);
            prog_page(MC_FW_SLOT_B + s_written, s_page);
            s_written += FLASH_PAGE_SIZE;
            s_page_used = 0;
        }
    }
    return true;
}

// The transport CRC only proves the bytes arrived intact — not that
// they are firmware, or firmware for THIS board. A wrong file here
// bricks the cart until BOOTSEL recovery, so sanity-check before the
// header write makes the staging real:
//   1. word 0 = initial stack pointer inside SRAM;
//   2. word 1 = thumb-bit reset vector inside slot A;
//   3. the image contains this board's MC_HOSTNAME string (a multicart
//      .bin uploaded to a videocart fails here, and vice versa).
static bool staged_image_plausible(uint32_t len) {
    const uint8_t *img =
        (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + MC_FW_SLOT_B);
    uint32_t sp, rv;
    memcpy(&sp, img, 4);
    memcpy(&rv, img + 4, 4);
    if (sp < 0x20000000u || sp > 0x20082000u) return false;
    if (!(rv & 1u) || rv < 0x10000000u ||
        rv >= 0x10000000u + MC_FW_SLOT_BYTES) return false;

    static const char tag[] = MC_HOSTNAME;
    const uint32_t tl = sizeof(tag) - 1;
    if (len < tl) return false;
    for (uint32_t i = 0; i + tl <= len; i++)
        if (img[i] == tag[0] && !memcmp(img + i, tag, tl)) return true;
    return false;
}

bool ota_end(void) {
    if (!s_active) return false;
    s_active = false;

    if (s_page_used) {
        memset(s_page + s_page_used, 0xFF, FLASH_PAGE_SIZE - s_page_used);
        erase_upto(s_written + FLASH_PAGE_SIZE);
        prog_page(MC_FW_SLOT_B + s_written, s_page);
        s_written += s_page_used;
        s_page_used = 0;
    }
    if (s_written != s_expect) {
        printf("ota: length mismatch (%u vs %u)\n",
               (unsigned)s_written, (unsigned)s_expect);
        return false;
    }
    if (!staged_image_plausible(s_expect)) {
        printf("ota: rejected — not a %s firmware image\n", MC_HOSTNAME);
        return false;
    }

    // Read the image BACK out of flash and re-CRC it before the header
    // makes the staging real. s_crc only proves the bytes arrived intact
    // over the wire; this proves they actually landed. Without it a flash
    // that did not take looks like a clean upload, and the failure
    // reappears later as a silent no-op at boot with the web page still
    // reporting success.
    //
    // REPORTS, never rejects: a read-back check that is itself wrong
    // would refuse every upload, a worse failure than the one it is here
    // to catch. Make it blocking once the panel has shown it agreeing.
    //
    // NOCACHE: these bytes were written microseconds ago and the XIP
    // cache may still hold what was there before the erase.
    {
        const uint8_t *back =
            (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + MC_FW_SLOT_B);
        uint32_t rb = crc32_up(0, back, s_expect);
        if (rb != s_crc)
            printf("ota: flash read-back mismatch (wire %08x, flash %08x)\n",
                   (unsigned)s_crc, (unsigned)rb);
        s_last_rb  = rb;
        s_wire_crc = s_crc;
    }

    ota_hdr_t h = { OTA_MAGIC, s_expect, s_crc, 0 };
    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    memcpy(page, &h, sizeof(h));
    prog_page(HDR_OFFSET, page);

    printf("ota: staged %u bytes, crc %08x\n",
           (unsigned)s_expect, (unsigned)s_crc);
    return true;
}

void ota_abort(void) {
    if (!s_active) return;
    s_active = false;
    erase_sector(HDR_OFFSET);       // no valid header -> nothing pending
}

void ota_commit_after_response(void) {
    s_commit_pending = true;
    s_commit_at = make_timeout_time_ms(700);   // let the 200 flush first
}

void ota_pump(void) {
    if (!s_commit_pending) return;
    if (absolute_time_diff_us(get_absolute_time(), s_commit_at) > 0) return;
    printf("ota: rebooting into copier\n");
    watchdog_reboot(0, 0, 50);
    for (;;) tight_loop_contents();
}

// ----------------------------------------------------------------------
// Boot-time apply. Everything from here down runs with interrupts off
// and must not touch XIP — hence __not_in_flash_func on the copier.
// ----------------------------------------------------------------------

static bool staged_valid(const ota_hdr_t *h) {
    if (h->magic != OTA_MAGIC) return false;
    if (h->len == 0 || h->len > IMAGE_MAX) return false;
    const uint8_t *img = (const uint8_t *)(XIP_BASE + MC_FW_SLOT_B);
    return crc32_up(0, img, h->len) == h->crc;
}

// THIS FUNCTION NEVER RETURNS, AND NOTHING IT TOUCHES MAY LIVE IN FLASH.
// It erases slot A — the code that called it — so the instant the first
// sector goes, every flash address is gone: our return address, any libc
// helper, even the long-branch veneers the linker would insert for a
// flash->RAM call.
//
// Two compiler behaviours actively fight this and BOTH must be blocked
// (verified in the disassembly of the linked image):
//   1. __not_in_flash_func alone does not stop GCC inlining a static
//      function into a flash-resident caller, which silently relocates
//      the whole thing back into flash. Hence __no_inline_not_in_flash_func.
//   2. GCC recognises byte-fill/copy loops and rewrites them as calls to
//      memset/memcpy, which are in flash. A volatile destination pointer
//      prevents that pattern match.
static void __no_inline_not_in_flash_func(copy_b_to_a)(uint32_t len) {
    static uint8_t sect[FLASH_SECTOR_SIZE];
    volatile uint8_t *dst = sect;          // volatile: no memset/memcpy

    uint32_t done = 0;
    while (done < len) {
        uint32_t n = len - done;
        if (n > FLASH_SECTOR_SIZE) n = FLASH_SECTOR_SIZE;

        const volatile uint8_t *src =
            (const volatile uint8_t *)(XIP_NOCACHE_NOALLOC_BASE
                                       + MC_FW_SLOT_B + done);
        for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
        for (uint32_t i = n; i < FLASH_SECTOR_SIZE; i++) dst[i] = 0xFF;

        flash_range_erase(MC_FW_SLOT_A + done, FLASH_SECTOR_SIZE);
        flash_range_program(MC_FW_SLOT_A + done, sect, FLASH_SECTOR_SIZE);
        done += FLASH_SECTOR_SIZE;
    }
    // Invalidate the staged header so we don't loop on the next boot.
    flash_range_erase(HDR_OFFSET, FLASH_SECTOR_SIZE);

    // Raw watchdog trigger — RAM-safe reboot into the new image.
    watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;
    for (;;) { __asm volatile ("nop"); }
}

// Why the last boot did not apply a staged image. Lives in plain RAM on
// purpose: it is set before cfg or the filesystem exist, and it only has
// to survive until the panel is fetched. The whole point is that
// "nothing happened" stops being an unreadable outcome.
static uint8_t  s_why;          // ota_why_t
static uint32_t s_why_len, s_why_hcrc, s_why_icrc;

uint8_t  ota_last_apply(void) { return s_why; }
uint32_t ota_staged_len(void) { return s_why_len; }
uint32_t ota_staged_crc(void) { return s_why_hcrc; }
uint32_t ota_image_crc(void)  { return s_why_icrc; }
uint32_t ota_readback_crc(void) { return s_last_rb; }
uint32_t ota_wire_crc(void)     { return s_wire_crc; }

void ota_apply_pending_if_any(void) {
    const ota_hdr_t *h = (const ota_hdr_t *)(XIP_BASE + HDR_OFFSET);

    // Snapshot the header and the image's ACTUAL crc first, so the panel
    // can show both halves of the comparison that decided this.
    if (h->magic != OTA_MAGIC) { s_why = OTA_WHY_NONE; return; }
    s_why_len  = h->len;
    s_why_hcrc = h->crc;
    if (h->len == 0 || h->len > IMAGE_MAX) { s_why = OTA_WHY_BADLEN; return; }
    s_why_icrc = crc32_up(0, (const uint8_t *)(XIP_BASE + MC_FW_SLOT_B),
                          h->len);

    if (!staged_valid(h)) { s_why = OTA_WHY_CRC; return; }

    // Already running this image? (Same length and CRC in slot A.)
    const uint8_t *a = (const uint8_t *)(XIP_BASE + MC_FW_SLOT_A);
    if (crc32_up(0, a, h->len) == h->crc) {
        uint32_t save = save_and_disable_interrupts();
        flash_range_erase(HDR_OFFSET, FLASH_SECTOR_SIZE);
        restore_interrupts(save);
        printf("ota: staged image already active\n");
        s_why = OTA_WHY_SAME;
        return;
    }

    printf("ota: applying staged image (%u bytes) — DO NOT POWER OFF\n",
           (unsigned)h->len);
    sleep_ms(50);                       // let the message reach USB CDC

    s_why = OTA_WHY_APPLIED;
    uint32_t len = h->len;
    (void)save_and_disable_interrupts();
    copy_b_to_a(len);                   // never returns (reboots from RAM)
    __builtin_unreachable();
}
