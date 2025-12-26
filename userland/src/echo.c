#include "syscall.h"

static void putc1(char c) {
    (void)sys_write(1, &c, 1);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc <= 1) {
        putc1('\n');
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (i > 1) putc1(' ');
        sys_puts(argv[i]);
    }
    putc1('\n');
    return 0;
}
