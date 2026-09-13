static const uint16_t mux_sampler_program_instructions[] = {
            //     .wrap_target
    0x2088, //  0: wait   1 gpio, 8       side 0
    0x2708, //  1: wait   0 gpio, 8       side 0 [7]
    0xa5c1, //  2: mov    isr, x          side 0 [5]
    0xa0e0, //  3: mov    osr, pins       side 0
    0x48e3, //  4: in     osr, 3          side 1
    0x6c6d, //  5: out    null, 13        side 1 [4]
    0x4be1, //  6: in     osr, 1          side 1 [3]
    0xa8e0, //  7: mov    osr, pins       side 1
    0x50e3, //  8: in     osr, 3          side 2
    0x746d, //  9: out    null, 13        side 2 [4]
    0x53e1, // 10: in     osr, 1          side 2 [3]
    0xb0e0, // 11: mov    osr, pins       side 2
    0x58e3, // 12: in     osr, 3          side 3
    0x7c6d, // 13: out    null, 13        side 3 [4]
    0x7b41, // 14: out    y, 1            side 3 [3]
    0xb8e0, // 15: mov    osr, pins       side 3
    0x40e3, // 16: in     osr, 3          side 0
    0x0077, // 17: jmp    !y, 23          side 0
    0x606d, // 18: out    null, 13        side 0
    0x6041, // 19: out    y, 1            side 0
    0x0077, // 20: jmp    !y, 23          side 0
    0x4061, // 21: in     null, 1         side 0
    0x8000, // 22: push   noblock         side 0
    0xa0c3, // 23: mov    isr, null       side 0
            //     .wrap
};
static const uint16_t sweep_follower_program_instructions[] = {
            //     .wrap_target
    0x2088, //  0: wait   1 gpio, 8
    0x2708, //  1: wait   0 gpio, 8              [7]
    0xe540, //  2: set    y, 0                   [5]
    0x400e, //  3: in     pins, 14
    0x4042, //  4: in     y, 2
    0xe841, //  5: set    y, 1                   [8]
    0x400e, //  6: in     pins, 14
    0x4042, //  7: in     y, 2
    0xe842, //  8: set    y, 2                   [8]
    0x400e, //  9: in     pins, 14
    0x4042, // 10: in     y, 2
    0xe843, // 11: set    y, 3                   [8]
    0x400e, // 12: in     pins, 14
    0x4042, // 13: in     y, 2
            //     .wrap
};
static const uint16_t wr_capture_program_instructions[] = {
    0xe021, //  0: set    x, 1
            //     .wrap_target
    0x2028, //  1: wait   0 pin, 8
    0x20a8, //  2: wait   1 pin, 8
    0xe05d, //  3: set    y, 29
    0x0084, //  4: jmp    y--, 4
    0x4021, //  5: in     x, 1
    0x4008, //  6: in     pins, 8
    0x8020, //  7: push   block
            //     .wrap
};
static const uint16_t bus_serve_program_instructions[] = {
            //     .wrap_target
    0x80a0, //  0: pull   block
    0x2088, //  1: wait   1 gpio, 8
    0x6008, //  2: out    pins, 8
    0x00c5, //  3: jmp    pin, 5
    0x0007, //  4: jmp    7
    0x6088, //  5: out    pindirs, 8
    0x2008, //  6: wait   0 gpio, 8
    0xa0e3, //  7: mov    osr, null
    0x6088, //  8: out    pindirs, 8
            //     .wrap
};
static inline float bus_pio_div(void) {
    return (float)clock_get_hz(clk_sys) / (float)(MC_PIO_REF_KHZ * 1000u);
}
