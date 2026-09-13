// ======================================================================
// mklfs — bake ROMs into a LittleFS image the firmware can mount as-is.
//
// Built from the SAME vendored lib/littlefs/lfs.c the firmware uses, so
// the on-disk format is guaranteed compatible — no version drift. The
// geometry (partition offset/size, block/page size) comes from the
// board's config.h + the flash shim, so there is ONE source of truth.
//
// Output is a UF2 targeting the filesystem partition (0x10000000 +
// MC_FS_FLASH_OFFSET). Flashing it does NOT touch the firmware slot or
// the cfg sector — reflash carts without reprovisioning WiFi.
//
//   mklfs --carts-dir DIR [--conf FILE] --family 0xNNNNNNNN --out OUT.uf2
//
// Entry selection:
//   * with --conf: each non-comment line is
//         filename [| title [| autostart(0/1) [| scheme(flat|bank16k|auto)
//                   [| insert(0/1) ]]]]
//     filename resolved relative to --carts-dir.
//   * without --conf: every regular file in --carts-dir (sorted, dot-
//     files and "carts.conf" skipped), default metadata.
//
// A catalog.txt matching romfs.c's "file|title|autostart|scheme|insert" format
// is written into the image so titles/flags survive exactly.
// ======================================================================

#include "config.h"          // MC_FS_FLASH_OFFSET / _BYTES, ROMFS_* via...
#include "hardware/flash.h"  // FLASH_SECTOR_SIZE / FLASH_PAGE_SIZE (shim)
#include "lfs.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Mirror romfs.h's limits (MUST match, or baked names the firmware then
// rejects get silently dropped).
#define NAME_MAX_LEN   80       // ROMFS_NAME_MAX
#define TITLE_MAX_LEN  24       // ROMFS_TITLE_MAX
#define MAX_ENTRIES    256      // ROMFS_MAX_ROMS
#define BANK_BYTES     (16 * 1024)

#define XIP_FLASH_BASE 0x10000000u

// ---- RAM block device (NOR semantics: prog only clears bits) ---------
static uint8_t   *g_img;
static uint32_t   g_img_bytes;

static int bd_read(const struct lfs_config *c, lfs_block_t blk,
                   lfs_off_t off, void *buf, lfs_size_t sz) {
    (void)c; memcpy(buf, g_img + blk * FLASH_SECTOR_SIZE + off, sz); return 0;
}
static int bd_prog(const struct lfs_config *c, lfs_block_t blk,
                   lfs_off_t off, const void *buf, lfs_size_t sz) {
    (void)c;
    uint8_t *d = g_img + blk * FLASH_SECTOR_SIZE + off;
    const uint8_t *s = buf;
    for (lfs_size_t i = 0; i < sz; i++) d[i] &= s[i];   // NOR AND
    return 0;
}
static int bd_erase(const struct lfs_config *c, lfs_block_t blk) {
    (void)c; memset(g_img + blk * FLASH_SECTOR_SIZE, 0xFF, FLASH_SECTOR_SIZE);
    return 0;
}
static int bd_sync(const struct lfs_config *c) { (void)c; return 0; }

// ---- entry table -----------------------------------------------------
typedef struct {
    char     name[NAME_MAX_LEN];
    char     path[1024];
    char     title[TITLE_MAX_LEN];
    int      autostart;     // -1 = default (1)
    int      scheme;        // -1 = default (auto by size)
    int      insert;        // -1 = default (0: fast reset-path start)
} entry_t;

static entry_t g_ent[MAX_ENTRIES];
static int     g_nent;

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void default_title(const char *file, char *out) {
    int n = 0;
    for (const char *p = file; *p && n < TITLE_MAX_LEN - 1; p++) {
        if (*p == '.') break;
        char ch = (*p == '_' || *p == '-') ? ' ' : *p;
        out[n++] = (char)toupper((unsigned char)ch);
    }
    out[n] = 0;
    if (!n) snprintf(out, TITLE_MAX_LEN, "%s", file);
}

