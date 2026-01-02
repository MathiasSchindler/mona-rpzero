#include "syscall.h"

enum {
    DNS_PORT = 53,
    DNS_MAX_MSG = 512,
};

typedef struct __attribute__((packed)) {
    uint16_t id_be;
    uint16_t flags_be;
    uint16_t qdcount_be;
    uint16_t ancount_be;
    uint16_t nscount_be;
    uint16_t arcount_be;
} dns_hdr_t;

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static void write_all(const char *s) {
    (void)sys_write(1, s, cstr_len_u64_local(s));
}

static void write_u64_dec(uint64_t v) {
    char buf[32];
    uint64_t n = 0;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[32];
        uint64_t t = v;
        uint64_t m = 0;
        while (t > 0 && m < sizeof(tmp)) {
            tmp[m++] = (char)('0' + (t % 10));
            t /= 10;
        }
        while (m > 0) {
            buf[n++] = tmp[--m];
        }
    }
    (void)sys_write(1, buf, n);
}

static uint64_t now_ms_monotonic(void) {
    linux_timespec_t ts;
    if ((int64_t)sys_clock_gettime(1, &ts) < 0) {
        return 0;
    }
    uint64_t s = (ts.tv_sec < 0) ? 0 : (uint64_t)ts.tv_sec;
    uint64_t ns = (ts.tv_nsec < 0) ? 0 : (uint64_t)ts.tv_nsec;
    return s * 1000ull + (ns / 1000000ull);
}

static int is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint8_t hex_val(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
    if (c >= 'A' && c <= 'F') return (uint8_t)(10 + (c - 'A'));
    return 0xff;
}

static int parse_ipv6(const char *s, uint8_t out[16]) {
    if (!s || !out) return -1;

    uint16_t words[8];
    for (int i = 0; i < 8; i++) words[i] = 0;

    int nwords = 0;
    int compress_at = -1;

    const char *p = s;
    if (*p == ':') {
        if (p[1] != ':') return -1;
        compress_at = 0;
        p += 2;
    }

    while (*p != '\0') {
        if (nwords >= 8) return -1;

        if (*p == ':') {
            if (p[1] != ':') return -1;
            if (compress_at >= 0) return -1;
            compress_at = nwords;
            p += 2;
            if (*p == '\0') break;
            continue;
        }

        uint32_t v = 0;
        int digits = 0;
        while (*p != '\0' && *p != ':') {
            if (!is_hex(*p)) return -1;
            uint8_t hv = hex_val(*p);
            if (hv == 0xff) return -1;
            v = (v << 4) | (uint32_t)hv;
            digits++;
            if (digits > 4) return -1;
            p++;
        }
        if (digits == 0) return -1;
        words[nwords++] = (uint16_t)v;

        if (*p == ':') {
            if (p[1] == ':') {
                continue;
            }
            p++;
            if (*p == '\0') return -1;
        }
    }

    if (compress_at >= 0) {
        int fill = 8 - nwords;
        if (fill < 0) return -1;

        for (int i = nwords - 1; i >= compress_at; i--) {
            words[i + fill] = words[i];
        }
        for (int i = compress_at; i < compress_at + fill; i++) {
            words[i] = 0;
        }
        nwords = 8;
    }

    if (nwords != 8) return -1;

    for (int i = 0; i < 8; i++) {
        out[i * 2 + 0] = (uint8_t)((words[i] >> 8) & 0xff);
        out[i * 2 + 1] = (uint8_t)(words[i] & 0xff);
    }
    return 0;
}

static void write_ipv6_full(const uint8_t ip[16]) {
    static const char *hex = "0123456789abcdef";
    char out[8 * 4 + 7];
    uint64_t n = 0;
    for (int w = 0; w < 8; w++) {
        uint16_t v = (uint16_t)((uint16_t)ip[w * 2] << 8) | (uint16_t)ip[w * 2 + 1];
        out[n++] = hex[(v >> 12) & 0xf];
        out[n++] = hex[(v >> 8) & 0xf];
        out[n++] = hex[(v >> 4) & 0xf];
        out[n++] = hex[v & 0xf];
        if (w != 7) out[n++] = ':';
    }
    (void)sys_write(1, out, n);
}

