#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Poll the six physical navigation keys. Must run from the LVGL loop. */
void bsp_wio_buttons_poll(void);

/* Advance delayed WiFi reconnect work without blocking the LVGL loop. */
void bsp_wio_wifi_poll(void);

/* The RTL8720DN RPC transport cannot reliably scan and service sockets at the
   same time. The Moonraker worker uses this handshake to yield the transport
   before bsp_wifi_scan_poll() starts the actual scan. */
bool bsp_wio_wifi_scan_requested(void);
void bsp_wio_wifi_network_idle(bool idle);

#ifdef __cplusplus
}
#endif
