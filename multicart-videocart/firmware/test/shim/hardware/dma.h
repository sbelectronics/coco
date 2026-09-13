#ifndef SHIM_HARDWARE_DMA_H
#define SHIM_HARDWARE_DMA_H

// Host-test shim of the DMA API surface hstx.c uses. It doesn't emulate
// the engine — it RECORDS what the code programmed, so the test can
// assert the topology and then walk the descriptor table itself.
//
// channel_config_set_ring deliberately mimics the real hardware
// property that a channel has ONE ring (a second call overwrites the
// first) — the bug class this project already hit once.

#include <stdint.h>
#include <stdbool.h>

typedef unsigned int uint;

typedef struct { uint32_t ctrl; } dma_channel_config;

enum dma_channel_transfer_size { DMA_SIZE_8, DMA_SIZE_16, DMA_SIZE_32 };

#define DREQ_HSTX 66

// Shim ctrl encoding (not hardware layout; both sides of the test use
// these macros):
#define SHIM_CTRL_CHAIN(c)     ((c) & 0xFu)
#define SHIM_CTRL_RD_INC       (1u << 4)
#define SHIM_CTRL_WR_INC       (1u << 5)
#define SHIM_CTRL_SIZE_LSB     6
#define SHIM_CTRL_DREQ_LSB     8       // dreq+1, 0 = unpaced
#define SHIM_CTRL_RING_LSB     16      // ring size_bits
#define SHIM_CTRL_RING_SEL     (1u << 21)  // set = write ring

static inline dma_channel_config dma_channel_get_default_config(uint ch) {
    dma_channel_config c = { .ctrl = ch & 0xFu };   // chain-to-self
    return c;
}
static inline void channel_config_set_transfer_data_size(
        dma_channel_config *c, enum dma_channel_transfer_size s) {
    c->ctrl = (c->ctrl & ~(3u << SHIM_CTRL_SIZE_LSB))
            | ((uint32_t)s << SHIM_CTRL_SIZE_LSB);
}
static inline void channel_config_set_read_increment(dma_channel_config *c,
                                                     bool inc) {
    c->ctrl = inc ? (c->ctrl | SHIM_CTRL_RD_INC) : (c->ctrl & ~SHIM_CTRL_RD_INC);
}
static inline void channel_config_set_write_increment(dma_channel_config *c,
                                                      bool inc) {
    c->ctrl = inc ? (c->ctrl | SHIM_CTRL_WR_INC) : (c->ctrl & ~SHIM_CTRL_WR_INC);
}
static inline void channel_config_set_dreq(dma_channel_config *c, uint dreq) {
    c->ctrl = (c->ctrl & ~(0xFFu << SHIM_CTRL_DREQ_LSB))
            | ((dreq + 1u) << SHIM_CTRL_DREQ_LSB);
}
static inline void channel_config_set_chain_to(dma_channel_config *c, uint ch) {
    c->ctrl = (c->ctrl & ~0xFu) | (ch & 0xFu);
}
static inline void channel_config_set_ring(dma_channel_config *c, bool write,
                                           uint size_bits) {
    // ONE ring per channel: overwrite, exactly like the silicon.
    c->ctrl = (c->ctrl & ~((0x1Fu << SHIM_CTRL_RING_LSB) | SHIM_CTRL_RING_SEL))
            | (size_bits << SHIM_CTRL_RING_LSB)
            | (write ? SHIM_CTRL_RING_SEL : 0);
}
#define SHIM_CTRL_IRQ_QUIET    (1u << 22)

static inline void channel_config_set_irq_quiet(dma_channel_config *c,
                                                bool quiet) {
    c->ctrl = quiet ? (c->ctrl | SHIM_CTRL_IRQ_QUIET)
                    : (c->ctrl & ~SHIM_CTRL_IRQ_QUIET);
}
static inline uint32_t channel_config_get_ctrl_value(dma_channel_config *c) {
    return c->ctrl;
}

// Recorded channel state (offsets are NOT hardware-accurate; the test
// interprets semantically).
typedef struct {
    volatile uint32_t read_addr;
    volatile uint32_t write_addr;
    volatile uint32_t transfer_count;
    volatile uint32_t ctrl_trig;
    volatile uint32_t al3_read_addr_trig;
} shim_dma_ch_t;

// ints0 exists so the island-refill IRQ path compiles and can be driven
// by a test; nothing in the shim raises it on its own.
typedef struct { shim_dma_ch_t ch[16]; volatile uint32_t ints0; } shim_dma_hw_t;
extern shim_dma_hw_t shim_dma;              // define once per binary
#define dma_hw (&shim_dma)

extern int  shim_dma_next_chan;
extern uint shim_dma_started;               // bitmask of started channels
extern uint32_t shim_dma_cfg_write[16];     // dma_channel_configure records
extern uint32_t shim_dma_cfg_read[16];
extern uint32_t shim_dma_cfg_count[16];
extern uint32_t shim_dma_cfg_ctrl[16];
extern bool     shim_dma_cfg_trig[16];

static inline int dma_claim_unused_channel(bool required) {
    (void)required;
    return shim_dma_next_chan++;
}
static inline void dma_channel_configure(uint ch, dma_channel_config *c,
                                         volatile void *write_addr,
                                         const volatile void *read_addr,
                                         uint32_t count, bool trigger) {
    shim_dma_cfg_write[ch] = (uint32_t)(uintptr_t)write_addr;
    shim_dma_cfg_read[ch]  = (uint32_t)(uintptr_t)read_addr;
    shim_dma_cfg_count[ch] = count;
    shim_dma_cfg_ctrl[ch]  = c->ctrl;
    shim_dma_cfg_trig[ch]  = trigger;
    if (trigger) shim_dma_started |= (1u << ch);
}
static inline void dma_channel_start(uint ch) { shim_dma_started |= (1u << ch); }


// ---- interrupt surface -----------------------------------------------
// The videocart raises a DMA interrupt only on data-island descriptors,
// to refill an island slot a few lines ahead. The host tests never run
// the handler asynchronously; these exist so hstx.c compiles and so a
// test can call the handler directly if it wants to.
#define DMA_IRQ_0 11
static inline void dma_channel_set_irq0_enabled(uint ch, bool en) {
    (void)ch; (void)en;
}
static inline void irq_set_exclusive_handler(uint num, void (*fn)(void)) {
    (void)num; (void)fn;
}
static inline void irq_set_priority(uint num, uint8_t pri) {
    (void)num; (void)pri;
}
static inline void irq_set_enabled(uint num, bool en) { (void)num; (void)en; }
#endif
