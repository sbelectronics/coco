#ifndef MULTICART_EXPANDER_H
#define MULTICART_EXPANDER_H

#include <stdint.h>
#include <stdbool.h>

// PCF8574 access (encoder inputs + !RESET / CART_EN enables). All calls
// are core-0 only; the shadow keeps written state so input reads don't
// disturb the enables. Quasi-bidirectional rules: a bit written 1 is a
// weak-high input; written 0 drives low.

void    expander_init(void);            // also inits MC_I2C @100 kHz
bool    expander_ok(void);              // false if the chip never ACKed

// Read all 8 pins (encoder bits are meaningful; enable bits read back
// as driven). Returns MC_XP_IDLE pattern on I2C failure.
uint8_t expander_read(void);

// Drive an enable low (assert) or release to weak-high.
void    expander_assert(uint8_t bit);   // write shadow & ~bit
void    expander_release(uint8_t bit);  // write shadow |  bit

#endif
