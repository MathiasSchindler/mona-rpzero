#include "syscall.h"

#define AT_FDCWD ((long)-100)

enum {
    O_RDONLY = 0,
};

typedef enum {
    ADDR_OCT = 1,
    ADDR_DEC,
    ADDR_HEX,
    ADDR_NONE,
} addr_base_t;

typedef enum {
    FMT_O1 = 1,
    FMT_U1,
    FMT_X1,
    FMT_C,
} fmt_t;

typedef struct {
    char buf[512];
    uint64_t n;
} out_t;

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int write_all(uint64_t fd, const char *buf, uint64_t len) {
    uint64_t off = 0;
    while (off < len) {
        long rc = (long)sys_write(fd, buf + off, len - off);
        if (rc < 0) {
            if (rc == -11) continue; /* EAGAIN */
            return -1;
        }
        if (rc == 0) return -1;
        off += (uint64_t)rc;
    }
    return 0;
}

static int out_flush(out_t *o) {
    if (!o) return -1;
    if (o->n == 0) return 0;
    int rc = write_all(1, o->buf, o->n);
    o->n = 0;
    return rc;
}

static int out_putc(out_t *o, char c) {
    if (!o) return -1;
    if (o->n >= sizeof(o->buf)) {
        if (out_flush(o) != 0) return -1;
    }
    o->buf[o->n++] = c;
    return 0;
}

static int out_write(out_t *o, const char *s, uint64_t n) {
    if (!o || !s) return -1;

    if (n >= sizeof(o->buf)) {
        if (out_flush(o) != 0) return -1;
        return write_all(1, s, n);
    }

    if (o->n + n > sizeof(o->buf)) {
        if (out_flush(o) != 0) return -1;
    }

    for (uint64_t i = 0; i < n; i++) {
        o->buf[o->n++] = s[i];
    }
    return 0;
}

static int out_puts(out_t *o, const char *s) {
    if (!s) return 0;
    return out_write(o, s, cstr_len_u64_local(s));
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int parse_u64_dec(const char *s, uint64_t *out) {
    if (!s || !out) return -1;
    if (s[0] == '\0') return -1;
    uint64_t v = 0;
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        if (!is_digit(s[i])) return -1;
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static int out_u64_base(out_t *o, uint64_t v, addr_base_t base, int pad_to, char pad_ch) {
    char tmp[64];
    uint64_t n = 0;
    uint64_t b = 10;

    if (base == ADDR_HEX) b = 16;
    else if (base == ADDR_OCT) b = 8;
    else b = 10;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v != 0 && n + 1 < sizeof(tmp)) {
            uint64_t q = v / b;
            uint64_t r = v - q * b;
            char c;
            if (r < 10) c = (char)('0' + r);
            else c = (char)('a' + (r - 10));
            tmp[n++] = c;
            v = q;
        }
    }

    int width = (int)n;
    while (width < pad_to) {
        if (out_putc(o, pad_ch) != 0) return -1;
        width++;
    }

    while (n > 0) {
        if (out_putc(o, tmp[--n]) != 0) return -1;
    }

    return 0;
}

static int out_hex2(out_t *o, uint8_t v) {
    const char *hex = "0123456789abcdef";
    char s[2];
    s[0] = hex[(v >> 4) & 0xf];
    s[1] = hex[v & 0xf];
    return out_write(o, s, 2);
}

static int out_oct3(out_t *o, uint8_t v) {
    char s[3];
    s[0] = (char)('0' + ((v >> 6) & 0x7));
    s[1] = (char)('0' + ((v >> 3) & 0x7));
    s[2] = (char)('0' + (v & 0x7));
    return out_write(o, s, 3);
}

static void usage(void) {
    sys_puts("usage: od [-A x|d|o|n] [-t x1|o1|u1|c] [-C] [-N BYTES] [-j SKIP] [FILE]\n");
    sys_puts("  default: -A o -t o1\n");
    sys_puts("  -C: canonical hex+ASCII (hexdump-style)\n");
    sys_puts("  FILE may be '-' for stdin\n");
}

static int is_dash(const char *s) {
    return s && s[0] == '-' && s[1] == '\0';
}

static int open_ro_maybe_stdin(const char *path, uint64_t *out_fd, int *out_is_stdin) {
    if (!out_fd || !out_is_stdin) return -1;
    if (!path || is_dash(path)) {
        *out_fd = 0;
        *out_is_stdin = 1;
        return 0;
    }

    int64_t fd = (int64_t)sys_openat((uint64_t)AT_FDCWD, path, O_RDONLY, 0);
    if (fd < 0) return -1;
    *out_fd = (uint64_t)fd;
    *out_is_stdin = 0;
    return 0;
}

