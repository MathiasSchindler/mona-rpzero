#include "syscall.h"

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - ('a' - 'A'));
    return c;
}

static int streq_ci(const char *a, const char *b) {
    if (!a || !b) return 0;
    for (;;) {
        char ca = to_upper(*a);
        char cb = to_upper(*b);
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
        a++;
        b++;
    }
}

static int parse_u64(const char *s, uint64_t *out) {
    if (!s || !out) return -1;
    if (s[0] == '\0') return -1;

    uint64_t v = 0;
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        if (!is_digit(s[i])) return -1;
        v = v * 10u + (uint64_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static int parse_signal(const char *s, uint64_t *out_sig) {
    if (!s || !out_sig) return -1;
    if (s[0] == '\0') return -1;

    /* Numeric form first (e.g. 9, 15, 0). */
    uint64_t v = 0;
    if (parse_u64(s, &v) == 0) {
        *out_sig = v;
        return 0;
    }

    /* Named signals (minimal set). Allow optional SIG prefix. */
    const char *name = s;
    if ((to_upper(name[0]) == 'S') && (to_upper(name[1]) == 'I') && (to_upper(name[2]) == 'G')) {
        name += 3;
    }

    if (streq_ci(name, "KILL")) {
        *out_sig = 9;
        return 0;
    }
    if (streq_ci(name, "TERM")) {
        *out_sig = 15;
        return 0;
    }
    return -1;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc < 2) {
        sys_puts("usage: kill [-SIGNAL] PID\n");
        sys_puts("  SIGNAL: 0, 9, 15, KILL, TERM (optionally SIG*)\n");
        return 1;
    }

    uint64_t sig = 15; /* default SIGTERM */
    const char *pid_s = 0;

    if (argv[1] && argv[1][0] == '-') {
        if (argc < 3) {
            sys_puts("usage: kill [-SIGNAL] PID\n");
            return 1;
        }
        if (parse_signal(argv[1] + 1, &sig) != 0) {
            sys_puts("kill: bad signal\n");
            return 1;
        }
        pid_s = argv[2];
    } else {
        pid_s = argv[1];
    }

    uint64_t pid = 0;
    if (parse_u64(pid_s, &pid) != 0 || pid == 0) {
        sys_puts("kill: bad pid\n");
        return 1;
    }

    long rc = (long)sys_kill((int64_t)pid, sig);
    if (rc < 0) {
        sys_puts("kill: syscall failed\n");
        return 1;
    }

    return 0;
}
