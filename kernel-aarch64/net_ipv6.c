#include "net_ipv6.h"

#include "errno.h"
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

static void uart_write_ipv6_hex(const uint8_t ip[16]) {
    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 16; i += 2) {
        uint8_t b0 = ip[i];
        uint8_t b1 = ip[i + 1];
        uart_putc(hex[(b0 >> 4) & 0x0f]);
        uart_putc(hex[b0 & 0x0f]);
        uart_putc(hex[(b1 >> 4) & 0x0f]);
        uart_putc(hex[b1 & 0x0f]);
        if (i != 14) uart_putc(':');
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

    return ipv6_send_icmp(nif, /*src_ip_opt=*/0, dst_ip, dst_mac, /*hop_limit=*/255, ICMPV6_ROUTER_SOLICIT, 0, body, sizeof(body));
}

static int send_echo_request(netif_t *nif, const uint8_t dst_ip[16], const uint8_t dst_mac[6], uint16_t ident, uint16_t seq) {
    /* ICMPv6 echo body: ident, seq, payload. */
    uint8_t body[4 + 32];
    be16_store(body + 0, ident);
    be16_store(body + 2, seq);
    for (size_t i = 0; i < 32; i++) body[4 + i] = (uint8_t)('A' + (i % 26));

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
    for (int i = 0; i < ND_CACHE_SIZE; i++) g_nd[i].used = 0;
    g_ping_inflight = 0;
    g_ping_proc_idx = -1;
    g_ping_nif = 0;
    g_ping_phase = 0;
}

void net_ipv6_configure_netif(netif_t *nif) {
    if (!nif) return;
    ipv6_make_link_local_from_mac(nif->ipv6_ll, nif->mac);
    nif->ipv6_ll_valid = 1;

    /* Best-effort: kick Router Solicitation to speed up SLAAC in TAP setups. */
    (void)send_router_solicitation(nif);
}

int net_ipv6_ping6_start(int proc_idx, netif_t *nif, const uint8_t dst_ip[16], uint16_t ident, uint16_t seq) {
    if (!nif || !dst_ip) return -(int)EINVAL;
    if (proc_idx < 0 || proc_idx >= (int)MAX_PROCS) return -(int)EINVAL;

    int dst_is_mcast = ipv6_is_multicast(dst_ip);
    if (!ipv6_is_linklocal(dst_ip) && !dst_is_mcast) {
        /* Off-link/global requires us to have a configured global address. */
        if (!nif->ipv6_global_valid) return -(int)EAFNOSUPPORT;
    }

    if (!nif->ipv6_ll_valid) {
        net_ipv6_configure_netif(nif);
    }

    if (g_ping_inflight) {
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
        return send_echo_request(nif, dst_ip, mac, ident, seq);
    }

    if (nd_lookup_mac(nh_ip, mac) == 0) {
        /* Next hop known: send echo immediately. */
        proc_t *p = &g_procs[proc_idx];
        p->ping6_start_ns = time_now_ns();
        g_ping_phase = 2;
        return send_echo_request(nif, dst_ip, mac, ident, seq);
    }

    /* Neighbor unknown: start NDP. */
    g_ping_phase = 1;
    return send_neighbor_solicitation(nif, nh_ip);
}

void net_ipv6_input(netif_t *nif, const uint8_t src_mac[6], const uint8_t *pkt, size_t len) {
    if (!nif || !src_mac || !pkt) return;
    if (len < sizeof(ipv6_hdr_t)) {
        nif->rx_drops++;
        return;
    }

    const ipv6_hdr_t *ip6 = (const ipv6_hdr_t *)pkt;
    uint32_t vtcfl = ((uint32_t)pkt[0] << 24) | ((uint32_t)pkt[1] << 16) | ((uint32_t)pkt[2] << 8) | (uint32_t)pkt[3];
    uint8_t ver = (uint8_t)((vtcfl >> 28) & 0x0f);
    if (ver != 6) return;

    uint16_t payload_len = be16_load((const uint8_t *)&ip6->payload_len_be);
    if ((size_t)payload_len + sizeof(ipv6_hdr_t) > len) {
        nif->rx_drops++;
        return;
    }

    if (!nif->ipv6_ll_valid) {
        net_ipv6_configure_netif(nif);
    }

    const uint8_t *payload = pkt + sizeof(ipv6_hdr_t);
    size_t pl_len = (size_t)payload_len;

    if (ip6->next_header != IPV6_NH_ICMPV6) {
        /* Only ICMPv6 for now. */
        return;
    }

    if (pl_len < sizeof(icmpv6_hdr_t)) {
        nif->rx_drops++;
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
            return;
        }
    }

    if (icmp->type == ICMPV6_ROUTER_ADVERT) {
        /* Basic RFC checks: hop limit 255, source is link-local. */
        if (ip6->hop_limit != 255) return;
        if (!ipv6_is_linklocal(ip6->src)) return;
        if (icmp_body_len < 12) return;

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
                    memcpy_u8(nif->ipv6_prefix, prefix, 16);
                    nif->ipv6_prefix_len = prefix_len;
                    memcpy_u8(nif->ipv6_global, new_global, 16);
                    nif->ipv6_global_valid = 1;

                    uart_write("ipv6: slaac global=");
                    uart_write_ipv6_hex(nif->ipv6_global);
                    uart_write(" router=");
                    uart_write_ipv6_hex(nif->ipv6_router_ll);
                    uart_write("\n");
                }
            }

            opt += opt_bytes;
            opt_len -= opt_bytes;
        }
        return;
    }

    if (icmp->type == ICMPV6_NEIGHBOR_SOLICIT) {
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

        /* If it's for us, reply with NA. */
        if (memeq(target, nif->ipv6_ll, 16) || (nif->ipv6_global_valid && memeq(target, nif->ipv6_global, 16))) {
            (void)send_neighbor_advertisement(nif, ip6->src, src_mac, nif->ipv6_ll);
        }
        return;
    }

    if (icmp->type == ICMPV6_NEIGHBOR_ADVERT) {
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
