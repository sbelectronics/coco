#ifndef MULTICART_UI_H
#define MULTICART_UI_H

#include <stdbool.h>

// OLED menu + EC12E encoder. Rotate = scroll, push = select,
// long-push = per-ROM settings.

void ui_init(void);
void ui_pump(void);              // call from the core-0 loop
void ui_notify_list_changed(void); // web UI added/removed a ROM
void ui_set_ip(const char *ip);    // shown in the header when connected

#endif
