#ifndef MULTICART_WEB_H
#define MULTICART_WEB_H

// HTTP server + JSON API + embedded UI page.
// web_start() is called once by net.c when the link comes up;
// web_pump() must be called from the core-0 loop — that is where all
// parsing and flash access happens (see the threading rule in web.c).

void web_start(void);
void web_pump(void);

#endif
