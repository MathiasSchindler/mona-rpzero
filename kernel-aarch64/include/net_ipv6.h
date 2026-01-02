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

typedef struct {
	uint64_t rx_ipv6_packets;
	uint64_t rx_drop_short;
	uint64_t rx_drop_len;
	uint64_t rx_drop_csum;

	uint64_t rx_udp;
	uint64_t rx_udp_delivered;

	uint64_t rx_icmpv6;
	uint64_t rx_icmpv6_ra;
	uint64_t rx_icmpv6_ra_drop_hlim;
	uint64_t rx_icmpv6_ra_drop_src;
	uint64_t rx_icmpv6_ra_drop_short;

	uint64_t rx_icmpv6_ns;
	uint64_t rx_icmpv6_na;
	uint64_t rx_icmpv6_echo_req;
	uint64_t rx_icmpv6_echo_reply;

	/* TX-side observability (best-effort). */
	uint64_t tx_icmpv6_rs;
	uint64_t tx_icmpv6_ns;
	uint64_t tx_icmpv6_echo_req;

	/* ping6_start() path observability (best-effort). */
	uint64_t ping6_start_calls;
	uint64_t ping6_start_eagain;
	uint64_t ping6_start_ebusy;
	uint64_t ping6_start_sent_echo;
	uint64_t ping6_start_sent_ns;

	uint8_t last_icmp_type;
	uint8_t last_hop_limit;
} net_ipv6_debug_t;

/* Best-effort debug snapshot.
 * Returns 0 on success.
 */
int net_ipv6_get_debug(net_ipv6_debug_t *out);

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
