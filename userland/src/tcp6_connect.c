#include "syscall.h"

int dns6_resolve_aaaa_one(const char *name, const uint8_t dns_ip[16], uint64_t timeout_ms, uint8_t out_ip[16]);
int parse_ipv6_literal(const char *s, uint8_t out[16]);

enum { DEFAULT_TIMEOUT_MS = 5000 };

typedef struct {
    uint8_t bytes[16];
} ipv6_addr_t;

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

static int parse_u64(const char *s, uint64_t *out) {
    if (!s || !out) return -1;
    uint64_t v = 0;
    uint64_t i = 0;
    if (s[0] == '\0') return -1;
    while (s[i] != '\0') {
        char c = s[i++];
        if (c < '0' || c > '9') return -1;
        v = v * 10 + (uint64_t)(c - '0');
    }
    *out = v;
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

static void usage(void) {
    write_all("usage: tcp6_connect <ipv6-addr|hostname> [port] [timeout_ms] [dns_server_ipv6]\n");
    write_all("  default port: 443\n");
    write_all("  if port==80, sends HTTP GET / and prints response\n");
}

static int parse_ipv6_or_resolve(const char *s,
                                uint64_t timeout_ms,
                                const uint8_t dns_ip[16],
                                uint8_t out_ip[16]) {
    if (parse_ipv6_literal(s, out_ip) == 0) return 0;
    return dns6_resolve_aaaa_one(s, dns_ip, timeout_ms, out_ip);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc < 2) {
        usage();
        return 1;
    }

    uint64_t port = 443;
    uint64_t timeout_ms = DEFAULT_TIMEOUT_MS;

    if (argc >= 3) {
        if (parse_u64(argv[2], &port) != 0 || port == 0 || port > 65535ull) {
            write_all("tcp6_connect: invalid port\n");
            return 1;
        }
    }

    if (argc >= 4) {
        if (parse_u64(argv[3], &timeout_ms) != 0) {
            write_all("tcp6_connect: invalid timeout_ms\n");
            return 1;
        }
    }

    uint8_t dns_ip[16];
    if (argc >= 5) {
        if (parse_ipv6_literal(argv[4], dns_ip) != 0) {
            write_all("tcp6_connect: invalid dns_server_ipv6\n");
            return 1;
        }
    } else {
        if (sys_mona_net6_get_dns(dns_ip) != 0) {
            const uint8_t slirp[16] = {0xfe,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03};
            for (int i = 0; i < 16; i++) dns_ip[i] = slirp[i];
        }
    }

    uint8_t dst_ip[16];
    int rc = -1;

    write_all("tcp6_connect: dns server=");
    write_ipv6_full(dns_ip);
    write_all("\n");

    for (int attempt = 0; attempt < 3; attempt++) {
        rc = parse_ipv6_or_resolve(argv[1], timeout_ms, dns_ip, dst_ip);
        if (rc == 0) break;
        if (rc != -(int)110) break;
        write_all("tcp6_connect: resolve timeout, retrying...\n");
    }

    if (rc != 0) {
        write_all("tcp6_connect: resolve failed errno=");
        write_u64_dec((uint64_t)(-(int64_t)rc));
        write_all("\n");
        return 1;
    }

    write_all("tcp6_connect: resolved ");
    write_all(argv[1]);
    write_all(" -> ");
    write_ipv6_full(dst_ip);
    write_all("\n");

    uint64_t fd = sys_mona_tcp6_connect(dst_ip, port, timeout_ms);
    if ((int64_t)fd < 0) {
        write_all("tcp6_connect: connect failed errno=");
        write_u64_dec((uint64_t)(-(int64_t)fd));
        write_all("\n");
        return 1;
    }

    write_all("tcp6_connect: connected fd=");
    write_u64_dec(fd);
    write_all("\n");

    if (port == 80) {
        const char *req = "GET / HTTP/1.1\r\nHost: de.wikipedia.org\r\nConnection: close\r\n\r\n";
        uint64_t s = sys_mona_tcp6_send(fd, req, cstr_len_u64_local(req));
        if ((int64_t)s < 0) {
            write_all("tcp6_connect: send failed errno=");
            write_u64_dec((uint64_t)(-(int64_t)s));
            write_all("\n");
            return 1;
        }

        uint8_t buf[1024];
        for (;;) {
            uint64_t n = sys_mona_tcp6_recv(fd, buf, sizeof(buf), 1000);
            if ((int64_t)n < 0) {
                uint64_t err = (uint64_t)(-(int64_t)n);
                if (err == 110ull) break; /* ETIMEDOUT */
                write_all("tcp6_connect: recv failed errno=");
                write_u64_dec(err);
                write_all("\n");
                return 1;
            }
            if (n == 0) break;
            (void)sys_write(1, buf, n);
        }
    }

    return 0;
}
