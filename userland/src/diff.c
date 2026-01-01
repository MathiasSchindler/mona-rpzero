#include "syscall.h"

#define AT_FDCWD ((long)-100)

enum {
    O_RDONLY = 0,
    MAP_PRIVATE = 0x02,
    MAP_ANONYMOUS = 0x20,
    PROT_READ = 0x1,
    PROT_WRITE = 0x2,
};

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

static void u64_to_dec(char *out, uint64_t cap, uint64_t v) {
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

static void usage(void) {
    sys_puts("usage: diff [-q] [-s] [-u] [-U N] FILE1 FILE2\n");
    sys_puts("  -q  quiet (no output, status only)\n");
    sys_puts("  -s  report identical files\n");
    sys_puts("  -u  unified diff (single hunk)\n");
    sys_puts("  -U  unified context lines (default 3)\n");
}

static void print_differ(const char *a, const char *b, uint64_t byte_pos_1based, uint64_t line_1based) {
    sys_puts("diff: ");
    sys_puts(a);
    sys_puts(" ");
    sys_puts(b);
    sys_puts(": differ at byte ");

    char nb[32];
    u64_to_dec(nb, sizeof(nb), byte_pos_1based);
    sys_puts(nb);
    sys_puts(", line ");
    u64_to_dec(nb, sizeof(nb), line_1based);
    sys_puts(nb);
    sys_puts("\n");
}

static int is_dash(const char *s) {
    return s && s[0] == '-' && s[1] == '\0';
}

static long open_ro_maybe_stdin(const char *path) {
    if (is_dash(path)) return 0;
    return (long)sys_openat((uint64_t)AT_FDCWD, path, O_RDONLY, 0);
}

static uint64_t min_u64(uint64_t a, uint64_t b) {
    return (a < b) ? a : b;
}

static int parse_u64(const char *s, uint64_t *out) {
    if (!s || !out) return -1;
    if (s[0] == '\0') return -1;
    uint64_t v = 0;
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        char c = s[i];
        if (c < '0' || c > '9') return -1;
        v = v * 10u + (uint64_t)(c - '0');
    }
    *out = v;
    return 0;
}

typedef struct {
    char *p;
    uint64_t len;
    uint64_t cap;
} buf_t;

