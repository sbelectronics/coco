#ifndef SHIM_PICO_STDLIB_H
#define SHIM_PICO_STDLIB_H

// Host-test shim for pico/stdlib.h: a controllable fake clock plus the
// handful of time helpers the common code uses. Tests advance time
// explicitly with shim_advance_us() — nothing here ever blocks.

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t absolute_time_t;

// RAM-placement attributes are meaningless on the host.
#define __not_in_flash_func(x)            x
#define __no_inline_not_in_flash_func(x)  x
#define __not_in_flash(group)             /* nothing */

extern uint64_t shim_now_us;    // define once per test binary

static inline absolute_time_t get_absolute_time(void) { return shim_now_us; }

static inline absolute_time_t delayed_by_us(absolute_time_t t, uint64_t us) {
    return t + us;
}
static inline absolute_time_t make_timeout_time_ms(uint32_t ms) {
    return shim_now_us + (uint64_t)ms * 1000;
}
// SDK semantics: returns (to - from) in microseconds.
static inline int64_t absolute_time_diff_us(absolute_time_t from,
                                            absolute_time_t to) {
    return (int64_t)(to - from);
}
static inline void sleep_ms(uint32_t ms) { shim_now_us += (uint64_t)ms * 1000; }
static inline void sleep_us(uint64_t us) { shim_now_us += us; }
static inline void tight_loop_contents(void) {}
static inline void stdio_init_all(void) {}

static inline void shim_advance_us(uint64_t us) { shim_now_us += us; }

#endif
