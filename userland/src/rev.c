#include "syscall.h"

#define AT_FDCWD ((long)-100)

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int write_all(uint64_t fd, const char *buf, uint64_t len) {
    uint64_t off = 0;
    while (off < len) {
        long rc = (long)sys_write(fd, buf + off, len - off);
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
    sys_puts("usage: rev [FILE...]\n");
}

enum {
    LINE_MAX = 4096,
};

static int rev_fd(uint64_t fd) {
    char line[LINE_MAX];
    uint64_t n = 0;
    int truncated = 0;

    for (;;) {
        char c = 0;
        long rc = (long)sys_read(fd, &c, 1);
        if (rc == 0) {
            /* EOF: flush pending data (no trailing newline) */
            if (n > 0) {
                for (uint64_t i = 0; i < n / 2; i++) {
                    char t = line[i];
                    line[i] = line[n - 1 - i];
                    line[n - 1 - i] = t;
                }
                if (write_all(1, line, n) != 0) return -1;
            }
            return 0;
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
            /* Reverse only the part we kept. */
            for (uint64_t i = 0; i < n / 2; i++) {
                char t = line[i];
                line[i] = line[n - 1 - i];
                line[n - 1 - i] = t;
            }
            if (write_all(1, line, n) != 0) return -1;
            if (write_all(1, "\n", 1) != 0) return -1;

            n = 0;
            truncated = 0;
            continue;
        }

        if (!truncated) {
            if (n + 1 < (uint64_t)sizeof(line)) {
                line[n++] = c;
            } else {
                /* Too long: keep consuming until newline. */
                truncated = 1;
            }
        }
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc >= 2 && argv[1] && (streq(argv[1], "-h") || streq(argv[1], "--help"))) {
        usage();
        return 0;
    }

    if (argc < 2) {
        int rc = rev_fd(0);
        if (rc != 0) {
            sys_puts("rev: read failed\n");
            return 1;
        }
        return 0;
    }

    int rc_any = 0;
    for (int i = 1; i < argc; i++) {
        const char *path = argv[i];
        if (!path || path[0] == '\0') continue;

        long fd = (long)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
        if (fd < 0) {
            sys_puts("rev: openat failed\n");
            rc_any = 1;
            continue;
        }

        int r = rev_fd((uint64_t)fd);
        (void)sys_close((uint64_t)fd);
        if (r != 0) {
            sys_puts("rev: read failed\n");
            rc_any = 1;
        }
    }

    return rc_any;
}