static int do_skip(uint64_t fd, int is_stdin, uint64_t skip) {
    if (skip == 0) return 0;

    if (!is_stdin) {
        int64_t rc = (int64_t)sys_lseek(fd, (int64_t)skip, 0 /* SEEK_SET */);
        if (rc >= 0) return 0;
        /* fall back to read/discard */
    }

    char tmp[256];
    uint64_t left = skip;
    while (left > 0) {
        uint64_t n = (left < sizeof(tmp)) ? left : (uint64_t)sizeof(tmp);
        long r = (long)sys_read(fd, tmp, n);
        if (r < 0) {
            if (r == -11) continue;
            return -1;
        }
        if (r == 0) break;
        left -= (uint64_t)r;
    }

    return 0;
}

static int print_line(out_t *o,
                      addr_base_t addr_base,
                      fmt_t fmt,
                      int show_ascii,
                      int canonical,
                      uint64_t addr,
                      const uint8_t *buf,
                      uint64_t n) {
    if (canonical) {
        /* hexdump -C style: 8-hex addr, two spaces, 16 bytes, two spaces, |ascii| */
        if (out_u64_base(o, addr, ADDR_HEX, 8, '0') != 0) return -1;
        if (out_puts(o, "  ") != 0) return -1;

        for (uint64_t i = 0; i < 16; i++) {
            if (i == 8) {
                if (out_putc(o, ' ') != 0) return -1;
            }
            if (out_putc(o, ' ') != 0) return -1;
            if (i < n) {
                if (out_hex2(o, buf[i]) != 0) return -1;
            } else {
                if (out_puts(o, "  ") != 0) return -1;
            }
        }

        if (out_puts(o, "  |") != 0) return -1;
        for (uint64_t i = 0; i < n; i++) {
            uint8_t b = buf[i];
            char c = (b >= 32 && b <= 126) ? (char)b : '.';
            if (out_putc(o, c) != 0) return -1;
        }
        for (uint64_t i = n; i < 16; i++) {
            if (out_putc(o, ' ') != 0) return -1;
        }
        if (out_puts(o, "|\n") != 0) return -1;
        return 0;
    }

    if (addr_base != ADDR_NONE) {
        int pad = 0;
        if (addr_base == ADDR_HEX) pad = 8;
        else if (addr_base == ADDR_OCT) pad = 7;
        else pad = 8;
        if (out_u64_base(o, addr, addr_base, pad, '0') != 0) return -1;
        if (out_puts(o, " ") != 0) return -1;
    }

    for (uint64_t i = 0; i < n; i++) {
        if (i != 0) {
            if (out_putc(o, ' ') != 0) return -1;
        }

        uint8_t b = buf[i];
        if (fmt == FMT_X1) {
            if (out_hex2(o, b) != 0) return -1;
        } else if (fmt == FMT_O1) {
            if (out_oct3(o, b) != 0) return -1;
        } else if (fmt == FMT_U1) {
            if (out_u64_base(o, (uint64_t)b, ADDR_DEC, 0, '0') != 0) return -1;
        } else {
            /* FMT_C */
            char c = (b >= 32 && b <= 126) ? (char)b : '.';
            if (out_putc(o, c) != 0) return -1;
        }
    }

    if (show_ascii && fmt != FMT_C) {
        if (out_puts(o, "  |") != 0) return -1;
        for (uint64_t i = 0; i < n; i++) {
            uint8_t b = buf[i];
            char c = (b >= 32 && b <= 126) ? (char)b : '.';
            if (out_putc(o, c) != 0) return -1;
        }
        if (out_putc(o, '|') != 0) return -1;
    }

    if (out_putc(o, '\n') != 0) return -1;
    return 0;
}

static int od_fd(uint64_t fd, int is_stdin, addr_base_t addr_base, fmt_t fmt, int show_ascii, uint64_t limit, uint64_t skip) {
    if (do_skip(fd, is_stdin, skip) != 0) return -1;

    uint8_t buf[256];
    uint64_t addr = skip;
    uint64_t left = limit;
    int limited = (limit != (uint64_t)-1);

    out_t o;
    o.n = 0;

    int canonical = 0;
    (void)canonical;

    for (;;) {
        uint64_t want = 16;
        if (limited && left < want) want = left;
        if (limited && left == 0) break;

        long r = (long)sys_read(fd, buf, want);
        if (r < 0) {
            if (r == -11) continue;
            return -1;
        }
        if (r == 0) break;

        uint64_t n = (uint64_t)r;
        if (print_line(&o, addr_base, fmt, show_ascii, 0, addr, buf, n) != 0) return -1;

        addr += n;
        if (limited) left -= n;

        if (out_flush(&o) != 0) return -1;
    }

    return out_flush(&o);
}