// Neutralise the same characters romfs_set_meta does (JSON + catalog).
static void sanitize_title(char *t) {
    for (char *p = t; *p; p++)
        if (*p == '"' || *p == '\\' || *p == '|' || (unsigned char)*p < 0x20)
            *p = '\'';
}

static int name_ok(const char *n) {
    size_t len = strlen(n);
    if (!len || len >= NAME_MAX_LEN) return 0;
    if (strstr(n, "..") || strchr(n, '/') || strchr(n, '\\')) return 0;
    // Same downstream-breakers the firmware's web.c blocks, so anything
    // we bake is guaranteed selectable/deletable via the web UI.
    if (strchr(n, '"') || strchr(n, '|')) return 0;
    return 1;
}

static void add_entry(const char *dir, const char *name, const char *title,
                      int autostart, int scheme, int insert) {
    if (g_nent >= MAX_ENTRIES) {
        fprintf(stderr, "mklfs: too many carts (max %d)\n", MAX_ENTRIES);
        exit(1);
    }
    if (!name_ok(name)) {
        fprintf(stderr, "mklfs: skipping bad filename '%s'\n", name);
        return;
    }
    entry_t *e = &g_ent[g_nent++];
    memset(e, 0, sizeof(*e));
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->path, sizeof(e->path), "%s/%s", dir, name);
    if (title && title[0]) snprintf(e->title, sizeof(e->title), "%s", title);
    else                   default_title(name, e->title);
    sanitize_title(e->title);
    e->autostart = autostart;
    e->scheme    = scheme;
    e->insert    = insert;
}

static int cmp_name(const void *a, const void *b) {
    return strcmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

static int parse_scheme(const char *s) {
    if (!strcasecmp(s, "flat"))    return 0;
    if (!strcasecmp(s, "bank16k")) return 1;
    if (!strcasecmp(s, "auto"))    return -1;
    fprintf(stderr, "mklfs: unknown scheme '%s' (flat|bank16k|auto)\n", s);
    exit(1);
}

// Apply per-cart metadata from carts.conf to the already-scanned list.
// The file is an OVERLAY: leaving a cart out just means "use defaults",
// so dropping a new ROM into carts/ never requires editing the conf.
static void apply_conf(const char *conf) {
    FILE *f = fopen(conf, "r");
    if (!f) { perror(conf); exit(1); }
    char line[1200];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *fld[5] = { s, NULL, NULL, NULL, NULL };
        int nf = 1;
        for (char *p = s; *p && nf < 5; p++)
            if (*p == '|') { *p = 0; fld[nf++] = p + 1; }
        char *name  = trim(fld[0]);
        char *title = (nf >= 2) ? trim(fld[1]) : "";
        int autostart = (nf >= 3) ? (atoi(trim(fld[2])) != 0) : -1;
        int scheme    = (nf >= 4) ? parse_scheme(trim(fld[3])) : -1;
        int insert    = (nf >= 5) ? (atoi(trim(fld[4])) != 0) : -1;

        entry_t *e = NULL;
        for (int i = 0; i < g_nent; i++)
            if (!strcmp(g_ent[i].name, name)) { e = &g_ent[i]; break; }
        if (!e) {
            fprintf(stderr, "mklfs: warning: carts.conf names '%s', which is "
                            "not in the carts dir -- ignored\n", name);
            continue;
        }
        if (*title)        snprintf(e->title, sizeof(e->title), "%s", title);
        if (autostart >= 0) e->autostart = autostart;
        if (scheme >= 0)    e->scheme    = scheme;
        if (insert >= 0)    e->insert    = insert;
    }
    fclose(f);
}

static void scan_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) { perror(dir); exit(1); }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (!strcmp(de->d_name, "carts.conf")) continue;
        if (!strcmp(de->d_name, "README.md"))  continue;
        add_entry(dir, de->d_name, NULL, -1, -1, -1);
    }
    closedir(d);
    qsort(g_ent, g_nent, sizeof(g_ent[0]), cmp_name);
}

