#include "expander.h"
#include "config.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include <stdio.h>

static uint8_t s_shadow = MC_XP_IDLE;
static bool    s_ok     = false;

static bool xp_write(uint8_t v) {
    int n = i2c_write_timeout_us(MC_I2C, MC_I2C_ADDR_EXPANDER, &v, 1,
                                 false, 2000);
    return n == 1;
}

void expander_init(void) {
    i2c_init(MC_I2C, MC_I2C_BAUD);
    gpio_set_function(MC_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(MC_PIN_SCL, GPIO_FUNC_I2C);
    // The board has external 4.7k pull-ups, so no internal pull-UP is
    // needed — but the pads still come up with the internal pull-DOWN
    // enabled (PADS_BANK0 resets with PDE set) and gpio_set_function()
    // does not clear it. Leaving it fights the external resistor into a
    // divider, and RP2350 erratum E9 latches a pad held above ~2.2 V by
    // a weak source, which is exactly what a 4.7k pull-up is. Clear it
    // explicitly, the same way the bus pins do.
    gpio_disable_pulls(MC_PIN_SDA);
    gpio_disable_pulls(MC_PIN_SCL);

    // Establish the safe idle state explicitly (the chip powers up all-
    // high anyway — the '125 polarity makes that safe — but
    // writing it confirms the part is present).
    s_shadow = MC_XP_IDLE;
    s_ok = xp_write(s_shadow);
    if (!s_ok) printf("expander: PCF8574 not responding at 0x%02x\n",
                      MC_I2C_ADDR_EXPANDER);
}

bool expander_ok(void) { return s_ok; }

uint8_t expander_read(void) {
    uint8_t v = MC_XP_IDLE;
    int n = i2c_read_timeout_us(MC_I2C, MC_I2C_ADDR_EXPANDER, &v, 1,
                                false, 2000);
    // Track health here too, not just on writes: a bus where only reads
    // fail would otherwise keep reporting "expander: ok" while the encoder
    // silently reads as idle.
    s_ok = (n == 1);
    return s_ok ? v : MC_XP_IDLE;
}

void expander_assert(uint8_t bit) {
    s_shadow &= (uint8_t)~bit;
    s_ok = xp_write(s_shadow);
}

void expander_release(uint8_t bit) {
    s_shadow |= bit;
    s_ok = xp_write(s_shadow);
}
