#include "net_ipv6.h"

#include "errno.h"
#include "net_udp6.h"
#include "proc.h"
#include "time.h"
#include "uart_pl011.h"

/*
 * Minimal IPv6/ICMPv6/NDP implementation aimed at enabling a first ping6.
 *
 * Notes:
 * - Only link-local (fe80::/64) is configured.
 * - No extension header support beyond "drop".
 * - No fragmentation/reassembly.
 * - Neighbor cache is tiny and best-effort.
 */

static uint16_t be16_load(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t be32_load(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void be16_store(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)((v >> 8) & 0xff);
    p[1] = (uint8_t)(v & 0xff);
}

static void be32_store(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xff);
    p[1] = (uint8_t)((v >> 16) & 0xff);
    p[2] = (uint8_t)((v >> 8) & 0xff);
    p[3] = (uint8_t)(v & 0xff);
}

static int memeq(const uint8_t *a, const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static void memcpy_u8(uint8_t *dst, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
}

/* Internet checksum (ones-complement) helpers. */

static uint32_t csum_add16(uint32_t sum, uint16_t v) {
    sum += (uint32_t)v;
    return sum;
}

static uint32_t csum_add_buf(uint32_t sum, const uint8_t *buf, size_t len) {
    size_t i = 0;
    while (i + 1 < len) {
        uint16_t w = (uint16_t)((uint16_t)buf[i] << 8) | (uint16_t)buf[i + 1];
        sum = csum_add16(sum, w);
        i += 2;
    }
    if (i < len) {
        uint16_t w = (uint16_t)((uint16_t)buf[i] << 8);
        sum = csum_add16(sum, w);
    }
    return sum;
}

static uint16_t csum_finish(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

static uint16_t icmpv6_checksum(const uint8_t src_ip[16], const uint8_t dst_ip[16],
                               const uint8_t *icmp, size_t icmp_len) {
    uint32_t sum = 0;

    sum = csum_add_buf(sum, src_ip, 16);
    sum = csum_add_buf(sum, dst_ip, 16);

    /* Upper-layer packet length (32-bit). */
    uint8_t len_be[4];
    be32_store(len_be, (uint32_t)icmp_len);
    sum = csum_add_buf(sum, len_be, 4);

    /* 3 bytes zero + next header (58). */
    uint8_t nh[4] = {0, 0, 0, 58};
    sum = csum_add_buf(sum, nh, 4);

    sum = csum_add_buf(sum, icmp, icmp_len);

    return csum_finish(sum);
}

static uint16_t udp6_checksum(const uint8_t src_ip[16], const uint8_t dst_ip[16],
                              const uint8_t *udp, size_t udp_len) {
    uint32_t sum = 0;

    sum = csum_add_buf(sum, src_ip, 16);
    sum = csum_add_buf(sum, dst_ip, 16);

    uint8_t len_be[4];
    be32_store(len_be, (uint32_t)udp_len);
    sum = csum_add_buf(sum, len_be, 4);

    uint8_t nh[4] = {0, 0, 0, 17};
    sum = csum_add_buf(sum, nh, 4);

    sum = csum_add_buf(sum, udp, udp_len);

    return csum_finish(sum);
}

/* Ethernet framing for IPv6. */

typedef struct __attribute__((packed)) {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t ethertype_be;
} eth_hdr_t;

#define ETH_TYPE_IPV6 0x86DDu

static int eth_send_ipv6(netif_t *nif, const uint8_t dst_mac[6], const uint8_t *ipv6_pkt, size_t ipv6_len) {
    if (!nif || !dst_mac || !ipv6_pkt) return -(int)EINVAL;
    if (ipv6_len == 0) return -(int)EINVAL;
    if (nif->mtu != 0 && ipv6_len > nif->mtu) return -(int)EMSGSIZE;

    uint8_t frame[14 + 160];
    if (ipv6_len > (sizeof(frame) - sizeof(eth_hdr_t))) {
        return -(int)EMSGSIZE;
    }

    eth_hdr_t *h = (eth_hdr_t *)frame;
    memcpy_u8(h->dst, dst_mac, 6);
    memcpy_u8(h->src, nif->mac, 6);
    be16_store((uint8_t *)&h->ethertype_be, (uint16_t)ETH_TYPE_IPV6);

    memcpy_u8(frame + sizeof(eth_hdr_t), ipv6_pkt, ipv6_len);
    int rc = netif_tx_frame(nif, frame, sizeof(eth_hdr_t) + ipv6_len);
    return (rc == 0) ? 0 : -(int)EIO;
}

/* IPv6 basics. */

typedef struct __attribute__((packed)) {
    uint32_t vtcfl_be;
    uint16_t payload_len_be;
    uint8_t next_header;
    uint8_t hop_limit;
    uint8_t src[16];
    uint8_t dst[16];
} ipv6_hdr_t;

#define IPV6_NH_ICMPV6 58u
#define IPV6_NH_UDP 17u

static int ipv6_is_multicast(const uint8_t ip[16]) {
    return ip[0] == 0xff;
}

static int ipv6_is_linklocal(const uint8_t ip[16]) {
    /* fe80::/10 */
    return ip[0] == 0xfe && ((ip[1] & 0xc0u) == 0x80u);
}

static void ipv6_make_solicited_node_multicast(uint8_t out[16], const uint8_t target[16]) {
    for (int i = 0; i < 16; i++) out[i] = 0;
    out[0] = 0xff;
    out[1] = 0x02;
    out[11] = 0x01;
    out[12] = 0xff;
    out[13] = target[13];
    out[14] = target[14];
    out[15] = target[15];
}

static void ipv6_multicast_to_eth(const uint8_t ip[16], uint8_t out_mac[6]) {
    /* 33:33 + last 32 bits */
    out_mac[0] = 0x33;
    out_mac[1] = 0x33;
    out_mac[2] = ip[12];
    out_mac[3] = ip[13];
    out_mac[4] = ip[14];
    out_mac[5] = ip[15];
}

static void ipv6_make_link_local_from_mac(uint8_t out_ip[16], const uint8_t mac[6]) {
    for (int i = 0; i < 16; i++) out_ip[i] = 0;
    out_ip[0] = 0xfe;
    out_ip[1] = 0x80;

    /* EUI-64: flip U/L bit and insert ff:fe. */
    out_ip[8] = (uint8_t)(mac[0] ^ 0x02u);
    out_ip[9] = mac[1];
    out_ip[10] = mac[2];
    out_ip[11] = 0xff;
    out_ip[12] = 0xfe;
    out_ip[13] = mac[3];
    out_ip[14] = mac[4];
    out_ip[15] = mac[5];
}

/* NDP neighbor cache. */

#define ND_CACHE_SIZE 8

typedef struct {
    uint8_t used;
    uint8_t ip[16];
    uint8_t mac[6];
} nd_entry_t;

static nd_entry_t g_nd[ND_CACHE_SIZE];

static int nd_lookup_mac(const uint8_t ip[16], uint8_t out_mac[6]) {
    for (int i = 0; i < ND_CACHE_SIZE; i++) {
        if (!g_nd[i].used) continue;
        if (memeq(g_nd[i].ip, ip, 16)) {
            memcpy_u8(out_mac, g_nd[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

static void nd_update(const uint8_t ip[16], const uint8_t mac[6]) {
    int free_idx = -1;
    for (int i = 0; i < ND_CACHE_SIZE; i++) {
        if (g_nd[i].used && memeq(g_nd[i].ip, ip, 16)) {
            memcpy_u8(g_nd[i].mac, mac, 6);
            return;
        }
        if (!g_nd[i].used && free_idx < 0) free_idx = i;
    }
    int idx = (free_idx >= 0) ? free_idx : 0;
    g_nd[idx].used = 1;
    memcpy_u8(g_nd[idx].ip, ip, 16);
    memcpy_u8(g_nd[idx].mac, mac, 6);
}

/* ICMPv6 */

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t csum_be;
} icmpv6_hdr_t;

#define ICMPV6_ECHO_REQUEST 128u
#define ICMPV6_ECHO_REPLY 129u
#define ICMPV6_ROUTER_SOLICIT 133u
#define ICMPV6_ROUTER_ADVERT 134u
#define ICMPV6_NEIGHBOR_SOLICIT 135u
#define ICMPV6_NEIGHBOR_ADVERT 136u

/* Debug counters for diagnosing RA/SLAAC and RX drops. */
static net_ipv6_debug_t g_ipv6_dbg;

int net_ipv6_get_debug(net_ipv6_debug_t *out) {
    if (!out) return -1;
    memcpy_u8((uint8_t *)out, (const uint8_t *)&g_ipv6_dbg, sizeof(g_ipv6_dbg));
    return 0;
}

/* One in-flight ping6 waiter (enough for early bring-up). */
static int g_ping_inflight = 0;
static int g_ping_proc_idx = -1;
static netif_t *g_ping_nif = 0;
static uint8_t g_ping_dst_ip[16];
static uint8_t g_ping_nh_ip[16];
static uint16_t g_ping_ident = 0;
static uint16_t g_ping_seq = 0;
static uint8_t g_ping_phase = 0; /* 0 idle, 1 waiting NA, 2 waiting echo */

static void ping_complete(uint64_t ret, uint64_t rtt_ns) {
    if (!g_ping_inflight || g_ping_proc_idx < 0) return;
    if (g_ping_proc_idx >= (int)MAX_PROCS) return;

    proc_t *p = &g_procs[g_ping_proc_idx];
    if (!p->pending_ping6) return;

    p->ping6_done = 1;
    p->ping6_ret = ret;
    p->ping6_rtt_ns = rtt_ns;
    p->sleep_deadline_ns = 0;
    if (p->state == PROC_SLEEPING) {
        p->state = PROC_RUNNABLE;
    }

    g_ping_inflight = 0;
    g_ping_proc_idx = -1;
    g_ping_nif = 0;
    g_ping_phase = 0;
}

void net_ipv6_ping6_cancel(int proc_idx) {
    if (!g_ping_inflight) return;
    if (g_ping_proc_idx < 0 || g_ping_proc_idx >= (int)MAX_PROCS) return;
    if (g_ping_proc_idx != proc_idx) return;

    g_ping_inflight = 0;
    g_ping_proc_idx = -1;
    g_ping_nif = 0;
    g_ping_phase = 0;
}

/* Forward declarations for helpers used by UDP6 (defined later in this file). */
static int ipv6_select_next_hop(const netif_t *nif, const uint8_t dst_ip[16], uint8_t out_nh_ip[16]);
static const uint8_t *ipv6_select_src_ip(const netif_t *nif, const uint8_t dst_ip[16]);
static int send_neighbor_solicitation(netif_t *nif, const uint8_t target_ip[16]);
static int send_router_solicitation(netif_t *nif);

/* UDP6 sockets.
 *
 * Fixed-size table with small per-socket RX queue.
 */

typedef struct {
    uint8_t used;
    uint32_t refs;
    uint8_t bound;
    uint16_t bound_port;

    uint8_t q_head;
    uint8_t q_tail;
    uint8_t q_count;
    udp6_dgram_t q[8];
} udp6_sock_t;

static udp6_sock_t g_udp6[8];
static uint16_t g_udp6_ephemeral_next = 49152u;

static int udp6_sock_id_ok(uint32_t sid) {
    return sid < (uint32_t)(sizeof(g_udp6) / sizeof(g_udp6[0]));
}

static int udp6_port_in_use(uint16_t port, uint32_t skip_sid) {
    if (port == 0) return 1;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_udp6) / sizeof(g_udp6[0])); i++) {
        if (i == skip_sid) continue;
        if (!g_udp6[i].used) continue;
        if (!g_udp6[i].bound) continue;
        if (g_udp6[i].bound_port == port) return 1;
    }
    return 0;
}

static int udp6_bind_ephemeral(uint32_t sid, uint16_t *out_port) {
    if (!udp6_sock_id_ok(sid)) return -(int)EINVAL;
    udp6_sock_t *s = &g_udp6[sid];
    if (!s->used) return -(int)EBADF;

    for (int tries = 0; tries < 1024; tries++) {
        uint16_t p = g_udp6_ephemeral_next;
        g_udp6_ephemeral_next++;
        if (g_udp6_ephemeral_next < 49152u) g_udp6_ephemeral_next = 49152u;

        if (!udp6_port_in_use(p, sid)) {
            s->bound = 1;
            s->bound_port = p;
            if (out_port) *out_port = p;
            return 0;
        }
    }
    return -(int)EADDRINUSE;
}

void net_udp6_init(void) {
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_udp6) / sizeof(g_udp6[0])); i++) {
        g_udp6[i].used = 0;
        g_udp6[i].refs = 0;
        g_udp6[i].bound = 0;
        g_udp6[i].bound_port = 0;
        g_udp6[i].q_head = 0;
        g_udp6[i].q_tail = 0;
        g_udp6[i].q_count = 0;
    }
    g_udp6_ephemeral_next = 49152u;
}

