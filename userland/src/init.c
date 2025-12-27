#include "syscall.h"

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    sys_puts("[init] hello\n");

    /* Phase-7 smoke test: a simple pipeline. */
    sys_puts("[init] selftest: /bin/sh -c \"/bin/echo hello | /bin/cat\"\n");
    long pid = (long)sys_fork();
    if (pid == 0) {
        const char *const test_argv[] = {"sh", "-c", "/bin/echo hello | /bin/cat", 0};
        (void)sys_execve("/bin/sh", test_argv, 0);
        sys_puts("[init] selftest execve failed\n");
        sys_exit_group(127);
    } else if (pid > 0) {
        int status = 0;
        (void)sys_wait4(pid, &status, 0, 0);
    } else {
        sys_puts("[init] selftest fork failed\n");
    }

    /* Phase-8 smoke test: process identity. */
    sys_puts("[init] selftest: /bin/pid\n");
    pid = (long)sys_fork();
    if (pid == 0) {
        const char *const test_argv[] = {"pid", 0};
        (void)sys_execve("/bin/pid", test_argv, 0);
        sys_puts("[init] selftest execve failed\n");
        sys_exit_group(127);
    } else if (pid > 0) {
        int status = 0;
        (void)sys_wait4(pid, &status, 0, 0);
    } else {
        sys_puts("[init] selftest fork failed\n");
    }

    /* Phase-8 smoke test: uname. */
    sys_puts("[init] selftest: /bin/uname\n");
    pid = (long)sys_fork();
    if (pid == 0) {
        const char *const test_argv[] = {"uname", 0};
        (void)sys_execve("/bin/uname", test_argv, 0);
        sys_puts("[init] selftest execve failed\n");
        sys_exit_group(127);
    } else if (pid > 0) {
        int status = 0;
        (void)sys_wait4(pid, &status, 0, 0);
    } else {
        sys_puts("[init] selftest fork failed\n");
    }

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
