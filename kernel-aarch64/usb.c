#include "usb.h"

#include "usb_host.h"
#include "usb_kbd.h"
#include "usb_net.h"

#include "uart_pl011.h"

/* QEMU-first, polled USB glue.
 *
 * For now, we do one-time enumeration at init, then polling for drivers that
 * need it (keyboard, usb-net RX).
 */

#define USB_MAX_DEVS 8

static usb_device_t g_devs[USB_MAX_DEVS];
static int g_dev_count = 0;

void usb_init(void) {
    uart_write("usb: init\n");

    if (usb_host_init() != 0) {
        uart_write("usb: host init failed\n");
        return;
    }

    g_dev_count = usb_host_enumerate(g_devs, USB_MAX_DEVS);

    uart_write("usb: devices=");
    uart_write_hex_u64((uint64_t)g_dev_count);
    uart_write("\n");

    for (int i = 0; i < g_dev_count; i++) {
        const usb_device_t *d = &g_devs[i];
        uart_write("  dev addr=");
        uart_write_hex_u64(d->addr);
        uart_write(" ls=");
        uart_write_hex_u64(d->low_speed);
        uart_write(" class=");
        uart_write_hex_u64(d->dev_desc[4]);
        uart_write(" vid=");
        uart_write_hex_u64((uint64_t)((uint16_t)d->dev_desc[8] | ((uint16_t)d->dev_desc[9] << 8)));
        uart_write(" pid=");
        uart_write_hex_u64((uint64_t)((uint16_t)d->dev_desc[10] | ((uint16_t)d->dev_desc[11] << 8)));
        uart_write("\n");

#ifdef ENABLE_USB_KBD
        if (usb_kbd_try_bind(d) == 0) {
            uart_write("  -> usb-kbd bound\n");
            continue;
        }
#endif

#ifdef ENABLE_USB_NET
        if (usb_net_try_bind(d) == 0) {
            uart_write("  -> usb-net bound\n");
            continue;
        }
#endif
    }
}

void usb_poll(void) {
#ifdef ENABLE_USB_KBD
    usb_kbd_poll();
#endif
#ifdef ENABLE_USB_NET
    usb_net_poll();
#endif
}