void net_udp6_on_desc_incref(uint32_t sock_id) {
    if (!udp6_sock_id_ok(sock_id)) return;
    udp6_sock_t *s = &g_udp6[sock_id];
    if (!s->used) return;
    s->refs++;
}

void net_udp6_on_desc_decref(uint32_t sock_id) {
    if (!udp6_sock_id_ok(sock_id)) return;
    udp6_sock_t *s = &g_udp6[sock_id];
    if (!s->used) return;
    if (s->refs > 0) s->refs--;
    if (s->refs == 0) {
        s->used = 0;
        s->bound = 0;
        s->bound_port = 0;
        s->q_head = 0;
        s->q_tail = 0;
        s->q_count = 0;
    }
}

int net_udp6_socket_alloc(uint32_t *out_sock_id) {
    if (!out_sock_id) return -(int)EINVAL;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_udp6) / sizeof(g_udp6[0])); i++) {
        if (!g_udp6[i].used) {
            g_udp6[i].used = 1;
            g_udp6[i].refs = 1;
            g_udp6[i].bound = 0;
            g_udp6[i].bound_port = 0;
            g_udp6[i].q_head = 0;
            g_udp6[i].q_tail = 0;
            g_udp6[i].q_count = 0;
            *out_sock_id = i;
            return 0;
        }
    }
    return -(int)ENOMEM;
}