static int od_fd_canonical(uint64_t fd, int is_stdin, uint64_t limit, uint64_t skip) {
    if (do_skip(fd, is_stdin, skip) != 0) return -1;

    uint8_t buf[256];
    uint64_t addr = skip;
    uint64_t left = limit;
    int limited = (limit != (uint64_t)-1);

    out_t o;
    o.n = 0;

    for (;;) {
        uint64_t want = 16;
        if (limited && left < want) want = left;
        if (limited && left == 0) break;

        long r = (long)sys_read(fd, buf, want);
        if (r < 0) {
            if (r == -11) continue;
            return -1;
        }
        if (r == 0) break;

        uint64_t n = (uint64_t)r;
        if (print_line(&o, ADDR_HEX, FMT_X1, 1, 1, addr, buf, n) != 0) return -1;
        addr += n;
        if (limited) left -= n;
        if (out_flush(&o) != 0) return -1;
    }

    /* final offset line */
    if (out_u64_base(&o, addr, ADDR_HEX, 8, '0') != 0) return -1;
    if (out_putc(&o, '\n') != 0) return -1;
    return out_flush(&o);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    addr_base_t addr_base = ADDR_OCT;
    fmt_t fmt = FMT_O1;
    int show_ascii = 0;
    int canonical = 0;
    uint64_t limit = (uint64_t)-1;
    uint64_t skip = 0;

    const char *path = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a) break;
        if (a[0] != '-') {
            path = a;
            i++;
            break;
        }

        if (streq(a, "--")) {
            i++;
            break;
        }
        if (streq(a, "-h") || streq(a, "--help")) {
            usage();
            return 0;
        }

        if (streq(a, "-C")) {
            canonical = 1;
            continue;
        }

        if (streq(a, "-A")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            const char *v = argv[++i];
            if (streq(v, "x")) addr_base = ADDR_HEX;
            else if (streq(v, "d")) addr_base = ADDR_DEC;
            else if (streq(v, "o")) addr_base = ADDR_OCT;
            else if (streq(v, "n")) addr_base = ADDR_NONE;
            else {
                usage();
                return 2;
            }
            continue;
        }

        if (streq(a, "-t")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            const char *v = argv[++i];
            if (streq(v, "x1")) fmt = FMT_X1;
            else if (streq(v, "o1")) fmt = FMT_O1;
            else if (streq(v, "u1")) fmt = FMT_U1;
            else if (streq(v, "c")) fmt = FMT_C;
            else {
                usage();
                return 2;
            }
            continue;
        }

        if (streq(a, "-N")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            uint64_t v = 0;
            if (parse_u64_dec(argv[++i], &v) != 0) {
                usage();
                return 2;
            }
            limit = v;
            continue;
        }

        if (streq(a, "-j")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            uint64_t v = 0;
            if (parse_u64_dec(argv[++i], &v) != 0) {
                usage();
                return 2;
            }
            skip = v;
            continue;
        }

        if (streq(a, "-c")) {
            fmt = FMT_C;
            continue;
        }

        if (streq(a, "-x")) {
            fmt = FMT_X1;
            continue;
        }

        if (streq(a, "-o")) {
            fmt = FMT_O1;
            continue;
        }

        if (streq(a, "-a")) {
            show_ascii = 1;
            continue;
        }

        usage();
        return 2;
    }

    if (path == 0 && i < argc) {
        path = argv[i];
    }

    uint64_t fd = 0;
    int is_stdin = 1;
    if (open_ro_maybe_stdin(path, &fd, &is_stdin) != 0) {
        sys_puts("od: cannot open\n");
        return 1;
    }

    int rc = 0;
    if (canonical) {
        rc = od_fd_canonical(fd, is_stdin, limit, skip);
    } else {
        rc = od_fd(fd, is_stdin, addr_base, fmt, show_ascii, limit, skip);
    }

    if (rc != 0) {
        sys_puts("od: read failed\n");
        if (!is_stdin) (void)sys_close(fd);
        return 1;
    }

    if (!is_stdin) (void)sys_close(fd);
    return 0;
}
