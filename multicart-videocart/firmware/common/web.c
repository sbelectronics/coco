// ======================================================================
// web.c — minimal HTTP/1.1 server on raw lwIP (piconet-style: no
// lwip httpd, no filesystem images).
//
// Threading rule: the lwIP callbacks (background context under
// pico_cyw43_arch_lwip_threadsafe_background) do NOTHING but queue
// pbufs and set flags. All parsing, all flash access and all tcp_write
// happen in web_pump() on the core-0 loop, wrapped in
// cyw43_arch_lwip_begin/end. That keeps multi-millisecond flash erases
// out of interrupt context, and TCP backpressure falls out naturally:
// we only call tcp_recved() once bytes have actually been consumed.
// ======================================================================

#include "web.h"
#include "config.h"
#include "romfs.h"
#include "select.h"
#include "bus.h"
#include "expander.h"
#include "cfg.h"
#include "net.h"
#include "ui.h"
#include "ota.h"
#include "webui.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/watchdog.h"
#include "lwip/tcp.h"
#include "lwip/pbuf.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>

#define MAX_CONNS       4
#define HDR_MAX         1280       // browser fetch() headers run 400-800 B
#define TX_MAX          20480      // whole-response buffer (ROM list)

typedef enum { PH_HDR, PH_ROUTE, PH_BODY, PH_SEND, PH_CLOSE } phase_t;
typedef enum { BODY_NONE, BODY_ROM, BODY_OTA } body_t;

typedef struct {
    struct tcp_pcb *pcb;
    bool     used;
    bool     peer_closed;
    uint8_t  hdr_bad;          // 1 = overflow (431), 2 = malformed (400)
    uint8_t  idle_polls;       // bumped by on_poll, cleared by on_recv

    struct pbuf *rx;
    uint16_t rx_off;

    phase_t  phase;
    char     hdr[HDR_MAX];
    uint16_t hdrlen;

    uint16_t hdr_end;          // offset of the first body byte in hdr[]
    char     method[8];
    char     path[192];
    uint32_t clen, got;
    body_t   body;
    bool     body_ok;

    const char *tx;
    uint32_t    txlen, txoff;
} conn_t;

static conn_t   s_conns[MAX_CONNS];
static struct tcp_pcb *s_listen;

// One response buffer, exclusively owned. TX_MAX responses can outlive a
// pump call (they may exceed the TCP send buffer), so a connection must
// hold the buffer from build to close; others wait in PH_ROUTE.
static char     s_tx[TX_MAX];
static conn_t  *s_tx_owner;

static bool tx_acquire(conn_t *c) {
    if (s_tx_owner && s_tx_owner != c) return false;
    s_tx_owner = c;
    return true;
}

static void tx_release(conn_t *c) {
    if (s_tx_owner == c) s_tx_owner = NULL;
}

// ------------------------------------------------------------ helpers --

static void conn_release(conn_t *c) {
    if (c->rx) { pbuf_free(c->rx); c->rx = NULL; }
    if (c->body == BODY_ROM && romfs_upload_active()) romfs_upload_abort();
    if (c->body == BODY_OTA) ota_abort();
    tx_release(c);
    memset(c, 0, sizeof(*c));
}

static void conn_close(conn_t *c) {
    struct tcp_pcb *pcb = c->pcb;
    conn_release(c);
    if (pcb) {
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0);
        if (tcp_close(pcb) != ERR_OK) tcp_abort(pcb);
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            *o++ = (char)(hexval(s[1]) * 16 + hexval(s[2]));
            s += 2;
        } else if (*s == '+') {
            *o++ = ' ';
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

// Filename filter. A narrow allowlist wrongly rejects real cartridge
// names like "Clowns & Balloons ...ccc" and "EDTASM+ ...ccc", so instead
// block only what actually causes trouble: path traversal ('/', '\',
// ".."), control chars, and the characters that would break the JSON we
// emit unescaped ('"') or the catalog's '|' delimiter. Everything else
// printable — & + ' ! [ ] etc. — is allowed. (The web UI builds its DOM
// via createElement, never by interpolating the name into markup, so
// quoting is not a concern on that side; see webui.h.)
static bool name_ok(const char *n) {
    size_t len = strlen(n);
    if (!len || len >= ROMFS_NAME_MAX) return false;
    if (strstr(n, "..")) return false;
    for (const char *p = n; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < 0x20) return false;                 // control chars
        if (ch == '/' || ch == '\\') return false;   // path separators
        if (ch == '"' || ch == '|') return false;    // JSON / catalog
    }
    return true;
}

// The body is built at s_tx + HDR_RESERVE and the header is written
// backwards to butt against it, so a response needs exactly one buffer.
#define HDR_RESERVE  160
static char    *body_ptr(void) { return s_tx + HDR_RESERVE; }
static uint32_t body_max(void) { return TX_MAX - HDR_RESERVE; }

static void resp_finish(conn_t *c, const char *status, const char *ctype,
                        uint32_t blen) {
    // Callers pass an snprintf() return, which is the length it WANTED to
    // write, not what fit. Clamping here (rather than at each call site)
    // means a body that outgrows the buffer is truncated instead of
    // transmitting whatever follows it in memory.
    if (blen > body_max()) blen = body_max();
    char h[HDR_RESERVE];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
                     "Content-Length: %u\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     status, ctype, (unsigned)blen);
    if (n < 0 || n >= HDR_RESERVE) n = 0;     // cannot happen; stay safe
    memcpy(s_tx + HDR_RESERVE - n, h, n);
    c->tx    = s_tx + HDR_RESERVE - n;
    c->txlen = (uint32_t)n + blen;
    c->txoff = 0;
    c->phase = PH_SEND;
}

