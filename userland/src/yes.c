#include "syscall.h"

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

static int write_all(uint64_t fd, const void *buf, uint64_t len) {
    const char *p = (const char *)buf;
    uint64_t off = 0;
    while (off < len) {
        long rc = (long)sys_write(fd, p + off, len - off);
        if (rc < 0) {
            /* EAGAIN (11) => retry (pipes). */
            if (rc == -11) continue;
            return -1;
        }
        if (rc == 0) return -1;
        off += (uint64_t)rc;
    }
    return 0;
}

static void usage(void) {
    sys_puts("usage: yes [STRING...]\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc >= 2 && argv[1] && (streq(argv[1], "-h") || streq(argv[1], "--help"))) {
        usage();
        return 0;
    }

    if (argc <= 1) {
        static const char s[] = "y\n";
        for (;;) {
            if (write_all(1, s, sizeof(s) - 1) != 0) return 1;
        }
    }

    /* Fast path: build a single line if it fits in our buffer. */
    char line[512];
    uint64_t n = 0;
    int fits = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i] ? argv[i] : "";
        uint64_t alen = cstr_len_u64_local(a);
        if (i > 1) {
            if (n + 1 >= sizeof(line)) {
                fits = 0;
                break;
            }
            line[n++] = ' ';
        }
        if (n + alen >= sizeof(line)) {
            fits = 0;
            break;
        }
        for (uint64_t k = 0; k < alen; k++) line[n++] = a[k];
    }
    if (fits) {
        if (n + 1 >= sizeof(line)) fits = 0;
        else line[n++] = '\n';
    }

    if (fits) {
        for (;;) {
            if (write_all(1, line, n) != 0) return 1;
        }
    }

    /* Fallback: stream argv each iteration (handles arbitrarily long input). */
    for (;;) {
        for (int i = 1; i < argc; i++) {
            if (i > 1) {
                if (write_all(1, " ", 1) != 0) return 1;
            }
            const char *a = argv[i] ? argv[i] : "";
            uint64_t alen = cstr_len_u64_local(a);
            if (alen != 0) {
                if (write_all(1, a, alen) != 0) return 1;
            }
        }
        if (write_all(1, "\n", 1) != 0) return 1;
    }
}
