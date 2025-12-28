#include "syscall.h"

static int tokenize(char *buf, char **argv, int max_argv);
static int find_pipe_pos(char **av);
static int run_command(char **av);
static int run_pipeline(char **av, int pipe_pos);

static int is_space(char c);

static void print_prompt(void);

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

static int read_line(char *buf, int cap) {
    int n = 0;

    /* History browsing state (local to a single line edit). */
    int hist_pos = 0; /* 0 = not browsing; 1 = newest; 2 = one older; ... */
    char saved[LINE_MAX];
    saved[0] = '\0';

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
            continue;
        }

        buf[n++] = c;
        (void)sys_write(1, &c, 1);
        /* Editing cancels history browsing. */
        hist_pos = 0;
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
