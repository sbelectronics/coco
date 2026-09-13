// ======================================================================
// net.c — WiFi bring-up (piconet's cfg-driven pattern) + mDNS + web.
//
// Non-blocking: connection attempts are async so the OLED/encoder and
// the bus never wait on the radio. Retries back off to 30 s.
// ======================================================================

#include "net.h"
#include "config.h"
#include "cfg.h"
#include "web.h"
#include "ui.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"

#include <stdio.h>

typedef enum { NS_OFF, NS_INIT, NS_CONNECTING, NS_UP, NS_RETRY } nstate_t;

static nstate_t s_state = NS_OFF;
static absolute_time_t s_next;
static uint32_t s_backoff_ms = 2000;
static bool s_web_started;
static char s_ip[20];

const char *net_ip(void) { return s_ip; }
bool net_up(void) { return s_state == NS_UP; }

static void start_mdns(void) {
#if LWIP_MDNS_RESPONDER
    static bool started;
    if (started) return;
    // lwIP state is touched from the main loop -> must hold the arch lock.
    cyw43_arch_lwip_begin();
    mdns_resp_init();
    mdns_resp_add_netif(netif_default, MC_HOSTNAME);
    mdns_resp_add_service(netif_default, MC_HOSTNAME, "_http",
                          DNSSD_PROTO_TCP, MC_WEB_PORT, NULL, NULL);
    cyw43_arch_lwip_end();
    started = true;
#endif
}

void net_pump(void) {
    cfg_t *c = cfg_get();

    switch (s_state) {

    case NS_OFF: {
        // Local flag, NOT cfg: mutating the live cfg would let an
        // unrelated SAVE silently persist wifi-off.
        static bool s_radio_dead;
        if (s_radio_dead || !c->wifi.enabled || !cfg_is_usable()) return;
        if (cyw43_arch_init()) {
            printf("net: cyw43_arch_init failed\n");
            s_radio_dead = true;               // don't spin on a dead radio
            return;
        }
    }
        cyw43_arch_enable_sta_mode();
        cyw43_arch_lwip_begin();
        netif_set_hostname(netif_default, MC_HOSTNAME);
        cyw43_arch_lwip_end();
        s_state = NS_INIT;
        break;

    case NS_INIT:
        printf("net: connecting to '%s'...\n", c->wifi.ssid);
        // NULL (not "") for open networks — some SDK versions treat any
        // non-NULL key as a request for WPA.
        if (cyw43_arch_wifi_connect_async(c->wifi.ssid,
                                          c->wifi.psk[0] ? c->wifi.psk : NULL,
                                          cfg_auth_to_cyw43(c->wifi.auth))) {
            s_state = NS_RETRY;
            s_next  = make_timeout_time_ms(s_backoff_ms);
            break;
        }
        s_state = NS_CONNECTING;
        s_next  = make_timeout_time_ms(20000);
        break;

    case NS_CONNECTING: {
        int st = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (st == CYW43_LINK_UP) {
            const ip4_addr_t *ip = netif_ip4_addr(netif_default);
            snprintf(s_ip, sizeof(s_ip), "%s", ip4addr_ntoa(ip));
            printf("net: up, http://%s/ (also %s.local)\n",
                   s_ip, MC_HOSTNAME);
            ui_set_ip(s_ip);
            start_mdns();
            if (!s_web_started) { web_start(); s_web_started = true; }
            s_backoff_ms = 2000;
            s_state = NS_UP;
        } else if (st < 0 || absolute_time_diff_us(get_absolute_time(),
                                                   s_next) <= 0) {
            printf("net: connect failed (status %d)\n", st);
            s_state = NS_RETRY;
            s_next  = make_timeout_time_ms(s_backoff_ms);
            if (s_backoff_ms < 30000) s_backoff_ms *= 2;
        }
        break;
    }

    case NS_UP:
        if (cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA)
                != CYW43_LINK_UP) {
            printf("net: link lost\n");
            s_ip[0] = 0;
            ui_set_ip("");           // don't advertise a dead address
            s_state = NS_RETRY;
            s_next  = make_timeout_time_ms(s_backoff_ms);
        }
        break;

    case NS_RETRY:
        if (absolute_time_diff_us(get_absolute_time(), s_next) <= 0)
            s_state = NS_INIT;
        break;
    }
}
