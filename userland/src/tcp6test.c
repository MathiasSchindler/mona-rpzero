#include "syscall.h"

int dns6_resolve_aaaa_one(const char *name, const uint8_t dns_ip[16], uint64_t timeout_ms, uint8_t out_ip[16]);

enum {
    TCP6TEST_TIMEOUT_MS = 8000,
    TCP6TEST_PORT = 443,
};

#define AT_FDCWD ((uint64_t)-100)
#define O_RDONLY 0

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

static int read_whole_file(const char *path, char *out, uint64_t cap, uint64_t *out_len) {
    if (!out || cap == 0) return 1;
    out[0] = '\0';

    uint64_t fd = sys_openat(AT_FDCWD, path, O_RDONLY, 0);
    if ((int64_t)fd < 0) return 1;

    uint64_t pos = 0;
    while (pos + 1 < cap) {
        int64_t n = (int64_t)sys_read(fd, out + pos, cap - pos - 1);
        if (n < 0) {
            if (n == -11) continue; /* EAGAIN */
            (void)sys_close(fd);
            return 1;
        }
        if (n == 0) break;
        pos += (uint64_t)n;
    }
    out[pos] = '\0';
    (void)sys_close(fd);
    if (out_len) *out_len = pos;
    return 0;
}

static void sleep_ms(uint64_t ms) {
    linux_timespec_t req;
    req.tv_sec = (int64_t)(ms / 1000ull);
    req.tv_nsec = (int64_t)((ms % 1000ull) * 1000000ull);
    (void)sys_nanosleep(&req, 0);
}

static const char *find_line_starting_with(const char *buf, uint64_t len, const char *prefix) {
    uint64_t plen = cstr_len_u64_local(prefix);
    if (!buf || plen == 0) return 0;

    for (uint64_t i = 0; i + plen <= len; i++) {
        if (i != 0 && buf[i - 1] != '\n') continue;
        int ok = 1;
        for (uint64_t j = 0; j < plen; j++) {
            if (buf[i + j] != prefix[j]) {
                ok = 0;
                break;
            }
        }
        if (ok) return buf + i;
    }
    return 0;
}

static int extract_field(const char *line, uint64_t field_index, const char **out_start, uint64_t *out_len) {
    if (!line || !out_start || !out_len) return 1;

    uint64_t cur = 0;
    const char *p = line;
    const char *field_start = p;

    while (*p != '\0' && *p != '\n') {
        if (*p == '\t') {
            if (cur == field_index) {
                *out_start = field_start;
                *out_len = (uint64_t)(p - field_start);
                return 0;
            }
            cur++;
            field_start = p + 1;
        }
        p++;
    }

    if (cur == field_index) {
        *out_start = field_start;
        *out_len = (uint64_t)(p - field_start);
        return 0;
    }

    return 1;
}

static int field_is_dash(const char *s, uint64_t n) {
    return (n == 1 && s && s[0] == '-');
}

static int usb0_has_global_and_router(const char *proc_net, uint64_t len) {
    const char *line = find_line_starting_with(proc_net, len, "usb0\t");
    if (!line) return 0;

    const char *global_s = 0;
    uint64_t global_n = 0;
    const char *router_s = 0;
    uint64_t router_n = 0;

    if (extract_field(line, 8, &global_s, &global_n) != 0) return 0;
    if (extract_field(line, 9, &router_s, &router_n) != 0) return 0;
    if (field_is_dash(global_s, global_n)) return 0;
    if (field_is_dash(router_s, router_n)) return 0;
    return 1;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    const char *host = "de.wikipedia.org";

    write_all("[tcp6test] starting\n");

    char proc_net[2048];
    uint64_t proc_len = 0;
    int ready = 0;
    for (uint64_t i = 0; i < 90; i++) {
        if (read_whole_file("/proc/net", proc_net, sizeof(proc_net), &proc_len) == 0 &&
            usb0_has_global_and_router(proc_net, proc_len)) {
            ready = 1;
            break;
        }
        sleep_ms(100);
    }
    if (!ready) {
        write_all("[tcp6test] FAIL: no SLAAC/router within timeout\n");
        return 1;
    }

    uint8_t dns_ip[16];
    {
        int got_dns = 0;
        for (uint64_t i = 0; i < 50; i++) {
            if (sys_mona_net6_get_dns(dns_ip) == 0) {
                got_dns = 1;
                break;
            }
            sleep_ms(100);
        }
        if (!got_dns) {
            write_all("[tcp6test] FAIL: no DNS server from RDNSS\n");
            return 1;
        }
    }

    write_all("[tcp6test] dns=");
    write_ipv6_full(dns_ip);
    write_all("\n");

    uint8_t dst_ip[16];
    int rc = dns6_resolve_aaaa_one(host, dns_ip, TCP6TEST_TIMEOUT_MS, dst_ip);
    if (rc != 0) {
        write_all("[tcp6test] FAIL: resolve errno=");
        write_u64_dec((uint64_t)(-(int64_t)rc));
        write_all("\n");
        return 1;
    }

    write_all("[tcp6test] aaaa=");
    write_ipv6_full(dst_ip);
    write_all("\n");

    uint64_t fd = sys_mona_tcp6_connect(dst_ip, TCP6TEST_PORT, TCP6TEST_TIMEOUT_MS);
    if ((int64_t)fd < 0) {
        write_all("[tcp6test] FAIL: connect errno=");
        write_u64_dec((uint64_t)(-(int64_t)fd));
        write_all("\n");
        return 1;
    }

    write_all("[tcp6test] connected fd=");
    write_u64_dec(fd);
    write_all("\n");

    write_all("[tcp6test] PASS\n");
    return 0;
}
