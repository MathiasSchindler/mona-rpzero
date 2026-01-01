#include "syscall.h"

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static char *strip_outer_quotes_inplace(char *s) {
    if (!s) return s;
    uint64_t n = cstr_len_u64_local(s);
    if (n < 2) return s;
    char q = s[0];
    if ((q == '\'' || q == '"') && s[n - 1] == q) {
        s[n - 1] = '\0';
        return s + 1;
    }
    return s;
}

static int write_all(uint64_t fd, const char *buf, uint64_t len) {
    uint64_t off = 0;
    while (off < len) {
        long rc = (long)sys_write(fd, buf + off, len - off);
        if (rc < 0) {
            /* EAGAIN (11) => retry (pipes). */
            if (rc == -11) continue;
            return -1;
        }
        if (rc == 0) return -1;
        off += (uint64_t)rc;
    }
    return 0;
}

typedef struct {
    char buf[256];
    uint64_t n;
} out_t;

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

    /* Large writes bypass the buffer. */
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

static uint8_t hex_val(char c) {
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
    if (c >= 'A' && c <= 'F') return (uint8_t)(10 + (c - 'A'));
    return 0xff;
}

/*
 * Returns:
 *  0 => not an escape (no '\\' at s[*i_inout])
 *  1 => parsed into *out_byte and advanced *i_inout
 *  2 => '\\c' encountered (stop output)
 * -1 => invalid escape
 */
static int parse_escape(const char *s, uint64_t *i_inout, uint8_t *out_byte) {
    uint64_t i = *i_inout;
    if (!s || s[i] != '\\') return 0;
    i++;
    char c = s[i];
    if (c == '\0') return -1;

    if (c == 'c') {
        *i_inout = i + 1;
        return 2;
    }
    if (c == 'n') {
        *out_byte = (uint8_t)'\n';
        *i_inout = i + 1;
        return 1;
    }
    if (c == 't') {
        *out_byte = (uint8_t)'\t';
        *i_inout = i + 1;
        return 1;
    }
    if (c == 'r') {
        *out_byte = (uint8_t)'\r';
        *i_inout = i + 1;
        return 1;
    }
    if (c == 'b') {
        *out_byte = (uint8_t)'\b';
        *i_inout = i + 1;
        return 1;
    }
    if (c == 'f') {
        *out_byte = (uint8_t)'\f';
        *i_inout = i + 1;
        return 1;
    }
    if (c == 'v') {
        *out_byte = (uint8_t)'\v';
        *i_inout = i + 1;
        return 1;
    }
    if (c == 'a') {
        *out_byte = (uint8_t)'\a';
        *i_inout = i + 1;
        return 1;
    }
    if (c == '\\') {
        *out_byte = (uint8_t)'\\';
        *i_inout = i + 1;
        return 1;
    }
    if (c == '\'' || c == '"') {
        *out_byte = (uint8_t)c;
        *i_inout = i + 1;
        return 1;
    }

    if (c == 'x') {
        uint8_t h1 = hex_val(s[i + 1]);
        uint8_t h2 = hex_val(s[i + 2]);
        if (h1 == 0xff || h2 == 0xff) return -1;
        *out_byte = (uint8_t)((h1 << 4) | h2);
        *i_inout = i + 3;
        return 1;
    }

    if (c >= '0' && c <= '7') {
        /* Octal: up to 3 digits (including this one). */
        uint8_t v = (uint8_t)(c - '0');
        uint64_t j = i + 1;
        for (int k = 0; k < 2; k++) {
            char d = s[j];
            if (d < '0' || d > '7') break;
            v = (uint8_t)((v << 3) | (uint8_t)(d - '0'));
            j++;
        }
        *out_byte = v;
        *i_inout = j;
        return 1;
    }

    /* Default: literal next character. */
    *out_byte = (uint8_t)c;
    *i_inout = i + 1;
    return 1;
}

static int parse_i64_local(const char *s, int64_t *out, int *ok) {
    if (ok) *ok = 0;
    if (!s || !out) return -1;

    int neg = 0;
    uint64_t i = 0;
    if (s[0] == '-') {
        neg = 1;
        i++;
    }

    int any = 0;
    uint64_t v = 0;
    for (; s[i] != '\0'; i++) {
        char c = s[i];
        if (c < '0' || c > '9') break;
        any = 1;
        uint64_t d = (uint64_t)(c - '0');
        uint64_t nv = v * 10u + d;
        if (nv < v) return -1;
        v = nv;
    }

    if (!any) return -1;

    if (neg) {
        if (v > ((uint64_t)1u << 63)) return -1;
        *out = (v == ((uint64_t)1u << 63)) ? (int64_t)((uint64_t)1u << 63) : -(int64_t)v;
    } else {
        if (v > (uint64_t)((int64_t)(((uint64_t)1u << 63) - 1u))) return -1;
        *out = (int64_t)v;
    }

    if (ok) *ok = 1;
    return 0;
}

