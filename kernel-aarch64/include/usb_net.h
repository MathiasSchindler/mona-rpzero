#pragma once

#include "usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Attempt to bind a USB Ethernet-like device.
 * Returns 0 on success (driver claimed device).
 */
int usb_net_try_bind(const usb_device_t *dev);

/* Poll RX path (bulk IN) and feed frames into netif.
 * Safe to call frequently.
 */
void usb_net_poll(void);

#ifdef __cplusplus
}
#endif
