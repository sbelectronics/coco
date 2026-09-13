// ======================================================================
// main.c — CoCo VIDEOCART firmware entry.
//
// The multicart's boot order with video brought up early, so the monitor
// shows life before anything slow happens:
//
//   1. apply any staged OTA image (never returns if there is one)
//   2. clocks + HSTX + test pattern
//   3. cfg, CDC menu, I2C expander
//   4. bus + core-1 launch (snoop starts; the 64K mirror begins filling)
//   5. renderer takes over the screen
//   6. romfs, auto-serve cfg.last_rom
//   7. OLED/encoder UI
//   8. core-0 pump
// ======================================================================

#include "config.h"
#include "cfg.h"
#include "expander.h"
#include "bus.h"
#include "audio.h"
#include "romfs.h"
#include "select.h"
#include "ui.h"
#include "net.h"
#include "web.h"
#include "ota.h"
#include "cdcmenu.h"
#include "video.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"

#include <stdio.h>

int main(void) {
    // 250 MHz, not 125. The reason is core 1: its snoop loop has to finish
    // inside the CoCo's 1117 ns bus cycle, and at 125 MHz the worst body
    // measured 1336 ns. It overran, lost the lap, and with it that cycle's
    // screen byte or register write -- thousands of times a minute. The
    // same work at 250 MHz is about 668 ns, and SRAM serves twice as many
    // accesses per second, which halves the contention bursts that were
    // the actual trigger.
    //
    // Two things that WOULD have moved with clk_sys are pinned instead, so
    // nothing timing-critical changes: clk_hstx just below, and the PIO
    // sweep state machines in bus_pio.pio.
    set_sys_clock_khz(MC_SYS_CLK_KHZ, true);

    // The pixel clock is clk_hstx / 5 -- 10 TMDS bits per pixel at 2 bits
    // per cycle -- so 125 MHz here is the 25 MHz that 640x480p60 needs.
    // Left to follow clk_sys it would have become 50 MHz and there would
    // be no picture at all. The divider field is two bits wide, which is
    // enough for the /2 this takes; config.h asserts that.
    clock_configure(clk_hstx, 0,
                    CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS,
                    MC_SYS_CLK_KHZ * 1000u, MC_HSTX_CLK_KHZ * 1000u);

    stdio_init_all();

    ota_apply_pending_if_any();

    // cfg_init FIRST: hstx_init needs hdmi_audio out of it, and reading
    // cfg_get() before this point returns the zero-initialised struct,
    // which silently selects DVI mode and leaves the entire HDMI audio
    // path compiled in but never executed.
    cfg_init();

    hstx_init(cfg_get()->hdmi_audio != 0);
    hstx_test_pattern();          // proves scanout with no CoCo attached

    cdcmenu_init();
    expander_init();

    // Renderer state (including the core-1 -> core-0 event ring) must
    // be initialized BEFORE core 1 starts pushing into it: the CoCo may
    // already be running and writing video registers.
    vdg_init();
    vdg_set_artifact(cfg_get()->artifact);   // after init, which resets state
    audio_set_gain(cfg_get()->audio_gain);
    audio_set_filter(cfg_get()->audio_filter);
    audio_set_onebit(cfg_get()->audio_onebit);

    bus_init(cfg_get()->strobe_delay_ns);
    // AFTER bus_init, never before: bus_init memsets the whole core-1
    // control block, which includes this flag. Setting it earlier (as
    // hstx_init used to) meant core 1 never captured an audio event.
    bus_audio_enable(hstx_hdmi_audio());
    multicore_launch_core1(bus_core1_main);

    if (romfs_mount()) {
        if (cfg_get()->last_rom[0] && !select_boot_rom(cfg_get()->last_rom))
            printf("boot: '%s' missing — starting idle\n",
                   cfg_get()->last_rom);
    }

    ui_init();

    printf("CoCo VIDEOCART %s ready\n", MC_FW_VERSION);

    for (;;) {
        ui_pump();
        select_pump();
        bus_pump();     // drain core 1's write pairs BEFORE rendering
        vdg_pump();
        net_pump();
        web_pump();
        ota_pump();
        cdcmenu_pump();
    }
}
