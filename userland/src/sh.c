#include "syscall.h"

static int tokenize(char *buf, char **argv, int max_argv);
static int find_pipe_pos(char **av);
static int run_command(char **av);
static int run_pipeline(char **av, int pipe_pos);

static int is_space(char c);

static void print_prompt(void);

typedef struct {
    unsigned long long d_ino;
    long long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
} linux_dirent64_t;

static int str_eq(const char *a, const char *b);
static int str_starts_with(const char *s, const char *pre);
static int join_path(char *out, uint64_t cap, const char *dir, const char *name);
static int is_dir_path(const char *path);
static int find_token_start(const char *buf, int n);
static int is_first_word_segment(const char *buf, int tok_start);
static int do_tab_complete(char *buf, int *n_inout, int cap, int show_list);

enum {
    LINE_MAX = 256,
    HIST_MAX = 16,
};

static char g_hist[HIST_MAX][LINE_MAX];
static int g_hist_count = 0;
static int g_hist_next = 0;

static void tty_putc(char c) {
    (void)sys_write(1, &c, 1);
}

static void tty_erase_chars(int n) {
    /* erase the last n characters from the current cursor position */
    for (int i = 0; i < n; i++) {
        tty_putc('\b');
        tty_putc(' ');
        tty_putc('\b');
    }
}

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static int is_blank_line(const char *s) {
    if (!s) return 1;
    for (int i = 0; s[i]; i++) {
        if (!is_space(s[i])) return 0;
    }
    return 1;
}

static const char *hist_get_from_end(int back) {
    /* back=0 => newest entry */
    if (back < 0 || back >= g_hist_count) return 0;
    int newest = g_hist_next - 1;
    if (newest < 0) newest += HIST_MAX;
    int idx = newest - back;
    while (idx < 0) idx += HIST_MAX;
    return g_hist[idx];
}