int net_udp6_bind(uint32_t sock_id, uint16_t port) {
    if (!udp6_sock_id_ok(sock_id)) return -(int)EBADF;
    udp6_sock_t *s = &g_udp6[sock_id];
    if (!s->used) return -(int)EBADF;

    if (port == 0) {
        return udp6_bind_ephemeral(sock_id, 0);
    }

    if (udp6_port_in_use(port, sock_id)) {
        return -(int)EADDRINUSE;
    }

    s->bound = 1;
    s->bound_port = port;
    return 0;
}

typedef struct __attribute__((packed)) {
    uint16_t src_port_be;
    uint16_t dst_port_be;
    uint16_t len_be;
    uint16_t csum_be;
} udp_hdr_t;

static int udp6_send_raw(netif_t *nif,
                         const uint8_t src_ip[16],
                         const uint8_t dst_ip[16],
                         const uint8_t dst_mac[6],
                         uint16_t src_port,
                         uint16_t dst_port,
                         const uint8_t *payload,
                         size_t payload_len) {
    uint8_t pkt[40 + 8 + UDP6_MAX_PAYLOAD];
    if (payload_len > (size_t)UDP6_MAX_PAYLOAD) return -(int)EMSGSIZE;

    size_t udp_len = sizeof(udp_hdr_t) + payload_len;
    size_t total_len = sizeof(ipv6_hdr_t) + udp_len;
    if (total_len > sizeof(pkt)) return -(int)EMSGSIZE;

    ipv6_hdr_t *ip6 = (ipv6_hdr_t *)pkt;
    be32_store((uint8_t *)&ip6->vtcfl_be, 0x60000000u);
    be16_store((uint8_t *)&ip6->payload_len_be, (uint16_t)udp_len);
    ip6->next_header = IPV6_NH_UDP;
    ip6->hop_limit = 64;
    memcpy_u8(ip6->src, src_ip, 16);
    memcpy_u8(ip6->dst, dst_ip, 16);

    udp_hdr_t *uh = (udp_hdr_t *)(pkt + sizeof(ipv6_hdr_t));
    be16_store((uint8_t *)&uh->src_port_be, src_port);
    be16_store((uint8_t *)&uh->dst_port_be, dst_port);
    be16_store((uint8_t *)&uh->len_be, (uint16_t)udp_len);
    uh->csum_be = 0;

    if (payload_len) {
        memcpy_u8((uint8_t *)uh + sizeof(udp_hdr_t), payload, payload_len);
    }

    uint16_t csum = udp6_checksum(ip6->src, ip6->dst, (const uint8_t *)uh, udp_len);
    uh->csum_be = (uint16_t)((csum >> 8) | (csum << 8));

    return eth_send_ipv6(nif, dst_mac, pkt, total_len);
}

int net_udp6_sendto(uint32_t sock_id,
                    const uint8_t dst_ip[16],
                    uint16_t dst_port,
                    const uint8_t *payload,
                    size_t payload_len) {
    if (!dst_ip || (!payload && payload_len != 0)) return -(int)EINVAL;
    if (!udp6_sock_id_ok(sock_id)) return -(int)EBADF;
    udp6_sock_t *s = &g_udp6[sock_id];
    if (!s->used) return -(int)EBADF;

    netif_t *nif = netif_get(0);
    if (!nif) return -(int)ENODEV;
    if (!nif->ipv6_ll_valid) net_ipv6_configure_netif(nif);

    /* If we're asked to send off-link before SLAAC/default router is learned, let userland retry. */
    if (!ipv6_is_linklocal(dst_ip) && !ipv6_is_multicast(dst_ip) && !nif->ipv6_global_valid) {
        uint64_t now = time_now_ns();
        if (now != 0) {
            uint64_t last = nif->ipv6_last_rs_ns;
            if (last == 0 || now - last >= 250000000ull) {
                (void)send_router_solicitation(nif);
                nif->ipv6_last_rs_ns = now;
            }
        }
        return -(int)EAGAIN;
    }

    if (!s->bound) {
        int brc = udp6_bind_ephemeral(sock_id, 0);
        if (brc < 0) return brc;
    }

    uint8_t nh_ip[16];
    uint8_t dst_mac[6];

    if (ipv6_is_multicast(dst_ip)) {
        ipv6_multicast_to_eth(dst_ip, dst_mac);
        const uint8_t *src_ip = ipv6_select_src_ip(nif, dst_ip);
        if (!src_ip) return -(int)EAFNOSUPPORT;
        int rc = udp6_send_raw(nif, src_ip, dst_ip, dst_mac, s->bound_port, dst_port, payload, payload_len);
        return (rc == 0) ? (int)payload_len : rc;
    }

    int rc = ipv6_select_next_hop(nif, dst_ip, nh_ip);
    if (rc < 0) {
        /* Likely missing RA/default route; allow userland to retry within its timeout budget. */
        if (rc == -(int)ENETUNREACH) return -(int)EAGAIN;
        return rc;
    }

    if (nd_lookup_mac(nh_ip, dst_mac) != 0) {
        (void)send_neighbor_solicitation(nif, nh_ip);

        /*
         * Some environments (notably QEMU user-mode networking) may not respond
         * to NDP for certain on-link-looking addresses (e.g. fec0::3 DNS).
         * If we have a default router with a known MAC, fall back to that MAC
         * as a best-effort L2 next hop.
         */
        if (nif->ipv6_router_valid && memeq(nh_ip, dst_ip, 16) &&
            !ipv6_is_linklocal(dst_ip) && !ipv6_is_multicast(dst_ip)) {
            uint8_t rmac[6];
            if (nd_lookup_mac(nif->ipv6_router_ll, rmac) == 0) {
                memcpy_u8(dst_mac, rmac, 6);
            } else {
                (void)send_neighbor_solicitation(nif, nif->ipv6_router_ll);
                return -(int)EAGAIN;
            }
        } else {
            return -(int)EAGAIN;
        }
    }

    const uint8_t *src_ip = ipv6_select_src_ip(nif, dst_ip);
    if (!src_ip) return -(int)EAGAIN;
    rc = udp6_send_raw(nif, src_ip, dst_ip, dst_mac, s->bound_port, dst_port, payload, payload_len);
    return (rc == 0) ? (int)payload_len : rc;
}

int net_udp6_try_recv(uint32_t sock_id, udp6_dgram_t *out) {
    if (!out) return -(int)EINVAL;
    if (!udp6_sock_id_ok(sock_id)) return -(int)EBADF;
    udp6_sock_t *s = &g_udp6[sock_id];
    if (!s->used) return -(int)EBADF;
    if (s->q_count == 0) return -(int)EAGAIN;

    udp6_dgram_t *src = &s->q[s->q_head];
    memcpy_u8(out->src_ip, src->src_ip, 16);
    out->src_port = src->src_port;
    out->dst_port = src->dst_port;
    out->len = src->len;
    if (src->len) {
        memcpy_u8(out->data, src->data, (size_t)src->len);
    }
    s->q_head = (uint8_t)((s->q_head + 1u) % (uint8_t)(sizeof(s->q) / sizeof(s->q[0])));
    s->q_count--;
    return 0;
}

