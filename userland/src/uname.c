#include "syscall.h"

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    linux_utsname_t u;
    uint64_t rc = sys_uname(&u);
    if ((int64_t)rc < 0) {
        sys_puts("uname: uname failed\n");
        return 1;
    }

    sys_puts(u.sysname);
    sys_puts(" ");
    sys_puts(u.release);
    sys_puts(" ");
    sys_puts(u.machine);
    sys_puts("\n");

    return 0;
}
