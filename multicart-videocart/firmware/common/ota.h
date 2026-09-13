#ifndef MULTICART_OTA_H
#define MULTICART_OTA_H

#include <stdint.h>
#include <stdbool.h>

// Firmware update over the web UI.
//
// Scheme: the uploaded .bin is staged into flash slot B with a header
// (magic + length + CRC32). On the next boot, if a valid *and different*
// staged image is present, a RAM-resident copier moves B -> A and
// reboots into it.
//
// Honest risk note: the B->A copy takes ~10-15 s with interrupts off.
// Power loss inside that window leaves slot A incomplete and the board
// needs BOOTSEL + UF2 recovery. The CRC check means a bad upload is
// never copied, so the only exposure is physical power loss during the
// copy — which the web UI warns about.

bool ota_begin(uint32_t len);          // len = bytes of firmware to come
bool ota_chunk(const void *data, uint32_t len);
bool ota_end(void);                    // finalize + write header
void ota_abort(void);
bool ota_active(void);

// Called after the HTTP 200 has been flushed: reboots into the copier.
void ota_commit_after_response(void);
void ota_pump(void);                   // performs the deferred reboot

// Called early in main(): if a valid staged image is pending, copy it
// over slot A and reboot. Never returns in that case.
void ota_apply_pending_if_any(void);

// Why the LAST boot did not apply a staged image. An OTA that stages
// cleanly, reboots and then quietly declines is otherwise indistinguish-
// able from one that never happened: the web page has already said
// "rebooting" by then, and nothing ever contradicts it.
typedef enum {
    OTA_WHY_NONE = 0,       // nothing was staged
    OTA_WHY_BADLEN,         // header length absurd
    OTA_WHY_CRC,            // image did not match its header crc
    OTA_WHY_SAME,           // already running this exact image
    OTA_WHY_APPLIED,        // copied (only visible if the copy then failed)
} ota_why_t;

uint8_t  ota_last_apply(void);     // ota_why_t
uint32_t ota_staged_len(void);     // length from the staged header
uint32_t ota_staged_crc(void);     // crc the header claims
uint32_t ota_image_crc(void);      // crc the staged image actually has
uint32_t ota_wire_crc(void);       // crc of the last upload off the wire
uint32_t ota_readback_crc(void);   // ...and of the same bytes read back


#endif
