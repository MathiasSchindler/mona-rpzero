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

static int strip_outer_quotes(const char *in, char *out, uint64_t cap) {
    if (!in || !out || cap == 0) return -1;

    uint64_t n = cstr_len_u64_local(in);
    if (n + 1 > cap) return -1;

    char q = 0;
    if (n >= 2 && ((in[0] == '\'' && in[n - 1] == '\'') || (in[0] == '"' && in[n - 1] == '"'))) {
        q = in[0];
    }

    uint64_t start = (q != 0) ? 1 : 0;
    uint64_t end = (q != 0) ? (n - 1) : n;

    uint64_t o = 0;
    for (uint64_t i = start; i < end; i++) {
        if (o + 1 >= cap) return -1;
        out[o++] = in[i];
    }
    out[o] = '\0';
    return 0;
}

static void usage(void) {
    sys_puts("usage: sed [-n] [-e SCRIPT]... [SCRIPT] [FILE...]\n");
    sys_puts("\n");
    sys_puts("Supported commands (very small subset):\n");
    sys_puts("  s/OLD/NEW/[gp]   substring replacement; flags: g=global, p=print if replaced\n");
    sys_puts("  d                delete line (suppress output)\n");
    sys_puts("  p                print line\n");
    sys_puts("\n");
    sys_puts("Notes: no regex, no addresses, no hold space. Commands may be separated by ';'.\n");
}

enum {
    LINE_MAX = 1024,
    MAX_CMDS = 16,
    MAX_TEXT = 128,
};

typedef enum {
    CMD_SUBST,
    CMD_DELETE,
    CMD_PRINT,
} cmd_type_t;

typedef struct {
    cmd_type_t type;

    /* for substitution */
    char pat[MAX_TEXT];
    char rep[MAX_TEXT];
    int flag_g;
    int flag_p;
} sed_cmd_t;

