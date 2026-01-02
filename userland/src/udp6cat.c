#include "syscall.h"

enum { UDP6_MAX_PAYLOAD_LOCAL = 1200 };

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

static int cstr_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    uint64_t i = 0;
    for (;;) {
        char ca = a[i];
        char cb = b[i];
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
        i++;
    }
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

static void sleep_ms(uint64_t ms) {
    linux_timespec_t ts;
    ts.tv_sec = (int64_t)(ms / 1000ull);
    ts.tv_nsec = (int64_t)((ms % 1000ull) * 1000ull * 1000ull);
    (void)sys_nanosleep(&ts, 0);
}

static void usage(void) {
    write_all("usage:\n");
    write_all("  udp6cat -l <port> [timeout_ms]\n");
    write_all("  udp6cat [-p <local_port>] <dst_ipv6> <dst_port>\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc < 2) {
        usage();
        return 1;
    }

    /* Listen mode: udp6cat -l <port> [timeout_ms] */
    if (cstr_eq(argv[1], "-l")) {
        if (argc < 3) {
            usage();
            return 1;
        }

        uint64_t port_u64 = 0;
        if (parse_u64(argv[2], &port_u64) != 0 || port_u64 > 65535ull || port_u64 == 0) {
            write_all("udp6cat: invalid port\n");
            return 1;
        }

        uint64_t timeout_ms = 0;
        if (argc >= 4) {
            if (parse_u64(argv[3], &timeout_ms) != 0) {
                write_all("udp6cat: invalid timeout\n");
                return 1;
            }
        }

        uint64_t fd = sys_mona_udp6_socket();
        if ((int64_t)fd < 0) {
            write_all("udp6cat: udp6_socket failed errno=");
            write_u64_dec((uint64_t)(-(int64_t)fd));
            write_all("\n");
            return 1;
        }

        uint64_t rc = sys_mona_udp6_bind(fd, port_u64);
        if ((int64_t)rc < 0) {
            write_all("udp6cat: bind failed errno=");
            write_u64_dec((uint64_t)(-(int64_t)rc));
            write_all("\n");
            return 1;
        }

        uint8_t buf[UDP6_MAX_PAYLOAD_LOCAL];
        uint8_t src_ip[16];
        uint16_t src_port = 0;

        for (;;) {
            uint64_t n = sys_mona_udp6_recvfrom(fd, buf, sizeof(buf), src_ip, &src_port, timeout_ms);
            if ((int64_t)n < 0) {
                uint64_t err = (uint64_t)(-(int64_t)n);
                /* ETIMEDOUT: just keep waiting. */
                if (err == 110ull) {
                    continue;
                }
                write_all("udp6cat: recvfrom failed errno=");
                write_u64_dec(err);
                write_all("\n");
                return 1;
            }
            if (n != 0) {
                (void)sys_write(1, buf, n);
            }
        }
    }

    /* Client mode: udp6cat [-p <local_port>] <dst_ipv6> <dst_port> */
    int argi = 1;
    uint64_t local_port = 0;
    if (argi + 1 < argc && cstr_eq(argv[argi], "-p")) {
        if (argi + 2 >= argc) {
            usage();
            return 1;
        }
        if (parse_u64(argv[argi + 1], &local_port) != 0 || local_port > 65535ull) {
            write_all("udp6cat: invalid local port\n");
            return 1;
        }
        argi += 2;
    }

    if (argi + 2 > argc) {
        usage();
        return 1;
    }

    uint8_t dst_ip[16];
    if (parse_ipv6(argv[argi], dst_ip) != 0) {
        write_all("udp6cat: invalid IPv6 address\n");
        return 1;
    }

    uint64_t dst_port = 0;
    if (parse_u64(argv[argi + 1], &dst_port) != 0 || dst_port > 65535ull || dst_port == 0) {
        write_all("udp6cat: invalid destination port\n");
        return 1;
    }

    uint64_t fd = sys_mona_udp6_socket();
    if ((int64_t)fd < 0) {
        write_all("udp6cat: udp6_socket failed errno=");
        write_u64_dec((uint64_t)(-(int64_t)fd));
        write_all("\n");
        return 1;
    }

    if (local_port != 0) {
        uint64_t brc = sys_mona_udp6_bind(fd, local_port);
        if ((int64_t)brc < 0) {
            write_all("udp6cat: bind failed errno=");
            write_u64_dec((uint64_t)(-(int64_t)brc));
            write_all("\n");
            return 1;
        }
    }

    uint8_t buf[UDP6_MAX_PAYLOAD_LOCAL];
    for (;;) {
        uint64_t n = sys_read(0, buf, sizeof(buf));
        if ((int64_t)n < 0) {
            write_all("udp6cat: read failed errno=");
            write_u64_dec((uint64_t)(-(int64_t)n));
            write_all("\n");
            return 1;
        }
        if (n == 0) break;

        uint64_t off = 0;
        while (off < n) {
            uint64_t chunk = n - off;
            uint64_t rc = sys_mona_udp6_sendto(fd, dst_ip, dst_port, buf + off, chunk);
            if ((int64_t)rc == -(int64_t)11) {
                /* EAGAIN: neighbor unresolved; retry shortly. */
                sleep_ms(100);
                continue;
            }
            if ((int64_t)rc < 0) {
                write_all("udp6cat: sendto failed errno=");
                write_u64_dec((uint64_t)(-(int64_t)rc));
                write_all("\n");
                return 1;
            }
            off += rc;
        }
    }

    return 0;
}
