#include "syscall.h"

#define AT_FDCWD ((long)-100)

static void putc1(char c) {
    (void)sys_write(1, &c, 1);
}

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void usage(void) {
    sys_puts("usage: sort [-r] [-n] [-u] [FILE...]\n");
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

typedef struct {
    int opt_r;
    int opt_n;
    int opt_u;
} sort_opts_t;

enum {
    LINE_MAX = 512,
    POOL_CAP = 256 * 1024,
    MAX_LINES = 8192,
};

typedef struct {
    char pool[POOL_CAP];
    uint64_t pool_len;

    const char *lines[MAX_LINES];
    uint64_t nlines;
} lines_t;

static int read_line(uint64_t fd, char *out, uint64_t cap, int *out_eof) {
    if (!out || cap == 0 || !out_eof) return -1;

    uint64_t n = 0;
    *out_eof = 0;

    for (;;) {
        char c = 0;
        long rc = (long)sys_read(fd, &c, 1);
        if (rc == 0) {
            *out_eof = 1;
            out[n] = '\0';
            return (int)n;
        }
        if (rc < 0) {
            /* EAGAIN (11) => retry (pipes). */
            if (rc == -11) continue;
            return -1;
        }

        if (c == '\r') {
            /* normalize CRLF-ish inputs */
            continue;
        }
        if (c == '\n') {
            out[n] = '\0';
            return (int)n;
        }

        if (n + 1 < cap) {
            out[n++] = c;
        }
        /* If line is too long, we truncate (still consume input). */
    }
}

static int store_line(lines_t *ls, const char *line) {
    if (ls->nlines >= (uint64_t)MAX_LINES) return -1;

    uint64_t i = 0;
    while (line[i] != '\0') i++;
    uint64_t need = i + 1;

    if (ls->pool_len + need > (uint64_t)POOL_CAP) return -1;

    char *dst = &ls->pool[ls->pool_len];
    for (uint64_t k = 0; k < need; k++) dst[k] = line[k];

    ls->lines[ls->nlines++] = dst;
    ls->pool_len += need;
    return 0;
}

static int cmp_lex(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && b[i]) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca < cb) return -1;
        if (ca > cb) return 1;
        i++;
    }
    if (a[i] == b[i]) return 0;
    return a[i] ? 1 : -1;
}

static int parse_i64_prefix(const char *s, int64_t *out) {
    if (!s || !out) return -1;

    const char *p = s;
    while (*p && is_space(*p)) p++;

    int neg = 0;
    if (*p == '-') {
        neg = 1;
        p++;
    } else if (*p == '+') {
        p++;
    }

    if (!is_digit(*p)) return -1;

    int64_t v = 0;
    while (*p && is_digit(*p)) {
        int64_t d = (int64_t)(*p - '0');
        int64_t nv = v * 10 + d;
        if (nv < v) return -1;
        v = nv;
        p++;
    }

    *out = neg ? -v : v;
    return 0;
}

static int cmp_lines(const sort_opts_t *o, const char *a, const char *b) {
    int c = 0;
    if (o->opt_n) {
        int64_t va = 0, vb = 0;
        int ea = (parse_i64_prefix(a, &va) == 0);
        int eb = (parse_i64_prefix(b, &vb) == 0);

        if (ea && eb) {
            if (va < vb) c = -1;
            else if (va > vb) c = 1;
            else c = 0;
        } else if (ea && !eb) {
            c = -1;
        } else if (!ea && eb) {
            c = 1;
        } else {
            c = cmp_lex(a, b);
        }

        if (c == 0) {
            /* tie-breaker */
            c = cmp_lex(a, b);
        }
    } else {
        c = cmp_lex(a, b);
    }

    if (o->opt_r) c = -c;
    return c;
}

static void swap_ptr(const char **a, const char **b) {
    const char *t = *a;
    *a = *b;
    *b = t;
}

