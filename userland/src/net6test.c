#include "syscall.h"

/* Minimal integration test for IPv6 bringup on usb0.
 *
 * Goal: verify that RA/SLAAC configures a global address + router, and that
 * ICMPv6 echo to the host router address works.
 *
 * This is intentionally tiny and libc-free.
 */

#define AT_FDCWD ((uint64_t)-100)

/* openat flags (Linux). */
#define O_RDONLY 0

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static void puts_ln(const char *s) {
    sys_puts(s);
    sys_puts("\n");
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
            /* EAGAIN => retry */
            if (n == -11) continue;
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

static const char *find_line_starting_with(const char *buf, uint64_t len, const char *prefix) {
    uint64_t plen = cstr_len_u64_local(prefix);
    if (!buf || plen == 0) return 0;

    for (uint64_t i = 0; i + plen <= len; i++) {
        /* Only match at start of buffer or start of a line. */
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

static int is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static const char *find_value_line_starting_with(const char *buf, uint64_t len, const char *prefix) {
    /* For sections like "usbnet"/"ipv6dbg" where a header line and value line
     * share the same prefix. We want the line whose first field after the tab
     * starts with a digit.
     */
    uint64_t plen = cstr_len_u64_local(prefix);
    const char *best = 0;
    uint64_t i = 0;
    while (i + plen + 1 <= len) {
        const char *line = find_line_starting_with(buf + i, len - i, prefix);
        if (!line) break;
        uint64_t off = (uint64_t)(line - buf);
        if (off + plen + 1 < len) {
            char after_tab = buf[off + plen];
            if (after_tab == '\t' && is_digit(buf[off + plen + 1])) {
                best = buf + off;
            }
        }
        /* Advance to next line. */
        const char *p = buf + off;
        while ((uint64_t)(p - buf) < len && *p && *p != '\n') p++;
        if (*p == '\n') p++;
        i = (uint64_t)(p - buf);
    }
    return best;
}

static int extract_field(const char *line, uint64_t field_index, const char **out_start, uint64_t *out_len) {
    /* Tab-separated fields; field 0 starts at line start.
     * Returns 0 on success.
     */
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

static void sleep_ms(uint64_t ms) {
    linux_timespec_t req;
    req.tv_sec = (int64_t)(ms / 1000ull);
    req.tv_nsec = (int64_t)((ms % 1000ull) * 1000000ull);
    (void)sys_nanosleep(&req, 0);
}

static void dump_proc_net_selected(const char *buf, uint64_t len) {
    const char *usb0 = find_line_starting_with(buf, len, "usb0\t");
    const char *usbnet = find_value_line_starting_with(buf, len, "usbnet");
    const char *ipv6dbg = find_value_line_starting_with(buf, len, "ipv6dbg");

    sys_puts("[net6test] /proc/net selected lines:\n");
    if (usb0) {
        const char *p = usb0;
        while (*p && *p != '\n') p++;
        (void)sys_write(1, usb0, (uint64_t)(p - usb0));
        sys_puts("\n");
    } else {
        sys_puts("[net6test] usb0 line not found\n");
    }

    if (usbnet) {
        const char *p = usbnet;
        while (*p && *p != '\n') p++;
        (void)sys_write(1, usbnet, (uint64_t)(p - usbnet));
        sys_puts("\n");
    } else {
        sys_puts("[net6test] usbnet value line not found\n");
    }
    if (ipv6dbg) {
        const char *p = ipv6dbg;
        while (*p && *p != '\n') p++;
        (void)sys_write(1, ipv6dbg, (uint64_t)(p - ipv6dbg));
        sys_puts("\n");
    } else {
        sys_puts("[net6test] ipv6dbg value line not found\n");
    }
}

static int usb0_has_global_and_router(const char *proc_net, uint64_t len) {
    const char *line = find_line_starting_with(proc_net, len, "usb0\t");
    if (!line) return 0;

    /* /proc/net header:
     * iface mtu mac rx_frames rx_drops tx_frames tx_drops ipv6_ll ipv6_global ipv6_router_ll ipv6_dns
     * Field indices: 0..10
     */
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

    puts_ln("[net6test] starting");

    char proc_net[2048];
    uint64_t proc_len = 0;

    /* Wait up to ~6 seconds for SLAAC + router. */
    int ready = 0;
    for (uint64_t i = 0; i < 60; i++) {
        if (read_whole_file("/proc/net", proc_net, sizeof(proc_net), &proc_len) != 0) {
            puts_ln("[net6test] failed to read /proc/net");
            sleep_ms(100);
            continue;
        }

        if (usb0_has_global_and_router(proc_net, proc_len)) {
            ready = 1;
            break;
        }

        /* Keep the stack active during bringup (RS/NS). */
        {
            static const uint8_t host_ip[16] = {
                0xfd, 0x42, 0x6d, 0x6f, 0x6e, 0x61, 0x00, 0x01,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
            };
            uint64_t rtt = 0;
            (void)sys_mona_ping6(host_ip, 0x1234, i, 250, &rtt);
        }

        sleep_ms(100);
    }

    /* Final snapshot. */
    if (read_whole_file("/proc/net", proc_net, sizeof(proc_net), &proc_len) == 0) {
        dump_proc_net_selected(proc_net, proc_len);
    }

    if (!ready) {
        puts_ln("[net6test] FAIL: no SLAAC/router within timeout");
        (void)sys_reboot(0, 0, 0x4321fedcull, (void *)1);
        sys_exit_group(1);
    }

    /* Now require that ping6 to the host router works. */
    {
        static const uint8_t host_ip[16] = {
            0xfd, 0x42, 0x6d, 0x6f, 0x6e, 0x61, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
        };
        uint64_t rtt = 0;
        int64_t rc = (int64_t)sys_mona_ping6(host_ip, 0xbeef, 1, 1500, &rtt);
        if (rc != 0) {
            sys_puts("[net6test] FAIL: ping6 host rc=");
            /* tiny decimal print */
            {
                char buf[32];
                uint64_t n = 0;
                int64_t v = rc;
                if (v < 0) {
                    buf[n++] = '-';
                    v = -v;
                }
                if (v == 0) {
                    buf[n++] = '0';
                } else {
                    char tmp[32];
                    uint64_t t = 0;
                    while (v != 0 && t < sizeof(tmp)) {
                        tmp[t++] = (char)('0' + (v % 10));
                        v /= 10;
                    }
                    while (t > 0) {
                        buf[n++] = tmp[--t];
                    }
                }
                buf[n++] = '\n';
                (void)sys_write(1, buf, n);
            }
            (void)sys_reboot(0, 0, 0x4321fedcull, (void *)2);
            sys_exit_group(2);
        }

        puts_ln("[net6test] PASS");
    }

    (void)sys_reboot(0, 0, 0x4321fedcull, 0);
    sys_exit_group(0);
}
