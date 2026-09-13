#ifndef MULTICART_ROMFS_H
#define MULTICART_ROMFS_H

// LittleFS on the data partition + the ROM catalog.
//
// Flash-write rule: every erase/program runs from RAM with interrupts
// off for the duration of ONE sector/page only, so WiFi keeps running
// and core 1 (RAM-resident, no XIP) keeps serving the bus throughout.

#include <stdint.h>
#include <stdbool.h>

#define ROMFS_MAX_ROMS      256
// 80 fits real CoCo TOSEC/No-Intro filenames, e.g.
// "Audio Spectrum Analyzer (1981) (26-3156) (Tandy) (Coco 1-2).ccc" (63).
// NOTE: cfg_t.last_rom is [64], so auto-boot-on-power-on only PERSISTS
// for names <= 63 chars; longer-named ROMs still appear and are
// selectable, they just won't be remembered as the power-on default.
#define ROMFS_NAME_MAX      80
#define ROMFS_TITLE_MAX     24

typedef struct {
    char     file[ROMFS_NAME_MAX];
    char     title[ROMFS_TITLE_MAX];
    uint8_t  autostart;      // default 1
    uint8_t  scheme;         // bus_scheme_t (0=flat, 1=bank16k)
    // Insertion-style start: hold the slot EMPTY across the cold boot so
    // BASIC reaches READY, then serve + raise CART* (select.c). Costs
    // ~2 s per selection, so it is OFF by default and set only for the
    // few tape conversions that need a fully-initialised BASIC.
    uint8_t  insert;         // default 0
    uint32_t size;
} rom_entry_t;

bool romfs_mount(void);          // mounts; formats once if unformatted
bool romfs_mounted(void);
void romfs_rescan(void);         // rebuild the in-RAM list from the FS

// The returned pointers reference the in-RAM list and are INVALIDATED by
// anything that rebuilds or reorders it: romfs_rescan, romfs_delete,
// romfs_set_meta (which re-sorts by title) and a completed upload. Copy
// the filename and re-resolve rather than holding one across those calls.
int  romfs_count(void);
const rom_entry_t *romfs_entry(int idx);
const rom_entry_t *romfs_find(const char *file);
int  romfs_index_of(const char *file);

// Load a ROM image into dst (up to max bytes). Returns bytes read, <0 on
// error.
int32_t romfs_load(const char *file, uint8_t *dst, uint32_t max);

// ---- mutation (web UI) ----------------------------------------------
bool romfs_delete(const char *file);
bool romfs_set_meta(const char *file, const char *title,
                    int autostart, int scheme, int insert);

// Streaming upload: begin, feed chunks, end. abort() discards.
bool romfs_upload_begin(const char *file);
bool romfs_upload_chunk(const void *data, uint32_t len);
bool romfs_upload_end(void);
void romfs_upload_abort(void);
bool romfs_upload_active(void);

// Free space in bytes (approximate: block granularity).
uint32_t romfs_free_bytes(void);
uint32_t romfs_total_bytes(void);

#endif