static void resp_static(conn_t *c, const char *status, const char *ctype,
                        const char *body, uint32_t blen) {
    if (blen > body_max()) blen = body_max();
    memcpy(body_ptr(), body, blen);
    resp_finish(c, status, ctype, blen);
}

static void resp_text(conn_t *c, const char *status, const char *msg) {
    resp_static(c, status, "text/plain", msg, strlen(msg));
}

// ------------------------------------------------------------ routing --

// The renderer's DECODED state, videocart only. The $FF22/SAM values a
// cart programs are not evidence of what was decoded from the bus, so
// the decode itself is published and the two can be compared instead of
// assumed.
#if MC_BOARD_VIDEOCART
#include "video.h"
#include "audio.h"
#define VID_FMT  "\"vmode\":%u,\"vbase\":%u,\"vfrm\":%u,\"vregw\":%u," \
                 "\"vrate\":%u," \
                 "\"aev\":%u,\"asmp\":%u,\"ares\":%u,\"alate\":%u," \
                 "\"adep\":%u,\"aring\":%u,\"astate\":%u,\"aisl\":%u,\"adrop\":%u," \
                 "\"anull\":%u,\"atest\":%u,\"ifix\":%u," \
                 "\"agate\":%u,\"amuxoff\":%u,\"amux\":%u,\"agedge\":%u,\"afilt\":%u," "\"apeak\":%u,\"aclip\":%u,"
#define VID_ARGS (unsigned)vdg_mode_word(), (unsigned)vdg_sam_offset(), \
                 (unsigned)vdg_frames(), (unsigned)vdg_reg_writes(), \
                 (unsigned)vdg_mode_rate(), \
                 (unsigned)audio_events(),  (unsigned)audio_samples(), \
                 (unsigned)audio_resyncs(), (unsigned)audio_late(), \
                 (unsigned)bus_audio_depth(), (unsigned)bus_audio_ring_size(), \
                 (unsigned)audio_state(), (unsigned)hstx_island_lines(), \
                 (unsigned)d.aud_drop, \
                 (unsigned)audio_nulls(), (unsigned)audio_test(), \
                 (unsigned)hstx_island_fixes(), \
                 (unsigned)audio_gate_off(), (unsigned)audio_mux_off(), \
                 (unsigned)audio_mux_edges(), (unsigned)audio_gate_edges(), \
                 (unsigned)audio_filter(), \
                 (unsigned)audio_peak(), (unsigned)audio_clips(),
#else
#define VID_FMT  ""
#define VID_ARGS
#endif

static void api_status(conn_t *c) {
    bus_debug_t d;
    bus_debug(&d);
    char *body = body_ptr();
    int n = snprintf(body, body_max(),
        "{\"active\":\"%s\",\"status\":\"%s\",\"free\":%u,\"total\":%u,"
        "\"roms\":%d,\"ip\":\"%s\",\"fw\":\"%s\",\"busy\":%s,"
        "\"bus\":{\"strobes\":%u,\"bank\":%u,\"raw\":\"%08x\","
        "\"laps\":%u,\"drives\":%u,\"serving\":%u,"
        "\"writes\":%u,\"caplate\":%u,\"scsmism\":%u,"
        "\"svstall\":%u,\"pairdrop\":%u,\"tagslip\":%u," "\"tguard\":%u,\"tretry\":%u,\"rstv\":%u,"
        "\"lwait\":%u,\"lbody\":%u,\"lbmax\":%u,"
        "\"svlook\":%u,\"tblok\":%u,\"lag\":%u,\"svfifo\":%u,\"tagdiag\":%u,"
        "\"lagnow\":%u,\"laghi\":%u,\"pairhi\":%u,"
        "\"eok\":%u,\"ebad\":%u,\"ediag\":%u,\"afix\":%u,\"adist\":%u," \
        "\"owhy\":%u,\"olen\":%u,\"ohcrc\":%u,\"oicrc\":%u," \
        "\"owire\":%u,\"orb\":%u,"
        "\"ty\":%u,\"tyf\":%u,"
        "\"stubok\":%u,\"stubto\":%u,"
        VID_FMT
        "\"fifo\":%u,\"delay\":%u,"
        "\"xpok\":%u,\"xppins\":%u,"
        "\"dma\":[\"%08x\",\"%08x\",\"%08x\",\"%08x\"]}}",
        select_active(), select_status(),
        (unsigned)romfs_free_bytes(), (unsigned)romfs_total_bytes(),
        romfs_count(), net_ip(), MC_FW_VERSION,
        select_busy() ? "true" : "false",
        (unsigned)d.strobe_count, (unsigned)d.current_bank,
        (unsigned)d.strobe_raw, (unsigned)d.lap_count,
        (unsigned)d.drive_count, (unsigned)d.serving,
        (unsigned)bus_write_count(), (unsigned)bus_capture_behind(),
        (unsigned)bus_scs_disagree(),
        (unsigned)d.serve_stall, (unsigned)d.pair_drop,
        (unsigned)d.tag_slip, (unsigned)d.tag_guard, (unsigned)d.tag_retry,
        (unsigned)d.reset_vecs,
        (unsigned)d.lap_wait_ns, (unsigned)d.lap_body_ns,
        (unsigned)d.lap_bmax_ns,
        (unsigned)d.serve_look, (unsigned)d.tbl_ok,
        (unsigned)d.snoop_lag, (unsigned)d.serve_fifo,
        (unsigned)d.tag_diag,
        (unsigned)d.lag_now, (unsigned)d.lag_hi, (unsigned)d.pair_hi,
        (unsigned)d.echo_ok, (unsigned)d.echo_bad, (unsigned)d.echo_diag,
        (unsigned)d.anchor_fix, (unsigned)d.anchor_dist,
        (unsigned)ota_last_apply(), (unsigned)ota_staged_len(),
        (unsigned)ota_staged_crc(), (unsigned)ota_image_crc(),
        (unsigned)ota_wire_crc(), (unsigned)ota_readback_crc(),
        (unsigned)d.sam_ty, (unsigned)d.ty_flips,
        (unsigned)select_stub_ok(), (unsigned)select_stub_timeouts(),
        VID_ARGS
        (unsigned)d.fifo_level, (unsigned)d.strobe_delay_ns,
        expander_ok() ? 1u : 0u, (unsigned)expander_read(),
        (unsigned)d.dma_ctrl, (unsigned)d.dma_read,
        (unsigned)d.dma_write, (unsigned)d.dma_count);
    resp_finish(c, "200 OK", "application/json", (uint32_t)n);
}

