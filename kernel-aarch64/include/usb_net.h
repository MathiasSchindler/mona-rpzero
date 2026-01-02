#pragma once

#include "usb_host.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	/* RX polling / bus-level outcomes */
	uint64_t rx_poll_calls;
	uint64_t rx_naks;
	uint64_t rx_errors;

	uint64_t rx_usb_xfers;
	uint64_t rx_usb_bytes;

	uint64_t rx_rndis_ok;
	uint64_t rx_rndis_drop_small;
	uint64_t rx_rndis_drop_type;
	uint64_t rx_rndis_drop_bounds;

	uint32_t last_got;
	uint32_t last_msg_type;
	uint32_t last_data_off;
	uint32_t last_data_len;
	uint16_t last_ethertype;
} usb_net_debug_t;

/* Attempt to bind a USB Ethernet-like device.
 * Returns 0 on success (driver claimed device).
 */
int usb_net_try_bind(const usb_device_t *dev);

/* Poll RX path (bulk IN) and feed frames into netif.
 * Safe to call frequently.
 */
void usb_net_poll(void);

/* Best-effort debug snapshot for the currently bound device.
 * Returns 0 on success, -1 if no usb-net device is bound.
 */
int usb_net_get_debug(usb_net_debug_t *out);

#ifdef __cplusplus
}
#endif
