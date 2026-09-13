#ifndef SHIM_HARDWARE_I2C_H
#define SHIM_HARDWARE_I2C_H

// Host-test shim: routes I2C traffic to per-test hook functions so a
// test can model the SSD1306 / PCF8574 at the wire level.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct i2c_inst i2c_inst_t;
#define i2c0 ((i2c_inst_t *)0)
#define i2c1 ((i2c_inst_t *)1)

static inline void i2c_init(i2c_inst_t *i, uint32_t baud) {
    (void)i; (void)baud;
}

// Tests implement these.
int shim_i2c_write(uint8_t addr, const uint8_t *src, size_t len);
int shim_i2c_read(uint8_t addr, uint8_t *dst, size_t len);

static inline int i2c_write_timeout_us(i2c_inst_t *i, uint8_t addr,
                                       const uint8_t *src, size_t len,
                                       bool nostop, uint32_t to) {
    (void)i; (void)nostop; (void)to;
    return shim_i2c_write(addr, src, len);
}
static inline int i2c_read_timeout_us(i2c_inst_t *i, uint8_t addr,
                                      uint8_t *dst, size_t len,
                                      bool nostop, uint32_t to) {
    (void)i; (void)nostop; (void)to;
    return shim_i2c_read(addr, dst, len);
}

#endif
