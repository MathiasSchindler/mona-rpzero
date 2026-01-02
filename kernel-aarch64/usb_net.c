#include "usb_net.h"

#include "net.h"
#include "uart_pl011.h"

/*
 * Minimal USB "Ethernet-like" device support.
 *
 * This is intentionally best-effort: we look for a BULK IN + BULK OUT endpoint pair
 * and treat bulk payloads as raw Ethernet frames.
 *
 * Works for CDC-ECM-like devices; may not work for RNDIS/NCM.
 */

typedef struct {
    int bound;
    uint8_t addr;
    int low_speed;
    usb_ep_t ep_in;
    usb_ep_t ep_out;
    uint32_t in_pid;
    uint32_t out_pid;

    netif_t nif;
} usb_net_state_t;

static usb_net_state_t g_usbnet;

static int usbnet_tx_frame(netif_t *nif, const uint8_t *frame, size_t len);

static const netif_ops_t g_usbnet_ops = {
    .tx_frame = usbnet_tx_frame,
};

static int usbnet_tx_frame(netif_t *nif, const uint8_t *frame, size_t len) {
    (void)nif;
    if (!g_usbnet.bound) return -1;

    /* Simple MTU sanity: Ethernet frame is typically <= 1514 (+ VLAN). Allow a bit more. */
    if (len == 0 || len > 2048) return -1;

    int rc = usb_host_out_xfer(g_usbnet.addr, g_usbnet.low_speed, g_usbnet.ep_out, g_usbnet.out_pid, frame, (uint32_t)len);
    if (rc == 0) {
        g_usbnet.out_pid = (g_usbnet.out_pid == USB_PID_DATA0) ? USB_PID_DATA1 : USB_PID_DATA0;
    }
    return rc;
}

int usb_net_try_bind(const usb_device_t *dev) {
    if (!dev) return -1;
    if (g_usbnet.bound) return -1;

    usb_ep_t bin = {0};
    usb_ep_t bout = {0};
    if (usb_host_find_bulk_in_out_pair(dev, &bin, &bout) != 0) {
        return -1;
    }

    g_usbnet.bound = 1;
    g_usbnet.addr = dev->addr;
    g_usbnet.low_speed = (int)dev->low_speed;
    g_usbnet.ep_in = bin;
    g_usbnet.ep_out = bout;
    g_usbnet.in_pid = USB_PID_DATA0;
    g_usbnet.out_pid = USB_PID_DATA0;

    for (int i = 0; i < NETIF_NAME_MAX; i++) g_usbnet.nif.name[i] = 0;
    g_usbnet.nif.name[0] = 'u';
    g_usbnet.nif.name[1] = 's';
    g_usbnet.nif.name[2] = 'b';
    g_usbnet.nif.name[3] = '0';

    g_usbnet.nif.mtu = 1500;
    g_usbnet.nif.ops = &g_usbnet_ops;
    g_usbnet.nif.driver_ctx = 0;

    /* MAC is unknown here (would require CDC/RNDIS control path). Use a placeholder.
     * Phase 2 focuses on raw frame path bring-up.
     */
    g_usbnet.nif.mac[0] = 0x02;
    g_usbnet.nif.mac[1] = 0x00;
    g_usbnet.nif.mac[2] = 0x00;
    g_usbnet.nif.mac[3] = 0x00;
    g_usbnet.nif.mac[4] = 0x00;
    g_usbnet.nif.mac[5] = 0x01;

    (void)netif_register(&g_usbnet.nif);

    uart_write("usb-net: bound dev addr=");
    uart_write_hex_u64(g_usbnet.addr);
    uart_write(" bulk-in=");
    uart_write_hex_u64(g_usbnet.ep_in.ep_num);
    uart_write(" bulk-out=");
    uart_write_hex_u64(g_usbnet.ep_out.ep_num);
    uart_write("\n");

    return 0;
}

void usb_net_poll(void) {
    if (!g_usbnet.bound) return;

    uint8_t buf[2048];
    uint32_t got = 0;

    if (usb_host_in_xfer(g_usbnet.addr, g_usbnet.low_speed, g_usbnet.ep_in, g_usbnet.in_pid,
                         buf, sizeof(buf), &got, /*nak_ok=*/1) != 0) {
        return;
    }

    if (got == 0) return;

    /* Toggle PID only on successful (non-NAK) transactions with data. */
    g_usbnet.in_pid = (g_usbnet.in_pid == USB_PID_DATA0) ? USB_PID_DATA1 : USB_PID_DATA0;

    netif_rx_frame(&g_usbnet.nif, buf, (size_t)got);
}