static void udp6_wake_waiters(uint32_t sock_id) {
    for (int i = 0; i < (int)MAX_PROCS; i++) {
        proc_t *p = &g_procs[i];
        if (!p->pending_udp6_recv) continue;
        if (p->pending_udp6_sock_id != sock_id) continue;
        if (p->state == PROC_SLEEPING || p->state == PROC_BLOCKED_IO) {
            p->state = PROC_RUNNABLE;
            p->sleep_deadline_ns = 0;
        }
    }
}

static void udp6_deliver(const uint8_t src_ip[16], uint16_t src_port,
                         const uint8_t dst_ip[16], uint16_t dst_port,
                         const uint8_t *payload, size_t payload_len) {
    if (payload_len > (size_t)UDP6_MAX_PAYLOAD) {
        return;
    }

    for (uint32_t sid = 0; sid < (uint32_t)(sizeof(g_udp6) / sizeof(g_udp6[0])); sid++) {
        udp6_sock_t *s = &g_udp6[sid];
        if (!s->used) continue;
        if (!s->bound) continue;
        if (s->bound_port != dst_port) continue;

        if (s->q_count >= (uint8_t)(sizeof(s->q) / sizeof(s->q[0]))) {
            continue;
        }

        udp6_dgram_t *dg = &s->q[s->q_tail];
        memcpy_u8(dg->src_ip, src_ip, 16);
        dg->src_port = src_port;
        dg->dst_port = dst_port;
        dg->len = (uint16_t)payload_len;
        if (payload_len) memcpy_u8(dg->data, payload, payload_len);
        s->q_tail = (uint8_t)((s->q_tail + 1u) % (uint8_t)(sizeof(s->q) / sizeof(s->q[0])));
        s->q_count++;

        udp6_wake_waiters(sid);
    }
}

static int ipv6_prefix_match(const uint8_t ip[16], const uint8_t prefix[16], uint8_t prefix_len) {
    if (prefix_len == 0) return 0;
    if (prefix_len > 128) return 0;
    uint8_t full_bytes = (uint8_t)(prefix_len / 8u);
    uint8_t rem_bits = (uint8_t)(prefix_len % 8u);

    if (full_bytes) {
        if (!memeq(ip, prefix, full_bytes)) return 0;
    }
    if (rem_bits) {
        uint8_t mask = (uint8_t)(0xffu << (8u - rem_bits));
        if ((ip[full_bytes] & mask) != (prefix[full_bytes] & mask)) return 0;
    }
    return 1;
}

static int ipv6_select_next_hop(const netif_t *nif, const uint8_t dst_ip[16], uint8_t out_nh_ip[16]) {
    if (!nif || !dst_ip || !out_nh_ip) return -(int)EINVAL;

    if (ipv6_is_multicast(dst_ip) || ipv6_is_linklocal(dst_ip)) {
        memcpy_u8(out_nh_ip, dst_ip, 16);
        return 0;
    }

    if (nif->ipv6_prefix_len && ipv6_prefix_match(dst_ip, nif->ipv6_prefix, nif->ipv6_prefix_len)) {
        memcpy_u8(out_nh_ip, dst_ip, 16);
        return 0;
    }

    if (nif->ipv6_router_valid) {
        memcpy_u8(out_nh_ip, nif->ipv6_router_ll, 16);
        return 0;
    }

    return -(int)ENETUNREACH;
}

static const uint8_t *ipv6_select_src_ip(const netif_t *nif, const uint8_t dst_ip[16]) {
    if (!nif || !dst_ip) return 0;
    if (ipv6_is_linklocal(dst_ip) || ipv6_is_multicast(dst_ip)) {
        return nif->ipv6_ll_valid ? nif->ipv6_ll : 0;
    }
    return nif->ipv6_global_valid ? nif->ipv6_global : 0;
}

static void ipv6_make_global_from_prefix64(uint8_t out_ip[16], const uint8_t prefix[16], const uint8_t ll_ip[16]) {
    /* Use the advertised /64 prefix and our existing interface ID (bytes 8..15 from link-local). */
    memcpy_u8(out_ip, prefix, 16);
    memcpy_u8(out_ip + 8, ll_ip + 8, 8);
}

static int ipv6_send_icmp(netif_t *nif,
                          const uint8_t src_ip_opt[16],
                          const uint8_t dst_ip[16],
                          const uint8_t dst_mac[6],
                          uint8_t hop_limit,
                          uint8_t icmp_type,
                          uint8_t icmp_code,
                          const uint8_t *icmp_body,
                          size_t icmp_body_len);

static void netif_ipv6_update_multicast_list(netif_t *nif) {
    if (!nif || !nif->ops || !nif->ops->set_multicast_list) return;

    /* Keep this small: just what's needed for NDP + RA in our TAP setup.
     * Each entry is a 6-byte Ethernet multicast MAC.
     */
    uint8_t macs[6 * 4];
    size_t count = 0;

    /* ff02::1 all-nodes */
    macs[count * 6 + 0] = 0x33; macs[count * 6 + 1] = 0x33; macs[count * 6 + 2] = 0x00;
    macs[count * 6 + 3] = 0x00; macs[count * 6 + 4] = 0x00; macs[count * 6 + 5] = 0x01;
    count++;

    /* ff02::2 all-routers */
    macs[count * 6 + 0] = 0x33; macs[count * 6 + 1] = 0x33; macs[count * 6 + 2] = 0x00;
    macs[count * 6 + 3] = 0x00; macs[count * 6 + 4] = 0x00; macs[count * 6 + 5] = 0x02;
    count++;

    /* Solicited-node multicast for our primary address (prefer global if present). */
    {
        uint8_t sn_ip[16];
        uint8_t sn_mac[6];
        const uint8_t *target = nif->ipv6_global_valid ? nif->ipv6_global : (nif->ipv6_ll_valid ? nif->ipv6_ll : 0);
        if (target) {
            ipv6_make_solicited_node_multicast(sn_ip, target);
            ipv6_multicast_to_eth(sn_ip, sn_mac);
            for (int i = 0; i < 6; i++) macs[count * 6 + i] = sn_mac[i];
            count++;
        }
    }

    /* ff02::16 MLDv2 reports (harmless extra; helps some NIC filters). */
    macs[count * 6 + 0] = 0x33; macs[count * 6 + 1] = 0x33; macs[count * 6 + 2] = 0x00;
    macs[count * 6 + 3] = 0x00; macs[count * 6 + 4] = 0x00; macs[count * 6 + 5] = 0x16;
    count++;

    (void)nif->ops->set_multicast_list(nif, macs, count);
}

