// ======================================================================
// main.c — CoCo Multicart firmware entry.
//
// Boot order matters:
//   1. any pending OTA image is applied before anything else touches
//      flash (this call never returns if it has work to do)
//   2. cfg — nothing may read it earlier
//   3. expander — establishes the safe !RESET / CART gate idle state
//   4. bus + core-1 launch, so the slot is live before UI/net exist
//   5. romfs, then auto-serve cfg.last_rom: powering on the CoCo with
//      the cart inserted boots the last game with no interaction
//   6. UI, then the core-0 pump (encoder/OLED, selection, WiFi, web)
// ======================================================================

#include "config.h"
#include "cfg.h"
#include "expander.h"
#include "bus.h"
#include "romfs.h"
#include "select.h"
#include "ui.h"
#include "net.h"
#include "web.h"
#include "ota.h"
#include "cdcmenu.h"

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/clocks.h"       // set_sys_clock_khz

#include <stdio.h>

int main(void) {
    // 250 MHz, not the SDK default 150 MHz. This is a requirement, not a
    // tweak -- see the lap-time analysis at the top of bus.c.
    //
    // Short version: the core-1 serve loop samples !CTS once per lap, and
    // at 150 MHz a lap is ~300-374 ns against a !CTS window of only
    // ~636 ns. That gives ~1.7 samples per cart access, and a sample has
    // to land in the first ~318 ns for our data to beat the CoCo's latch.
    // At 374 ns spacing that succeeds ~85% of the time. Bare, the leftover
    // margin hides it; behind a Multi-Pak, !CTS arrives ~40 ns late
    // through a '367 and a '139 and it stops hiding -- intermittent 0xFF
    // reads, immune to drive strength and to WiFi.
    //
    // At 250 MHz the lap is ~224 ns, shorter than the 318 ns window, so at
    // least one sample lands in time on EVERY access instead of most of
    // them: a cart that would not boot in a Multi-Pak at 150 MHz loads
    // and runs at 250 MHz.
    //
    // Must precede bus_init(): the bank-strobe PIO derives its divider
    // from clock_get_hz(clk_sys), so the 600 ns sample delay rescales
    // itself only if the clock is already set.
    set_sys_clock_khz(250000, true);

    stdio_init_all();

    ota_apply_pending_if_any();     // never returns if an image is staged

    cfg_init();
    cdcmenu_init();
    expander_init();

    bus_init(cfg_get()->strobe_delay_ns);
    multicore_launch_core1(bus_core1_main);

    if (romfs_mount()) {
        if (cfg_get()->last_rom[0] && !select_boot_rom(cfg_get()->last_rom))
            printf("boot: '%s' missing — starting idle\n",
                   cfg_get()->last_rom);
    }

    ui_init();

    printf("CoCo Multicart %s ready\n", MC_FW_VERSION);

    for (;;) {
        ui_pump();
        select_pump();
        net_pump();
        web_pump();
        ota_pump();
        cdcmenu_pump();
    }
}