static void api_roms(conn_t *c) {
    char    *b   = body_ptr();
    uint32_t max = body_max();
    int n = snprintf(b, max, "{\"roms\":[");
    for (int i = 0; i < romfs_count(); i++) {
        const rom_entry_t *e = romfs_entry(i);
        int r = snprintf(b + n, max - n,
            "%s{\"file\":\"%s\",\"title\":\"%s\",\"size\":%u,"
            "\"autostart\":%u,\"scheme\":%u,\"insert\":%u}",
            i ? "," : "", e->file, e->title, (unsigned)e->size,
            e->autostart, e->scheme, e->insert);
        // Stop before the closing "]}" would not fit.
        if (r < 0 || (uint32_t)(n + r) >= max - 4) break;
        n += r;
    }
    n += snprintf(b + n, max - n, "]}");
    resp_finish(c, "200 OK", "application/json", (uint32_t)n);
}

// /api/mirror?addr=<hex>&len=<dec> — hex dump of the snooped 64K RAM
// mirror. Full-bus boards only (the multicart has no mirror). This is
// the single most useful remote-debug tool on the videocart: it shows
// whether capture is working before the renderer is trusted.
static void api_mirror(conn_t *c, char *query) {
    const uint8_t *m = bus_mirror();
    if (!m) { resp_text(c, "404 Not Found", "no mirror on this board"); return; }

    uint32_t addr = 0, len = 256;
    if (*query == '?') {
        for (char *tok = strtok(query + 1, "&"); tok; tok = strtok(NULL, "&")) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq++ = 0;
            if      (!strcmp(tok, "addr")) addr = (uint32_t)strtoul(eq, NULL, 16);
            else if (!strcmp(tok, "len"))  len  = (uint32_t)strtoul(eq, NULL, 10);
        }
    }
    if (len == 0 || len > 4096) len = 256;
    addr &= 0xFFFFu;
    if (addr + len > 0x10000u) len = 0x10000u - addr;

    char    *b   = body_ptr();
    uint32_t max = body_max();
    int n = 0;
    for (uint32_t i = 0; i < len; i += 16) {
        if ((uint32_t)n + 80 >= max) break;
        n += snprintf(b + n, max - n, "%04X ", (unsigned)(addr + i));
        for (uint32_t j = 0; j < 16 && i + j < len; j++)
            n += snprintf(b + n, max - n, "%02X ", m[(addr + i + j) & 0xFFFFu]);
        n += snprintf(b + n, max - n, "\n");
    }
    resp_finish(c, "200 OK", "text/plain", (uint32_t)n);
}

// /api/meta/<file>?title=..&autostart=0|1&scheme=0|1&insert=0|1
static void api_meta(conn_t *c, char *rest) {
    char *q = strchr(rest, '?');
    char title[ROMFS_TITLE_MAX] = "";
    int  autostart = -1, scheme = -1, insert = -1;
    if (q) {
        *q++ = 0;
        for (char *tok = strtok(q, "&"); tok; tok = strtok(NULL, "&")) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq++ = 0;
            url_decode(eq);
            if      (!strcmp(tok, "title"))     snprintf(title, sizeof(title), "%s", eq);
            else if (!strcmp(tok, "autostart")) autostart = atoi(eq);
            else if (!strcmp(tok, "scheme"))    scheme    = atoi(eq);
            else if (!strcmp(tok, "insert"))    insert    = atoi(eq);
        }
    }
    url_decode(rest);
    if (!name_ok(rest)) { resp_text(c, "400 Bad Request", "bad name"); return; }
    if (romfs_set_meta(rest, title[0] ? title : NULL, autostart, scheme,
                       insert)) {
        ui_notify_list_changed();
        resp_text(c, "200 OK", "ok");
    } else {
        resp_text(c, "404 Not Found", "no such rom");
    }
}

#if MC_BOARD_VIDEOCART
// GET /api/screen -> the live display buffer, as plain hex bytes.
//
// Ground truth for tuning the artifact decoder. Deliberately emitted as
// bare whitespace-separated hex with NO header line, because that is
// exactly what the coco-hdmi NTSC model script eats:
//     int(t, 16) for t in f.read().split()
// so a saved response can be rendered by the reference model with no
// conversion step. The mode and base that go with it are already in
// /api/status as vmode/vbase, and the byte count comes from the geometry
// the last frame actually used -- not recomputed here, so a dump can
// never describe a different screen than the one being displayed.
static void api_screen(conn_t *c) {
    const uint8_t *mem = bus_mirror();
    if (!mem) { resp_text(c, "503 Service Unavailable", "no mirror"); return; }
    uint32_t base = vdg_screen_base();
    uint32_t n    = vdg_screen_bytes();
    if (!n) { resp_text(c, "503 Service Unavailable", "no frame yet"); return; }

    static const char hx[] = "0123456789abcdef";
    char    *b   = body_ptr();
    uint32_t max = body_max();
    uint32_t o   = 0;
    for (uint32_t i = 0; i < n && o + 3 <= max; i++) {
        uint8_t v = mem[(uint16_t)(base + i)];
        b[o++] = hx[v >> 4];
        b[o++] = hx[v & 15];
        b[o++] = ((i & 31u) == 31u) ? '\n' : ' ';
    }
    resp_finish(c, "200 OK", "text/plain", o);
}

