#ifndef SHIM_HSTX_CTRL_H
#define SHIM_HSTX_CTRL_H

// Register-view shim. Field positions match the RP2350 datasheet layout
// (SEL_P [4:0], SEL_N [12:8], INV bit 16, CLK bit 17; EXPAND_SHIFT
// fields at 0/8/16/24), so the recorded values read like real ones.

#include <stdint.h>

#define HSTX_CTRL_CSR_EN_BITS               (1u << 0)
#define HSTX_CTRL_CSR_EXPAND_EN_BITS        (1u << 1)
#define HSTX_CTRL_CSR_SHIFT_LSB             8
#define HSTX_CTRL_CSR_N_SHIFTS_LSB          16
#define HSTX_CTRL_CSR_CLKDIV_LSB            28

#define HSTX_CTRL_EXPAND_SHIFT_RAW_SHIFT_LSB     0
#define HSTX_CTRL_EXPAND_SHIFT_RAW_N_SHIFTS_LSB  8
#define HSTX_CTRL_EXPAND_SHIFT_ENC_SHIFT_LSB     16
#define HSTX_CTRL_EXPAND_SHIFT_ENC_N_SHIFTS_LSB  24

#define HSTX_CTRL_EXPAND_TMDS_L0_ROT_LSB    0
#define HSTX_CTRL_EXPAND_TMDS_L0_NBITS_LSB  5
#define HSTX_CTRL_EXPAND_TMDS_L1_ROT_LSB    8
#define HSTX_CTRL_EXPAND_TMDS_L1_NBITS_LSB  13
#define HSTX_CTRL_EXPAND_TMDS_L2_ROT_LSB    16
#define HSTX_CTRL_EXPAND_TMDS_L2_NBITS_LSB  21

#define HSTX_CTRL_BIT0_SEL_P_LSB            0
#define HSTX_CTRL_BIT0_SEL_N_LSB            8
#define HSTX_CTRL_BIT0_INV_BITS             (1u << 16)
#define HSTX_CTRL_BIT0_CLK_BITS             (1u << 17)

typedef struct {
    uint32_t csr;
    uint32_t bit[8];
    uint32_t expand_shift;
    uint32_t expand_tmds;
} shim_hstx_ctrl_t;

extern shim_hstx_ctrl_t shim_hstx_ctrl;     // define once per binary
#define hstx_ctrl_hw (&shim_hstx_ctrl)

#endif