static int quicksort_iter(const sort_opts_t *o, const char **arr, int n) {
    if (n <= 1) return 0;

    typedef struct {
        int lo;
        int hi;
    } range_t;

    range_t stack[64];
    int sp = 0;
    stack[sp++] = (range_t){.lo = 0, .hi = n - 1};

    while (sp > 0) {
        range_t r = stack[--sp];
        int lo = r.lo;
        int hi = r.hi;

        while (lo < hi) {
            int mid = lo + ((hi - lo) / 2);
            const char *pivot = arr[mid];

            int i = lo;
            int j = hi;
            while (i <= j) {
                while (cmp_lines(o, arr[i], pivot) < 0) i++;
                while (cmp_lines(o, arr[j], pivot) > 0) j--;
                if (i <= j) {
                    swap_ptr(&arr[i], &arr[j]);
                    i++;
                    j--;
                }
            }

            /* Now: [lo..j] and [i..hi] are partitions */
            int left_lo = lo;
            int left_hi = j;
            int right_lo = i;
            int right_hi = hi;

            /* Recurse on smaller partition first; loop on larger to limit stack. */
            int left_sz = (left_hi - left_lo) + 1;
            int right_sz = (right_hi - right_lo) + 1;

            if (left_sz > 1 && right_sz > 1) {
                if (left_sz < right_sz) {
                    if (sp >= (int)(sizeof(stack) / sizeof(stack[0]))) return -1;
                    stack[sp++] = (range_t){.lo = right_lo, .hi = right_hi};
                    lo = left_lo;
                    hi = left_hi;
                } else {
                    if (sp >= (int)(sizeof(stack) / sizeof(stack[0]))) return -1;
                    stack[sp++] = (range_t){.lo = left_lo, .hi = left_hi};
                    lo = right_lo;
                    hi = right_hi;
                }
                continue;
            }

            if (left_sz > 1) {
                lo = left_lo;
                hi = left_hi;
                continue;
            }
            if (right_sz > 1) {
                lo = right_lo;
                hi = right_hi;
                continue;
            }

            break;
        }
    }

    return 0;
}

static int load_fd(lines_t *ls, uint64_t fd) {
    char line[LINE_MAX];

    for (;;) {
        int eof = 0;
        int n = read_line(fd, line, sizeof(line), &eof);
        if (n < 0) return -1;

        if (eof && line[0] == '\0') break;

        if (store_line(ls, line) != 0) {
            return -2;
        }

        if (eof) break;
    }

    return 0;
}

static void emit_line(const char *s) {
    sys_puts(s);
    putc1('\n');
}

static int is_same_line(const char *a, const char *b) {
    uint64_t i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    sort_opts_t o;
    o.opt_r = 0;
    o.opt_n = 0;
    o.opt_u = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a || a[0] != '-') break;
        if (streq(a, "--")) {
            i++;
            break;
        }
        if (streq(a, "-h") || streq(a, "--help")) {
            usage();
            return 0;
        }

        /* Allow combined flags like -ru */
        for (int j = 1; a[j]; j++) {
            char f = a[j];
            if (f == 'r') o.opt_r = 1;
            else if (f == 'n') o.opt_n = 1;
            else if (f == 'u') o.opt_u = 1;
            else {
                usage();
                return 2;
            }
        }
    }

    lines_t ls;
    ls.pool_len = 0;
    ls.nlines = 0;

    int nfiles = argc - i;
    if (nfiles <= 0) {
        int rc = load_fd(&ls, 0);
        if (rc == -2) {
            sys_puts("sort: input too large\n");
            return 1;
        }
        if (rc != 0) {
            sys_puts("sort: read failed\n");
            return 1;
        }
    } else {
        int status = 0;
        for (int fi = 0; fi < nfiles; fi++) {
            const char *path = argv[i + fi];
            long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
            if (fd < 0) {
                sys_puts("sort: cannot open: ");
                sys_puts(path);
                sys_puts("\n");
                status = 1;
                continue;
            }

            int rc = load_fd(&ls, (uint64_t)fd);
            (void)sys_close((uint64_t)fd);

            if (rc == -2) {
                sys_puts("sort: input too large\n");
                return 1;
            }
            if (rc != 0) {
                sys_puts("sort: read failed: ");
                sys_puts(path);
                sys_puts("\n");
                status = 1;
            }
        }

        if (status != 0 && ls.nlines == 0) {
            return 1;
        }
    }

    if (quicksort_iter(&o, ls.lines, (int)ls.nlines) != 0) {
        sys_puts("sort: internal error\n");
        return 1;
    }

    const char *prev = 0;
    for (uint64_t k = 0; k < ls.nlines; k++) {
        const char *s = ls.lines[k];
        if (o.opt_u) {
            if (prev && is_same_line(prev, s)) {
                continue;
            }
            prev = s;
        }
        emit_line(s);
    }

    return 0;
}
