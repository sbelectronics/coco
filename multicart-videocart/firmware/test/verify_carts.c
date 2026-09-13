// ======================================================================
// verify_carts — mount an mklfs-built partition image with the FIRMWARE's
// real romfs.c and prove it is what the menu/web will actually show.
//
//   verify_carts <fs.bin> [expected_count]
//
// Loads the raw partition image into fake flash at MC_FS_FLASH_OFFSET,
// mounts it through the shipping romfs code, lists every cart, and reads
// each one back in full (data integrity, not just a directory entry).
// Exit non-zero on any failure. Used by `make carts-selftest`, and handy
// for eyeballing a real carts/ build before flashing.
// ======================================================================

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/flash.h"

uint64_t shim_now_us;

#include "romfs.c"          // the shipping filesystem code

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: verify_carts <fs.bin> [count]\n"); return 2; }
    int expect = (argc >= 3) ? atoi(argv[2]) : -1;

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    size_t n = fread(shim_flash + MC_FS_FLASH_OFFSET, 1, MC_FS_FLASH_BYTES, f);
    fclose(f);
    printf("loaded %zu KB partition image\n", n / 1024);

    if (!romfs_mount()) { printf("VERIFY: FAIL — romfs_mount() failed\n"); return 1; }

    int bad = 0, count = romfs_count();
    printf("romfs mounted %d cart(s):\n", count);

    static uint8_t buf[256 * 1024];
    for (int i = 0; i < count; i++) {
        const rom_entry_t *e = romfs_entry(i);
        int32_t got = romfs_load(e->file, buf, sizeof(buf));
        int ok = (got == (int32_t)e->size);
        printf("  %-24s title='%s' auto=%u scheme=%u size=%u  read=%d %s\n",
               e->file, e->title, e->autostart, e->scheme,
               (unsigned)e->size, got, ok ? "" : "<-- READBACK MISMATCH");
        if (!ok) bad = 1;
        if (strchr(e->title, '|') || strchr(e->title, '"')) {
            printf("    <-- unsanitized title char\n"); bad = 1;
        }
    }

    if (expect >= 0 && count != expect) {
        printf("VERIFY: FAIL — expected %d carts, got %d\n", expect, count);
        bad = 1;
    }
    printf(bad ? "VERIFY: FAIL\n"
               : "VERIFY: OK (firmware romfs mounts the baked image)\n");
    return bad;
}
