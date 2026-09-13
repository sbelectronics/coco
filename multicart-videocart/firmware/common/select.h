#ifndef MULTICART_SELECT_H
#define MULTICART_SELECT_H

#include <stdint.h>
#include <stdbool.h>

// Selection sequencer: stub-serve -> reset pulse -> wait for the stub's
// magic strobe -> swap in the real image.

typedef enum {
    SEL_IDLE = 0,
    SEL_RESET_HOLD,     // !RESET asserted, stub being served
    SEL_WAIT_MAGIC,     // reset released, waiting for the stub to signal
    SEL_WAIT_BASIC,     // slot idle while BASIC finishes its cold boot;
                        // then serve + raise CART* to autostart the way a
                        // physically inserted cartridge does
    SEL_RESET2,         // timeout fallback: second (non-blocking) pulse
    SEL_DONE,           // swapped; transient status
    SEL_TIMEOUT,        // stub never signalled; swapped anyway
    SEL_ERROR,
} select_state_t;

// Serve a ROM at power-on: no reset pulse, no stub (the CoCo is coming
// up anyway). Used for cfg.last_rom.
bool select_boot_rom(const char *file);

// Full selection sequence. Returns false if the ROM can't be loaded.
bool select_rom(const char *file);

// Boot the bare machine to Extended Color BASIC (no cartridge served).
// Same cold-boot stub flow as select_rom, but the slot goes idle instead
// of serving a ROM. Returns false if busy or an OTA is in progress.
bool select_basic(void);

void            select_pump(void);
select_state_t  select_state(void);
bool            select_busy(void);
const char     *select_status(void);      // short line for the OLED
const char     *select_active(void);      // active ROM filename, "" if none

// Cold-boot stub health, for the panel. stub_ok counts selections where
// the stub ran and signalled; stub_timeouts counts the ones that fell
// through the WAIT_MAGIC timeout instead, i.e. did NOT get a guaranteed
// cold boot. On a healthy board the second number stays 0.
uint32_t select_stub_ok(void);
uint32_t select_stub_timeouts(void);

#endif
