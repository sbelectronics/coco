#ifndef SHIM_HARDWARE_CLOCKS_H
#define SHIM_HARDWARE_CLOCKS_H

// Host shim for hardware/clocks.h.
//
// Exists so tests can call the firmware's own divider arithmetic instead
// of re-deriving it. shim_clk_sys_hz is settable, which is the point: a
// test can ask "what would the firmware program at 250 MHz?" and compare
// against what the sweep timing actually requires.

#include <stdint.h>

typedef enum { clk_sys = 0, clk_hstx = 7 } shim_clock_index_t;

extern uint32_t shim_clk_sys_hz;

static inline uint32_t clock_get_hz(int which) {
    (void)which;
    return shim_clk_sys_hz;
}

#endif