static int send_unsolicited_neighbor_advertisement(netif_t *nif, const uint8_t target_ip[16]) {
    if (!nif || !target_ip) return -1;

    /* ICMPv6 NA body:
     * 4 bytes flags/reserved + 16 bytes target + option (TLLA) 8 bytes.
     */
    uint8_t body[4 + 16 + 8];
    for (size_t i = 0; i < sizeof(body); i++) body[i] = 0;

    /* Unsolicited NA: S=0, O=1. */
    body[0] = 0x20;
    memcpy_u8(body + 4, target_ip, 16);
    body[4 + 16 + 0] = 2;
    body[4 + 16 + 1] = 1;
    memcpy_u8(body + 4 + 16 + 2, nif->mac, 6);

    /* Send to all-nodes multicast (ff02::1). */
    uint8_t dst_ip[16];
    for (int i = 0; i < 16; i++) dst_ip[i] = 0;
    dst_ip[0] = 0xff;
    dst_ip[1] = 0x02;
    dst_ip[15] = 0x01;

    uint8_t dst_mac[6];
    ipv6_multicast_to_eth(dst_ip, dst_mac);

    /* 1) Multicast: helps passive listeners learn us. */
    (void)ipv6_send_icmp(nif, /*src_ip_opt=*/target_ip, dst_ip, dst_mac,
                         /*hop_limit=*/255, ICMPV6_NEIGHBOR_ADVERT, 0, body, sizeof(body));

    /* 2) Unicast to router (if known): encourages the host/router to install a
     * neighbor cache entry for our global address without requiring multicast NS.
     */
    if (nif->ipv6_router_valid) {
        uint8_t rmac[6];
        if (nd_lookup_mac(nif->ipv6_router_ll, rmac) == 0) {
            /* Prefer router global (::1 within the learned /64) if available; some stacks
             * may drop ND messages sent to a link-local destination with a global source.
             */
            if (nif->ipv6_prefix_len == 64) {
                uint8_t router_global[16];
                memcpy_u8(router_global, nif->ipv6_prefix, 16);
                for (int i = 8; i < 16; i++) router_global[i] = 0;
                router_global[15] = 1;
                (void)ipv6_send_icmp(nif, /*src_ip_opt=*/target_ip, router_global, rmac,
                                     /*hop_limit=*/255, ICMPV6_NEIGHBOR_ADVERT, 0, body, sizeof(body));
            }
            (void)ipv6_send_icmp(nif, /*src_ip_opt=*/target_ip, nif->ipv6_router_ll, rmac,
                                 /*hop_limit=*/255, ICMPV6_NEIGHBOR_ADVERT, 0, body, sizeof(body));
        }
    }

    return 0;
}

static void uart_write_ipv6_hex(const uint8_t ip[16]) {
    static const char *hex = "0123456789abcdef";
    char buf[6];
    buf[4] = ':';
    buf[5] = '\0';

    for (int i = 0; i < 16; i += 2) {
        uint8_t b0 = ip[i];
        uint8_t b1 = ip[i + 1];
        buf[0] = hex[(b0 >> 4) & 0x0f];
        buf[1] = hex[b0 & 0x0f];
        buf[2] = hex[(b1 >> 4) & 0x0f];
        buf[3] = hex[b1 & 0x0f];
        if (i == 14) {
            buf[4] = '\0';
        }
        uart_write(buf);
        buf[4] = ':';
    }
}

static int ipv6_send_icmp(netif_t *nif,
                          const uint8_t src_ip_opt[16],
                          const uint8_t dst_ip[16],
                          const uint8_t dst_mac[6],
                          uint8_t hop_limit,
                          uint8_t icmp_type,
                          uint8_t icmp_code,
                          const uint8_t *icmp_body,
                          size_t icmp_body_len) {
    uint8_t pkt[40 + 128];
    size_t icmp_len = sizeof(icmpv6_hdr_t) + icmp_body_len;
    size_t total_len = sizeof(ipv6_hdr_t) + icmp_len;
    if (total_len > sizeof(pkt)) return -(int)EMSGSIZE;

    ipv6_hdr_t *ip6 = (ipv6_hdr_t *)pkt;
    /* v=6, tc=0, fl=0 */
    be32_store((uint8_t *)&ip6->vtcfl_be, 0x60000000u);
    be16_store((uint8_t *)&ip6->payload_len_be, (uint16_t)icmp_len);
    ip6->next_header = IPV6_NH_ICMPV6;
    ip6->hop_limit = hop_limit;

    const uint8_t *src_sel = src_ip_opt ? src_ip_opt : ipv6_select_src_ip(nif, dst_ip);
    if (!src_sel) return -(int)EAFNOSUPPORT;

    memcpy_u8(ip6->src, src_sel, 16);
    memcpy_u8(ip6->dst, dst_ip, 16);

    icmpv6_hdr_t *icmp = (icmpv6_hdr_t *)(pkt + sizeof(ipv6_hdr_t));
    icmp->type = icmp_type;
    icmp->code = icmp_code;
    icmp->csum_be = 0;
    if (icmp_body_len) {
        memcpy_u8((uint8_t *)icmp + sizeof(icmpv6_hdr_t), icmp_body, icmp_body_len);
    }

    uint16_t csum = icmpv6_checksum(ip6->src, ip6->dst, (const uint8_t *)icmp, icmp_len);
    icmp->csum_be = (uint16_t)((csum >> 8) | (csum << 8)); /* store big-endian without helpers */

    return eth_send_ipv6(nif, dst_mac, pkt, total_len);
}

static int send_neighbor_solicitation(netif_t *nif, const uint8_t target_ip[16]) {
    uint8_t dst_ip[16];
    ipv6_make_solicited_node_multicast(dst_ip, target_ip);

    uint8_t dst_mac[6];
    ipv6_multicast_to_eth(dst_ip, dst_mac);

    /* ICMPv6 NS body:
     * 4 bytes reserved + 16 bytes target + option (SLLA) 8 bytes.
     */
    uint8_t body[4 + 16 + 8];
    for (size_t i = 0; i < sizeof(body); i++) body[i] = 0;

    /* target */
    memcpy_u8(body + 4, target_ip, 16);

    /* Option: Source Link-Layer Address (type=1, len=1 (8 bytes)). */
    body[4 + 16 + 0] = 1;
    body[4 + 16 + 1] = 1;
    memcpy_u8(body + 4 + 16 + 2, nif->mac, 6);

    g_ipv6_dbg.tx_icmpv6_ns++;
    return ipv6_send_icmp(nif, /*src_ip_opt=*/0, dst_ip, dst_mac, /*hop_limit=*/255, ICMPV6_NEIGHBOR_SOLICIT, 0, body, sizeof(body));
}

static int send_neighbor_advertisement(netif_t *nif,
                                      const uint8_t dst_ip[16],
                                      const uint8_t dst_mac[6],
                                      const uint8_t target_ip[16]) {
    /* ICMPv6 NA body:
     * 4 bytes flags/reserved + 16 bytes target + option (TLLA) 8 bytes.
     */
    uint8_t body[4 + 16 + 8];
    for (size_t i = 0; i < sizeof(body); i++) body[i] = 0;

    /* Set S(olicited)=1, O(verride)=1. */
    body[0] = 0x60; /* 0110 0000 */

    memcpy_u8(body + 4, target_ip, 16);

    /* Option: Target Link-Layer Address (type=2, len=1). */
    body[4 + 16 + 0] = 2;
    body[4 + 16 + 1] = 1;
    memcpy_u8(body + 4 + 16 + 2, nif->mac, 6);

    return ipv6_send_icmp(nif, /*src_ip_opt=*/0, dst_ip, dst_mac, /*hop_limit=*/255, ICMPV6_NEIGHBOR_ADVERT, 0, body, sizeof(body));
}

