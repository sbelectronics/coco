#ifndef SHIM_HARDWARE_FLASH_H
#define SHIM_HARDWARE_FLASH_H

// Host-test shim: a 4 MB fake flash with REAL flash semantics —
// program can only clear bits (dest &= src), erase sets a sector to
// 0xFF. That catches program-without-erase bugs a plain memcpy model
// would hide. XIP reads go straight at the array (see the macros).

#include <stdint.h>
#include <stddef.h>

#define FLASH_PAGE_SIZE    256u
#define FLASH_SECTOR_SIZE  4096u
#define SHIM_FLASH_BYTES   (4u * 1024 * 1024)

extern uint8_t  shim_flash[SHIM_FLASH_BYTES];   // define once per binary
extern uint32_t shim_flash_erases;              // stats for assertions
extern uint32_t shim_flash_progs;

// The firmware forms pointers as (XIP_*_BASE + offset).
#define XIP_BASE                  ((uintptr_t)shim_flash)
#define XIP_NOCACHE_NOALLOC_BASE  ((uintptr_t)shim_flash)

void flash_range_erase(uint32_t offset, size_t count);
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count);

#endif