// GET /api/pcm -> the last samples sent to the sink, one signed decimal
// per line, oldest first. The spectrum itself, for when the ear cannot
// tell two filter settings apart; it also answers whether a control is
// reaching the synth at all. 4096 samples at 48 kHz is 85 ms, about
// 12 Hz of resolution, and fits the response buffer as decimal text.
static void api_pcm(conn_t *c) {
    char    *b   = body_ptr();
    uint32_t max = body_max();
    uint32_t o   = 0;
    uint32_t n   = audio_pcm_count();
    o += (uint32_t)snprintf(b + o, max - o,
                            "# %u samples @ 48 kHz, oldest first, filter=%u "
                            "onebit=%u gain=%u%s\n", n, audio_filter(),
                            audio_onebit(), audio_gain(),
                            audio_pcm_ready() ? "" : " (PARTIAL: still filling)");
    for (uint32_t i = 0; i < n && o + 10 < max; i++)
        o += (uint32_t)snprintf(b + o, max - o, "%d\n",
                                (int)audio_pcm_at(i));
    // Re-arm only after the window has been read, so what was printed is
    // one coherent stretch rather than a window that slid forward while
    // it was being printed.
    audio_pcm_rearm();
    resp_finish(c, "200 OK", "text/plain", o);
}

// GET /api/audiolog -> every audio-register write still held, oldest
// first, decoded: lap stamp, register, value, and the sound state each
// one produced. "live" is the important column: it is whether that write
// left the DAC actually connected to the speaker.
static void api_audiolog(conn_t *c) {
    // reg 6 is a SAM write. The SAM has no data bus -- the ADDRESS written
    // is the command, an even one clearing a bit and the odd one above it
    // setting it -- so it is decoded, not printed as a value.
    static const char *const SAMN[10] = {
        "V0", "V1", "V2", "F0", "F1", "F2", "F3", "F4", "F5", "F6"
    };
    static const char *const RN[7] = {
        "FF20 dac ", "FF21 cra ", "FF22 pb  ", "FF23 crb ", "FF01 sel1",
        "FF03 sel2", "SAM      "
    };
    char    *b   = body_ptr();
    uint32_t max = body_max();
    uint32_t o   = 0;
    uint32_t n   = audio_log_count();

    // VIDEO events first, and in their own list. The sound flood cannot
    // evict them, so this section spans the whole session even when the
    // one below covers a quarter of a second.
    {
        static const char *const SN[10] = {
            "V0", "V1", "V2", "F0", "F1", "F2", "F3", "F4", "F5", "F6"
        };
        uint32_t vn = audio_vlog_count();
        o += (uint32_t)snprintf(b + o, max - o,
                                "# VIDEO events: %u (SAM writes, and $FF22 "
                                "writes that moved the mode bits)\n", vn);
        for (uint32_t i = 0; i < vn && o + 80 < max; i++) {
            uint32_t ev  = audio_vlog_at(i);
            uint32_t lap = (ev >> 12) & 0xFFFFFu;
            uint32_t reg = (ev >> 8) & 0xFu;
            uint32_t val = ev & 0xFFu;
            if (reg == 6u) {
                uint32_t which = val >> 1;
                o += (uint32_t)snprintf(b + o, max - o,
                        "%8u  SAM %04x  %s %s\n", (unsigned)lap,
                        (unsigned)(0xFFC0u + val),
                        which < 10 ? SN[which] : "??",
                        (val & 1u) ? "SET" : "clr");
            } else if (reg == 3u) {
                // CRB bit 2 is what decides whether a $FF22 write is a
                // mode change or a direction-register write at all.
                o += (uint32_t)snprintf(b + o, max - o,
                        "%8u  FF23 crb  %02x  $FF22 is %s\n",
                        (unsigned)lap, (unsigned)val,
                        (val & 0x04u) ? "the OUTPUT register (modes count)"
                                      : "the DIRECTION register (modes ignored)");
            } else {
                o += (uint32_t)snprintf(b + o, max - o,
                        "%8u  FF22 mode %02x  A/G=%u GM=%u CSS=%u\n",
                        (unsigned)lap, (unsigned)val,
                        (unsigned)((val >> 7) & 1u),
                        (unsigned)((val >> 4) & 7u),
                        (unsigned)((val >> 3) & 1u));
            }
        }
    }

    o += (uint32_t)snprintf(b + o, max - o,
                            "\n# %u audio events, oldest first\n"
                            "# lap      reg        val  dac gate mux 1bit live\n", n);

    // The state is RECORDED with each event, not replayed here. Replaying
    // from power-on defaults is only correct when every register involved
    // was written inside the window, and a program that set $FF23 before
    // the window opened would read back as "gate 0" throughout.
    for (uint32_t i = 0; i < n && o + 80 < max; i++) {
        uint32_t ev  = audio_log_at(i);
        uint32_t st  = audio_log_state(i);
        uint32_t lap = (ev >> 12) & 0xFFFFFu;
        uint32_t reg = (ev >> 8) & 0xFu;
        uint32_t val = ev & 0xFFu;
        uint32_t dac  = (st >> 10) & 0x3Fu;
        uint32_t gate = (st >> 3) & 1u;
        uint32_t sel  = (st >> 1) & 3u;
        uint32_t ob   = st & 1u;
        if (reg == 6u) {
            // $FFC0+val: bit pairs, even clears and odd sets.
            uint32_t which = val >> 1;
            o += (uint32_t)snprintf(b + o, max - o,
                                    "%8u  SAM %04x  %s %s\n",
                                    (unsigned)lap,
                                    (unsigned)(0xFFC0u + val),
                                    which < 10 ? SAMN[which] : "??",
                                    (val & 1u) ? "SET" : "clr");
            continue;
        }
        o += (uint32_t)snprintf(b + o, max - o,
                                "%8u  %s  %02x   %2u   %u   %u    %u    %u\n",
                                (unsigned)lap, RN[reg < 7 ? reg : 0],
                                (unsigned)val, (unsigned)dac, (unsigned)gate,
                                (unsigned)sel, (unsigned)ob,
                                (unsigned)(gate && sel == 0u));
    }
    resp_finish(c, "200 OK", "text/plain", o);
}
#endif

