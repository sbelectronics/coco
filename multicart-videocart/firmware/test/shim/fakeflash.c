#include "hardware/flash.h"

#include <assert.h>
#include <string.h>

uint8_t  shim_flash[SHIM_FLASH_BYTES];
uint32_t shim_flash_erases;
uint32_t shim_flash_progs;

void flash_range_erase(uint32_t offset, size_t count) {
    assert(offset % FLASH_SECTOR_SIZE == 0);
    assert(count  % FLASH_SECTOR_SIZE == 0);
    assert(offset + count <= SHIM_FLASH_BYTES);
    memset(shim_flash + offset, 0xFF, count);
    shim_flash_erases += (uint32_t)(count / FLASH_SECTOR_SIZE);
}

void flash_range_program(uint32_t offset, const uint8_t *data, size_t count) {
    assert(offset % FLASH_PAGE_SIZE == 0);
    assert(count  % FLASH_PAGE_SIZE == 0);
    assert(offset + count <= SHIM_FLASH_BYTES);
    // NOR semantics: programming clears bits only.
    for (size_t i = 0; i < count; i++)
        shim_flash[offset + i] &= data[i];
    shim_flash_progs += (uint32_t)(count / FLASH_PAGE_SIZE);
}
