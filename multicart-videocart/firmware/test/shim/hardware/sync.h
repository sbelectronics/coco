#ifndef SHIM_HARDWARE_SYNC_H
#define SHIM_HARDWARE_SYNC_H

#include <stdint.h>

static inline void __dmb(void) {}
static inline uint32_t save_and_disable_interrupts(void) { return 0; }
static inline void restore_interrupts(uint32_t s) { (void)s; }

#endif
