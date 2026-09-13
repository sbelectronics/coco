// ======================================================================
// test_romfs.c — the real romfs.c + real LittleFS running over the fake
// NOR flash (program-can-only-clear-bits semantics, so any
// program-without-erase bug fails here, not on the bench).
//
// Exercises: format-on-first-mount, chunked upload, catalog defaults
// and persistence across "reboot" (unmount/remount), metadata editing
// with hostile titles, the delete guards, upload abort, free space, the
// over-long-filename ghost guard, and a full-ish filesystem.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"

uint64_t shim_now_us;

#include "romfs.c"          // code under test (statics visible)

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); putchar('\n'); } \
} while (0)

static void upload(const char *name, const uint8_t *data, uint32_t len) {
    CHECK(romfs_upload_begin(name), "upload_begin('%s')", name);
    uint32_t off = 0;
    while (off < len) {                      // odd chunking on purpose
        uint32_t n = (len - off > 777) ? 777 : (len - off);
        CHECK(romfs_upload_chunk(data + off, n), "chunk %s@%u", name, off);
        off += n;
    }
    CHECK(romfs_upload_end(), "upload_end('%s')", name);
}

static void reboot(void) {                   // simulate a power cycle
    memset(&s_lfs, 0, sizeof(s_lfs));
    s_mounted = false;
    s_nroms = 0;
    CHECK(romfs_mount(), "remount after reboot");
}

int main(void) {
    memset(shim_flash, 0x00, sizeof(shim_flash));   // worst case: not erased

    // ---- first mount formats ----
    CHECK(romfs_mount(), "initial mount/format");
    CHECK(romfs_count() == 0, "fresh fs not empty");
    uint32_t free0 = romfs_free_bytes();
    CHECK(free0 > 2000 * 1024, "fresh free %u too small", free0);

    // ---- uploads ----
    static uint8_t rom_a[16384], rom_b[4096], rom_c[40000];
    for (unsigned i = 0; i < sizeof(rom_a); i++) rom_a[i] = (uint8_t)(i * 7);
    for (unsigned i = 0; i < sizeof(rom_b); i++) rom_b[i] = (uint8_t)(i ^ 0x5A);
    for (unsigned i = 0; i < sizeof(rom_c); i++) rom_c[i] = (uint8_t)(i >> 3);

    upload("zaxxon.rom", rom_a, sizeof(rom_a));
    upload("mega_game(1).ccc", rom_c, sizeof(rom_c));
    upload("tiny-util.bin", rom_b, sizeof(rom_b));
    CHECK(romfs_count() == 3, "count %d != 3", romfs_count());

    // Default titles: uppercased, extension dropped, -/_ to space; the
    // list is title-sorted.
    const rom_entry_t *e = romfs_find("mega_game(1).ccc");
    CHECK(e && !strcmp(e->title, "MEGA GAME(1)"), "default title '%s'",
          e ? e->title : "?");
    CHECK(e && e->autostart == 1, "autostart default not ON");
    CHECK(e && e->scheme == 1, "40000-byte ROM not marked banked");
    e = romfs_find("zaxxon.rom");
    CHECK(e && e->scheme == 0, "16K ROM wrongly banked");

    // ---- read back ----
    static uint8_t buf[65536];
    int32_t n = romfs_load("zaxxon.rom", buf, sizeof(buf));
    CHECK(n == (int32_t)sizeof(rom_a), "load size %d", (int)n);
    CHECK(!memcmp(buf, rom_a, sizeof(rom_a)), "zaxxon content mismatch");

    // ---- metadata: hostile title is neutralised everywhere ----
    CHECK(romfs_set_meta("tiny-util.bin", "Bad\"ti|tle\nX", 0, -1, 1),
          "set_meta");
    e = romfs_find("tiny-util.bin");
    CHECK(e && !strchr(e->title, '"') && !strchr(e->title, '|') &&
          !strchr(e->title, '\n'),
          "hostile chars survived: '%s'", e ? e->title : "?");
    CHECK(e && e->autostart == 0, "autostart not cleared");
    CHECK(e && e->insert == 1, "insert flag not set");

    // ---- persistence across reboot ----
    reboot();
    CHECK(romfs_count() == 3, "post-reboot count %d", romfs_count());
    e = romfs_find("tiny-util.bin");
    CHECK(e && e->autostart == 0, "autostart didn't persist");
    CHECK(e && e->insert == 1, "insert flag didn't persist");
    CHECK(e && !strncmp(e->title, "Bad'ti'tle'X", 5), "title didn't persist "
          "('%s')", e ? e->title : "?");
    n = romfs_load("mega_game(1).ccc", buf, sizeof(buf));
    CHECK(n == (int32_t)sizeof(rom_c) && !memcmp(buf, rom_c, sizeof(rom_c)),
          "mega content lost across reboot");

    // ---- delete guards ----
    CHECK(!romfs_delete("catalog.txt"), "catalog deletable via API!");
    CHECK(romfs_upload_begin("partial.rom"), "begin partial");
    CHECK(romfs_upload_chunk(rom_b, 100), "chunk partial");
    CHECK(!romfs_delete("partial.rom"), "open upload deletable!");
    romfs_upload_abort();
    CHECK(romfs_find("partial.rom") == NULL, "aborted upload left a file");
    CHECK(romfs_count() == 3, "count after abort %d", romfs_count());

    CHECK(romfs_delete("tiny-util.bin"), "delete");
    CHECK(romfs_count() == 2 && !romfs_find("tiny-util.bin"), "delete stale");
    reboot();
    CHECK(romfs_count() == 2, "deleted file resurrected");

    // ---- over-long filename ghost guard ----
    {
        char longname[ROMFS_NAME_MAX + 20];
        memset(longname, 'x', sizeof(longname) - 1);
        longname[sizeof(longname) - 1] = 0;
        memcpy(longname + sizeof(longname) - 5, ".rom", 5);
        lfs_file_t f;                        // written "elsewhere"
        CHECK(lfs_file_opencfg(&s_lfs, &f, longname,
                               LFS_O_WRONLY | LFS_O_CREAT, &s_file_cfg) >= 0,
              "create long name");
        lfs_file_write(&s_lfs, &f, "hi", 2);
        lfs_file_close(&s_lfs, &f);
        romfs_rescan();
        CHECK(romfs_count() == 2, "over-long name entered catalog");
        lfs_remove(&s_lfs, longname);
        romfs_rescan();
    }

    // ---- free space accounting ----
    uint32_t free1 = romfs_free_bytes();
    CHECK(free1 < free0, "free space didn't shrink");
    CHECK(romfs_total_bytes() == MC_FS_FLASH_BYTES, "total bytes");

    // ---- flash discipline: no program-without-erase happened ----
    // (the AND-semantics fake would have corrupted littlefs and some
    // check above would have failed; make it explicit)
    n = romfs_load("zaxxon.rom", buf, sizeof(buf));
    CHECK(n == (int32_t)sizeof(rom_a) && !memcmp(buf, rom_a, sizeof(rom_a)),
          "flash-semantics corruption");

    printf("stats: %u sector erases, %u page programs\n",
           shim_flash_erases, shim_flash_progs);

    if (failures) { printf("test_romfs: %d FAILURES\n", failures); return 1; }
    printf("test_romfs: OK (mount/format, uploads, catalog persistence, "
           "hostile titles, delete guards, abort, ghost guard)\n");
    return 0;
}
