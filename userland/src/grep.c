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
    sys_puts("usage: grep [-n] [-v] [-c] [-q] PATTERN [FILE...]\n");
}

enum {
    LINE_MAX = 512,
    READ_BUF = 512,
    KMP_MAX = 128,
};

static int build_kmp_lps(const char *pat, uint64_t m, uint16_t *lps) {
    if (m == 0) return 0;
    if (m > KMP_MAX) return -1;

    lps[0] = 0;
    uint64_t len = 0;
    for (uint64_t i = 1; i < m; i++) {
        while (len > 0 && pat[i] != pat[len]) {
            len = lps[len - 1];
        }
        if (pat[i] == pat[len]) {
            len++;
        }
        lps[i] = (uint16_t)len;
    }
    return 0;
}

typedef struct {
    int opt_n;
    int opt_v;
    int opt_c;
    int opt_q;

    const char *pattern;
    uint64_t pat_len;
    uint16_t lps[KMP_MAX];

    int show_filename;
} grep_opts_t;

static void print_prefix(const grep_opts_t *o, const char *name, uint64_t line_no) {
    if (o->show_filename && name) {
        sys_puts(name);
        putc1(':');
    }
    if (o->opt_n) {
        char nb[32];
        u64_to_dec(nb, sizeof(nb), line_no);
        sys_puts(nb);
        putc1(':');
    }
}

static int grep_fd(uint64_t fd, const char *name, const grep_opts_t *o, uint64_t *out_count, int *out_any_match) {
    uint64_t line_no = 1;
    uint64_t match_count = 0;
    int any_match = 0;

    char rbuf[READ_BUF];

    char line[LINE_MAX];
    uint64_t line_len = 0;
    int line_trunc = 0;

    uint64_t k = 0; /* KMP state */
    int line_has_match = (o->pat_len == 0) ? 1 : 0;

    for (;;) {
        long nread = (long)sys_read(fd, rbuf, sizeof(rbuf));
        if (nread == 0) break;
        if (nread < 0) {
            /* EAGAIN (11) => retry (pipes). */
            if (nread == -11) continue;
            return -1;
        }

        for (long i = 0; i < nread; i++) {
            char ch = rbuf[i];

            if (ch == '\n') {
                int selected = o->opt_v ? !line_has_match : line_has_match;
                if (selected) {
                    any_match = 1;
                    match_count++;
                    if (o->opt_q) {
                        *out_count = match_count;
                        *out_any_match = any_match;
                        return 1; /* early exit */
                    }
                    if (!o->opt_c) {
                        print_prefix(o, name, line_no);
                        (void)sys_write(1, line, line_len);
                        if (line_trunc) sys_puts("...\n");
                        else putc1('\n');
                    }
                }

                /* reset per-line state */
                line_no++;
                line_len = 0;
                line_trunc = 0;
                k = 0;
                line_has_match = (o->pat_len == 0) ? 1 : 0;
                continue;
            }

            if (!line_trunc) {
                if (line_len + 1 < (uint64_t)sizeof(line)) {
                    line[line_len++] = ch;
                } else {
                    line_trunc = 1;
                }
            }

            if (!line_has_match && o->pat_len != 0) {
                while (k > 0 && ch != o->pattern[k]) {
                    k = o->lps[k - 1];
                }
                if (ch == o->pattern[k]) {
                    k++;
                    if (k == o->pat_len) {
                        line_has_match = 1;
                        k = o->lps[k - 1];
                    }
                }
            }
        }
    }

    /* Handle trailing unterminated line. */
    if (line_len > 0 || line_trunc) {
        int selected = o->opt_v ? !line_has_match : line_has_match;
        if (selected) {
            any_match = 1;
            match_count++;
            if (o->opt_q) {
                *out_count = match_count;
                *out_any_match = any_match;
                return 1;
            }
            if (!o->opt_c) {
                print_prefix(o, name, line_no);
                (void)sys_write(1, line, line_len);
                if (line_trunc) sys_puts("...\n");
                else putc1('\n');
            }
        }
    }

    *out_count = match_count;
    *out_any_match = any_match;
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    grep_opts_t o;
    o.opt_n = 0;
    o.opt_v = 0;
    o.opt_c = 0;
    o.opt_q = 0;
    o.pattern = 0;
    o.pat_len = 0;
    o.show_filename = 0;

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

        for (int j = 1; a[j]; j++) {
            char f = a[j];
            if (f == 'n') o.opt_n = 1;
            else if (f == 'v') o.opt_v = 1;
            else if (f == 'c') o.opt_c = 1;
            else if (f == 'q') o.opt_q = 1;
            else {
                usage();
                return 2;
            }
        }
    }

    if (i >= argc) {
        usage();
        return 2;
    }

    o.pattern = argv[i++];
    o.pat_len = cstr_len_u64_local(o.pattern);
    if (o.pat_len > 0) {
        if (build_kmp_lps(o.pattern, o.pat_len, o.lps) != 0) {
            sys_puts("grep: pattern too long\n");
            return 2;
        }
    }

    int nfiles = argc - i;
    if (nfiles > 1) o.show_filename = 1;

    int overall_any = 0;
    int overall_err = 0;

    if (nfiles <= 0) {
        uint64_t cnt = 0;
        int any = 0;
        int rc = grep_fd(0, 0, &o, &cnt, &any);
        if (rc < 0) {
            sys_puts("grep: read failed\n");
            return 2;
        }
        if (o.opt_c && !o.opt_q) {
            char nb[32];
            u64_to_dec(nb, sizeof(nb), cnt);
            sys_puts(nb);
            putc1('\n');
        }
        overall_any = any;
        return overall_any ? 0 : 1;
    }

    for (int fi = 0; fi < nfiles; fi++) {
        const char *path = argv[i + fi];
        if (!path || path[0] == '\0') continue;

        long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
        if (fd < 0) {
            sys_puts("grep: cannot open: ");
            sys_puts(path);
            sys_puts("\n");
            overall_err = 1;
            continue;
        }

        uint64_t cnt = 0;
        int any = 0;
        int rc = grep_fd((uint64_t)fd, path, &o, &cnt, &any);
        (void)sys_close((uint64_t)fd);

        if (rc < 0) {
            sys_puts("grep: read failed: ");
            sys_puts(path);
            sys_puts("\n");
            overall_err = 1;
            continue;
        }

        if (rc > 0 && o.opt_q) {
            return 0;
        }

        if (o.opt_c && !o.opt_q) {
            if (o.show_filename) {
                sys_puts(path);
                putc1(':');
            }
            char nb[32];
            u64_to_dec(nb, sizeof(nb), cnt);
            sys_puts(nb);
            putc1('\n');
        }

        if (any) overall_any = 1;
    }

    if (o.opt_q) {
        return overall_any ? 0 : (overall_err ? 2 : 1);
    }

    if (overall_err) return 2;
    return overall_any ? 0 : 1;
}
