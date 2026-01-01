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
    sys_puts("Usage: clear\n");
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc > 1) {
        if (streq(argv[1], "-h") || streq(argv[1], "--help")) {
            usage();
            return 0;
        }
        usage();
        return 2;
    }

    /* Clear entire screen + home cursor (ANSI/VT100). */
    sys_puts("\x1b[2J\x1b[H");
    return 0;
}
