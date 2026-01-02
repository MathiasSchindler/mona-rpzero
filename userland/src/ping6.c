#include "syscall.h"

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

        /* Handle '::' in the middle. */
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

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc < 2) {
        write_all("usage: ping6 <ipv6-addr> [count] [timeout_ms]\n");
        return 1;
    }

    uint64_t count = 4;
    uint64_t timeout_ms = 1000;

    if (argc >= 3) {
        if (parse_u64(argv[2], &count) != 0) {
            write_all("ping6: invalid count\n");
            return 1;
        }
    }
    if (argc >= 4) {
        if (parse_u64(argv[3], &timeout_ms) != 0) {
            write_all("ping6: invalid timeout\n");
            return 1;
        }
    }

    uint8_t dst[16];
    if (parse_ipv6(argv[1], dst) != 0) {
        write_all("ping6: invalid IPv6 address (try fe80::1)\n");
        return 1;
    }

    uint16_t ident = (uint16_t)(sys_getpid() & 0xffffu);

    for (uint64_t i = 0; i < count; i++) {
        uint64_t rtt_ns = 0;
        uint64_t rc = sys_mona_ping6(dst, ident, (uint16_t)i, timeout_ms, &rtt_ns);

        write_all("ping6 ");
        write_all(argv[1]);
        write_all(": seq=");
        write_u64_dec(i);

        if ((int64_t)rc < 0) {
            write_all(" error=");
            write_u64_dec((uint64_t)(-(int64_t)rc));
            write_all("\n");
        } else {
            write_all(" time=");
            write_u64_dec(rtt_ns / 1000ull);
            write_all("us\n");
        }

        /* Small delay for readability. */
        linux_timespec_t ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 200 * 1000 * 1000;
        (void)sys_nanosleep(&ts, 0);
    }

    return 0;
}
