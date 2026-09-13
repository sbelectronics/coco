#ifndef MULTICART_CDCMENU_H
#define MULTICART_CDCMENU_H

#include <stdbool.h>

// USB-CDC provisioning console (piconet pattern): any keystroke on the
// CDC interface enters the menu; WiFi credentials and the bank-write
// strobe delay are set here and persisted to the flash cfg sector.

void cdcmenu_init(void);
void cdcmenu_pump(void);
bool cdcmenu_active(void);

#endif
