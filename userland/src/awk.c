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

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static int mem_contains(const char *hay, uint64_t hay_len, const char *needle, uint64_t nlen) {
    if (nlen == 0) return 1;
    if (!hay || !needle) return 0;
    if (hay_len < nlen) return 0;

    for (uint64_t i = 0; i + nlen <= hay_len; i++) {
        int ok = 1;
        for (uint64_t j = 0; j < nlen; j++) {
            if (hay[i + j] != needle[j]) {
                ok = 0;
                break;
            }
        }
        if (ok) return 1;
    }
    return 0;
}

static void u64_to_dec(char *out, uint64_t cap, uint64_t v) {
    if (!out || cap == 0) return;

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
    sys_puts("usage: awk [-F SEP] PROGRAM [FILE...]\n");
    sys_puts("supported PROGRAM forms:\n");
    sys_puts("  {print} | {print $0} | {print $N} | {print NR} | {print NF}\n");
    sys_puts("  /TEXT/ {print ...}  (TEXT is a plain substring, not regex)\n");
}

typedef enum {
    AWK_ITEM_LINE = 1,
    AWK_ITEM_FIELD,
    AWK_ITEM_NR,
    AWK_ITEM_NF,
} awk_item_kind_t;

typedef struct {
    awk_item_kind_t kind;
    uint32_t field; /* 0 for $0 */
} awk_item_t;

typedef struct {
    int has_pattern;
    char pattern[128];
    uint64_t pattern_len;

    awk_item_t items[8];
    int nitems;

    int fs_is_char;
    char fs_char;
} awk_prog_t;

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int starts_with_kw(const char *s, uint64_t i, const char *kw) {
    for (uint64_t k = 0; kw[k] != '\0'; k++) {
        if (s[i + k] != kw[k]) return 0;
    }
    return 1;
}

static void skip_ws(const char *s, uint64_t *i_inout) {
    uint64_t i = *i_inout;
    while (s[i] != '\0' && is_space(s[i])) i++;
    *i_inout = i;
}

static int parse_uint(const char *s, uint64_t *i_inout, uint64_t *out) {
    uint64_t i = *i_inout;
    if (!is_digit(s[i])) return -1;
    uint64_t v = 0;
    while (is_digit(s[i])) {
        v = v * 10u + (uint64_t)(s[i] - '0');
        i++;
    }
    *i_inout = i;
    *out = v;
    return 0;
}

static int add_item(awk_prog_t *p, awk_item_kind_t kind, uint32_t field) {
    if (p->nitems >= (int)(sizeof(p->items) / sizeof(p->items[0]))) return -1;
    p->items[p->nitems].kind = kind;
    p->items[p->nitems].field = field;
    p->nitems++;
    return 0;
}

static int parse_action_items(const char *act, awk_prog_t *p) {
    uint64_t i = 0;
    skip_ws(act, &i);

    if (act[i] == '\0') {
        return add_item(p, AWK_ITEM_LINE, 0);
    }

    if (!starts_with_kw(act, i, "print")) {
        return -1;
    }
    i += 5;
    skip_ws(act, &i);

    if (act[i] == '\0') {
        return add_item(p, AWK_ITEM_LINE, 0);
    }

    for (;;) {
        skip_ws(act, &i);
        if (act[i] == '\0') break;
        if (act[i] == ',') {
            i++;
            continue;
        }

        if (act[i] == '$') {
            i++;
            uint64_t f = 0;
            if (parse_uint(act, &i, &f) != 0) return -1;
            if (f > 0xffffffffu) return -1;
            if (add_item(p, (f == 0) ? AWK_ITEM_LINE : AWK_ITEM_FIELD, (uint32_t)f) != 0) return -1;
        } else if (starts_with_kw(act, i, "NR")) {
            i += 2;
            if (add_item(p, AWK_ITEM_NR, 0) != 0) return -1;
        } else if (starts_with_kw(act, i, "NF")) {
            i += 2;
            if (add_item(p, AWK_ITEM_NF, 0) != 0) return -1;
        } else {
            /* Unknown token */
            return -1;
        }

        /* Skip trailing token chars (e.g., accidentally "NRx") */
        if (is_alpha(act[i]) || is_digit(act[i]) || act[i] == '_') return -1;

        skip_ws(act, &i);
        if (act[i] == ',') {
            i++;
            continue;
        }

        if (act[i] == '\0') break;
        /* allow whitespace-separated list */
    }

    if (p->nitems == 0) return add_item(p, AWK_ITEM_LINE, 0);
    return 0;
}

