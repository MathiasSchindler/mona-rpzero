#include "syscall.h"

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static void usage(void) {
    sys_puts("usage: hexdump [-C] [FILE|-]\n");
    sys_puts("  Minimal implementation: forwards to 'od -C'.\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    const char *path = 0;
    int want_canonical = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) break;

        if (streq(a, "-h") || streq(a, "--help")) {
            usage();
            return 0;
        }

        if (streq(a, "-C")) {
            want_canonical = 1;
            continue;
        }

        if (!path) {
            path = a;
            continue;
        }

        usage();
        return 2;
    }

    if (!want_canonical) {
        /* Accept no flags and behave like -C (common usage in this repo). */
        want_canonical = 1;
    }

    if (path) {
        const char *const od_argv[] = {"od", "-C", path, 0};
        (void)sys_execve("/bin/od", od_argv, 0);
    } else {
        const char *const od_argv[] = {"od", "-C", 0};
        (void)sys_execve("/bin/od", od_argv, 0);
    }

    sys_puts("hexdump: exec /bin/od failed\n");
    return 127;
}
