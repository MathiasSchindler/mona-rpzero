#include "syscall.h"

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
    while (n + 1 < cap) {
        char c = 0;
        long rc = (long)sys_read(0, &c, 1);
        if (rc < 0) return -1;
        if (rc == 0) continue;

        if (c == '\r') c = '\n';

        if (c == '\n') {
            buf[n] = '\0';
            sys_puts("\n");
            return n;
        }

        /* Basic backspace handling */
        if (c == 0x7f || c == '\b') {
            if (n > 0) {
                n--;
                sys_puts("\b \b");
            }
            continue;
        }

        buf[n++] = c;
        (void)sys_write(1, &c, 1);
    }

    buf[n] = '\0';
    return n;
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

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)envp;

    sys_puts("mona sh (tiny)\n");
    sys_puts("type: ls | cat /hello.txt | echo hello | echo hello | cat\n");
    sys_puts("builtins: help exit\n\n");

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

        int ac = tokenize(cmd, av, (int)(sizeof(av) / sizeof(av[0])));
        if (ac == 0) return 0;

        int pp = find_pipe_pos(av);
        if (pp >= 0) {
            return run_pipeline(av, pp);
        }
        return run_command(av);
    }

    for (;;) {
        sys_puts("> ");
        int n = read_line(line, (int)sizeof(line));
        if (n < 0) {
            sys_puts("read error\n");
            continue;
        }

        int ac = tokenize(line, av, (int)(sizeof(av) / sizeof(av[0])));
        if (ac == 0) continue;

        if (streq(av[0], "exit")) {
            sys_exit_group(0);
        }

        if (streq(av[0], "help")) {
            sys_puts("programs: ls cat echo true\n");
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
