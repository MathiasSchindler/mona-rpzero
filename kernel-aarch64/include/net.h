#pragma once

#include "stddef.h"
#include "stdint.h"

/*
 * Minimal networking scaffolding (Phase 1).
 *
 * Design constraints:
 * - No external dependencies.
 * - No libc usage.
 * - Driver-agnostic: netif provides tx(), driver pushes rx frames.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define NETIF_NAME_MAX 8

typedef struct netif netif_t;

typedef struct {
    /* Transmit a full Ethernet frame.
     * Returns 0 on success, negative on error.
     */
    int (*tx_frame)(netif_t *nif, const uint8_t *frame, size_t len);
} netif_ops_t;

struct netif {
    char name[NETIF_NAME_MAX];
    uint8_t mac[6];
    uint16_t mtu; /* payload MTU (not including Ethernet header) */

    /* Phase 3: minimal IPv6 state (link-local only for now). */
    uint8_t ipv6_ll[16];
    uint8_t ipv6_ll_valid;

    /* Phase 4: minimal RA/SLAAC state (best-effort). */
    uint8_t ipv6_global[16];
    uint8_t ipv6_global_valid;
    uint8_t ipv6_prefix[16];
    uint8_t ipv6_prefix_len; /* 0 means unset. */
    uint8_t ipv6_router_ll[16];
    uint8_t ipv6_router_valid;

    const netif_ops_t *ops;
    void *driver_ctx;

    /* Stats (best-effort, non-atomic). */
    uint64_t rx_frames;
    uint64_t tx_frames;
    uint64_t rx_drops;
    uint64_t tx_drops;
};

void net_init(void);

/* Register a netif with the stack. Returns 0 on success. */
int netif_register(netif_t *nif);

/* Introspection helpers (primarily for procfs/debug). */
uint32_t netif_count(void);
netif_t *netif_get(uint32_t idx);

/* Inject a received Ethernet frame into the stack (driver calls this). */
void netif_rx_frame(netif_t *nif, const uint8_t *frame, size_t len);

/* Send a full Ethernet frame (stack/driver boundary). */
int netif_tx_frame(netif_t *nif, const uint8_t *frame, size_t len);

#ifdef __cplusplus
}
#endif
