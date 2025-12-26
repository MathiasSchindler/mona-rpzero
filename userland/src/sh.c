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

static int run_command(char **av) {
    if (!av || !av[0]) return -1;

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

    long pid = (long)sys_fork();
    if (pid < 0) {
        sys_puts("fork failed\n");
        return -1;
    }

    if (pid == 0) {
        (void)sys_execve(path, (const char *const *)av, 0);
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

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)envp;

    sys_puts("mona sh (tiny)\n");
    sys_puts("type: ls | cat /hello.txt | echo hello | exit\n\n");

    char line[256];
    char *av[16];

    /* Non-interactive mode: `sh -c "cmd ..."`
     * Important: the current kernel supports only a minimal 2-process model,
     * so in `-c` mode we run the command via direct `execve` (no extra fork).
     */
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

        char path[64];
        if (av[0][0] == '/') {
            int k = 0;
            while (av[0][k] && k + 1 < (int)sizeof(path)) {
                path[k] = av[0][k];
                k++;
            }
            path[k] = '\0';
        } else {
            const char *pre = "/bin/";
            int k = 0;
            while (pre[k] && k + 1 < (int)sizeof(path)) {
                path[k] = pre[k];
                k++;
            }
            int j = 0;
            while (av[0][j] && k + 1 < (int)sizeof(path)) {
                path[k++] = av[0][j++];
            }
            path[k] = '\0';
        }

        (void)sys_execve(path, (const char *const *)av, 0);
        sys_puts("execve failed\n");
        return 127;
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
            sys_puts("builtins: help exit\n");
            sys_puts("programs:  ls cat echo true\n");
            continue;
        }

        (void)run_command(av);
    }
}