// GET /api/settings -> current device settings
static void api_settings_get(conn_t *c) {
    const cfg_t *cf = cfg_get();
    char *body = body_ptr();
    int n = snprintf(body, body_max(),
        "{\"wifi\":%u,\"strobe\":%u,\"artifact\":%u,"
        "\"hdmiaudio\":%u,\"audiogain\":%u," "\"audiofilter\":%u,\"audioonebit\":%u}",
        cf->wifi.enabled ? 1u : 0u, (unsigned)cf->strobe_delay_ns,
        (unsigned)cf->artifact,
        (unsigned)cf->hdmi_audio, (unsigned)cf->audio_gain,
        (unsigned)cf->audio_filter, (unsigned)cf->audio_onebit);
    resp_finish(c, "200 OK", "application/json", (uint32_t)n);
}

#if MC_BOARD_VIDEOCART
// POST /api/afilt?n=0..3 -- the analogue output model's low-pass. Live,
// not persisted: what the CoCo's audio path really did to these edges is
// a judgement the ear has to make, so it has to be turnable while the
// sound is playing. Replies with the state actually in force.
static void api_afilt(conn_t *c, char *rest) {
    char *q = rest;
    if (*q == '?') q++;
    uint8_t n = 0;
    for (char *tok = strtok(q, "&"); tok; tok = strtok(NULL, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(tok, "n")) { int v = atoi(eq);
                                 n = (v < 0 || v > 3) ? 0u : (uint8_t)v; }
    }
    audio_set_filter(n);
    { char s[2] = { (char)(0x30 + (audio_filter() & 3u)), 0 };
      resp_text(c, "200 OK", s); }
}