static int buf_reserve(buf_t *b, uint64_t need_cap) {
    if (!b) return -1;
    if (need_cap <= b->cap) return 0;

    uint64_t new_cap = (b->cap == 0) ? 4096u : b->cap;
    while (new_cap < need_cap) {
        if (new_cap > (1ull << 31)) return -1;
        new_cap *= 2u;
    }

    uint64_t newp_u = sys_mmap(0, new_cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((int64_t)newp_u < 0) return -1;
    char *newp = (char *)(uintptr_t)newp_u;

    for (uint64_t i = 0; i < b->len; i++) newp[i] = b->p[i];

    if (b->p && b->cap) {
        (void)sys_munmap(b->p, b->cap);
    }
    b->p = newp;
    b->cap = new_cap;
    return 0;
}

static int read_all(uint64_t fd, buf_t *out) {
    if (!out) return -1;
    out->p = 0;
    out->len = 0;
    out->cap = 0;

    char tmp[512];
    for (;;) {
        long n = (long)sys_read(fd, tmp, sizeof(tmp));
        if (n < 0) {
            if (n == -11) continue; /* EAGAIN */
            return -1;
        }
        if (n == 0) break;

        if (buf_reserve(out, out->len + (uint64_t)n + 1) != 0) return -1;
        for (long i = 0; i < n; i++) {
            out->p[out->len++] = tmp[i];
        }
    }

    if (buf_reserve(out, out->len + 1) != 0) return -1;
    out->p[out->len] = '\0';
    return 0;
}

typedef struct {
    uint64_t off;
    uint64_t len;
} line_t;

typedef struct {
    line_t *lines;
    uint64_t nlines;
} lines_t;

static int lines_from_buf(const buf_t *b, lines_t *out) {
    if (!b || !out) return -1;
    out->lines = 0;
    out->nlines = 0;

    uint64_t n = 0;
    for (uint64_t i = 0; i < b->len; i++) {
        if (b->p[i] == '\n') n++;
    }
    if (b->len > 0 && b->p[b->len - 1] != '\n') n++;

    uint64_t bytes = n * (uint64_t)sizeof(line_t);
    if (bytes == 0) {
        out->lines = 0;
        out->nlines = 0;
        return 0;
    }

    uint64_t p_u = sys_mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((int64_t)p_u < 0) return -1;
    line_t *arr = (line_t *)(uintptr_t)p_u;

    uint64_t idx = 0;
    uint64_t start = 0;
    for (uint64_t i = 0; i < b->len; i++) {
        if (b->p[i] == '\n') {
            if (idx < n) {
                arr[idx].off = start;
                arr[idx].len = i - start;
            }
            idx++;
            start = i + 1;
        }
    }
    if (b->len > 0 && b->p[b->len - 1] != '\n') {
        if (idx < n) {
            arr[idx].off = start;
            arr[idx].len = b->len - start;
        }
        idx++;
    }

    out->lines = arr;
    out->nlines = n;
    return 0;
}

static int line_eq(const buf_t *a, line_t la, const buf_t *b, line_t lb) {
    if (la.len != lb.len) return 0;
    for (uint64_t i = 0; i < la.len; i++) {
        if (a->p[la.off + i] != b->p[lb.off + i]) return 0;
    }
    return 1;
}

static void write_line_prefixed(char prefix, const buf_t *b, line_t l) {
    (void)sys_write(1, &prefix, 1);
    if (l.len > 0) (void)sys_write(1, b->p + l.off, l.len);
    (void)sys_write(1, "\n", 1);
}

static void write_unified_header(const char *a, const char *b) {
    sys_puts("--- ");
    sys_puts(a);
    sys_puts("\n");
    sys_puts("+++ ");
    sys_puts(b);
    sys_puts("\n");
}

static void write_unified_hunk_header(uint64_t a_start1, uint64_t a_len, uint64_t b_start1, uint64_t b_len) {
    char nb[32];
    sys_puts("@@ -");
    u64_to_dec(nb, sizeof(nb), a_start1);
    sys_puts(nb);
    sys_puts(",");
    u64_to_dec(nb, sizeof(nb), a_len);
    sys_puts(nb);
    sys_puts(" +");
    u64_to_dec(nb, sizeof(nb), b_start1);
    sys_puts(nb);
    sys_puts(",");
    u64_to_dec(nb, sizeof(nb), b_len);
    sys_puts(nb);
    sys_puts(" @@\n");
}

static int diff_unified(const char *a_path, const char *b_path, int opt_q, int opt_s, uint64_t context) {
    if (is_dash(a_path) && is_dash(b_path)) {
        if (!opt_q) sys_puts("diff: cannot compare - to - (stdin used twice)\n");
        return 2;
    }

    long fda = open_ro_maybe_stdin(a_path);
    if (fda < 0) {
        if (!opt_q) {
            sys_puts("diff: cannot open: ");
            sys_puts(a_path);
            sys_puts("\n");
        }
        return 2;
    }

    long fdb = open_ro_maybe_stdin(b_path);
    if (fdb < 0) {
        if (!is_dash(a_path)) (void)sys_close((uint64_t)fda);
        if (!opt_q) {
            sys_puts("diff: cannot open: ");
            sys_puts(b_path);
            sys_puts("\n");
        }
        return 2;
    }

    buf_t a;
    buf_t b;
    if (read_all((uint64_t)fda, &a) != 0 || read_all((uint64_t)fdb, &b) != 0) {
        if (!is_dash(a_path)) (void)sys_close((uint64_t)fda);
        if (!is_dash(b_path)) (void)sys_close((uint64_t)fdb);
        if (!opt_q) sys_puts("diff: read failed\n");
        return 2;
    }

    if (!is_dash(a_path)) (void)sys_close((uint64_t)fda);
    if (!is_dash(b_path)) (void)sys_close((uint64_t)fdb);

    lines_t al;
    lines_t bl;
    if (lines_from_buf(&a, &al) != 0 || lines_from_buf(&b, &bl) != 0) {
        if (!opt_q) sys_puts("diff: out of memory\n");
        return 2;
    }

    uint64_t pfx = 0;
    while (pfx < al.nlines && pfx < bl.nlines) {
        if (!line_eq(&a, al.lines[pfx], &b, bl.lines[pfx])) break;
        pfx++;
    }

    uint64_t sfx = 0;
    while (sfx < (al.nlines - pfx) && sfx < (bl.nlines - pfx)) {
        uint64_t ai = al.nlines - 1 - sfx;
        uint64_t bi = bl.nlines - 1 - sfx;
        if (!line_eq(&a, al.lines[ai], &b, bl.lines[bi])) break;
        sfx++;
    }

    if (pfx == al.nlines && pfx == bl.nlines) {
        if (opt_s && !opt_q) {
            sys_puts("Files ");
            sys_puts(a_path);
            sys_puts(" and ");
            sys_puts(b_path);
            sys_puts(" are identical\n");
        }
        return 0;
    }

    if (opt_q) return 1;

    uint64_t pre_ctx = min_u64(context, pfx);
    uint64_t suf_ctx = min_u64(context, sfx);

    uint64_t start = pfx - pre_ctx;
    uint64_t a_mid_end = al.nlines - sfx;
    uint64_t b_mid_end = bl.nlines - sfx;
    uint64_t a_end = min_u64(al.nlines, a_mid_end + suf_ctx);
    uint64_t b_end = min_u64(bl.nlines, b_mid_end + suf_ctx);

    write_unified_header(a_path, b_path);
    write_unified_hunk_header(start + 1, a_end - start, start + 1, b_end - start);

    for (uint64_t i = start; i < pfx; i++) {
        write_line_prefixed(' ', &a, al.lines[i]);
    }

    for (uint64_t i = pfx; i < a_mid_end; i++) {
        write_line_prefixed('-', &a, al.lines[i]);
    }

    for (uint64_t i = pfx; i < b_mid_end; i++) {
        write_line_prefixed('+', &b, bl.lines[i]);
    }

    for (uint64_t i = a_mid_end; i < a_end; i++) {
        write_line_prefixed(' ', &a, al.lines[i]);
    }

    return 1;
}

static int diff_files(const char *a, const char *b, int opt_q, int opt_s) {
    if (is_dash(a) && is_dash(b)) {
        if (!opt_q) sys_puts("diff: cannot compare - to - (stdin used twice)\n");
        return 2;
    }

    long fda = open_ro_maybe_stdin(a);
    if (fda < 0) {
        if (!opt_q) {
            sys_puts("diff: cannot open: ");
            sys_puts(a);
            sys_puts("\n");
        }
        return 2;
    }

    long fdb = open_ro_maybe_stdin(b);
    if (fdb < 0) {
        if (!is_dash(a)) (void)sys_close((uint64_t)fda);
        if (!opt_q) {
            sys_puts("diff: cannot open: ");
            sys_puts(b);
            sys_puts("\n");
        }
        return 2;
    }

    char bufa[512];
    char bufb[512];
    uint64_t na = 0, nb = 0;
    uint64_t ia = 0, ib = 0;

    uint64_t byte_pos = 1;
    uint64_t line_no = 1;

    for (;;) {
        if (ia == na) {
            long r = (long)sys_read((uint64_t)fda, bufa, sizeof(bufa));
            if (r < 0) {
                if (r == -11) continue; /* EAGAIN */
                (void)sys_close((uint64_t)fda);
                (void)sys_close((uint64_t)fdb);
                if (!opt_q) sys_puts("diff: read failed\n");
                return 2;
            }
            na = (uint64_t)r;
            ia = 0;
        }

        if (ib == nb) {
            long r = (long)sys_read((uint64_t)fdb, bufb, sizeof(bufb));
            if (r < 0) {
                if (r == -11) continue; /* EAGAIN */
                (void)sys_close((uint64_t)fda);
                (void)sys_close((uint64_t)fdb);
                if (!opt_q) sys_puts("diff: read failed\n");
                return 2;
            }
            nb = (uint64_t)r;
            ib = 0;
        }

        if (na == 0 && nb == 0) {
            if (!is_dash(a)) (void)sys_close((uint64_t)fda);
            if (!is_dash(b)) (void)sys_close((uint64_t)fdb);
            if (opt_s && !opt_q) {
                sys_puts("Files ");
                sys_puts(a);
                sys_puts(" and ");
                sys_puts(b);
                sys_puts(" are identical\n");
            }
            return 0;
        }

        if (na == 0 || nb == 0) {
            if (!is_dash(a)) (void)sys_close((uint64_t)fda);
            if (!is_dash(b)) (void)sys_close((uint64_t)fdb);
            if (!opt_q) print_differ(a, b, byte_pos, line_no);
            return 1;
        }

        uint64_t ra = na - ia;
        uint64_t rb = nb - ib;
        uint64_t m = (ra < rb) ? ra : rb;

        for (uint64_t k = 0; k < m; k++) {
            char ca = bufa[ia++];
            char cb = bufb[ib++];
            if (ca != cb) {
                if (!is_dash(a)) (void)sys_close((uint64_t)fda);
                if (!is_dash(b)) (void)sys_close((uint64_t)fdb);
                if (!opt_q) print_differ(a, b, byte_pos, line_no);
                return 1;
            }
            if (ca == '\n') line_no++;
            byte_pos++;
        }

        if (ia == na) {
            na = 0;
            ia = 0;
        }
        if (ib == nb) {
            nb = 0;
            ib = 0;
        }
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int opt_q = 0;
    int opt_s = 0;
    int opt_u = 0;
    uint64_t opt_context = 3;

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

        if (streq(a, "-U")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            uint64_t v = 0;
            if (parse_u64(argv[i + 1], &v) != 0) {
                usage();
                return 2;
            }
            opt_context = v;
            i++;
            continue;
        }

        for (int j = 1; a[j] != '\0'; j++) {
            char f = a[j];
            if (f == 'q') opt_q = 1;
            else if (f == 's') opt_s = 1;
            else if (f == 'u') opt_u = 1;
            else {
                usage();
                return 2;
            }
        }
    }

    if (argc - i != 2) {
        usage();
        return 2;
    }

    const char *file1 = argv[i];
    const char *file2 = argv[i + 1];
    if (!file1 || !file2 || cstr_len_u64_local(file1) == 0 || cstr_len_u64_local(file2) == 0) {
        usage();
        return 2;
    }

    if (opt_u) {
        return diff_unified(file1, file2, opt_q, opt_s, opt_context);
    }

    return diff_files(file1, file2, opt_q, opt_s);
}