static void hist_add(const char *line) {
    if (!line || line[0] == '\0') return;
    if (is_blank_line(line)) return;

    /* Avoid adding consecutive duplicates. */
    if (g_hist_count > 0) {
        const char *last = hist_get_from_end(0);
        if (last) {
            int same = 1;
            for (int i = 0;; i++) {
                if (line[i] != last[i]) {
                    same = 0;
                    break;
                }
                if (line[i] == '\0') break;
            }
            if (same) return;
        }
    }

    uint64_t n = cstr_len_u64_local(line);
    if (n + 1 > (uint64_t)LINE_MAX) n = (uint64_t)LINE_MAX - 1;
    for (uint64_t i = 0; i < n; i++) g_hist[g_hist_next][i] = line[i];
    g_hist[g_hist_next][n] = '\0';

    g_hist_next = (g_hist_next + 1) % HIST_MAX;
    if (g_hist_count < HIST_MAX) g_hist_count++;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int str_starts_with(const char *s, const char *pre) {
    if (!s || !pre) return 0;
    for (uint64_t i = 0; pre[i] != '\0'; i++) {
        if (s[i] != pre[i]) return 0;
    }
    return 1;
}

static int join_path(char *out, uint64_t cap, const char *dir, const char *name) {
    if (!out || cap == 0 || !dir || !name) return -1;
    uint64_t dlen = cstr_len_u64_local(dir);
    uint64_t nlen = cstr_len_u64_local(name);
    if (dlen == 0) return -1;

    int dir_is_root = (dlen == 1 && dir[0] == '/');
    uint64_t need = dlen + (dir_is_root ? 0u : 1u) + nlen + 1u;
    if (need > cap) return -1;

    uint64_t o = 0;
    for (uint64_t i = 0; i < dlen; i++) out[o++] = dir[i];
    if (!dir_is_root) out[o++] = '/';
    for (uint64_t i = 0; i < nlen; i++) out[o++] = name[i];
    out[o] = '\0';
    return 0;
}

static int is_dir_path(const char *path) {
    linux_stat_t st;
    long rc = (long)sys_newfstatat((uint64_t)-100, path, &st, 0);
    if (rc < 0) return 0;
    return ((st.st_mode & 0170000u) == 0040000u);
}

static int find_token_start(const char *buf, int n) {
    if (!buf || n <= 0) return 0;
    int i = n - 1;
    while (i >= 0) {
        char c = buf[i];
        if (is_space(c) || c == '|' || c == ';') break;
        i--;
    }
    return i + 1;
}

static int is_first_word_segment(const char *buf, int tok_start) {
    if (!buf) return 1;
    int j = tok_start - 1;
    while (j >= 0 && is_space(buf[j])) j--;
    if (j < 0) return 1;
    if (buf[j] == '|' || buf[j] == ';') return 1;
    return 0;
}

static void print_candidates(const char names[][64], int count) {
    sys_puts("\n");
    for (int i = 0; i < count; i++) {
        sys_puts(names[i]);
        sys_puts("\n");
    }
}

static int is_builtin_name(const char *s) {
    if (!s) return 0;
    static const char *builtins[] = {"help", "exit", "cd", "shutdown", "poweroff", 0};
    for (int i = 0; builtins[i]; i++) {
        if (str_eq(s, builtins[i])) return 1;
    }
    return 0;
}

static int do_tab_complete(char *buf, int *n_inout, int cap, int show_list) {
    enum {
        MAX_CANDS = 64,
        NAME_MAX = 64,
    };

    if (!buf || !n_inout || cap <= 0) return 0;
    int n = *n_inout;
    if (n < 0) n = 0;

    int tok_start = find_token_start(buf, n);
    if (tok_start < 0) tok_start = 0;
    if (tok_start > n) tok_start = n;

    char token[128];
    int tok_len = n - tok_start;
    if (tok_len < 0) tok_len = 0;
    if (tok_len + 1 > (int)sizeof(token)) tok_len = (int)sizeof(token) - 1;
    for (int i = 0; i < tok_len; i++) token[i] = buf[tok_start + i];
    token[tok_len] = '\0';

    /* Determine completion domain. */
    int has_slash = 0;
    for (int i = 0; token[i] != '\0'; i++) {
        if (token[i] == '/') {
            has_slash = 1;
            break;
        }
    }

    char dir[256];
    char prefix[128];
    dir[0] = '\0';
    prefix[0] = '\0';

    if (has_slash) {
        int last = -1;
        for (int i = 0; token[i] != '\0'; i++) {
            if (token[i] == '/') last = i;
        }

        if (last == 0) {
            dir[0] = '/';
            dir[1] = '\0';
        } else if (last > 0) {
            int dl = last;
            if (dl + 1 > (int)sizeof(dir)) dl = (int)sizeof(dir) - 1;
            for (int i = 0; i < dl; i++) dir[i] = token[i];
            dir[dl] = '\0';
        } else {
            dir[0] = '/';
            dir[1] = '\0';
        }

        int pl = 0;
        for (int i = last + 1; token[i] != '\0' && pl + 1 < (int)sizeof(prefix); i++) {
            prefix[pl++] = token[i];
        }
        prefix[pl] = '\0';
    } else {
        int first = is_first_word_segment(buf, tok_start);
        if (first) {
            /* Command completion: builtins + /bin. */
            dir[0] = '\0';
            int pl = 0;
            for (int i = 0; token[i] != '\0' && pl + 1 < (int)sizeof(prefix); i++) prefix[pl++] = token[i];
            prefix[pl] = '\0';
        } else {
            /* Filename completion: current working directory. */
            uint64_t rc = sys_getcwd(dir, sizeof(dir));
            if ((int64_t)rc < 0) {
                dir[0] = '/';
                dir[1] = '\0';
            }
            int pl = 0;
            for (int i = 0; token[i] != '\0' && pl + 1 < (int)sizeof(prefix); i++) prefix[pl++] = token[i];
            prefix[pl] = '\0';
        }
    }

    char cands[MAX_CANDS][NAME_MAX];
    int cand_count = 0;

    int cmd_pos = (!has_slash && is_first_word_segment(buf, tok_start));
    const char *base_dir = 0;
    if (has_slash) {
        base_dir = dir;
    } else {
        base_dir = cmd_pos ? "/bin" : dir;
    }

    int want_hidden = (prefix[0] == '.');

    /* Builtins for command position. */
    if (cmd_pos) {
        static const char *builtins[] = {"help", "exit", "cd", "shutdown", "poweroff", 0};
        for (int i = 0; builtins[i]; i++) {
            if (!str_starts_with(builtins[i], prefix)) continue;
            if (cand_count >= MAX_CANDS) break;
            int k = 0;
            while (builtins[i][k] && k + 1 < NAME_MAX) {
                cands[cand_count][k] = builtins[i][k];
                k++;
            }
            cands[cand_count][k] = '\0';
            cand_count++;
        }
    }

    /* Directory listing candidates */
    {
        long fd = (long)sys_openat((uint64_t)-100, base_dir, 0, 0);
        if (fd >= 0) {
            char dbuf[512];
            for (;;) {
                long dn = (long)sys_getdents64((uint64_t)fd, dbuf, sizeof(dbuf));
                if (dn < 0) break;
                if (dn == 0) break;

                unsigned long off = 0;
                while (off < (unsigned long)dn) {
                    linux_dirent64_t *d = (linux_dirent64_t *)(dbuf + off);
                    if (d->d_reclen == 0) break;

                    const char *name = d->d_name;
                    if (!want_hidden && name[0] == '.') {
                        off += d->d_reclen;
                        continue;
                    }
                    if (str_eq(name, ".") || str_eq(name, "..")) {
                        off += d->d_reclen;
                        continue;
                    }
                    if (!str_starts_with(name, prefix)) {
                        off += d->d_reclen;
                        continue;
                    }

                    /* Avoid duplicates (builtins vs /bin). */
                    int dup = 0;
                    for (int i = 0; i < cand_count; i++) {
                        if (str_eq(cands[i], name)) {
                            dup = 1;
                            break;
                        }
                    }
                    if (!dup && cand_count < MAX_CANDS) {
                        int k = 0;
                        while (name[k] && k + 1 < NAME_MAX) {
                            cands[cand_count][k] = name[k];
                            k++;
                        }
                        cands[cand_count][k] = '\0';
                        cand_count++;
                    }

                    off += d->d_reclen;
                }
            }
            (void)sys_close((uint64_t)fd);
        }
    }

    if (cand_count == 0) {
        return 0;
    }

    /* Compute longest common prefix among candidates. */
    uint64_t lcp = cstr_len_u64_local(cands[0]);
    for (int i = 1; i < cand_count; i++) {
        uint64_t j = 0;
        while (j < lcp && cands[0][j] && cands[i][j] && cands[0][j] == cands[i][j]) {
            j++;
        }
        lcp = j;
    }

    uint64_t pre_len = cstr_len_u64_local(prefix);
    uint64_t extend = (lcp > pre_len) ? (lcp - pre_len) : 0;

    if (extend > 0) {
        /* Append the extension characters into the current line buffer. */
        uint64_t inserted = 0;
        for (uint64_t i = 0; i < extend; i++) {
            if (n + 1 >= cap) break;
            char ch = cands[0][pre_len + i];
            buf[n++] = ch;
            tty_putc(ch);
            inserted++;
        }
        buf[n] = '\0';
        *n_inout = n;

        /* If this completion resolved to a unique full match, append '/' for dirs or space otherwise. */
        if (cand_count == 1 && inserted == extend) {
            int is_dir = 0;
            if (!(cmd_pos && is_builtin_name(cands[0]))) {
                char full[512];
                if (join_path(full, sizeof(full), base_dir, cands[0]) == 0 && is_dir_path(full)) {
                    is_dir = 1;
                }
            }

            if (is_dir) {
                if (n + 1 < cap && (n == 0 || buf[n - 1] != '/')) {
                    buf[n++] = '/';
                    tty_putc('/');
                    buf[n] = '\0';
                    *n_inout = n;
                }
            } else {
                if (n + 1 < cap && (n == 0 || buf[n - 1] != ' ')) {
                    buf[n++] = ' ';
                    tty_putc(' ');
                    buf[n] = '\0';
                    *n_inout = n;
                }
            }
            return 0;
        }

        return 1;
    }

    if (cand_count == 1) {
        /* Full match already typed; add '/' for dirs or space after completion. */
        int is_dir = 0;
        if (!(cmd_pos && is_builtin_name(cands[0]))) {
            char full[512];
            if (join_path(full, sizeof(full), base_dir, cands[0]) == 0 && is_dir_path(full)) {
                is_dir = 1;
            }
        }

        if (is_dir) {
            if (n + 1 < cap && (n == 0 || buf[n - 1] != '/')) {
                buf[n++] = '/';
                tty_putc('/');
                buf[n] = '\0';
            }
        } else {
            if (n + 1 < cap && (n == 0 || buf[n - 1] != ' ')) {
                buf[n++] = ' ';
                tty_putc(' ');
                buf[n] = '\0';
            }
        }
        *n_inout = n;
        return 0;
    }

    if (show_list) {
        print_candidates(cands, cand_count);
        print_prompt();
        (void)sys_write(1, buf, (uint64_t)n);
    }

    return 1;
}

static int read_line(char *buf, int cap) {
    int n = 0;

    /* History browsing state (local to a single line edit). */
    int hist_pos = 0; /* 0 = not browsing; 1 = newest; 2 = one older; ... */
    char saved[LINE_MAX];
    saved[0] = '\0';

    /* TAB completion state: first TAB extends common prefix, second TAB lists candidates. */
    int tab_pending = 0;

    while (n + 1 < cap) {
        char c = 0;
        long rc = (long)sys_read(0, &c, 1);
        if (rc < 0) return -1;
        if (rc == 0) return -2;

        if (c == '\r') c = '\n';

        if (c == '\n') {
            buf[n] = '\0';
            sys_puts("\n");
            return n;
        }

        if (c == '\t') {
            int ambiguous = do_tab_complete(buf, &n, cap, tab_pending);
            tab_pending = ambiguous ? 1 : 0;
            /* Any completion action cancels history browsing. */
            hist_pos = 0;
            continue;
        }

        /* Arrow keys: typically ESC [ A/B. */
        if (c == 0x1b) {
            char c2 = 0;
            char c3 = 0;
            long r2 = (long)sys_read(0, &c2, 1);
            if (r2 <= 0) continue;
            if (c2 != '[' && c2 != 'O') continue;
            long r3 = (long)sys_read(0, &c3, 1);
            if (r3 <= 0) continue;

            if (c3 == 'A') {
                /* Up */
                if (g_hist_count == 0) continue;
                if (hist_pos == 0) {
                    /* Save current line once when starting history browse. */
                    int sn = n;
                    if (sn + 1 > (int)sizeof(saved)) sn = (int)sizeof(saved) - 1;
                    for (int i = 0; i < sn; i++) saved[i] = buf[i];
                    saved[sn] = '\0';
                }
                if (hist_pos < g_hist_count) hist_pos++;
                const char *h = hist_get_from_end(hist_pos - 1);
                if (!h) continue;

                tty_erase_chars(n);
                uint64_t hn = cstr_len_u64_local(h);
                if (hn + 1 > (uint64_t)cap) hn = (uint64_t)cap - 1;
                for (uint64_t i = 0; i < hn; i++) buf[i] = h[i];
                buf[hn] = '\0';
                n = (int)hn;
                (void)sys_write(1, buf, (uint64_t)n);
                tab_pending = 0;
                continue;
            }

            if (c3 == 'B') {
                /* Down */
                if (hist_pos == 0) continue;
                hist_pos--;

                const char *h = 0;
                if (hist_pos > 0) {
                    h = hist_get_from_end(hist_pos - 1);
                } else {
                    h = saved;
                }
                if (!h) h = "";

                tty_erase_chars(n);
                uint64_t hn = cstr_len_u64_local(h);
                if (hn + 1 > (uint64_t)cap) hn = (uint64_t)cap - 1;
                for (uint64_t i = 0; i < hn; i++) buf[i] = h[i];
                buf[hn] = '\0';
                n = (int)hn;
                (void)sys_write(1, buf, (uint64_t)n);
                tab_pending = 0;
                continue;
            }

            continue;
        }

        /* Basic backspace handling */
        if (c == 0x7f || c == '\b') {
            if (n > 0) {
                n--;
                sys_puts("\b \b");
            }
            /* Editing cancels history browsing. */
            hist_pos = 0;
            tab_pending = 0;
            continue;
        }

        buf[n++] = c;
        (void)sys_write(1, &c, 1);
        /* Editing cancels history browsing. */
        hist_pos = 0;
        tab_pending = 0;
    }

    buf[n] = '\0';
    return n;
}

static int run_script(char *cmd, char **av, int av_cap) {
    int last_status = 0;

    char *p = cmd;
    for (;;) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;

        char *seg = p;
        while (*p && *p != ';') p++;
        if (*p == ';') {
            *p = '\0';
            p++;
        }

        int ac = tokenize(seg, av, av_cap);
        if (ac == 0) continue;

        int pp = find_pipe_pos(av);
        if (pp >= 0) {
            last_status = run_pipeline(av, pp);
        } else {
            last_status = run_command(av);
        }
    }

    return last_status;
}

