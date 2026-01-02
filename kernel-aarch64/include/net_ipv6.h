#pragma once

#include "net.h"
#include "stddef.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Phase 3/4: minimal IPv6 + ICMPv6 + Neighbor Discovery.
 *
 * Current scope:
 * - Link-local addressing (EUI-64 from MAC)
 * - ICMPv6 echo request/reply
 * - NDP NS/NA (enough to resolve a neighbor MAC for ping6)
 */

void net_ipv6_init(void);

/* Called by net core when a netif is registered.
 * Computes and stores the link-local address.
 */
void net_ipv6_configure_netif(netif_t *nif);

/* IPv6 input entry point (Ethernet payload for EtherType 0x86DD). */
void net_ipv6_input(netif_t *nif, const uint8_t src_mac[6], const uint8_t *pkt, size_t len);

/* Start an ICMPv6 echo exchange on behalf of a process.
 * Returns 0 on "started" (request sent or NDP started), negative errno on failure.
 * Completion is reported by the scheduler via the process's saved x0 return value.
 */
int net_ipv6_ping6_start(int proc_idx, netif_t *nif, const uint8_t dst_ip[16], uint16_t ident, uint16_t seq);

/* Cancel an in-flight ping started for proc_idx (best-effort). */
void net_ipv6_ping6_cancel(int proc_idx);

#ifdef __cplusplus
}
#endif
