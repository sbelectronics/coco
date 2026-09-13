#ifndef SHIM_PICO_CYW43_ARCH_H
#define SHIM_PICO_CYW43_ARCH_H

// Host shim: cfg.c includes this only for the CYW43_AUTH_* values it maps
// MC_WIFI_AUTH through. No radio is involved, so the constants are all
// that is needed. Values match the SDK's.

#define CYW43_AUTH_OPEN            (0)
#define CYW43_AUTH_WPA_TKIP_PSK    (0x00200002)
#define CYW43_AUTH_WPA2_AES_PSK    (0x00400004)
#define CYW43_AUTH_WPA2_MIXED_PSK  (0x00400006)

#endif
