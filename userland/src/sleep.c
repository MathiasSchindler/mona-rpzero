#include "syscall.h"

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int cstr_eq(const char *a, const char *b) {
    uint64_t i = 0;
    for (;;) {
        char ca = a ? a[i] : '\0';
        char cb = b ? b[i] : '\0';
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
        i++;
    }
}

static int parse_sleep_seconds(const char *s, linux_timespec_t *out) {
    if (!s || !out) return -1;
    if (s[0] == '\0') return -1;

    /* Parse: DIGITS[.DIGITS] where fractional part is up to 9 digits. */
    uint64_t i = 0;
    if (!is_digit(s[i])) return -1;

    uint64_t sec = 0;
    while (is_digit(s[i])) {
        sec = sec * 10u + (uint64_t)(s[i] - '0');
        i++;
    }

    uint64_t nsec = 0;
    if (s[i] == '.') {
        i++;
        uint64_t frac_len = 0;
        while (is_digit(s[i])) {
            if (frac_len >= 9) return -1;
            nsec = nsec * 10u + (uint64_t)(s[i] - '0');
            frac_len++;
            i++;
        }
        while (frac_len < 9) {
            nsec *= 10u;
            frac_len++;
        }
    }

    if (s[i] != '\0') return -1;

    /* Clamp to int64 range the kernel expects. */
    if (sec > 0x7fffffffffffffffULL) return -1;

    out->tv_sec = (int64_t)sec;
    out->tv_nsec = (int64_t)nsec;
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc == 2 && (cstr_eq(argv[1], "-h") || cstr_eq(argv[1], "--help"))) {
        sys_puts("usage: sleep SECONDS[.FRACTION]\n");
        sys_puts("example: sleep 0.2\n");
        return 0;
    }

    linux_timespec_t req;
    req.tv_sec = 1;
    req.tv_nsec = 0;

    if (argc == 2) {
        if (parse_sleep_seconds(argv[1], &req) != 0) {
            sys_puts("sleep: bad seconds\n");
            return 2;
        }
    } else if (argc != 1) {
        sys_puts("usage: sleep SECONDS[.FRACTION]\n");
        return 2;
    }

    uint64_t rc = sys_nanosleep(&req, 0);
    if ((int64_t)rc < 0) {
        sys_puts("sleep: nanosleep failed\n");
        return 1;
    }
    return 0;
}