// POST /api/atest?on=0|1|2 -- the diagnostic tone.
//
// Its OWN endpoint rather than a /api/settings field, for two reasons.
// /api/settings ends in cfg_save(), and a flash write holds the island
// refill IRQ off for tens of ms: paying that to toggle a probe would put
// a real audio dropout exactly where the probe is meant to be looking.
// And the tone must not survive a reboot, so it has no business in cfg.
//
// Replies with the state actually in force, so the page can report what
// the FIRMWARE says rather than what it hoped.
static void api_atest(conn_t *c, char *rest) {
    char *q = rest;
    if (*q == '?') q++;
    uint8_t on = 0;
    for (char *tok = strtok(q, "&"); tok; tok = strtok(NULL, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        // 0 = off, 1 = 1 kHz, 2 = 500 Hz.
        if (!strcmp(tok, "on")) { int v = atoi(eq);
                                  on = (v < 0 || v > 2) ? 0u : (uint8_t)v; }
    }
    audio_set_test(on);
    { char s[2] = { (char)(0x30 + (audio_test() & 3u)), 0 };
      resp_text(c, "200 OK", s); }
}
#endif

// POST /api/settings?wifi=0|1&strobe=<ns>&...&reboot=0|1
// WiFi and strobe only take effect after a reboot (staged), so the UI
// exposes an explicit "Save & Reboot". A deferred watchdog reboot lets
// this response flush to the browser first; nothing pets the watchdog.
static void api_settings_post(conn_t *c, char *rest) {

    cfg_t *cf = cfg_get();
    int do_reboot = 0;
    char *q = rest;
    if (*q == '?') q++;
    for (char *tok = strtok(q, "&"); tok; tok = strtok(NULL, "&")) {
        char *eq = strchr(tok, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(tok, "wifi")) {
            cf->wifi.enabled = atoi(eq) ? 1 : 0;
        } else if (!strcmp(tok, "strobe")) {
            uint32_t v = (uint32_t)strtoul(eq, NULL, 10);
            if (v < MC_STROBE_DELAY_NS_MIN) v = MC_STROBE_DELAY_NS_MIN;
            if (v > MC_STROBE_DELAY_NS_MAX) v = MC_STROBE_DELAY_NS_MAX;
            cf->strobe_delay_ns = v;
        } else if (!strcmp(tok, "artifact")) {
            // Applied LIVE, unlike wifi/strobe. Which phase is correct
            // can only be judged by looking at the screen, so comparing
            // them must not cost a reboot -- let alone a reflash.
            uint32_t v = (uint32_t)strtoul(eq, NULL, 10);
            if (v > 2) v = 0;
            cf->artifact = (uint8_t)v;
#if MC_BOARD_VIDEOCART
            vdg_set_artifact(cf->artifact);
#endif
        } else if (!strcmp(tok, "hdmiaudio")) {
            // STAGED, unlike artifact and gain: the descriptor table's
            // shape differs between DVI and HDMI (island lines scan as
            // two blocks), and it is built once in hstx_init().
            cf->hdmi_audio = atoi(eq) ? 1 : 0;
        } else if (!strcmp(tok, "audiofilter")) {
            uint32_t v = (uint32_t)strtoul(eq, NULL, 10);
            if (v > 3) v = 0;
            cf->audio_filter = (uint8_t)v;
#if MC_BOARD_VIDEOCART
            audio_set_filter(cf->audio_filter);  // live: judged by ear
#endif
        } else if (!strcmp(tok, "audioonebit")) {
            uint32_t v = (uint32_t)strtoul(eq, NULL, 10);
            if (v > 3) v = 3;
            cf->audio_onebit = (uint8_t)v;
#if MC_BOARD_VIDEOCART
            audio_set_onebit(cf->audio_onebit);  // live: judged by ear
#endif
        } else if (!strcmp(tok, "audiogain")) {
            uint32_t v = (uint32_t)strtoul(eq, NULL, 10);
            if (v > 3) v = 3;
            cf->audio_gain = (uint8_t)v;
#if MC_BOARD_VIDEOCART
            audio_set_gain(cf->audio_gain);      // live: judged by ear
#endif
        } else if (!strcmp(tok, "reboot")) {
            do_reboot = atoi(eq);
        }
    }
    if (!cfg_save()) { resp_text(c, "500 Internal Server Error", "save failed"); return; }
    if (do_reboot) {
        resp_text(c, "200 OK", "saved; rebooting");
        watchdog_reboot(0, 0, 1200);        // fires after the response flushes
        return;
    }
    resp_text(c, "200 OK", "saved");
}

static void route(conn_t *c) {
    char *p = c->path;

    if (!strcmp(c->method, "GET")) {
        if (!strcmp(p, "/") || !strcmp(p, "/index.html")) {
            resp_static(c, "200 OK", "text/html", webui_html,
                        sizeof(webui_html) - 1);
            return;
        }
        if (!strcmp(p, "/api/status"))   { api_status(c);       return; }
        if (!strcmp(p, "/api/settings")) { api_settings_get(c); return; }
#if MC_BOARD_VIDEOCART
        if (!strcmp(p, "/api/audiolog")) { api_audiolog(c);     return; }
        if (!strcmp(p, "/api/pcm"))      { api_pcm(c);          return; }
        if (!strcmp(p, "/api/screen"))   { api_screen(c);       return; }
#endif
        if (!strcmp(p, "/api/roms"))     { api_roms(c);         return; }
        if (!strncmp(p, "/api/mirror", 11)) { api_mirror(c, p + 11); return; }
        resp_text(c, "404 Not Found", "not found");
        return;
    }

    if (!strcmp(c->method, "POST")) {
        if (!strcmp(p, "/api/basic")) {
            if (select_basic()) resp_text(c, "200 OK", "booting to BASIC");
            else                resp_text(c, "409 Conflict", "busy");
            return;
        }
        if (!strcmp(p, "/api/reset")) {
            // Remote reset: re-run the active cart's cold-boot + reload
            // (pulses !RESET exactly like the physical button). With no
            // cart active, cold-boot to BASIC instead.
            const char *a = select_active();
            bool ok = (a && a[0]) ? select_rom(a) : select_basic();
            if (ok) resp_text(c, "200 OK", "resetting");
            else    resp_text(c, "409 Conflict", "busy");
            return;
        }
        if (!strncmp(p, "/api/select/", 12)) {
            char *f = p + 12;
            url_decode(f);
            if (!name_ok(f)) { resp_text(c, "400 Bad Request", "bad name"); return; }
            if (select_rom(f)) resp_text(c, "200 OK", "selecting");
            else               resp_text(c, "409 Conflict", "busy or missing");
            return;
        }
        if (!strncmp(p, "/api/meta/", 10)) { api_meta(c, p + 10); return; }
        if (!strncmp(p, "/api/settings", 13)) { api_settings_post(c, p + 13); return; }
#if MC_BOARD_VIDEOCART
        if (!strncmp(p, "/api/atest", 10)) { api_atest(c, p + 10); return; }
        if (!strncmp(p, "/api/afilt", 10)) { api_afilt(c, p + 10); return; }
#endif

        if (!strncmp(p, "/api/roms/", 10)) {
            char *f = p + 10;
            url_decode(f);
            if (!name_ok(f))  { resp_text(c, "400 Bad Request", "bad name"); return; }
            if (!c->clen)     { resp_text(c, "411 Length Required", "need length"); return; }
            if (c->clen > MC_IMAGE_BUF_BYTES) {
                // Larger than the serve buffer would ever load.
                resp_text(c, "413 Payload Too Large", "max 128K"); return;
            }
            if (c->clen > romfs_free_bytes()) {
                resp_text(c, "507 Insufficient Storage", "no space"); return;
            }
            if (!romfs_upload_begin(f)) {
                resp_text(c, "500 Internal Server Error", "open failed"); return;
            }
            c->body    = BODY_ROM;
            c->body_ok = true;
            c->phase   = PH_BODY;
            return;
        }
        if (!strcmp(p, "/api/ota")) {
            if (!c->clen) { resp_text(c, "411 Length Required", "need length"); return; }
            if (select_busy()) {
                // Mirror of select_rom()'s ota_active() guard: an OTA's
                // flash erases could stall the stub-swap window.
                resp_text(c, "409 Conflict", "selection in progress"); return;
            }
            if (!ota_begin(c->clen)) {
                resp_text(c, "500 Internal Server Error", "ota open failed"); return;
            }
            c->body    = BODY_OTA;
            c->body_ok = true;
            c->phase   = PH_BODY;
            return;
        }
        resp_text(c, "404 Not Found", "not found");
        return;
    }

    if (!strcmp(c->method, "DELETE")) {
        if (!strncmp(p, "/api/roms/", 10)) {
            char *f = p + 10;
            url_decode(f);
            if (!name_ok(f)) { resp_text(c, "400 Bad Request", "bad name"); return; }
            if (romfs_delete(f)) {
                ui_notify_list_changed();
                resp_text(c, "200 OK", "deleted");
            } else {
                resp_text(c, "404 Not Found", "no such rom");
            }
            return;
        }
        resp_text(c, "404 Not Found", "not found");
        return;
    }

    resp_text(c, "405 Method Not Allowed", "method");
}

// ------------------------------------------------------- header parse --

static bool parse_headers(conn_t *c) {
    char *end = strstr(c->hdr, "\r\n\r\n");
    if (!end) return false;
    // Anything we already pulled in past the blank line is body data.
    c->hdr_end = (uint16_t)((end - c->hdr) + 4);
    *end = 0;

    // Request line. No response is built here — the shared tx buffer is
    // not ours yet; PH_ROUTE turns hdr_bad into the actual reply.
    char *sp1 = strchr(c->hdr, ' ');
    if (!sp1) { c->hdr_bad = 2; return true; }
    *sp1 = 0;
    snprintf(c->method, sizeof(c->method), "%s", c->hdr);
    char *sp2 = strchr(sp1 + 1, ' ');
    if (sp2) *sp2 = 0;
    snprintf(c->path, sizeof(c->path), "%s", sp1 + 1);

    // Content-Length (case-insensitive scan over the remaining headers).
    c->clen = 0;
    for (char *l = (sp2 ? sp2 + 1 : end); l && l < end; ) {
        char *nl = strstr(l, "\r\n");
        if (!nl) break;
        *nl = 0;
        if (!strncasecmp(l, "Content-Length:", 15))
            c->clen = (uint32_t)strtoul(l + 15, NULL, 10);
        *nl = '\r';
        l = nl + 2;
    }
    return true;
}

// ---------------------------------------------------------------- rx --

// Copy up to `max` bytes out of the queued pbuf chain.
// (Not "pbuf_take" — that is an lwIP API function.)
static uint32_t rx_pull(conn_t *c, void *dst, uint32_t max) {
    uint32_t took = 0;
    while (c->rx && took < max) {
        uint16_t avail = c->rx->len - c->rx_off;
        if (!avail) {
            struct pbuf *n = c->rx->next;
            if (n) pbuf_ref(n);
            pbuf_free(c->rx);
            c->rx = n;
            c->rx_off = 0;
            continue;
        }
        uint32_t n = (max - took < avail) ? (max - took) : avail;
        if (dst) memcpy((uint8_t *)dst + took,
                        (const uint8_t *)c->rx->payload + c->rx_off, n);
        c->rx_off += n;
        took += n;
    }
    return took;
}

// Body bytes: first whatever over-read into hdr[], then the pbuf chain.
// Only pbuf bytes are acked here — the hdr[] remainder was acked when it
// was read in during PH_HDR.
static uint32_t body_take(conn_t *c, void *dst, uint32_t max,
                          uint32_t *ack_out) {
    uint32_t took = 0;
    *ack_out = 0;

    uint32_t pre = (c->hdrlen > c->hdr_end) ? (c->hdrlen - c->hdr_end) : 0;
    if (pre) {
        uint32_t n = (pre < max) ? pre : max;
        memcpy(dst, c->hdr + c->hdr_end, n);
        c->hdr_end += n;
        took += n;
    }
    if (took < max) {
        uint32_t n = rx_pull(c, (uint8_t *)dst + took, max - took);
        took += n;
        *ack_out = n;
    }
    return took;
}

static void pump_conn(conn_t *c) {
    if (!c->used) return;
    if (!c->pcb) { conn_close(c); return; }    // errored out; reap here

    switch (c->phase) {

    case PH_HDR: {
        uint32_t room = HDR_MAX - 1 - c->hdrlen;
        uint32_t n = rx_pull(c, c->hdr + c->hdrlen, room);
        if (n) {
            c->hdrlen += n;
            c->hdr[c->hdrlen] = 0;
            tcp_recved(c->pcb, n);
            if (parse_headers(c)) {
                c->phase = PH_ROUTE;
            } else if (c->hdrlen >= HDR_MAX - 1) {
                c->hdr_bad = 1;
                c->phase   = PH_ROUTE;
            }
        }
        if (c->phase != PH_ROUTE) break;
    }
    /* fall through */

    case PH_ROUTE: {
        // Responses need the shared tx buffer; wait here until it frees.
        if (!tx_acquire(c)) break;
        if (c->hdr_bad) {
            resp_text(c, (c->hdr_bad == 1)
                             ? "431 Request Header Fields Too Large"
                             : "400 Bad Request",
                      "bad request");
            break;
        }
        route(c);
        // Upload routes respond only at body completion — don't hold the
        // buffer hostage for the (possibly long) duration of the upload.
        if (c->phase == PH_BODY) tx_release(c);
        else break;
    }
    /* fall through */

    case PH_BODY: {
        if (c->phase != PH_BODY) break;    // (route produced a response)
        uint8_t buf[512];
        for (int i = 0; i < 8; i++) {          // bounded work per pump
            uint32_t want = c->clen - c->got;
            if (!want) break;
            if (want > sizeof(buf)) want = sizeof(buf);
            uint32_t ack = 0;
            uint32_t n = body_take(c, buf, want, &ack);
            if (!n) break;
            bool ok = (c->body == BODY_ROM) ? romfs_upload_chunk(buf, n)
                                            : ota_chunk(buf, n);
            if (!ok) c->body_ok = false;
            c->got += n;
            if (ack) tcp_recved(c->pcb, ack);
        }
        if (c->got >= c->clen) {
            if (!tx_acquire(c)) break;     // response needs the buffer
            bool ok = c->body_ok;
            if (c->body == BODY_ROM) {
                // A failed chunk means the file on flash is truncated.
                // Finishing would COMMIT it to the catalog as a
                // selectable ROM that boots garbage — abort deletes it.
                if (ok) ok = romfs_upload_end();
                else    romfs_upload_abort();
                ui_notify_list_changed();
            } else {
                if (ok) ok = ota_end();
                else    ota_abort();
            }
            body_t b = c->body;
            c->body = BODY_NONE;
            if (ok) {
                resp_text(c, "200 OK", "ok");
                if (b == BODY_OTA) ota_commit_after_response();
            } else {
                resp_text(c, "500 Internal Server Error", "write failed");
            }
        }
        break;
    }

    case PH_SEND: {
        // If this response REJECTED a request mid-body (413/507/...),
        // the client is often still streaming. Discard-and-ack what
        // arrives so it can finish and read the error, instead of
        // hitting an RST and surfacing a useless network failure.
        uint32_t drained = rx_pull(c, NULL, 4096);
        if (drained) tcp_recved(c->pcb, drained);

        while (c->txoff < c->txlen) {
            uint32_t left = c->txlen - c->txoff;
            uint16_t space = tcp_sndbuf(c->pcb);
            if (!space) break;
            uint16_t n = (left < space) ? (uint16_t)left : space;
            err_t e = tcp_write(c->pcb, c->tx + c->txoff, n,
                                TCP_WRITE_FLAG_COPY);
            if (e != ERR_OK) break;
            c->txoff += n;
        }
        tcp_output(c->pcb);
        if (c->txoff >= c->txlen) c->phase = PH_CLOSE;
        break;
    }

    case PH_CLOSE:
        conn_close(c);
        break;
    }

    // A FIN only means the peer is done SENDING — a half-closing client
    // (shutdown(WR) after a complete request; every nc/socat smoke test)
    // still expects its response, and a request that arrived whole may
    // still be queued in c->rx. Reap only when no progress is possible:
    //   - waiting for headers that can no longer arrive,
    //   - a body short of Content-Length with nothing left to drain,
    //   - or the 2-minute idle backstop.
    if (c->used && c->peer_closed) {
        bool rx_dry  = (c->rx == NULL);
        bool no_hdr  = (c->phase == PH_HDR) && rx_dry;
        bool starved = (c->phase == PH_BODY) && rx_dry &&
                       (c->hdrlen <= c->hdr_end) && (c->got < c->clen);
        if (no_hdr || starved || c->idle_polls > 30)
            conn_close(c);
    }
}

// ------------------------------------------------------- lwIP callbacks --
// These only queue work; see the threading rule at the top of the file.

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                     err_t err) {
    conn_t *c = (conn_t *)arg;
    if (!c) { if (p) pbuf_free(p); tcp_abort(pcb); return ERR_ABRT; }
    if (err != ERR_OK) { if (p) pbuf_free(p); return err; }
    if (!p) { c->peer_closed = true; return ERR_OK; }
    c->idle_polls = 0;
    if (c->rx) pbuf_cat(c->rx, p);
    else       c->rx = p;
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    (void)err;
    conn_t *c = (conn_t *)arg;
    if (!c) return;
    // Only flag it: releasing here would abort an upload (flash writes)
    // from interrupt context. web_pump() reaps it.
    c->pcb = NULL;              // lwIP has already freed the pcb
    c->peer_closed = true;
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb) {
    conn_t *c = (conn_t *)arg;
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }
    // Idle reaper: ~4 s per poll; a connection that makes no progress
    // for ~2 minutes is dropped so it can't pin a conn slot (or the tx
    // buffer) forever.
    if (++c->idle_polls > 30) c->peer_closed = true;
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) return ERR_VAL;

    conn_t *c = NULL;
    for (int i = 0; i < MAX_CONNS; i++)
        if (!s_conns[i].used) { c = &s_conns[i]; break; }
    if (!c) { tcp_abort(pcb); return ERR_ABRT; }

    memset(c, 0, sizeof(*c));
    c->used  = true;
    c->pcb   = pcb;
    c->phase = PH_HDR;

    tcp_arg(pcb, c);
    tcp_recv(pcb, on_recv);
    tcp_err(pcb, on_err);
    tcp_poll(pcb, on_poll, 8);       // ~4 s
    return ERR_OK;
}

// ------------------------------------------------------------- public --

void web_start(void) {
    cyw43_arch_lwip_begin();
    s_listen = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (s_listen) {
        if (tcp_bind(s_listen, IP_ANY_TYPE, MC_WEB_PORT) == ERR_OK) {
            s_listen = tcp_listen_with_backlog(s_listen, MAX_CONNS);
            if (s_listen) {
                tcp_accept(s_listen, on_accept);
                printf("web: listening on :%d\n", MC_WEB_PORT);
            } else {
                printf("web: listen alloc failed\n");
            }
        } else {
            printf("web: bind failed\n");
            tcp_close(s_listen);
            s_listen = NULL;
        }
    }
    cyw43_arch_lwip_end();
}

void web_pump(void) {
    if (!s_listen) return;
    cyw43_arch_lwip_begin();
    for (int i = 0; i < MAX_CONNS; i++) pump_conn(&s_conns[i]);
    cyw43_arch_lwip_end();
}
