// ======================================================================
// cdcmenu.c — USB-CDC configuration console (piconet pattern).
//
// Any keystroke enters the menu. Provisions WiFi, the scope-tunable
// bank-write strobe delay, and exposes ROM/diagnostic state
// for bring-up before the web UI is reachable.
// ======================================================================

#include "cdcmenu.h"
#include "cfg.h"
#include "config.h"
#include "net.h"
#include "romfs.h"
#include "select.h"
#include "bus.h"
#include "expander.h"
#include "ssd1306.h"

#include "pico/stdlib.h"
#include "hardware/watchdog.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define LINE_BUF 160

static bool s_active;
static char s_line[LINE_BUF];
static int  s_len;

bool cdcmenu_active(void) { return s_active; }

void cdcmenu_init(void) { s_active = false; s_len = 0; }

static void prompt(void) { fputs(MC_PROMPT, stdout); fflush(stdout); }

static void show_help(void) {
    fputs(
        "Commands (case-insensitive):\r\n"
        "  SHOW                  configuration + status\r\n"
        "  LIST                  ROMs in flash (numbered)\r\n"
        "  PLAY <n|file>         select ROM by LIST number (or exact name)\r\n"
        "  BASIC                 boot the CoCo to Extended BASIC (no cart)\r\n"
        "  SET SSID <v>          WiFi SSID\r\n"
        "  SET PSK <v>           WiFi pre-shared key\r\n"
        "  SET AUTH OPEN|WPA2|WPA3|MIXED\r\n"
        "  SET WIFI ON|OFF       radio enabled at boot (power budget)\r\n"
        "  SET STROBE <ns>       bank-write sample delay (scope-tunable)\r\n"
        "  SAVE                  write config to flash\r\n"
        "  WIPE                  erase flash config\r\n"
        "  DIAG                  bus/I2C diagnostics\r\n"
        "  REBOOT                reset the Pico\r\n"
        "  HELP, ?               this list\r\n"
        "  EXIT                  resume normal operation\r\n",
        stdout);
}

static void show_config(void) {
    cfg_t *c = cfg_get();
    printf("  source   : %s\r\n", cfg_loaded_from_flash() ? "flash"
                                                          : "defaults");
    printf("  ssid     : %s\r\n", c->wifi.ssid[0] ? c->wifi.ssid : "(unset)");
    printf("  auth     : %s\r\n", cfg_auth_name(c->wifi.auth));
    printf("  wifi     : %s\r\n", c->wifi.enabled ? "on" : "off");
    printf("  ip       : %s\r\n", net_up() ? net_ip() : "(down)");
    printf("  strobe   : %u ns\r\n", (unsigned)c->strobe_delay_ns);
    printf("  last rom : %s\r\n", c->last_rom[0] ? c->last_rom : "(none)");
    printf("  active   : %s\r\n", select_active()[0] ? select_active()
                                                     : "(none)");
    printf("  roms     : %d, %u KB free\r\n", romfs_count(),
           (unsigned)(romfs_free_bytes() / 1024));
}

static void show_diag(void) {
    printf("  expander : %s\r\n", expander_ok() ? "ok" : "NOT RESPONDING");
    printf("  oled     : %s\r\n", oled_present() ? "ok" : "NOT RESPONDING");
    printf("  xp pins  : 0x%02x\r\n", expander_read());
    printf("  strobes  : %u (bank %u)\r\n",
           (unsigned)bus_strobe_count(), bus_current_bank());
    printf("  romfs    : %s\r\n", romfs_mounted() ? "mounted" : "unmounted");

    // Full-bus boards (videocart) only; the multicart returns zeros.
    if (bus_mirror()) {
        bus_debug_t d;
        bus_debug(&d);
        printf("  writes   : %u\r\n", (unsigned)bus_write_count());
        printf("  cap late : %u  (should stay 0)\r\n",
               (unsigned)bus_capture_behind());
        printf("  scs mism : %u  (address decode cross-check)\r\n",
               (unsigned)bus_scs_disagree());
        printf("  delay    : %u ns\r\n", (unsigned)d.strobe_delay_ns);
        // Serve-loop laps. Run DIAG twice a known interval apart and
        // divide: lap = interval / delta. The CoCo pushes one capture
        // per ~1.1 us bus cycle and the loop drains at most ONE per lap,
        // so a lap above ~1.1 us means writes are being dropped outright.
        printf("  laps     : %u  (DIAG twice; lap = dt / delta)\r\n",
               (unsigned)d.lap_count);
    }
}

