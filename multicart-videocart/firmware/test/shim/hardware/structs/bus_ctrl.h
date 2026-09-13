#ifndef SHIM_BUS_CTRL_H
#define SHIM_BUS_CTRL_H

#include <stdint.h>

#define BUSCTRL_BUS_PRIORITY_DMA_W_BITS (1u << 12)
#define BUSCTRL_BUS_PRIORITY_DMA_R_BITS (1u << 8)

typedef struct { uint32_t priority; } shim_bus_ctrl_t;
extern shim_bus_ctrl_t shim_bus_ctrl;       // define once per binary
#define bus_ctrl_hw (&shim_bus_ctrl)

#endif
