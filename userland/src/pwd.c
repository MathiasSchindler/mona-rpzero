#include "syscall.h"

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    char buf[256];
    uint64_t rc = sys_getcwd(buf, sizeof(buf));
    if ((int64_t)rc < 0) {
        sys_puts("pwd: getcwd failed\n");
        return 1;
    }

    sys_puts(buf);
    sys_puts("\n");
    return 0;
}