static void handle(char *line) {
    while (*line == ' ') line++;
    if (!*line) return;

    char *arg = strchr(line, ' ');
    if (arg) { *arg++ = 0; while (*arg == ' ') arg++; }

    if (!strcasecmp(line, "HELP") || !strcmp(line, "?")) { show_help(); return; }
    if (!strcasecmp(line, "SHOW")) { show_config(); return; }
    if (!strcasecmp(line, "DIAG")) { show_diag(); return; }

    if (!strcasecmp(line, "LIST")) {
        int n = romfs_count();
        if (!n) { fputs("  (no ROMs — upload via web UI)\r\n", stdout); return; }
        for (int i = 0; i < n; i++) {
            const rom_entry_t *e = romfs_entry(i);
            // Number first (that's what PLAY wants); size/flags in a
            // parenthesised suffix so the filename stands on its own and
            // can't be confused with an adjacent column.
            printf("  %2d. %s  (%uK %s%s)\r\n", i, e->file,
                   (unsigned)(e->size / 1024),
                   e->autostart ? "auto" : "manual",
                   e->scheme ? " banked" : "");
        }
        return;
    }

    if (!strcasecmp(line, "PLAY")) {
        if (!arg || !*arg) {
            fputs("usage: PLAY <number|file>  (numbers are from LIST)\r\n",
                  stdout);
            return;
        }
        const rom_entry_t *e = NULL;
        char *end;
        long idx = strtol(arg, &end, 10);
        if (end != arg && !*end) {              // arg is a pure number
            e = romfs_entry((int)idx);
            if (!e) { printf("no ROM at index %ld (see LIST)\r\n", idx); return; }
        } else {                                // arg is an exact filename
            e = romfs_find(arg);
            if (!e) {
                fputs("not found — use PLAY <number> from LIST "
                      "(long names are painful to type)\r\n", stdout);
                return;
            }
        }
        printf("selecting %s...\r\n", e->file);
        select_rom(e->file);
        return;
    }

    if (!strcasecmp(line, "BASIC")) {
        fputs(select_basic() ? "booting to Extended BASIC...\r\n"
                             : "busy\r\n", stdout);
        return;
    }

    if (!strcasecmp(line, "SET")) {
        if (!arg) { fputs("usage: SET <key> <value>\r\n", stdout); return; }
        char *val = strchr(arg, ' ');
        if (val) { *val++ = 0; while (*val == ' ') val++; }
        cfg_t *c = cfg_get();

        if (!strcasecmp(arg, "SSID") && val) {
            snprintf(c->wifi.ssid, sizeof(c->wifi.ssid), "%s", val);
        } else if (!strcasecmp(arg, "PSK") && val) {
            snprintf(c->wifi.psk, sizeof(c->wifi.psk), "%s", val);
        } else if (!strcasecmp(arg, "AUTH") && val) {
            cfg_auth_t a;
            if (!cfg_auth_parse(val, &a)) { fputs("bad auth\r\n", stdout); return; }
            c->wifi.auth = a;
        } else if (!strcasecmp(arg, "WIFI") && val) {
            if      (!strcasecmp(val, "ON"))  c->wifi.enabled = 1;
            else if (!strcasecmp(val, "OFF")) c->wifi.enabled = 0;
            else { fputs("want ON or OFF\r\n", stdout); return; }
        } else if (!strcasecmp(arg, "STROBE") && val) {
            // Reject out-of-range rather than clamping: this console can
            // reach values the OLED editor cannot display or correct, so
            // silently accepting one would strand the setting.
            uint32_t v = (uint32_t)strtoul(val, NULL, 10);
            if (v < MC_STROBE_DELAY_NS_MIN || v > MC_STROBE_DELAY_NS_MAX) {
                printf("want %u..%u ns\r\n",
                       (unsigned)MC_STROBE_DELAY_NS_MIN,
                       (unsigned)MC_STROBE_DELAY_NS_MAX);
                return;
            }
            c->strobe_delay_ns = v;
            fputs("takes effect after SAVE + REBOOT\r\n", stdout);
        } else {
            fputs("unknown key\r\n", stdout);
            return;
        }
        fputs("ok (SAVE to persist)\r\n", stdout);
        return;
    }

    if (!strcasecmp(line, "SAVE")) {
        fputs(cfg_save() ? "saved\r\n" : "SAVE FAILED\r\n", stdout);
        return;
    }
    if (!strcasecmp(line, "WIPE")) {
        fputs(cfg_wipe() ? "wiped\r\n" : "WIPE FAILED\r\n", stdout);
        return;
    }
    if (!strcasecmp(line, "REBOOT")) {
        fputs("rebooting\r\n", stdout);
        fflush(stdout);
        watchdog_reboot(0, 0, 50);
        for (;;) tight_loop_contents();
    }
    if (!strcasecmp(line, "EXIT")) {
        s_active = false;
        fputs("resuming\r\n", stdout);
        return;
    }
    fputs("unknown command (HELP)\r\n", stdout);
}

void cdcmenu_pump(void) {
    int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT) return;

    if (!s_active) {
        s_active = true;
        s_len = 0;
        fputs("\r\n" MC_BOARD_TITLE " configuration\r\n"
              "HELP for commands, EXIT to resume.\r\n", stdout);
        show_config();
        prompt();
        if (ch == '\r' || ch == '\n') return;
    }

    if (ch == '\r' || ch == '\n') {
        fputs("\r\n", stdout);
        s_line[s_len] = 0;
        handle(s_line);
        s_len = 0;
        if (s_active) prompt();
        return;
    }
    if (ch == 8 || ch == 127) {              // backspace
        if (s_len) { s_len--; fputs("\b \b", stdout); fflush(stdout); }
        return;
    }
    if (ch >= 32 && ch < 127 && s_len < LINE_BUF - 1) {
        s_line[s_len++] = (char)ch;
        putchar(ch);
        fflush(stdout);
    }
}
