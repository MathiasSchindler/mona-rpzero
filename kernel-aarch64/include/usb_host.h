#pragma once

#include "stddef.h"
#include "stdint.h"

/*
 * Minimal USB host support (DWC2, polled, QEMU-first).
 *
 * Goals (Phase 2):
 * - Enumerate devices behind a (potential) root hub.
 * - Provide control + bulk/intr transfers.
 * - Keep it small and dependency-free.
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t addr;
    uint8_t low_speed;

    /* Cached device descriptor (first 18 bytes). */
    uint8_t dev_desc[18];

    /* Cached configuration descriptor blob (best-effort, truncated to buf size). */
    uint8_t cfg[512];
    uint16_t cfg_len;

    /* bConfigurationValue for the active configuration. */
    uint8_t cfg_value;
} usb_device_t;

/* Endpoint types (USB 2.0). */
#define USB_EPTYP_CTRL 0u
#define USB_EPTYP_ISO  1u
#define USB_EPTYP_BULK 2u
#define USB_EPTYP_INTR 3u

typedef struct {
    uint8_t ep_num;   /* 0..15 */
    uint8_t ep_type;  /* USB_EPTYP_* */
    uint8_t ep_in;    /* 1 for IN, 0 for OUT */
    uint16_t mps;     /* max packet size */
} usb_ep_t;

/* Initialize the DWC2 host controller and reset the root port.
 * Returns 0 on success.
 */
int usb_host_init(void);

/* Enumerate devices on the bus.
 * - Returns number of devices found (0..max_devs).
 * - Each usb_device_t includes cached descriptors.
 */
int usb_host_enumerate(usb_device_t *out_devs, int max_devs);

/* Standard transfers. */

typedef struct __attribute__((packed)) {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

int usb_host_control_xfer(uint8_t dev_addr, int low_speed, usb_setup_t setup,
                          uint8_t *data, uint32_t data_len_inout, uint32_t *out_got);

/* For bulk/intr endpoints. `pid` is the DATA PID value used by DWC2.
 * Callers should toggle DATA0/DATA1 on successful transfers.
 */
#define USB_PID_DATA0 0u
#define USB_PID_DATA1 2u

int usb_host_out_xfer(uint8_t dev_addr, int low_speed, usb_ep_t ep, uint32_t pid,
                      const uint8_t *data, uint32_t len);

/* Return value from usb_host_in_xfer when nak_ok is enabled and no data was
 * available within the short polling window (e.g. device NAK/timeout).
 *
 * Note: This is distinct from a successful transfer that completes with
 * out_got == 0 (a valid ZLP), which should still advance DATA PID toggling.
 */
#define USB_XFER_NODATA 1

int usb_host_in_xfer(uint8_t dev_addr, int low_speed, usb_ep_t ep, uint32_t pid,
                     uint8_t *out, uint32_t len, uint32_t *out_got, int nak_ok);

/* Fetch a USB string descriptor and decode as ASCII (best-effort).
 * Returns 0 on success and null-terminates `out`.
 */
int usb_host_get_string_ascii(uint8_t dev_addr, int low_speed, uint8_t str_index,
                              char *out, size_t out_len);

/* Standard request: SET_INTERFACE (alternate setting) for a specific interface.
 * Returns 0 on success.
 */
int usb_host_set_interface(uint8_t dev_addr, int low_speed, uint8_t if_num, uint8_t alt_setting);

/* Descriptor parsing helpers over `usb_device_t.cfg`. */
int usb_host_find_hid_kbd_intr_in(const usb_device_t *dev, usb_ep_t *out_intr_in);

/* Best-effort: find a data interface with bulk IN+OUT endpoints (CDC-ECM-like).
 * Returns 0 and fills endpoints if found.
 */
int usb_host_find_bulk_in_out_pair(const usb_device_t *dev, usb_ep_t *out_in, usb_ep_t *out_out);

#ifdef __cplusplus
}
#endif
