#include "syscall.h"

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    sys_puts("[init] starting shell\n");

    /* Next stage: run the tiny shell. */
    const char *const sh_argv[] = {"sh", 0};
    uint64_t rc = sys_execve("/bin/sh", sh_argv, 0);

    /* If that fails, fall back to ls (no argv/envp). */
    if ((int64_t)rc < 0) {
        (void)sys_execve("/bin/ls", 0, 0);
    }

    sys_puts("[init] execve failed\n");
    (void)rc;
    return 1;
}