static int parse_program(const char *prog_s, awk_prog_t *p) {
    if (!prog_s || !p) return -1;

    p->has_pattern = 0;
    p->pattern[0] = '\0';
    p->pattern_len = 0;
    p->nitems = 0;

    uint64_t i = 0;
    skip_ws(prog_s, &i);

    if (prog_s[i] == '/') {
        i++;
        uint64_t start = i;
        while (prog_s[i] != '\0' && prog_s[i] != '/') {
            i++;
        }
        if (prog_s[i] != '/') return -1;
        uint64_t len = i - start;
        if (len >= sizeof(p->pattern)) len = sizeof(p->pattern) - 1;
        for (uint64_t k = 0; k < len; k++) p->pattern[k] = prog_s[start + k];
        p->pattern[len] = '\0';
        p->pattern_len = len;
        p->has_pattern = 1;
        i++; /* skip closing / */
        skip_ws(prog_s, &i);
    }

    /* Optional { ... } action */
    if (prog_s[i] == '{') {
        i++;
        uint64_t start = i;
        while (prog_s[i] != '\0' && prog_s[i] != '}') {
            i++;
        }
        if (prog_s[i] != '}') return -1;
        uint64_t len = i - start;

        char act[256];
        if (len >= sizeof(act)) len = sizeof(act) - 1;
        for (uint64_t k = 0; k < len; k++) act[k] = prog_s[start + k];
        act[len] = '\0';

        if (parse_action_items(act, p) != 0) return -1;

        i++; /* } */
        skip_ws(prog_s, &i);
        if (prog_s[i] != '\0') {
            /* trailing junk */
            return -1;
        }
        return 0;
    }

    /* No braces: treat as default print (possibly pattern-only). */
    skip_ws(prog_s, &i);
    if (prog_s[i] != '\0') return -1;
    return add_item(p, AWK_ITEM_LINE, 0);
}

typedef struct {
    const char *p;
    uint64_t len;
} span_t;

static uint64_t split_fields_ws(const char *line, uint64_t len, span_t *out, uint64_t cap) {
    uint64_t nf = 0;
    uint64_t i = 0;
    while (i < len) {
        while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= len) break;
        uint64_t start = i;
        while (i < len && line[i] != ' ' && line[i] != '\t') i++;
        if (nf < cap) {
            out[nf].p = line + start;
            out[nf].len = i - start;
        }
        nf++;
    }
    return nf;
}

static uint64_t split_fields_char(const char *line, uint64_t len, char sep, span_t *out, uint64_t cap) {
    uint64_t nf = 0;
    uint64_t start = 0;
    for (uint64_t i = 0; i <= len; i++) {
        if (i == len || line[i] == sep) {
            if (nf < cap) {
                out[nf].p = line + start;
                out[nf].len = i - start;
            }
            nf++;
            start = i + 1;
        }
    }
    return nf;
}