static int tokenize(char *buf, char **argv, int max_argv) {
    int argc = 0;

    char *p = buf;
    while (*p) {
        while (*p && is_space(*p)) p++;
        if (!*p) break;

        if (argc + 1 >= max_argv) break;
        argv[argc++] = p;

        while (*p && !is_space(*p)) p++;
        if (*p) {
            *p = '\0';
            p++;
        }
    }

    argv[argc] = 0;
    return argc;
}

static void exec_argv(char **av) {
    char path[64];
    if (av[0][0] == '/') {
        /* absolute path */
        int i = 0;
        while (av[0][i] && i + 1 < (int)sizeof(path)) {
            path[i] = av[0][i];
            i++;
        }
        path[i] = '\0';
    } else {
        /* /bin/<cmd> */
        const char *pre = "/bin/";
        int i = 0;
        while (pre[i] && i + 1 < (int)sizeof(path)) {
            path[i] = pre[i];
            i++;
        }
        int j = 0;
        while (av[0][j] && i + 1 < (int)sizeof(path)) {
            path[i++] = av[0][j++];
        }
        path[i] = '\0';
    }

    (void)sys_execve(path, (const char *const *)av, 0);
}

static int find_pipe_pos(char **av) {
    if (!av) return -1;
    for (int i = 0; av[i]; i++) {
        if (streq(av[i], "|")) {
            return i;
        }
    }
    return -1;
}