// ---- write one file into the image -----------------------------------
static long write_rom(lfs_t *lfs, entry_t *e) {
    FILE *f = fopen(e->path, "rb");
    if (!f) { perror(e->path); exit(1); }

    lfs_file_t lf;
    if (lfs_file_open(lfs, &lf, e->name, LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0) {
        fprintf(stderr, "mklfs: lfs open '%s' failed\n", e->name); exit(1);
    }
    char buf[4096];
    long total = 0; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (lfs_file_write(lfs, &lf, buf, n) != (lfs_ssize_t)n) {
            fprintf(stderr, "mklfs: image full writing '%s' "
                    "(partition is %u KB)\n", e->name,
                    (unsigned)(g_img_bytes / 1024));
            exit(1);
        }
        total += (long)n;
    }
    lfs_file_close(lfs, &lf);
    fclose(f);

    // Resolve 'auto' scheme now that we know the size.
    if (e->scheme < 0) e->scheme = (total > BANK_BYTES) ? 1 : 0;
    if (e->autostart < 0) e->autostart = 1;
    return total;
}

static void write_catalog(lfs_t *lfs) {
    lfs_file_t lf;
    if (lfs_file_open(lfs, &lf, "catalog.txt",
                      LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) < 0) {
        fprintf(stderr, "mklfs: cannot create catalog.txt\n"); exit(1);
    }
    for (int i = 0; i < g_nent; i++) {
        char line[256];
        int n = snprintf(line, sizeof(line), "%s|%s|%d|%d|%d\n",
                         g_ent[i].name, g_ent[i].title,
                         g_ent[i].autostart, g_ent[i].scheme,
                         g_ent[i].insert < 0 ? 0 : g_ent[i].insert);
        lfs_file_write(lfs, &lf, line, n);
    }
    lfs_file_close(lfs, &lf);
}

// ---- UF2 emission ----------------------------------------------------
#define UF2_MAGIC0   0x0A324655u
#define UF2_MAGIC1   0x9E5D5157u
#define UF2_MAGICEND 0x0AB16F30u
#define UF2_FLAG_FAMILY 0x00002000u
#define UF2_PAYLOAD  256u

static void emit_uf2(const char *out, uint32_t family) {
    // Emit EVERY page of the partition, all-0xFF (erased) pages included.
    // Skipping erased pages assumes "the region is erased at flash time",
    // and it is NOT: picotool load writes only the blocks present in the
    // UF2 and never erases the rest. Flashing a sparse image over a
    // partition that live littlefs has been writing to (catalog updates,
    // web uploads) leaves stale higher-revision metadata and old data
    // blocks in the untouched space, and the next mount can stitch old and
    // new filesystems together, serving one cart's blocks under another
    // cart's name. A full-partition image makes a carts flash a TOTAL
    // replacement.
    // The cfg sector lives outside MC_FS_FLASH_BYTES, so WiFi creds
    // still survive.
    uint32_t base = XIP_FLASH_BASE + MC_FS_FLASH_OFFSET;
    uint32_t total = g_img_bytes / UF2_PAYLOAD;

    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); exit(1); }
    uint32_t blk = 0;
    for (uint32_t off = 0; off < g_img_bytes; off += UF2_PAYLOAD) {
        uint8_t *p = g_img + off;

        uint8_t b[512];
        memset(b, 0, sizeof(b));
        uint32_t hdr[8] = {
            UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY, base + off,
            UF2_PAYLOAD, blk++, total, family,
        };
        memcpy(b, hdr, sizeof(hdr));
        memcpy(b + 32, p, UF2_PAYLOAD);
        uint32_t end = UF2_MAGICEND;
        memcpy(b + 512 - 4, &end, 4);
        fwrite(b, 1, sizeof(b), f);
    }
    fclose(f);
    printf("mklfs: %s — %u UF2 blocks (%u KB flashed at 0x%08x)\n",
           out, total, (unsigned)(total * UF2_PAYLOAD / 1024), base);
}

