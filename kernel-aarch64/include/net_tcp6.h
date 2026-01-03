#pragma once

#include "stddef.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal TCP-over-IPv6 client support.
 *
 * Scope (Phase 0 for TLS bringup):
 * - Active open (connect) only
 * - Stream send/recv (best-effort; no fancy options)
 * - No listen/accept
 *
 * This is intentionally tiny and will evolve as TLS needs grow.
 */

void net_tcp6_init(void);

/* fd layer hooks: keep TCP connection lifetime tied to file description refs. */
void net_tcp6_on_desc_incref(uint32_t conn_id);
void net_tcp6_on_desc_decref(uint32_t conn_id);

int net_tcp6_conn_alloc(uint32_t *out_conn_id);

/* Start (or retry) connecting a connection.
 * Returns 0 if a SYN was sent or connection is already established.
 * Returns -EAGAIN if routing/NDP is not ready yet.
 */
int net_tcp6_connect_start(uint32_t conn_id, const uint8_t dst_ip[16], uint16_t dst_port);

/* Best-effort query.
 * Returns 1 if established, 0 otherwise.
 */
int net_tcp6_is_established(uint32_t conn_id);

/* Best-effort send on an established connection.
 * Returns bytes sent or negative errno.
 */
int net_tcp6_send(uint32_t conn_id, const uint8_t *buf, size_t len);

/* Try to receive up to len bytes from the connection's RX queue.
 * Returns bytes copied (>=0) or -EAGAIN if none available.
 */
int net_tcp6_try_recv(uint32_t conn_id, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