static int send_router_solicitation(netif_t *nif) {
    uint8_t dst_ip[16];
    for (int i = 0; i < 16; i++) dst_ip[i] = 0;
    dst_ip[0] = 0xff;
    dst_ip[1] = 0x02;
    dst_ip[15] = 0x02; /* ff02::2 all-routers */

    uint8_t dst_mac[6];
    ipv6_multicast_to_eth(dst_ip, dst_mac);

    /* ICMPv6 RS body: 4 bytes reserved + option (SLLA) 8 bytes. */
    uint8_t body[4 + 8];
    for (size_t i = 0; i < sizeof(body); i++) body[i] = 0;
    body[4 + 0] = 1;
    body[4 + 1] = 1;
    memcpy_u8(body + 4 + 2, nif->mac, 6);

    g_ipv6_dbg.tx_icmpv6_rs++;
    return ipv6_send_icmp(nif, /*src_ip_opt=*/0, dst_ip, dst_mac, /*hop_limit=*/255, ICMPV6_ROUTER_SOLICIT, 0, body, sizeof(body));
}

static int send_echo_request(netif_t *nif, const uint8_t dst_ip[16], const uint8_t dst_mac[6], uint16_t ident, uint16_t seq) {
    /* ICMPv6 echo body: ident, seq, payload. */
    uint8_t body[4 + 32];
    be16_store(body + 0, ident);
    be16_store(body + 2, seq);
    for (size_t i = 0; i < 32; i++) body[4 + i] = (uint8_t)('A' + (i % 26));

    g_ipv6_dbg.tx_icmpv6_echo_req++;
    return ipv6_send_icmp(nif, /*src_ip_opt=*/0, dst_ip, dst_mac, /*hop_limit=*/64, ICMPV6_ECHO_REQUEST, 0, body, sizeof(body));
}

static int send_echo_reply(netif_t *nif,
                           const uint8_t src_ip_opt[16],
                           const uint8_t dst_ip[16],
                           const uint8_t dst_mac[6],
                           const uint8_t *echo_body,
                           size_t echo_body_len) {
    return ipv6_send_icmp(nif, src_ip_opt, dst_ip, dst_mac, /*hop_limit=*/64, ICMPV6_ECHO_REPLY, 0, echo_body, echo_body_len);
}

static void maybe_kick_ping_after_nd_update(const uint8_t updated_ip[16]) {
    if (!g_ping_inflight || g_ping_phase != 1) return;
    if (!g_ping_nif) return;
    if (!memeq(updated_ip, g_ping_nh_ip, 16)) return;

    uint8_t mac[6];
    if (nd_lookup_mac(g_ping_nh_ip, mac) != 0) return;

    if (g_ping_proc_idx < 0 || g_ping_proc_idx >= (int)MAX_PROCS) return;
    proc_t *p = &g_procs[g_ping_proc_idx];
    if (!p->pending_ping6) return;

    /* Send echo now. */
    if (send_echo_request(g_ping_nif, g_ping_dst_ip, mac, g_ping_ident, g_ping_seq) == 0) {
        p->ping6_start_ns = time_now_ns();
        g_ping_phase = 2;
    }
}

void net_ipv6_init(void) {
    for (size_t i = 0; i < sizeof(g_ipv6_dbg); i++) {
        ((uint8_t *)&g_ipv6_dbg)[i] = 0;
    }
    for (int i = 0; i < ND_CACHE_SIZE; i++) g_nd[i].used = 0;
    g_ping_inflight = 0;
    g_ping_proc_idx = -1;
    g_ping_nif = 0;
    g_ping_phase = 0;

    net_udp6_init();
}

void net_ipv6_configure_netif(netif_t *nif) {
    if (!nif) return;
    ipv6_make_link_local_from_mac(nif->ipv6_ll, nif->mac);
    nif->ipv6_ll_valid = 1;

    /* Ensure we receive NDP/RA multicast early (especially important for USB RNDIS). */
    netif_ipv6_update_multicast_list(nif);

    /* Best-effort: kick Router Solicitation to speed up SLAAC in TAP setups. */
    (void)send_router_solicitation(nif);
    nif->ipv6_last_rs_ns = time_now_ns();
}

int net_ipv6_ping6_start(int proc_idx, netif_t *nif, const uint8_t dst_ip[16], uint16_t ident, uint16_t seq) {
    if (!nif || !dst_ip) return -(int)EINVAL;
    if (proc_idx < 0 || proc_idx >= (int)MAX_PROCS) return -(int)EINVAL;

    g_ipv6_dbg.ping6_start_calls++;

    if (!nif->ipv6_ll_valid) {
        /* Ensure we have a link-local address and send an RS to trigger SLAAC. */
        net_ipv6_configure_netif(nif);
    }

    int dst_is_mcast = ipv6_is_multicast(dst_ip);
    if (!ipv6_is_linklocal(dst_ip) && !dst_is_mcast) {
        /* Off-link/global requires us to have a configured global address. */
        if (!nif->ipv6_global_valid) {
            /* We might have sent an RS very early (before the host TAP router was ready).
             * Re-send RS occasionally so short ping timeouts still converge quickly.
             */
            uint64_t now = time_now_ns();
            if (now != 0) {
                uint64_t last = nif->ipv6_last_rs_ns;
                if (last == 0 || now - last >= 250000000ull) {
                    (void)send_router_solicitation(nif);
                    nif->ipv6_last_rs_ns = now;
                }
            }
            g_ipv6_dbg.ping6_start_eagain++;
            return -(int)EAGAIN;
        }
    }

    if (g_ping_inflight) {
        g_ipv6_dbg.ping6_start_ebusy++;
        return -(int)EBUSY;
    }

    g_ping_inflight = 1;
    g_ping_proc_idx = proc_idx;
    g_ping_nif = nif;
    memcpy_u8(g_ping_dst_ip, dst_ip, 16);
    g_ping_ident = ident;
    g_ping_seq = seq;

    uint8_t nh_ip[16];
    if (dst_is_mcast) {
        memcpy_u8(nh_ip, dst_ip, 16);
    } else {
        int rc = ipv6_select_next_hop(nif, dst_ip, nh_ip);
        if (rc < 0) {
            g_ping_inflight = 0;
            g_ping_proc_idx = -1;
            g_ping_nif = 0;
            g_ping_phase = 0;
            return rc;
        }
    }
    memcpy_u8(g_ping_nh_ip, nh_ip, 16);

    uint8_t mac[6];
    if (dst_is_mcast) {
        ipv6_multicast_to_eth(dst_ip, mac);
        proc_t *p = &g_procs[proc_idx];
        p->ping6_start_ns = time_now_ns();
        g_ping_phase = 2;
        g_ipv6_dbg.ping6_start_sent_echo++;
        return send_echo_request(nif, dst_ip, mac, ident, seq);
    }

    if (nd_lookup_mac(nh_ip, mac) == 0) {
        /* Next hop known: send echo immediately. */
        proc_t *p = &g_procs[proc_idx];
        p->ping6_start_ns = time_now_ns();
        g_ping_phase = 2;
        g_ipv6_dbg.ping6_start_sent_echo++;
        return send_echo_request(nif, dst_ip, mac, ident, seq);
    }

    /* Best-effort fallback: use router MAC as L2 next hop for unresolved on-link addresses. */
    if (nif->ipv6_router_valid && memeq(nh_ip, dst_ip, 16) &&
        !ipv6_is_linklocal(dst_ip) && !ipv6_is_multicast(dst_ip)) {
        if (nd_lookup_mac(nif->ipv6_router_ll, mac) == 0) {
            proc_t *p = &g_procs[proc_idx];
            p->ping6_start_ns = time_now_ns();
            g_ping_phase = 2;
            g_ipv6_dbg.ping6_start_sent_echo++;
            return send_echo_request(nif, dst_ip, mac, ident, seq);
        }
    }

    /* Neighbor unknown: start NDP. */
    g_ping_phase = 1;
    g_ipv6_dbg.ping6_start_sent_ns++;
    return send_neighbor_solicitation(nif, nh_ip);
}

