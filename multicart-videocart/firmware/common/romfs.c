// ======================================================================
// romfs.c — LittleFS on the flash data partition + the ROM catalog.
//
// Flash discipline: erase/program run from RAM (the SDK's
// flash_range_* are __not_in_flash_func) with interrupts off for exactly
// one sector/page each, so lwIP only ever sees a sub-100 ms hiccup and
// core 1 — which never touches XIP — keeps serving the bus throughout.
// ======================================================================

#include "romfs.h"
#include "config.h"
#include "bus.h"

#include "lfs.h"

#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdlib.h>

#define CATALOG_PATH   "catalog.txt"

// ---------------------------------------------------------------- bd --

static int bd_read(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, void *buffer, lfs_size_t size) {
    (void)c;
    // MUST go through the no-cache alias: flash_range_program does not
    // invalidate the XIP cache, so a cached read here could return the
    // pre-write contents of a block we just programmed — silent LittleFS
    // metadata corruption.
    const uint8_t *src = (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE
                                           + MC_FS_FLASH_OFFSET
                                           + block * FLASH_SECTOR_SIZE + off);
    memcpy(buffer, src, size);
    return 0;
}

static int bd_prog(const struct lfs_config *c, lfs_block_t block,
                   lfs_off_t off, const void *buffer, lfs_size_t size) {
    (void)c;
    uint32_t addr = MC_FS_FLASH_OFFSET + block * FLASH_SECTOR_SIZE + off;
    // One page at a time, IRQs off only for that page (~1 ms).
    const uint8_t *p = (const uint8_t *)buffer;
    while (size) {
        uint32_t n = size > FLASH_PAGE_SIZE ? FLASH_PAGE_SIZE : size;
        uint32_t save = save_and_disable_interrupts();
        flash_range_program(addr, p, n);
        restore_interrupts(save);
        addr += n; p += n; size -= n;
    }
    return 0;
}

static int bd_erase(const struct lfs_config *c, lfs_block_t block) {
    (void)c;
    uint32_t addr = MC_FS_FLASH_OFFSET + block * FLASH_SECTOR_SIZE;
    uint32_t save = save_and_disable_interrupts();
    flash_range_erase(addr, FLASH_SECTOR_SIZE);
    restore_interrupts(save);
    return 0;
}

static int bd_sync(const struct lfs_config *c) { (void)c; return 0; }

static uint8_t s_read_buf[512];
static uint8_t s_prog_buf[512];
static uint32_t s_lookahead[8] __attribute__((aligned(8)));   // 32 bytes

static const struct lfs_config s_lfs_cfg = {
    .read  = bd_read,
    .prog  = bd_prog,
    .erase = bd_erase,
    .sync  = bd_sync,

    .read_size      = 1,
    .prog_size      = FLASH_PAGE_SIZE,
    .block_size     = FLASH_SECTOR_SIZE,
    .block_count    = MC_FS_FLASH_BYTES / FLASH_SECTOR_SIZE,
    .block_cycles   = 500,
    .cache_size     = 512,
    .lookahead_size = 32,

    .read_buffer      = s_read_buf,
    .prog_buffer      = s_prog_buf,
    .lookahead_buffer = s_lookahead,
};

static lfs_t   s_lfs;
static bool    s_mounted;

// Upload-in-flight state (used by romfs_delete's open-file guard too).
static lfs_file_t s_upf;
static bool       s_up_open;
static char       s_up_name[ROMFS_NAME_MAX];

// Static file caches (no malloc).
static uint8_t s_file_buf[512];
static struct lfs_file_config s_file_cfg = { .buffer = s_file_buf };
static uint8_t s_up_buf[512];
static struct lfs_file_config s_up_cfg   = { .buffer = s_up_buf };

// ------------------------------------------------------------ catalog --

static rom_entry_t s_roms[ROMFS_MAX_ROMS];
static int         s_nroms;

static void default_title(const char *file, char *out, size_t outsz) {
    size_t n = 0;
    for (const char *p = file; *p && n + 1 < outsz; p++) {
        if (*p == '.') break;                 // drop extension
        char ch = *p;
        if (ch == '_' || ch == '-') ch = ' ';
        out[n++] = (char)toupper((unsigned char)ch);
    }
    out[n] = 0;
    if (!n) snprintf(out, outsz, "%s", file);
}

static int cmp_entry(const void *a, const void *b) {
    const rom_entry_t *x = (const rom_entry_t *)a;
    const rom_entry_t *y = (const rom_entry_t *)b;
    return strcasecmp(x->title, y->title);
}