static int read_line(uint64_t fd, char *line, uint64_t cap, int *out_eof) {
    if (!line || cap == 0 || !out_eof) return -1;

    uint64_t n = 0;
    *out_eof = 0;

    for (;;) {
        char c = 0;
        long rc = (long)sys_read(fd, &c, 1);
        if (rc == 0) {
            *out_eof = 1;
            line[n] = '\0';
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
            line[n] = '\0';
            return (int)n;
        }

        if (n + 1 < cap) {
            line[n++] = c;
        }
        /* If too long, we truncate (still consume input). */
    }
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static const char *skip_spaces(const char *p) {
    while (p && is_space(*p)) p++;
    return p;
}

static int parse_escaped_char(char esc, char *out) {
    if (!out) return -1;
    if (esc == 'n') *out = '\n';
    else if (esc == 't') *out = '\t';
    else if (esc == 'r') *out = '\r';
    else if (esc == '\\') *out = '\\';
    else return -1;
    return 0;
}

static int parse_delim_text(const char **pp, char delim, char *out, uint64_t cap) {
    const char *p = *pp;
    uint64_t o = 0;

    for (;;) {
        char c = *p;
        if (c == '\0') return -1;
        if (c == delim) {
            if (o >= cap) return -1;
            out[o] = '\0';
            p++; /* consume delim */
            *pp = p;
            return 0;
        }

        if (c == '\\') {
            p++;
            char e = *p;
            if (e == '\0') return -1;

            char decoded = 0;
            if (e == delim) {
                decoded = delim;
            } else if (parse_escaped_char(e, &decoded) == 0) {
                /* ok */
            } else {
                /* unknown escape: take literal */
                decoded = e;
            }

            if (o + 1 < cap) {
                out[o++] = decoded;
            }
            p++;
            continue;
        }

        if (o + 1 < cap) {
            out[o++] = c;
        }
        p++;
    }
}

static int parse_one_cmd(const char **pp, sed_cmd_t *out) {
    const char *p = skip_spaces(*pp);
    if (!p || *p == '\0') {
        *pp = p;
        return 1; /* no cmd */
    }

    if (*p == ';') {
        p++;
        *pp = p;
        return 1; /* empty cmd */
    }

    if (*p == 'd') {
        out->type = CMD_DELETE;
        out->pat[0] = '\0';
        out->rep[0] = '\0';
        out->flag_g = 0;
        out->flag_p = 0;
        p++;
        *pp = p;
        return 0;
    }

    if (*p == 'p') {
        out->type = CMD_PRINT;
        out->pat[0] = '\0';
        out->rep[0] = '\0';
        out->flag_g = 0;
        out->flag_p = 0;
        p++;
        *pp = p;
        return 0;
    }

    if (*p == 's') {
        p++;
        char delim = *p;
        if (delim == '\0') return -1;
        p++;

        out->type = CMD_SUBST;
        out->flag_g = 0;
        out->flag_p = 0;

        if (parse_delim_text(&p, delim, out->pat, sizeof(out->pat)) != 0) return -1;
        if (parse_delim_text(&p, delim, out->rep, sizeof(out->rep)) != 0) return -1;

        if (out->pat[0] == '\0') {
            /* avoid infinite-loop behavior; real sed supports this, but we don't */
            return -1;
        }

        for (;;) {
            char f = *p;
            if (f == 'g') {
                out->flag_g = 1;
                p++;
                continue;
            }
            if (f == 'p') {
                out->flag_p = 1;
                p++;
                continue;
            }
            break;
        }

        *pp = p;
        return 0;
    }

    return -1;
}

static void copy_cmd(sed_cmd_t *dst, const sed_cmd_t *src) {
    dst->type = src->type;
    dst->flag_g = src->flag_g;
    dst->flag_p = src->flag_p;
    for (uint64_t i = 0; i < (uint64_t)sizeof(dst->pat); i++) dst->pat[i] = src->pat[i];
    for (uint64_t i = 0; i < (uint64_t)sizeof(dst->rep); i++) dst->rep[i] = src->rep[i];
}

static int parse_script(const char *script, sed_cmd_t *cmds, uint64_t cap, uint64_t *out_added) {
    uint64_t n = 0;
    const char *p = script;

    while (p && *p) {
        if (n >= cap) return -1;

        sed_cmd_t cmd;
        int rc = parse_one_cmd(&p, &cmd);
        if (rc < 0) return -1;
        if (rc == 1) {
            /* skip separators/spaces */
            p = skip_spaces(p);
            if (*p == ';') {
                p++;
            } else if (*p == '\0') {
                break;
            } else {
                /* could be more text; loop */
            }
            continue;
        }

        copy_cmd(&cmds[n++], &cmd);

        /* allow ';' separators */
        p = skip_spaces(p);
        if (*p == ';') p++;
    }

    *out_added = n;
    return 0;
}

static int find_substr(const char *s, const char *pat, uint64_t start) {
    uint64_t sl = cstr_len_u64_local(s);
    uint64_t pl = cstr_len_u64_local(pat);
    if (pl == 0) return -1;
    if (start > sl) return -1;

    for (uint64_t i = start; i + pl <= sl; i++) {
        uint64_t k = 0;
        while (k < pl && s[i + k] == pat[k]) k++;
        if (k == pl) return (int)i;
    }
    return -1;
}

static int apply_subst(const char *in, char *out, uint64_t out_cap, const char *pat, const char *rep, int flag_g, int *out_replaced) {
    if (!in || !out || out_cap == 0 || !pat || !rep || !out_replaced) return -1;

    *out_replaced = 0;

    uint64_t in_len = cstr_len_u64_local(in);
    uint64_t pat_len = cstr_len_u64_local(pat);
    uint64_t rep_len = cstr_len_u64_local(rep);
    if (pat_len == 0) return -1;

    uint64_t o = 0;
    uint64_t pos = 0;

    for (;;) {
        int idx = find_substr(in, pat, pos);
        if (idx < 0) {
            /* copy remainder */
            while (pos < in_len && o + 1 < out_cap) {
                out[o++] = in[pos++];
            }
            break;
        }

        /* copy prefix */
        uint64_t i = pos;
        while (i < (uint64_t)idx && o + 1 < out_cap) {
            out[o++] = in[i++];
        }

        /* copy replacement */
        for (uint64_t r = 0; r < rep_len && o + 1 < out_cap; r++) {
            out[o++] = rep[r];
        }

        *out_replaced = 1;

        pos = (uint64_t)idx + pat_len;
        if (!flag_g) {
            /* copy rest and stop */
            while (pos < in_len && o + 1 < out_cap) {
                out[o++] = in[pos++];
            }
            break;
        }
    }

    out[o] = '\0';
    return 0;
}

static void emit_line(const char *line) {
    sys_puts(line);
    putc1('\n');
}

static int sed_fd(uint64_t fd, const sed_cmd_t *cmds, uint64_t ncmds, int opt_n) {
    char line[LINE_MAX];
    char tmp[LINE_MAX];

    for (;;) {
        int eof = 0;
        int n = read_line(fd, line, sizeof(line), &eof);
        if (n < 0) return -1;
        if (eof && line[0] == '\0') break;

        int deleted = 0;
        int explicit_prints = 0;
        int line_modified = 0;

        for (uint64_t ci = 0; ci < ncmds; ci++) {
            const sed_cmd_t *c = &cmds[ci];
            if (c->type == CMD_DELETE) {
                deleted = 1;
                break;
            } else if (c->type == CMD_PRINT) {
                explicit_prints++;
            } else if (c->type == CMD_SUBST) {
                int replaced = 0;
                if (apply_subst(line, tmp, sizeof(tmp), c->pat, c->rep, c->flag_g, &replaced) != 0) {
                    return -1;
                }
                if (replaced) {
                    line_modified = 1;
                    /* copy tmp back to line */
                    uint64_t k = 0;
                    while (tmp[k] && k + 1 < sizeof(line)) {
                        line[k] = tmp[k];
                        k++;
                    }
                    line[k] = '\0';

                    if (c->flag_p) {
                        explicit_prints++;
                    }
                }
            }
        }

        if (!deleted) {
            if (!opt_n) {
                emit_line(line);
            }
            if (opt_n) {
                while (explicit_prints-- > 0) {
                    emit_line(line);
                }
            } else {
                /* sed prints again for explicit 'p' even without -n */
                while (explicit_prints-- > 0) {
                    emit_line(line);
                }
            }
        }

        (void)n;
        (void)line_modified;

        if (eof) break;
    }

    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int opt_n = 0;

    /* Scripts (either via -e or the first non-flag arg) */
    const char *scripts[MAX_CMDS];
    uint64_t nscripts = 0;

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
        if (streq(a, "-n")) {
            opt_n = 1;
            continue;
        }
        if (streq(a, "-e")) {
            if (i + 1 >= argc) {
                usage();
                return 2;
            }
            if (nscripts >= (uint64_t)MAX_CMDS) {
                sys_puts("sed: too many -e scripts\n");
                return 2;
            }
            scripts[nscripts++] = argv[++i];
            continue;
        }

        usage();
        return 2;
    }

    if (nscripts == 0) {
        if (i >= argc) {
            usage();
            return 2;
        }
        scripts[nscripts++] = argv[i++];
    }

    sed_cmd_t cmds[MAX_CMDS];
    uint64_t ncmds = 0;
    for (uint64_t si = 0; si < nscripts; si++) {
        char script_buf[256];
        if (strip_outer_quotes(scripts[si], script_buf, sizeof(script_buf)) != 0) {
            sys_puts("sed: script too long\n");
            return 2;
        }

        uint64_t added = 0;
        if (parse_script(script_buf, cmds + ncmds, (uint64_t)MAX_CMDS - ncmds, &added) != 0) {
            sys_puts("sed: invalid script: ");
            sys_puts(scripts[si]);
            sys_puts("\n");
            return 2;
        }
        if (added == 0) {
            sys_puts("sed: empty script\n");
            return 2;
        }
        ncmds += added;
    }

    int nfiles = argc - i;
    if (nfiles <= 0) {
        if (sed_fd(0, cmds, ncmds, opt_n) != 0) {
            sys_puts("sed: read failed\n");
            return 1;
        }
        return 0;
    }

    int status = 0;
    for (int fi = 0; fi < nfiles; fi++) {
        const char *path = argv[i + fi];
        long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
        if (fd < 0) {
            sys_puts("sed: cannot open: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
            continue;
        }

        if (sed_fd((uint64_t)fd, cmds, ncmds, opt_n) != 0) {
            sys_puts("sed: read failed: ");
            sys_puts(path);
            sys_puts("\n");
            status = 1;
        }
        (void)sys_close((uint64_t)fd);
    }

    return status;
}
