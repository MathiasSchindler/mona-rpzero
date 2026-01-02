#pragma once

#include "stddef.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal UDP-over-IPv6 support (Phase 5).
 *
 * This is intentionally tiny: a small fixed socket table and a small fixed
 * receive queue per socket.
 */

enum {
    UDP6_MAX_PAYLOAD = 1200,
};

typedef struct {
    uint8_t src_ip[16];
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint8_t data[UDP6_MAX_PAYLOAD];
} udp6_dgram_t;

void net_udp6_init(void);

/* fd layer hooks: keep UDP socket lifetime tied to file description refs. */
void net_udp6_on_desc_incref(uint32_t sock_id);
void net_udp6_on_desc_decref(uint32_t sock_id);

int net_udp6_socket_alloc(uint32_t *out_sock_id);

/* Bind to a local UDP port (0 means pick an ephemeral port). */
int net_udp6_bind(uint32_t sock_id, uint16_t port);

/* Send a UDP datagram.
 * Returns payload length on success, negative errno on failure.
 */
int net_udp6_sendto(uint32_t sock_id,
                    const uint8_t dst_ip[16],
                    uint16_t dst_port,
                    const uint8_t *payload,
                    size_t payload_len);

/* Try to dequeue one received datagram.
 * Returns 0 on success, -EAGAIN if none available, other -errno on failure.
 */
int net_udp6_try_recv(uint32_t sock_id, udp6_dgram_t *out);

#ifdef __cplusplus
}
#endif
