#include "syscall.h"

static int run_test(const char *label, const char *path, const char *const argv[]) {
    sys_puts("[kinit] selftest: ");
    sys_puts(label);
    sys_puts("\n");

    long pid = (long)sys_fork();
    if (pid == 0) {
        (void)sys_execve(path, argv, 0);
        sys_puts("[kinit] selftest execve failed\n");
        sys_exit_group(127);
    } else if (pid < 0) {
        sys_puts("[kinit] selftest fork failed\n");
        return 1;
    }

    int status = 0;
    uint64_t rc = sys_wait4((uint64_t)pid, &status, 0, 0);
    if ((int64_t)rc < 0) {
        sys_puts("[kinit] selftest wait4 failed\n");
        return 1;
    }

    int exit_code = (status >> 8) & 0xff;
    if (exit_code != 0) {
        sys_puts("[kinit] selftest FAILED: ");
        sys_puts(label);
        sys_puts("\n");
        return 1;
    }

    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    sys_puts("[kinit] running selftests\n");

    int failed = 0;

    /* Phase-7 smoke test: a simple pipeline. */
    {
        const char *const test_argv[] = {"sh", "-c", "/bin/echo hello | /bin/cat", 0};
        failed |= run_test("/bin/sh -c \"/bin/echo hello | /bin/cat\"", "/bin/sh", test_argv);
    }

    /* Phase-8 smoke test: process identity. */
    {
        const char *const test_argv[] = {"pid", 0};
        failed |= run_test("/bin/pid", "/bin/pid", test_argv);
    }

    /* Phase-8 smoke test: uname. */
    {
        const char *const test_argv[] = {"uname", 0};
        failed |= run_test("/bin/uname", "/bin/uname", test_argv);
    }

    /* Tool smoke test: pwd. */
    {
        const char *const test_argv[] = {"pwd", 0};
        failed |= run_test("/bin/pwd", "/bin/pwd", test_argv);
    }

    /* Tool smoke test: mkdir (-p) + ls shows it. */
    {
        const char *const test_argv[] = {"sh", "-c", "mkdir -p /tmp/a; ls", 0};
        failed |= run_test("/bin/sh -c \"mkdir -p /tmp/a; ls\"", "/bin/sh", test_argv);
    }

    /* Tool smoke test: touch + rm (overlay regular files). */
    {
        const char *const test_argv[] = {"sh", "-c", "mkdir -p /tmp; touch /tmp/hi; ls /tmp; rm /tmp/hi; ls /tmp", 0};
        failed |= run_test("/bin/sh -c \"mkdir -p /tmp; touch /tmp/hi; ls /tmp; rm /tmp/hi; ls /tmp\"", "/bin/sh", test_argv);
    }

    /* Tool smoke test: rmdir (overlay directories). */
    {
        const char *const test_argv[] = {"sh", "-c", "mkdir -p /tmp/dir; ls /tmp; rmdir /tmp/dir; ls /tmp", 0};
        failed |= run_test("/bin/sh -c \"mkdir -p /tmp/dir; ls /tmp; rmdir /tmp/dir; ls /tmp\"", "/bin/sh", test_argv);
    }

    /* Tool smoke test: rm -r removes non-empty directory trees. */
    {
        const char *const test_argv[] = {"sh", "-c", "mkdir -p /rmrt/a; touch /rmrt/a/x; touch /rmrt/y; ls; rm -r /rmrt; ls", 0};
        failed |= run_test("/bin/sh -c \"... rm -r /rmrt ...\"", "/bin/sh", test_argv);
    }

    /* Tool smoke test: wc over a known file. */
    {
        const char *const test_argv[] = {"wc", "/uniq.txt", 0};
        failed |= run_test("/bin/wc /uniq.txt", "/bin/wc", test_argv);
    }

    /* Tool smoke test: uniq -c over a known file. */
    {
        const char *const test_argv[] = {"uniq", "-c", "/uniq.txt", 0};
        failed |= run_test("/bin/uniq -c /uniq.txt", "/bin/uniq", test_argv);
    }

    /* Tool smoke test: grep over a known file. */
    {
        const char *const test_argv[] = {"grep", "y", "/uniq.txt", 0};
        failed |= run_test("/bin/grep y /uniq.txt", "/bin/grep", test_argv);
    }

    /* Tool smoke test: pipeline into wc. */
    {
        const char *const test_argv[] = {"sh", "-c", "seq 1 10 | wc -l", 0};
        failed |= run_test("/bin/sh -c \"seq 1 10 | wc -l\"", "/bin/sh", test_argv);
    }

    {
        const char *const test_argv[] = {"sh", "-c", "cd /home; pwd", 0};
        failed |= run_test("/bin/sh -c \"cd /home; pwd\"", "/bin/sh", test_argv);
    }

    /* Phase-8 smoke test: brk. */
    {
        const char *const test_argv[] = {"brk", 0};
        failed |= run_test("/bin/brk", "/bin/brk", test_argv);
    }

    /* Phase-8 smoke test: mmap/munmap (anonymous). */
    {
        const char *const test_argv[] = {"mmap", 0};
        failed |= run_test("/bin/mmap", "/bin/mmap", test_argv);
    }

    {
        const char *const test_argv[] = {"cwd", 0};
        failed |= run_test("/bin/cwd", "/bin/cwd", test_argv);
    }

    {
        const char *const test_argv[] = {"tty", 0};
        failed |= run_test("/bin/tty", "/bin/tty", test_argv);
    }

    {
        const char *const test_argv[] = {"sleep", 0};
        failed |= run_test("/bin/sleep", "/bin/sleep", test_argv);
    }

    {
        const char *const test_argv[] = {"compat", 0};
        failed |= run_test("/bin/compat", "/bin/compat", test_argv);
    }

    if (failed) {
        sys_puts("[kinit] selftests FAILED\n");
        sys_exit_group(1);
    }

    sys_puts("[kinit] selftests OK\n");
    sys_exit_group(0);
    return 0;
}