static int parse_u64_local(const char *s, uint64_t *out, int *ok) {
    if (ok) *ok = 0;
    if (!s || !out) return -1;

    uint64_t i = 0;
    if (s[0] == '+') i++;

    int any = 0;
    uint64_t v = 0;
    for (; s[i] != '\0'; i++) {
        char c = s[i];
        if (c < '0' || c > '9') break;
        any = 1;
        uint64_t d = (uint64_t)(c - '0');
        uint64_t nv = v * 10u + d;
        if (nv < v) return -1;
        v = nv;
    }

    if (!any) return -1;
    *out = v;
    if (ok) *ok = 1;
    return 0;
}

static int out_put_u64_dec(out_t *o, uint64_t v) {
    char tmp[32];
    uint64_t n = 0;

    if (v == 0) {
        tmp[n++] = '0';
    } else {
        char rev[32];
        uint64_t r = 0;
        while (v != 0 && r < sizeof(rev)) {
            rev[r++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (r > 0) tmp[n++] = rev[--r];
    }

    return out_write(o, tmp, n);
}

static int out_put_i64_dec(out_t *o, int64_t v) {
    if (v < 0) {
        if (out_putc(o, '-') != 0) return -1;
        /* avoid overflow for INT64_MIN */
        uint64_t uv = (v == (int64_t)((uint64_t)1u << 63)) ? ((uint64_t)1u << 63) : (uint64_t)(-v);
        return out_put_u64_dec(o, uv);
    }
    return out_put_u64_dec(o, (uint64_t)v);
}

static int out_put_u64_hex(out_t *o, uint64_t v, int upper) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[32];
    uint64_t n = 0;

    if (v == 0) {
        tmp[n++] = '0';
        return out_write(o, tmp, n);
    }

    char rev[32];
    uint64_t r = 0;
    while (v != 0 && r < sizeof(rev)) {
        rev[r++] = digits[v & 0xFu];
        v >>= 4;
    }
    while (r > 0) tmp[n++] = rev[--r];
    return out_write(o, tmp, n);
}

static int out_put_escaped(out_t *o, const char *s, int *stop_out) {
    if (stop_out) *stop_out = 0;
    if (!s) return 0;

    for (uint64_t i = 0; s[i] != '\0';) {
        if (s[i] == '\\') {
            uint64_t t = i;
            uint8_t b = 0;
            int erc = parse_escape(s, &t, &b);
            if (erc == 2) {
                if (stop_out) *stop_out = 1;
                return 0;
            }
            if (erc < 0) return -1;
            if (erc > 0) {
                if (out_putc(o, (char)b) != 0) return -1;
                i = t;
                continue;
            }
        }

        if (out_putc(o, s[i]) != 0) return -1;
        i++;
    }

    return 0;
}

static void usage(void) {
    sys_puts("usage: printf FORMAT [ARG...]\n");
    sys_puts("supported escapes: \\\\n \\\\t \\\\r \\\\xNN \\\\0NNN \\\\c \\\\\\\
");
    sys_puts("supported conversions: %% %%s %%d %%u %%x %%X %%c %%b\n");
}

static int print_one_pass(out_t *o, const char *fmt, int argc, char **argv, int *arg_idx_io, int *stop_out, int *consumed_any_arg) {
    if (stop_out) *stop_out = 0;
    if (consumed_any_arg) *consumed_any_arg = 0;
    if (!o || !fmt || !arg_idx_io) return -1;

    int arg_idx = *arg_idx_io;

    for (uint64_t i = 0; fmt[i] != '\0';) {
        if (fmt[i] == '\\') {
            uint64_t t = i;
            uint8_t b = 0;
            int erc = parse_escape(fmt, &t, &b);
            if (erc == 2) {
                if (stop_out) *stop_out = 1;
                *arg_idx_io = arg_idx;
                return 0;
            }
            if (erc < 0) return -1;
            if (erc > 0) {
                if (out_putc(o, (char)b) != 0) return -1;
                i = t;
                continue;
            }
        }

        if (fmt[i] != '%') {
            if (out_putc(o, fmt[i]) != 0) return -1;
            i++;
            continue;
        }

        /* '%' sequence */
        i++;
        char c = fmt[i];
        if (c == '\0') {
            /* trailing '%' => literal */
            if (out_putc(o, '%') != 0) return -1;
            break;
        }

        /* Length modifiers we ignore but tolerate: l, ll, z */
        if (c == 'l') {
            i++;
            if (fmt[i] == 'l') i++;
            c = fmt[i];
            if (c == '\0') break;
        } else if (c == 'z') {
            i++;
            c = fmt[i];
            if (c == '\0') break;
        }

        if (c == '%') {
            if (out_putc(o, '%') != 0) return -1;
            i++;
            continue;
        }

        if (c == 's') {
            const char *s = (arg_idx < argc) ? argv[arg_idx] : "";
            if (arg_idx < argc) {
                arg_idx++;
                if (consumed_any_arg) *consumed_any_arg = 1;
            }
            if (out_puts(o, s) != 0) return -1;
            i++;
            continue;
        }

        if (c == 'b') {
            const char *s = (arg_idx < argc) ? argv[arg_idx] : "";
            if (arg_idx < argc) {
                arg_idx++;
                if (consumed_any_arg) *consumed_any_arg = 1;
            }
            int stop_b = 0;
            if (out_put_escaped(o, s, &stop_b) != 0) return -1;
            if (stop_b) {
                if (stop_out) *stop_out = 1;
                *arg_idx_io = arg_idx;
                return 0;
            }
            i++;
            continue;
        }

        if (c == 'c') {
            char ch = 0;
            if (arg_idx < argc && argv[arg_idx] && argv[arg_idx][0] != '\0') {
                ch = argv[arg_idx][0];
            }
            if (arg_idx < argc) {
                arg_idx++;
                if (consumed_any_arg) *consumed_any_arg = 1;
            }
            if (out_putc(o, ch) != 0) return -1;
            i++;
            continue;
        }

        if (c == 'd' || c == 'i') {
            int64_t v = 0;
            int ok = 0;
            if (arg_idx < argc) (void)parse_i64_local(argv[arg_idx], &v, &ok);
            if (arg_idx < argc) {
                arg_idx++;
                if (consumed_any_arg) *consumed_any_arg = 1;
            }
            if (!ok) v = 0;
            if (out_put_i64_dec(o, v) != 0) return -1;
            i++;
            continue;
        }

        if (c == 'u') {
            uint64_t v = 0;
            int ok = 0;
            if (arg_idx < argc) (void)parse_u64_local(argv[arg_idx], &v, &ok);
            if (arg_idx < argc) {
                arg_idx++;
                if (consumed_any_arg) *consumed_any_arg = 1;
            }
            if (!ok) v = 0;
            if (out_put_u64_dec(o, v) != 0) return -1;
            i++;
            continue;
        }

        if (c == 'x' || c == 'X') {
            uint64_t v = 0;
            int ok = 0;
            if (arg_idx < argc) (void)parse_u64_local(argv[arg_idx], &v, &ok);
            if (arg_idx < argc) {
                arg_idx++;
                if (consumed_any_arg) *consumed_any_arg = 1;
            }
            if (!ok) v = 0;
            if (out_put_u64_hex(o, v, (c == 'X')) != 0) return -1;
            i++;
            continue;
        }

        /* Unknown conversion: print it literally. */
        if (out_putc(o, '%') != 0) return -1;
        if (out_putc(o, c) != 0) return -1;
        i++;
    }

    *arg_idx_io = arg_idx;
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc >= 2 && argv[1] && (streq(argv[1], "-h") || streq(argv[1], "--help"))) {
        usage();
        return 0;
    }

    if (argc < 2 || !argv[1]) {
        /* POSIX printf with no args prints nothing and succeeds. */
        return 0;
    }

    /* Work around the current shell's lack of quote parsing. */
    for (int i = 1; i < argc; i++) {
        if (argv[i]) argv[i] = strip_outer_quotes_inplace(argv[i]);
    }

    const char *fmt = argv[1];
    out_t o;
    o.n = 0;

    int arg_idx = 2;
    int stop_out = 0;

    for (;;) {
        int consumed_any_arg = 0;
        if (print_one_pass(&o, fmt, argc, argv, &arg_idx, &stop_out, &consumed_any_arg) != 0) {
            (void)out_flush(&o);
            return 1;
        }
        if (stop_out) break;

        /* If we didn't consume any args, don't loop forever. */
        if (!consumed_any_arg) break;
        if (arg_idx >= argc) break;
    }

    (void)out_flush(&o);
    return 0;
}
