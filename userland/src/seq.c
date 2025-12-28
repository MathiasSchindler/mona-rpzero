#include "syscall.h"

#define I64_MIN ((int64_t)((uint64_t)1u << 63))
#define I64_MAX ((int64_t)(((uint64_t)1u << 63) - 1u))

static void putc1(char c) {
    (void)sys_write(1, &c, 1);
}

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int str_eq_local(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static void usage(void) {
    sys_puts("usage: seq [-w] [-s SEP] [FIRST [INCREMENT]] LAST\n");
}

static int parse_i64(const char *s, int64_t *out) {
    if (!s || !out) return -1;

    uint64_t i = 0;
    int neg = 0;
    if (s[i] == '+') {
        i++;
    } else if (s[i] == '-') {
        neg = 1;
        i++;
    }

    if (s[i] == '\0') return -1;

    int64_t v = 0;
    for (; s[i] != '\0'; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return -1;
        int64_t d = (int64_t)(c - '0');

        /* Overflow-safe accumulate for signed 64-bit.
           For negative values we allow one extra (INT64_MIN) via signed range check. */
        if (!neg) {
            if (v > (I64_MAX - d) / 10) return -1;
            v = v * 10 + d;
        } else {
            /* Build negative directly to allow INT64_MIN. */
            if (v < (I64_MIN + d) / 10) return -1;
            v = v * 10 - d;
        }
    }

    *out = v;
    return 0;
}

static uint64_t u64_abs_i64(int64_t v) {
    if (v >= 0) return (uint64_t)v;
    /* abs(INT64_MIN) cannot fit in int64_t; do it in uint64_t. */
    return (uint64_t)(-(v + 1)) + 1u;
}

static uint64_t u64_dec_digits(uint64_t v) {
    uint64_t d = 1;
    while (v >= 10u) {
        v /= 10u;
        d++;
    }
    return d;
}

static void write_u64_dec(char *out, uint64_t cap, uint64_t v) {
    if (cap == 0) return;

    char tmp[32];
    uint64_t n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    } else {
        while (v != 0 && n + 1 < sizeof(tmp)) {
            uint64_t q = v / 10u;
            uint64_t r = v - q * 10u;
            tmp[n++] = (char)('0' + (char)r);
            v = q;
        }
    }

    uint64_t o = 0;
    while (n > 0 && o + 1 < cap) {
        out[o++] = tmp[--n];
    }
    out[o] = '\0';
}

static void print_i64(int64_t v, uint64_t width, int pad_zero) {
    char digits[32];
    uint64_t av = u64_abs_i64(v);
    write_u64_dec(digits, sizeof(digits), av);

    uint64_t nd = cstr_len_u64_local(digits);
    uint64_t sign = (v < 0) ? 1u : 0u;

    if (width > sign + nd) {
        if (v < 0) putc1('-');
        uint64_t pad = width - sign - nd;
        char pc = pad_zero ? '0' : ' ';
        for (uint64_t i = 0; i < pad; i++) putc1(pc);
        sys_puts(digits);
        return;
    }

    if (v < 0) putc1('-');
    sys_puts(digits);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int opt_w = 0;
    const char *sep = "\n";

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a || a[0] != '-') break;
        if (str_eq_local(a, "--")) {
            i++;
            break;
        }
        if (str_eq_local(a, "-w")) {
            opt_w = 1;
            continue;
        }
        if (str_eq_local(a, "-s")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            sep = argv[++i];
            continue;
        }
        if (str_eq_local(a, "-h") || str_eq_local(a, "--help")) {
            usage();
            return 0;
        }

        usage();
        return 2;
    }

    int nnums = argc - i;
    int64_t first = 1;
    int64_t step = 1;
    int64_t last = 0;

    if (nnums == 1) {
        if (parse_i64(argv[i], &last) != 0) {
            usage();
            return 2;
        }
    } else if (nnums == 2) {
        if (parse_i64(argv[i], &first) != 0 || parse_i64(argv[i + 1], &last) != 0) {
            usage();
            return 2;
        }
        step = (last >= first) ? 1 : -1;
    } else if (nnums == 3) {
        if (parse_i64(argv[i], &first) != 0 || parse_i64(argv[i + 1], &step) != 0 || parse_i64(argv[i + 2], &last) != 0) {
            usage();
            return 2;
        }
    } else {
        usage();
        return 2;
    }

    if (step == 0) {
        sys_puts("seq: increment must not be 0\n");
        return 2;
    }

    uint64_t width = 0;
    if (opt_w) {
        uint64_t wf = u64_dec_digits(u64_abs_i64(first)) + ((first < 0) ? 1u : 0u);
        uint64_t wl = u64_dec_digits(u64_abs_i64(last)) + ((last < 0) ? 1u : 0u);
        width = (wf > wl) ? wf : wl;
    }

    int first_out = 1;
    int64_t cur = first;

    for (;;) {
        if (step > 0) {
            if (cur > last) break;
        } else {
            if (cur < last) break;
        }

        if (!first_out) sys_puts(sep);
        first_out = 0;

        print_i64(cur, width, 1);

        /* Detect overflow before advancing. */
        if (step > 0) {
            if (cur > I64_MAX - step) break;
        } else {
            if (cur < I64_MIN - step) break;
        }

        cur += step;
    }

    sys_puts("\n");
    return 0;
}