static int run_command(char **av) {
    if (!av || !av[0]) return -1;

    if (streq(av[0], "cd")) {
        const char *path = av[1] ? av[1] : "/";
        long rc = (long)sys_chdir(path);
        if (rc < 0) {
            sys_puts("cd failed\n");
            return -1;
        }
        return 0;
    }

    long pid = (long)sys_fork();
    if (pid < 0) {
        sys_puts("fork failed\n");
        return -1;
    }

    if (pid == 0) {
        exec_argv(av);
        sys_puts("execve failed\n");
        sys_exit_group(127);
    }

    int status = 0;
    long w = (long)sys_wait4(pid, &status, 0, 0);
    if (w < 0) {
        sys_puts("wait4 failed\n");
        return -1;
    }

    return status;
}

static int run_pipeline(char **av, int pipe_pos) {
    if (!av || !av[0]) return -1;
    if (pipe_pos <= 0) return -1;
    if (!av[pipe_pos + 1]) {
        sys_puts("syntax error near '|': missing rhs\n");
        return -1;
    }

    av[pipe_pos] = 0;
    char **left = av;
    char **right = &av[pipe_pos + 1];

    int pfds[2];
    long prc = (long)sys_pipe2(pfds, 0);
    if (prc < 0) {
        sys_puts("pipe2 failed\n");
        return -1;
    }

    long lpid = (long)sys_fork();
    if (lpid < 0) {
        sys_puts("fork failed\n");
        (void)sys_close((uint64_t)pfds[0]);
        (void)sys_close((uint64_t)pfds[1]);
        return -1;
    }
    if (lpid == 0) {
        (void)sys_dup2((uint64_t)pfds[1], 1);
        (void)sys_close((uint64_t)pfds[0]);
        (void)sys_close((uint64_t)pfds[1]);
        exec_argv(left);
        sys_puts("execve failed\n");
        sys_exit_group(127);
    }

    long rpid = (long)sys_fork();
    if (rpid < 0) {
        sys_puts("fork failed\n");
        (void)sys_close((uint64_t)pfds[0]);
        (void)sys_close((uint64_t)pfds[1]);
        return -1;
    }
    if (rpid == 0) {
        (void)sys_dup2((uint64_t)pfds[0], 0);
        (void)sys_close((uint64_t)pfds[0]);
        (void)sys_close((uint64_t)pfds[1]);
        exec_argv(right);
        sys_puts("execve failed\n");
        sys_exit_group(127);
    }

    (void)sys_close((uint64_t)pfds[0]);
    (void)sys_close((uint64_t)pfds[1]);

    int st_l = 0;
    int st_r = 0;
    (void)sys_wait4(lpid, &st_l, 0, 0);
    (void)sys_wait4(rpid, &st_r, 0, 0);
    return st_r;
}