void net_ipv6_input(netif_t *nif, const uint8_t src_mac[6], const uint8_t *pkt, size_t len) {
    if (!nif || !src_mac || !pkt) return;
    if (len < sizeof(ipv6_hdr_t)) {
        nif->rx_drops++;
        g_ipv6_dbg.rx_drop_short++;
        return;
    }

    g_ipv6_dbg.rx_ipv6_packets++;

    const ipv6_hdr_t *ip6 = (const ipv6_hdr_t *)pkt;
    uint32_t vtcfl = ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16) | ((uint32_t)pkt[2] << 8) | (uint32_t)pkt[3];
    uint8_t ver = (uint8_t)((vtcfl >> 28) & 0x0f);
    if (ver != 6) return;

    uint16_t payload_len = be16_load((const uint8_t *)&ip6->payload_len_be);
    if ((size_t)payload_len + sizeof(ipv6_hdr_t) > len) {
        nif->rx_drops++;
        g_ipv6_dbg.rx_drop_len++;
        return;
    }

    if (!nif->ipv6_ll_valid) {
        net_ipv6_configure_netif(nif);
    }

    /*
     * Opportunistically learn the sender's MAC from any IPv6 packet.
     * This is important for early bring-up / QEMU user networking where NDP
     * replies might be missing or delayed, but Router Advertisements still
     * provide a trustworthy (src IP, src MAC) pair.
     */
    int src_is_unspec = 1;
    for (int i = 0; i < 16; i++) {
        if (ip6->src[i] != 0) {
            src_is_unspec = 0;
            break;
        }
    }
    if (!src_is_unspec && !ipv6_is_multicast(ip6->src)) {
        nd_update(ip6->src, src_mac);
    }

    const uint8_t *payload = pkt + sizeof(ipv6_hdr_t);
    size_t pl_len = (size_t)payload_len;

    if (ip6->next_header == IPV6_NH_UDP) {
        g_ipv6_dbg.rx_udp++;
        if (pl_len < sizeof(udp_hdr_t)) {
            nif->rx_drops++;
            g_ipv6_dbg.rx_drop_short++;
            return;
        }

        /* Accept only if destined to us (link-local or configured global). */
        int dst_ok = memeq(ip6->dst, nif->ipv6_ll, 16) || (nif->ipv6_global_valid && memeq(ip6->dst, nif->ipv6_global, 16));
        if (!dst_ok) return;

        const udp_hdr_t *uh = (const udp_hdr_t *)payload;
        uint16_t src_port = be16_load((const uint8_t *)&uh->src_port_be);
        uint16_t dst_port = be16_load((const uint8_t *)&uh->dst_port_be);
        uint16_t udp_len = be16_load((const uint8_t *)&uh->len_be);
        if (udp_len < sizeof(udp_hdr_t) || (size_t)udp_len > pl_len) {
            nif->rx_drops++;
            g_ipv6_dbg.rx_drop_len++;
            return;
        }

        uint16_t got = (uint16_t)((uh->csum_be >> 8) | (uh->csum_be << 8));
        if (got == 0) {
            nif->rx_drops++;
            g_ipv6_dbg.rx_drop_csum++;
            return;
        }

        /* Verify checksum (best-effort, bounded by UDP6_MAX_PAYLOAD). */
        uint8_t tmp[sizeof(udp_hdr_t) + UDP6_MAX_PAYLOAD];
        if ((size_t)udp_len <= sizeof(tmp)) {
            memcpy_u8(tmp, payload, (size_t)udp_len);
            ((udp_hdr_t *)tmp)->csum_be = 0;
            uint16_t exp = udp6_checksum(ip6->src, ip6->dst, tmp, (size_t)udp_len);
            if (exp != got) {
                nif->rx_drops++;
                g_ipv6_dbg.rx_drop_csum++;
                return;
            }
        }

        /* Cache the sender (helps for quick replies). */
        nd_update(ip6->src, src_mac);

        const uint8_t *udp_payload = payload + sizeof(udp_hdr_t);
        size_t udp_payload_len = (size_t)udp_len - sizeof(udp_hdr_t);
        udp6_deliver(ip6->src, src_port, ip6->dst, dst_port, udp_payload, udp_payload_len);
        g_ipv6_dbg.rx_udp_delivered++;
        return;
    }

    if (ip6->next_header != IPV6_NH_ICMPV6) {
        /* Only ICMPv6 for now. */
        return;
    }

    g_ipv6_dbg.rx_icmpv6++;

    if (pl_len < sizeof(icmpv6_hdr_t)) {
        nif->rx_drops++;
        g_ipv6_dbg.rx_drop_short++;
        return;
    }

    const icmpv6_hdr_t *icmp = (const icmpv6_hdr_t *)payload;
    const uint8_t *icmp_body = payload + sizeof(icmpv6_hdr_t);
    size_t icmp_body_len = pl_len - sizeof(icmpv6_hdr_t);

    /* Basic checksum validation (optional but useful). */
    uint16_t got = (uint16_t)((icmp->csum_be >> 8) | (icmp->csum_be << 8));
    uint8_t tmp[160];
    if (pl_len <= sizeof(tmp)) {
        memcpy_u8(tmp, payload, pl_len);
        ((icmpv6_hdr_t *)tmp)->csum_be = 0;
        uint16_t exp = icmpv6_checksum(ip6->src, ip6->dst, tmp, pl_len);
        if (exp != got) {
            nif->rx_drops++;
            g_ipv6_dbg.rx_drop_csum++;
            return;
        }
    }

    g_ipv6_dbg.last_icmp_type = icmp->type;
    g_ipv6_dbg.last_hop_limit = ip6->hop_limit;

    if (icmp->type == ICMPV6_ROUTER_ADVERT) {
        g_ipv6_dbg.rx_icmpv6_ra++;
        /* Basic RFC checks: hop limit 255, source is link-local.
         * Note: Some host RA setups (observed with dnsmasq in our TAP workflow)
         * transmit RAs with hop limit 64. Accept that as a pragmatic fallback
         * so SLAAC works in development environments.
         */
        if (ip6->hop_limit != 255 && ip6->hop_limit != 64) {
            g_ipv6_dbg.rx_icmpv6_ra_drop_hlim++;
            return;
        }
        if (!ipv6_is_linklocal(ip6->src)) {
            g_ipv6_dbg.rx_icmpv6_ra_drop_src++;
            return;
        }
        if (icmp_body_len < 12) {
            g_ipv6_dbg.rx_icmpv6_ra_drop_short++;
            return;
        }

        uint16_t router_lifetime = be16_load(icmp_body + 2);
        if (router_lifetime != 0) {
            memcpy_u8(nif->ipv6_router_ll, ip6->src, 16);
            nif->ipv6_router_valid = 1;
        } else {
            nif->ipv6_router_valid = 0;
        }

        /* Parse options. */
        const uint8_t *opt = icmp_body + 12;
        size_t opt_len = icmp_body_len - 12;
        while (opt_len >= 2) {
            uint8_t opt_type = opt[0];
            uint8_t opt_l = opt[1];
            size_t opt_bytes = (size_t)opt_l * 8u;
            if (opt_bytes == 0 || opt_bytes > opt_len) break;

            if (opt_type == 1 && opt_bytes >= 8) {
                /* Source Link-Layer Address. */
                nd_update(ip6->src, opt + 2);
            } else if (opt_type == 3 && opt_bytes >= 32) {
                /* Prefix Information. */
                uint8_t prefix_len = opt[2];
                uint8_t flags = opt[3];
                int autonomous = (flags & 0x40u) != 0;
                if (autonomous && prefix_len == 64 && nif->ipv6_ll_valid) {
                    const uint8_t *prefix = opt + 16;
                    uint8_t new_global[16];
                    ipv6_make_global_from_prefix64(new_global, prefix, nif->ipv6_ll);

                    /* Install prefix + global address (best-effort, no DAD yet). */
                    int send_una = !nif->ipv6_global_valid || !memeq(nif->ipv6_global, new_global, 16);
                    memcpy_u8(nif->ipv6_prefix, prefix, 16);
                    nif->ipv6_prefix_len = prefix_len;
                    memcpy_u8(nif->ipv6_global, new_global, 16);
                    nif->ipv6_global_valid = 1;

                    /* Now that we have a stable address, refresh multicast filters
                     * so solicited-node NS for our global address is delivered.
                     */
                    netif_ipv6_update_multicast_list(nif);

                    /* Help the host learn our global->MAC mapping promptly.
                     * This avoids relying on host-to-guest multicast NS delivery.
                     */
                    if (send_una) {
                        (void)send_unsolicited_neighbor_advertisement(nif, nif->ipv6_global);
                    }

                    uart_write("ipv6: slaac global=");
                    uart_write_ipv6_hex(nif->ipv6_global);
                    uart_write(" router=");
                    uart_write_ipv6_hex(nif->ipv6_router_ll);
                    uart_write("\n");
                }
            } else if (opt_type == 25 && opt_bytes >= 24) {
                /* RDNSS (RFC 8106): type=25, len, 2B reserved, 4B lifetime, then 1+ IPv6 addrs. */
                uint32_t lifetime_s = be32_load(opt + 4);
                size_t addr_bytes = opt_bytes - 8u;
                size_t addr_count = addr_bytes / 16u;
                if (addr_count >= 1) {
                    if (lifetime_s != 0) {
                        memcpy_u8(nif->ipv6_dns, opt + 8, 16);
                        nif->ipv6_dns_valid = 1;
                        nif->ipv6_dns_lifetime_s = lifetime_s;

                        uart_write("ipv6: rdnss dns=");
                        uart_write_ipv6_hex(nif->ipv6_dns);
                        uart_write(" lifetime_s=");
                        uart_write_hex_u64((uint64_t)lifetime_s);
                        uart_write("\n");
                    } else {
                        nif->ipv6_dns_valid = 0;
                        nif->ipv6_dns_lifetime_s = 0;
                    }
                }
            }

            opt += opt_bytes;
            opt_len -= opt_bytes;
        }
        return;
    }

    if (icmp->type == ICMPV6_NEIGHBOR_SOLICIT) {
        g_ipv6_dbg.rx_icmpv6_ns++;
        if (icmp_body_len < 4 + 16) return;
        const uint8_t *target = icmp_body + 4;

        /* Parse options for source link-layer address. */
        const uint8_t *opt = icmp_body + 4 + 16;
        size_t opt_len = icmp_body_len - (4 + 16);
        while (opt_len >= 2) {
            uint8_t opt_type = opt[0];
            uint8_t opt_l = opt[1];
            size_t opt_bytes = (size_t)opt_l * 8u;
            if (opt_bytes == 0 || opt_bytes > opt_len) break;
            if (opt_type == 1 && opt_bytes >= 8) {
                nd_update(ip6->src, opt + 2);
                break;
            }
            opt += opt_bytes;
            opt_len -= opt_bytes;
        }

        /* If it's for us, reply with NA (for the exact target being queried). */
        const uint8_t *our_target = 0;
        if (memeq(target, nif->ipv6_ll, 16)) {
            our_target = nif->ipv6_ll;
        } else if (nif->ipv6_global_valid && memeq(target, nif->ipv6_global, 16)) {
            our_target = nif->ipv6_global;
        }
        if (our_target) {
            (void)send_neighbor_advertisement(nif, ip6->src, src_mac, our_target);
        }
        return;
    }

    if (icmp->type == ICMPV6_NEIGHBOR_ADVERT) {
        g_ipv6_dbg.rx_icmpv6_na++;
        if (icmp_body_len < 4 + 16) return;
        const uint8_t *target = icmp_body + 4;

        /* Parse options for target link-layer address. */
        const uint8_t *opt = icmp_body + 4 + 16;
        size_t opt_len = icmp_body_len - (4 + 16);
        while (opt_len >= 2) {
            uint8_t opt_type = opt[0];
            uint8_t opt_l = opt[1];
            size_t opt_bytes = (size_t)opt_l * 8u;
            if (opt_bytes == 0 || opt_bytes > opt_len) break;
            if (opt_type == 2 && opt_bytes >= 8) {
                nd_update(target, opt + 2);
                maybe_kick_ping_after_nd_update(target);
                break;
            }
            opt += opt_bytes;
            opt_len -= opt_bytes;
        }
        return;
    }

    if (icmp->type == ICMPV6_ECHO_REQUEST) {
        g_ipv6_dbg.rx_icmpv6_echo_req++;
        /* Reply only if destined to us. */
        const uint8_t *our_ip = 0;
        if (memeq(ip6->dst, nif->ipv6_ll, 16)) {
            our_ip = nif->ipv6_ll;
        } else if (nif->ipv6_global_valid && memeq(ip6->dst, nif->ipv6_global, 16)) {
            our_ip = nif->ipv6_global;
        } else {
            return;
        }

        (void)send_echo_reply(nif, our_ip, ip6->src, src_mac, icmp_body, icmp_body_len);
        return;
    }

    if (icmp->type == ICMPV6_ECHO_REPLY) {
        g_ipv6_dbg.rx_icmpv6_echo_reply++;
        if (!g_ping_inflight || g_ping_phase != 2) return;
        if (!memeq(ip6->src, g_ping_dst_ip, 16)) return;

        if (icmp_body_len < 4) return;
        uint16_t ident = be16_load(icmp_body + 0);
        uint16_t seq = be16_load(icmp_body + 2);
        if (ident != g_ping_ident || seq != g_ping_seq) return;

        if (g_ping_proc_idx < 0 || g_ping_proc_idx >= (int)MAX_PROCS) return;
        proc_t *p = &g_procs[g_ping_proc_idx];
        uint64_t now = time_now_ns();
        uint64_t start = p->ping6_start_ns;
        uint64_t rtt = (start != 0 && now >= start) ? (now - start) : 0;

        ping_complete(0, rtt);
        return;
    }
}
