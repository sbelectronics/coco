#ifndef SHIM_HARDWARE_WATCHDOG_H
#define SHIM_HARDWARE_WATCHDOG_H
// Host-test shim: reboot is a no-op. Tests must not exercise the
// Save & Reboot path (its `for(;;)` would hang the host run).
#include <stdint.h>
static inline void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms) {
    (void)pc; (void)sp; (void)delay_ms;
}
#endif