int main(int argc, char **argv) {
    const char *dir = NULL, *conf = NULL, *out = NULL, *binout = NULL;
    uint32_t family = 0xe48bff57u;   // 'absolute' — correct for data

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--carts-dir") && i + 1 < argc) dir    = argv[++i];
        else if (!strcmp(argv[i], "--conf")      && i + 1 < argc) conf   = argv[++i];
        else if (!strcmp(argv[i], "--out")       && i + 1 < argc) out    = argv[++i];
        else if (!strcmp(argv[i], "--bin")       && i + 1 < argc) binout = argv[++i];
        else if (!strcmp(argv[i], "--family")    && i + 1 < argc)
            family = (uint32_t)strtoul(argv[++i], NULL, 0);
        else { fprintf(stderr, "mklfs: bad arg '%s'\n", argv[i]); return 2; }
    }
    if (!dir || (!out && !binout)) {
        fprintf(stderr, "usage: mklfs --carts-dir DIR [--conf FILE] "
                        "[--family 0xNNNNNNNN] (--out OUT.uf2 | --bin OUT.bin)\n");
        return 2;
    }

    scan_dir(dir);
    if (conf) apply_conf(conf);
    if (!g_nent) {
        fprintf(stderr, "mklfs: no carts found in '%s' — nothing to bake\n", dir);
        return 1;
    }

    g_img_bytes = MC_FS_FLASH_BYTES;
    g_img = malloc(g_img_bytes);
    if (!g_img) { fprintf(stderr, "mklfs: OOM\n"); return 1; }
    memset(g_img, 0xFF, g_img_bytes);

    struct lfs_config cfg = {
        .read = bd_read, .prog = bd_prog, .erase = bd_erase, .sync = bd_sync,
        .read_size = 1, .prog_size = FLASH_PAGE_SIZE,
        .block_size = FLASH_SECTOR_SIZE,
        .block_count = MC_FS_FLASH_BYTES / FLASH_SECTOR_SIZE,
        .block_cycles = 500, .cache_size = 512, .lookahead_size = 32,
    };

    lfs_t lfs;
    if (lfs_format(&lfs, &cfg) < 0 || lfs_mount(&lfs, &cfg) < 0) {
        fprintf(stderr, "mklfs: lfs format/mount failed\n"); return 1;
    }

    long used = 0;
    for (int i = 0; i < g_nent; i++) {
        long n = write_rom(&lfs, &g_ent[i]);
        used += n;
        printf("  + %-28s %6ld B  %-4s %s\n", g_ent[i].name, n,
               g_ent[i].scheme ? "bank" : "flat", g_ent[i].title);
    }
    write_catalog(&lfs);

    // Round-trip check: unmount, remount, confirm every file reads back.
    lfs_unmount(&lfs);
    if (lfs_mount(&lfs, &cfg) < 0) {
        fprintf(stderr, "mklfs: remount of built image FAILED\n"); return 1;
    }
    for (int i = 0; i < g_nent; i++) {
        struct lfs_info info;
        if (lfs_stat(&lfs, g_ent[i].name, &info) < 0) {
            fprintf(stderr, "mklfs: '%s' missing after remount\n", g_ent[i].name);
            return 1;
        }
    }
    lfs_unmount(&lfs);

    printf("mklfs: %d cart(s), %ld KB of %u KB partition used\n",
           g_nent, used / 1024, (unsigned)(g_img_bytes / 1024));

    if (binout) {
        FILE *bf = fopen(binout, "wb");
        if (!bf) { perror(binout); return 1; }
        fwrite(g_img, 1, g_img_bytes, bf);
        fclose(bf);
        printf("mklfs: %s — %u KB raw partition image\n",
               binout, (unsigned)(g_img_bytes / 1024));
    }
    if (out) emit_uf2(out, family);
    free(g_img);
    return 0;
}
