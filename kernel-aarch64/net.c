#include "net.h"

#include "uart_pl011.h"

/*
 * Phase 1 goal:
 * - Provide a minimal netif abstraction.
 * - Provide an Ethernet demux entry point.
 * - Keep all of this dependency-free and safe to link even before a driver exists.
 */

#define NET_MAX_IFACES 4

typedef struct {
    netif_t *ifaces[NET_MAX_IFACES];
    uint32_t iface_count;
} net_state_t;

static net_state_t g_net;

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Ethernet */

typedef struct __attribute__((packed)) {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype_be;
} eth_hdr_t;

#define ETH_TYPE_IPV6 0x86DDu

static void ipv6_input(netif_t *nif, const uint8_t *pkt, size_t len) {
    (void)pkt;
    (void)len;
    /* Stub for Phase 1: just prove the demux plumbing exists. */
    (void)nif;
}

static void ether_input(netif_t *nif, const uint8_t *frame, size_t len) {
    if (!nif || !frame) return;
    if (len < sizeof(eth_hdr_t)) {
        nif->rx_drops++;
        return;
    }

    const eth_hdr_t *h = (const eth_hdr_t *)frame;
    uint16_t ethertype = be16((const uint8_t *)&h->ethertype_be);

    const uint8_t *payload = frame + sizeof(eth_hdr_t);
    size_t payload_len = len - sizeof(eth_hdr_t);

    switch (ethertype) {
        case ETH_TYPE_IPV6:
            ipv6_input(nif, payload, payload_len);
            break;
        default:
            /* Unknown EtherType: ignore for now. */
            break;
    }
}

void net_init(void) {
    for (uint32_t i = 0; i < NET_MAX_IFACES; i++) g_net.ifaces[i] = 0;
    g_net.iface_count = 0;
}

int netif_register(netif_t *nif) {
    if (!nif) return -1;
    if (g_net.iface_count >= NET_MAX_IFACES) return -1;

    /* Ensure name is NUL-terminated. */
    nif->name[NETIF_NAME_MAX - 1] = '\0';

    g_net.ifaces[g_net.iface_count++] = nif;
    return 0;
}

uint32_t netif_count(void) {
    return g_net.iface_count;
}

netif_t *netif_get(uint32_t idx) {
    if (idx >= g_net.iface_count) return 0;
    return g_net.ifaces[idx];
}

void netif_rx_frame(netif_t *nif, const uint8_t *frame, size_t len) {
    if (!nif || !frame || len == 0) return;

    nif->rx_frames++;
    ether_input(nif, frame, len);
}

int netif_tx_frame(netif_t *nif, const uint8_t *frame, size_t len) {
    if (!nif || !frame || len == 0) return -1;
    if (!nif->ops || !nif->ops->tx_frame) {
        nif->tx_drops++;
        return -1;
    }

    int rc = nif->ops->tx_frame(nif, frame, len);
    if (rc == 0) {
        nif->tx_frames++;
    } else {
        nif->tx_drops++;
    }
    return rc;
}

/* Optional tiny debug helper (kept non-static so it won't warn if unused). */
void net_debug_dump_ifaces(void) {
    uart_write("net: ifaces=");
    uart_write_hex_u64(g_net.iface_count);
    uart_write("\n");

    for (uint32_t i = 0; i < g_net.iface_count; i++) {
        netif_t *nif = g_net.ifaces[i];
        if (!nif) continue;
        uart_write("  ");
        uart_write(nif->name);
        uart_write(" rx=");
        uart_write_hex_u64(nif->rx_frames);
        uart_write(" tx=");
        uart_write_hex_u64(nif->tx_frames);
        uart_write("\n");
    }
}