static void apply_catalog(void) {
    lfs_file_t f;
    if (lfs_file_opencfg(&s_lfs, &f, CATALOG_PATH, LFS_O_RDONLY,
                         &s_file_cfg) < 0)
        return;

    char line[160];
    int  li = 0;
    bool overlong = false;      // current line exceeded the buffer
    for (;;) {
        char ch;
        lfs_ssize_t r = lfs_file_read(&s_lfs, &f, &ch, 1);
        bool eof = (r <= 0);
        if (!eof && ch != '\n') {
            // Keep consuming an over-long line to the newline rather than
            // parsing the fragment: splitting it would invent a second,
            // bogus record out of the tail.
            if (li < (int)sizeof(line) - 1) line[li++] = ch;
            else                            overlong = true;
            continue;
        }
        line[li] = 0;
        if (li && !overlong) {
            // file|title|autostart|scheme|insert
            char *p = line;
            char *fld[5] = { p, NULL, NULL, NULL, NULL };
            int nf = 1;
            for (; *p && nf < 5; p++) {
                if (*p == '|') { *p = 0; fld[nf++] = p + 1; }
            }
            if (nf >= 2) {
                for (int i = 0; i < s_nroms; i++) {
                    if (strcmp(s_roms[i].file, fld[0])) continue;
                    if (fld[1] && fld[1][0])
                        snprintf(s_roms[i].title, ROMFS_TITLE_MAX, "%s", fld[1]);
                    if (nf >= 3 && fld[2])
                        s_roms[i].autostart = (uint8_t)(atoi(fld[2]) != 0);
                    if (nf >= 4 && fld[3])
                        s_roms[i].scheme = (uint8_t)atoi(fld[3]);
                    if (nf >= 5 && fld[4])
                        s_roms[i].insert = (uint8_t)(atoi(fld[4]) != 0);
                    break;
                }
            }
        }
        li = 0;
        overlong = false;
        if (eof) break;
    }
    lfs_file_close(&s_lfs, &f);
}

static bool write_catalog(void) {
    if (!s_mounted) return false;
    lfs_file_t f;
    if (lfs_file_opencfg(&s_lfs, &f, CATALOG_PATH,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                         &s_file_cfg) < 0)
        return false;
    char line[160];
    for (int i = 0; i < s_nroms; i++) {
        int n = snprintf(line, sizeof(line), "%s|%s|%u|%u|%u\n",
                         s_roms[i].file, s_roms[i].title,
                         s_roms[i].autostart, s_roms[i].scheme,
                         s_roms[i].insert);
        // snprintf returns the length it WANTED; clamp so a future field
        // (or a longer ROMFS_NAME_MAX) can never write past the buffer.
        if (n < 0) continue;
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        if (lfs_file_write(&s_lfs, &f, line, n) < 0) {
            lfs_file_close(&s_lfs, &f);
            return false;
        }
    }
    lfs_file_close(&s_lfs, &f);
    return true;
}

void romfs_rescan(void) {
    s_nroms = 0;
    if (!s_mounted) return;

    lfs_dir_t dir;
    if (lfs_dir_open(&s_lfs, &dir, "/") < 0) return;

    struct lfs_info info;
    while (lfs_dir_read(&s_lfs, &dir, &info) > 0) {
        if (info.type != LFS_TYPE_REG) continue;
        if (!strcmp(info.name, CATALOG_PATH)) continue;
        if (s_nroms >= ROMFS_MAX_ROMS) break;
        // A name that doesn't fit would be truncated into the entry and
        // then never match the real file again — an un-loadable,
        // un-deletable ghost. The upload API can't create these, but a
        // filesystem written elsewhere could.
        if (strlen(info.name) >= ROMFS_NAME_MAX) {
            printf("romfs: skipping over-long name '%s'\n", info.name);
            continue;
        }

        rom_entry_t *e = &s_roms[s_nroms++];
        memset(e, 0, sizeof(*e));
        snprintf(e->file, ROMFS_NAME_MAX, "%s", info.name);
        default_title(info.name, e->title, ROMFS_TITLE_MAX);
        e->size      = info.size;
        e->autostart = 1;                                  // default ON
        e->scheme    = (info.size > MC_BANK_BYTES) ? BUS_SCHEME_BANK16K
                                                   : BUS_SCHEME_FLAT;
        e->insert    = 0;

    }
    lfs_dir_close(&s_lfs, &dir);

    apply_catalog();
    if (s_nroms > 1)
        qsort(s_roms, s_nroms, sizeof(s_roms[0]), cmp_entry);
}

bool romfs_mount(void) {
    int err = lfs_mount(&s_lfs, &s_lfs_cfg);
    if (err) {
        printf("romfs: mount failed (%d) — formatting\n", err);
        err = lfs_format(&s_lfs, &s_lfs_cfg);
        if (!err) err = lfs_mount(&s_lfs, &s_lfs_cfg);
        if (err) { printf("romfs: format failed (%d)\n", err); return false; }
    }
    s_mounted = true;
    romfs_rescan();
    printf("romfs: mounted, %d rom(s), %u KB free\n",
           s_nroms, (unsigned)(romfs_free_bytes() / 1024));
    return true;
}

