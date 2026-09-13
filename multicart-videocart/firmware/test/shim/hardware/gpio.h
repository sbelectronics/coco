#ifndef SHIM_HARDWARE_GPIO_H
#define SHIM_HARDWARE_GPIO_H

#include <stdint.h>
typedef unsigned int uint;

extern uint32_t shim_gpio_funcsel[48];      // define once per binary

static inline void gpio_set_function(uint pin, uint fn) {
    shim_gpio_funcsel[pin] = fn;
}

#endif
