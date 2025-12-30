#include "syscall.h"

static int is_digit(char c) {
    return c >= '0' && c <= '9';
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

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc == 2 && (argv[1][0] == '-' && argv[1][1] == 'h')) {
        sys_puts("usage: sleep SECONDS\n");
        return 0;
    }

    uint64_t secs = 1;
    if (argc >= 2) {
        if (parse_u64(argv[1], &secs) != 0) {
            sys_puts("sleep: bad seconds\n");
            return 2;
        }
    }

    linux_timespec_t req;
    req.tv_sec = (int64_t)secs;
    req.tv_nsec = 0;

    uint64_t rc = sys_nanosleep(&req, 0);
    if ((int64_t)rc < 0) {
        sys_puts("sleep: nanosleep failed\n");
        return 1;
    }
    return 0;
}