static void print_prompt(void) {
    char cwd[256];
    uint64_t rc = sys_getcwd(cwd, sizeof(cwd));
    if ((int64_t)rc < 0) {
        sys_puts("? > ");
        return;
    }

    sys_puts(cwd);
    sys_puts(" > ");
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)envp;

    char line[256];
    char *av[16];

    /* Non-interactive mode: `sh -c "cmd ..."` */
    if (argc >= 3 && argv && streq(argv[1], "-c")) {
        char cmd[256];
        int i = 0;
        while (argv[2][i] && i + 1 < (int)sizeof(cmd)) {
            cmd[i] = argv[2][i];
            i++;
        }
        cmd[i] = '\0';

        return run_script(cmd, av, (int)(sizeof(av) / sizeof(av[0])));
    }

    for (;;) {
        print_prompt();
        int n = read_line(line, (int)sizeof(line));
        if (n == -2) {
            sys_puts("\n");
            sys_exit_group(0);
        }
        if (n < 0) {
            sys_puts("read error\n");
            continue;
        }

        /* Add to history in interactive mode. */
        if (n > 0) {
            hist_add(line);
        }

        int ac = tokenize(line, av, (int)(sizeof(av) / sizeof(av[0])));
        if (ac == 0) continue;

        if (streq(av[0], "exit")) {
            sys_exit_group(0);
        }

        if (streq(av[0], "shutdown") || streq(av[0], "poweroff")) {
            const uint64_t LINUX_REBOOT_MAGIC1 = 0xfee1deadull;
            const uint64_t LINUX_REBOOT_MAGIC2 = 0x28121969ull;
            const uint64_t LINUX_REBOOT_CMD_POWER_OFF = 0x4321fedcull;
            uint64_t rc = sys_reboot(LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, 0);
            (void)rc;
            sys_exit_group(0);
        }

        if (streq(av[0], "help")) {
            sys_puts("builtins: help exit cd shutdown poweroff\n");
            sys_puts("pipeline: cmd1 | cmd2 (single pipe)\n");
            continue;
        }

        int pp = find_pipe_pos(av);
        if (pp >= 0) {
            (void)run_pipeline(av, pp);
        } else {
            (void)run_command(av);
        }
    }
}
