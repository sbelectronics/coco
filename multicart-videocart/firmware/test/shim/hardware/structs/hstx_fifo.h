#ifndef SHIM_HSTX_FIFO_H
#define SHIM_HSTX_FIFO_H

#include <stdint.h>

typedef struct { uint32_t stat; uint32_t fifo; } shim_hstx_fifo_t;
extern shim_hstx_fifo_t shim_hstx_fifo;     // define once per binary
#define hstx_fifo_hw (&shim_hstx_fifo)

#endif
