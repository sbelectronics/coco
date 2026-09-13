#ifndef MULTICART_NET_H
#define MULTICART_NET_H

#include <stdbool.h>

// WiFi bring-up driven by cfg (piconet pattern), plus mDNS and the web
// server. Non-blocking; call net_pump() from the core-0 loop.

void        net_pump(void);
bool        net_up(void);
const char *net_ip(void);

#endif