static uint16_t be16_load(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void be16_store(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

static int dns_encode_name(const char *name, uint8_t *out, uint64_t out_cap, uint64_t *out_len) {
    if (!name || !out || !out_len) return -1;

    uint64_t n = 0;
    uint64_t i = 0;

    if (name[0] == '\0') return -1;

    while (name[i] != '\0') {
        /* One label */
        uint64_t label_len = 0;
        uint64_t label_start = i;
        while (name[i] != '\0' && name[i] != '.') {
            label_len++;
            i++;
            if (label_len > 63) return -1;
        }

        if (label_len == 0) return -1;
        if (n + 1 + label_len > out_cap) return -1;
        out[n++] = (uint8_t)label_len;
        for (uint64_t k = 0; k < label_len; k++) {
            out[n++] = (uint8_t)name[label_start + k];
        }

        if (name[i] == '.') {
            i++;
            if (name[i] == '\0') {
                /* trailing dot */
                break;
            }
        }
    }

    if (n + 1 > out_cap) return -1;
    out[n++] = 0;
    *out_len = n;
    return 0;
}

static int dns_skip_name(const uint8_t *msg, uint64_t msg_len, uint64_t off, uint64_t *out_next) {
    if (!msg || !out_next) return -1;

    uint64_t cur = off;
    int jumped = 0;
    uint64_t jump_count = 0;

    for (;;) {
        if (cur >= msg_len) return -1;
        uint8_t len = msg[cur];
        if ((len & 0xc0u) == 0xc0u) {
            /* Compression pointer: two bytes */
            if (cur + 1 >= msg_len) return -1;
            if (!jumped) {
                *out_next = cur + 2;
            }
            /* Follow pointer for validation only */
            uint16_t ptr = (uint16_t)(((uint16_t)(len & 0x3fu) << 8) | (uint16_t)msg[cur + 1]);
            cur = (uint64_t)ptr;
            jumped = 1;
            if (++jump_count > 16) return -1;
            continue;
        }
        if (len == 0) {
            if (!jumped) {
                *out_next = cur + 1;
            }
            return 0;
        }
        if (len & 0xc0u) return -1;
        if (cur + 1 + (uint64_t)len > msg_len) return -1;
        cur += 1 + (uint64_t)len;
        if (!jumped) {
            /* continue consuming sequentially */
        }
    }
}

static void usage(void) {
    write_all("usage: dns6 <name> [dns_server_ipv6] [timeout_ms]\n");
    write_all("  default server: RA RDNSS, else fec0::3 (QEMU slirp), else 2001:4860:4860::8888 (Google)\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc < 2) {
        usage();
        return 1;
    }

    const char *qname = argv[1];

    uint8_t dns_ip[16];
    if (argc >= 3) {
        if (parse_ipv6(argv[2], dns_ip) != 0) {
            write_all("dns6: invalid dns_server_ipv6\n");
            return 1;
        }
    } else {
        /* Prefer RA-learned DNS server (RDNSS). Fallback to Google DNS. */
        if (sys_mona_net6_get_dns(dns_ip) != 0) {
            /* QEMU user-mode networking (slirp) commonly provides an IPv6 DNS server at fec0::3. */
            const uint8_t slirp[16] = {0xfe,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03};
            for (int i = 0; i < 16; i++) dns_ip[i] = slirp[i];
        }
    }

    write_all("dns6: server=");
    write_ipv6_full(dns_ip);
    write_all("\n");

    uint64_t timeout_ms = 1500;
    if (argc >= 4) {
        /* simple decimal */
        uint64_t v = 0;
        uint64_t i = 0;
        if (argv[3][0] == '\0') {
            write_all("dns6: invalid timeout\n");
            return 1;
        }
        while (argv[3][i] != '\0') {
            char c = argv[3][i++];
            if (c < '0' || c > '9') {
                write_all("dns6: invalid timeout\n");
                return 1;
            }
            v = v * 10 + (uint64_t)(c - '0');
        }
        timeout_ms = v;
    }

    uint64_t fd = sys_mona_udp6_socket();
    if ((int64_t)fd < 0) {
        write_all("dns6: udp6_socket failed errno=");
        write_u64_dec((uint64_t)(-(int64_t)fd));
        write_all("\n");
        return 1;
    }

    /* Build query */
    uint8_t msg[DNS_MAX_MSG];
    for (uint64_t i = 0; i < sizeof(msg); i++) msg[i] = 0;

    dns_hdr_t *h = (dns_hdr_t *)msg;
    uint16_t id = (uint16_t)(sys_getpid() & 0xffffu) ^ 0x1234u;
    be16_store((uint8_t *)&h->id_be, id);
    /* RD=1 */
    be16_store((uint8_t *)&h->flags_be, 0x0100u);
    be16_store((uint8_t *)&h->qdcount_be, 1);

    uint64_t off = sizeof(dns_hdr_t);
    uint64_t name_len = 0;
    if (dns_encode_name(qname, msg + off, sizeof(msg) - off, &name_len) != 0) {
        write_all("dns6: invalid name\n");
        return 1;
    }
    off += name_len;
    if (off + 4 > sizeof(msg)) {
        write_all("dns6: name too long\n");
        return 1;
    }

    /* QTYPE AAAA (28), QCLASS IN (1) */
    be16_store(msg + off + 0, 28);
    be16_store(msg + off + 2, 1);
    off += 4;

    /* Send (retry on EAGAIN due to NDP) */
    uint64_t start_ms = now_ms_monotonic();
    for (;;) {
        uint64_t elapsed_ms = now_ms_monotonic() - start_ms;
        if (elapsed_ms >= timeout_ms) {
            write_all("dns6: sendto timed out\n");
            return 1;
        }

        uint64_t rc = sys_mona_udp6_sendto(fd, dns_ip, DNS_PORT, msg, off);
        if ((int64_t)rc == -(int64_t)11) {
            /* EAGAIN: neighbor unresolved */
            linux_timespec_t ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 100 * 1000 * 1000;
            (void)sys_nanosleep(&ts, 0);
            continue;
        }
        if ((int64_t)rc < 0) {
            write_all("dns6: sendto failed errno=");
            write_u64_dec((uint64_t)(-(int64_t)rc));
            write_all("\n");
            return 1;
        }
        break;
    }

    uint8_t rx[DNS_MAX_MSG];
    uint8_t src_ip[16];
    uint16_t src_port = 0;

    uint64_t elapsed_ms = now_ms_monotonic() - start_ms;
    uint64_t remain_ms = (elapsed_ms >= timeout_ms) ? 0 : (timeout_ms - elapsed_ms);
    if (remain_ms == 0) {
        write_all("dns6: timed out\n");
        return 1;
    }

    uint64_t n = sys_mona_udp6_recvfrom(fd, rx, sizeof(rx), src_ip, &src_port, remain_ms);
    if ((int64_t)n < 0) {
        write_all("dns6: recvfrom failed errno=");
        write_u64_dec((uint64_t)(-(int64_t)n));
        write_all("\n");
        return 1;
    }
    if (n < sizeof(dns_hdr_t)) {
        write_all("dns6: short response\n");
        return 1;
    }

    const dns_hdr_t *rh = (const dns_hdr_t *)rx;
    uint16_t rid = be16_load((const uint8_t *)&rh->id_be);
    uint16_t rflags = be16_load((const uint8_t *)&rh->flags_be);
    uint16_t qd = be16_load((const uint8_t *)&rh->qdcount_be);
    uint16_t an = be16_load((const uint8_t *)&rh->ancount_be);

    if (rid != id) {
        write_all("dns6: id mismatch\n");
        return 1;
    }

    /* QR must be 1 */
    if ((rflags & 0x8000u) == 0) {
        write_all("dns6: not a response\n");
        return 1;
    }

    /* RCODE */
    if ((rflags & 0x000fu) != 0) {
        write_all("dns6: rcode=");
        write_u64_dec((uint64_t)(rflags & 0x000fu));
        write_all("\n");
        return 1;
    }

    uint64_t roff = sizeof(dns_hdr_t);

    /* Skip questions */
    for (uint16_t i = 0; i < qd; i++) {
        uint64_t next = 0;
        if (dns_skip_name(rx, n, roff, &next) != 0) {
            write_all("dns6: bad question name\n");
            return 1;
        }
        roff = next;
        if (roff + 4 > n) {
            write_all("dns6: bad question\n");
            return 1;
        }
        roff += 4;
    }

    int printed = 0;

    /* Parse answers */
    for (uint16_t i = 0; i < an; i++) {
        uint64_t next = 0;
        if (dns_skip_name(rx, n, roff, &next) != 0) {
            write_all("dns6: bad answer name\n");
            return 1;
        }
        roff = next;

        if (roff + 10 > n) {
            write_all("dns6: truncated answer\n");
            return 1;
        }

        uint16_t type = be16_load(rx + roff + 0);
        uint16_t klass = be16_load(rx + roff + 2);
        uint16_t rdlen = be16_load(rx + roff + 8);
        roff += 10;

        if (roff + rdlen > n) {
            write_all("dns6: truncated rdata\n");
            return 1;
        }

        if (type == 28 && klass == 1 && rdlen == 16) {
            write_ipv6_full(rx + roff);
            write_all("\n");
            printed++;
        }

        roff += rdlen;
    }

    if (!printed) {
        write_all("dns6: no AAAA records\n");
        return 1;
    }

    return 0;
}