bool romfs_mounted(void) { return s_mounted; }
int  romfs_count(void)   { return s_nroms; }

const rom_entry_t *romfs_entry(int idx) {
    if (idx < 0 || idx >= s_nroms) return NULL;
    return &s_roms[idx];
}

int romfs_index_of(const char *file) {
    for (int i = 0; i < s_nroms; i++)
        if (!strcmp(s_roms[i].file, file)) return i;
    return -1;
}

const rom_entry_t *romfs_find(const char *file) {
    int i = romfs_index_of(file);
    return (i < 0) ? NULL : &s_roms[i];
}

int32_t romfs_load(const char *file, uint8_t *dst, uint32_t max) {
    if (!s_mounted) return -1;
    lfs_file_t f;
    if (lfs_file_opencfg(&s_lfs, &f, file, LFS_O_RDONLY, &s_file_cfg) < 0)
        return -1;
    lfs_ssize_t n = lfs_file_read(&s_lfs, &f, dst, max);
    lfs_file_close(&s_lfs, &f);
    return (int32_t)n;
}

bool romfs_delete(const char *file) {
    if (!s_mounted) return false;
    if (!strcmp(file, CATALOG_PATH)) return false;   // not via the API
    // Removing a file littlefs has open (an in-flight upload) is
    // undefined; make the caller finish or abort the upload first.
    if (s_up_open && !strcmp(file, s_up_name)) return false;
    if (lfs_remove(&s_lfs, file) < 0) return false;
    romfs_rescan();
    write_catalog();
    return true;
}

bool romfs_set_meta(const char *file, const char *title,
                    int autostart, int scheme, int insert) {
    int i = romfs_index_of(file);
    if (i < 0) return false;
    if (title && title[0]) {
        snprintf(s_roms[i].title, ROMFS_TITLE_MAX, "%s", title);
        // Titles are emitted into JSON unescaped and stored '|'-delimited,
        // '\n'-terminated in the catalog: neutralise the characters that
        // break either — including control chars (a raw newline in a
        // title would corrupt the catalog AND make every /api/roms
        // response unparseable, bricking the web UI).
        for (char *p = s_roms[i].title; *p; p++)
            if (*p == '"' || *p == '\\' || *p == '|' ||
                (unsigned char)*p < 0x20) *p = '\'';
    }
    if (autostart >= 0)    s_roms[i].autostart = (uint8_t)(autostart != 0);
    if (scheme >= 0)       s_roms[i].scheme    = (uint8_t)scheme;
    if (insert >= 0)       s_roms[i].insert    = (uint8_t)(insert != 0);
    bool ok = write_catalog();
    if (s_nroms > 1) qsort(s_roms, s_nroms, sizeof(s_roms[0]), cmp_entry);
    return ok;
}

// ------------------------------------------------------------- upload --

bool romfs_upload_active(void) { return s_up_open; }

bool romfs_upload_begin(const char *file) {
    if (!s_mounted || s_up_open) return false;
    if (!strcmp(file, CATALOG_PATH)) return false;   // not via the API
    snprintf(s_up_name, sizeof(s_up_name), "%s", file);
    if (lfs_file_opencfg(&s_lfs, &s_upf, s_up_name,
                         LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC,
                         &s_up_cfg) < 0)
        return false;
    s_up_open = true;
    return true;
}

bool romfs_upload_chunk(const void *data, uint32_t len) {
    if (!s_up_open) return false;
    return lfs_file_write(&s_lfs, &s_upf, data, len) == (lfs_ssize_t)len;
}

bool romfs_upload_end(void) {
    if (!s_up_open) return false;
    int r = lfs_file_close(&s_lfs, &s_upf);
    s_up_open = false;
    romfs_rescan();
    write_catalog();
    return r >= 0;
}

void romfs_upload_abort(void) {
    if (!s_up_open) return;
    lfs_file_close(&s_lfs, &s_upf);
    s_up_open = false;
    lfs_remove(&s_lfs, s_up_name);
    romfs_rescan();
}

uint32_t romfs_total_bytes(void) { return MC_FS_FLASH_BYTES; }

uint32_t romfs_free_bytes(void) {
    if (!s_mounted) return 0;
    lfs_ssize_t used = lfs_fs_size(&s_lfs);
    if (used < 0) return 0;
    uint32_t total_blocks = MC_FS_FLASH_BYTES / FLASH_SECTOR_SIZE;
    if ((uint32_t)used > total_blocks) return 0;
    return (total_blocks - (uint32_t)used) * FLASH_SECTOR_SIZE;
}