static int awk_process_line(const awk_prog_t *p, const char *line, uint64_t len, uint64_t nr) {
    if (p->has_pattern) {
        if (!mem_contains(line, len, p->pattern, p->pattern_len)) {
            return 0;
        }
    }

    span_t fields[32];
    uint64_t nf = 0;
    if (p->fs_is_char) {
        nf = split_fields_char(line, len, p->fs_char, fields, (uint64_t)(sizeof(fields) / sizeof(fields[0])));
    } else {
        nf = split_fields_ws(line, len, fields, (uint64_t)(sizeof(fields) / sizeof(fields[0])));
    }

    for (int ii = 0; ii < p->nitems; ii++) {
        if (ii != 0) putc1(' ');

        awk_item_t it = p->items[ii];
        if (it.kind == AWK_ITEM_LINE) {
            (void)sys_write(1, line, len);
        } else if (it.kind == AWK_ITEM_FIELD) {
            uint32_t f = it.field;
            if (f == 0) {
                (void)sys_write(1, line, len);
            } else {
                if (f <= nf && f >= 1) {
                    span_t sp = fields[f - 1];
                    (void)sys_write(1, sp.p, sp.len);
                }
            }
        } else if (it.kind == AWK_ITEM_NR) {
            char nb[32];
            u64_to_dec(nb, sizeof(nb), nr);
            sys_puts(nb);
        } else if (it.kind == AWK_ITEM_NF) {
            char nb[32];
            u64_to_dec(nb, sizeof(nb), nf);
            sys_puts(nb);
        }
    }
    putc1('\n');
    return 0;
}

static int awk_fd(uint64_t fd, const awk_prog_t *p) {
    enum {
        READ_BUF = 512,
        LINE_MAX = 512,
    };

    char rbuf[READ_BUF];
    char line[LINE_MAX];
    uint64_t line_len = 0;
    int line_trunc = 0;

    uint64_t nr = 0;

    for (;;) {
        long nread = (long)sys_read(fd, rbuf, sizeof(rbuf));
        if (nread == 0) break;
        if (nread < 0) {
            if (nread == -11) continue; /* EAGAIN */
            return -1;
        }

        for (long i = 0; i < nread; i++) {
            char ch = rbuf[i];
            if (ch == '\n') {
                nr++;
                (void)awk_process_line(p, line, line_len, nr);
                line_len = 0;
                line_trunc = 0;
                continue;
            }

            if (!line_trunc) {
                if (line_len + 1 < (uint64_t)sizeof(line)) {
                    line[line_len++] = ch;
                } else {
                    line_trunc = 1;
                }
            }
        }
    }

    if (line_len > 0 || line_trunc) {
        nr++;
        (void)awk_process_line(p, line, line_len, nr);
    }

    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    awk_prog_t p;
    p.fs_is_char = 0;
    p.fs_char = ' ';

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!a) break;
        if (a[0] != '-') break;
        if (streq(a, "--")) {
            i++;
            break;
        }
        if (streq(a, "-h") || streq(a, "--help")) {
            usage();
            return 0;
        }
        if (streq(a, "-F")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            const char *sep = argv[i + 1];
            if (!sep || sep[0] == '\0' || sep[1] != '\0') {
                sys_puts("awk: -F expects a single character separator\n");
                return 2;
            }
            p.fs_is_char = 1;
            p.fs_char = sep[0];
            i++;
            continue;
        }

        usage();
        return 2;
    }

    if (i >= argc) {
        usage();
        return 2;
    }

    const char *prog_s = argv[i++];
    if (parse_program(prog_s, &p) != 0) {
        sys_puts("awk: parse error\n");
        return 2;
    }

    int nfiles = argc - i;
    if (nfiles <= 0) {
        if (awk_fd(0, &p) != 0) {
            sys_puts("awk: read failed\n");
            return 1;
        }
        return 0;
    }

    int status = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        const char *path = argv[i + fi];
        long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
        if (fd < 0) {
            sys_puts("awk: cannot open: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
            continue;
        }
        if (awk_fd((uint64_t)fd, &p) != 0) {
            sys_puts("awk: read failed: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
        }
        (void)sys_close((uint64_t)fd);
    }

    return status;
}
