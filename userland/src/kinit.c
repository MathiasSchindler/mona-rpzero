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

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static void u64_to_dec(uint64_t v, char *out, uint64_t cap) {
    if (!out || cap == 0) return;
    if (cap == 1) {
        out[0] = '\0';
        return;
    }

    if (v == 0) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }

    char tmp[32];
    uint64_t t = 0;
    while (v != 0 && t < sizeof(tmp)) {
        tmp[t++] = (char)('0' + (v % 10u));
        v /= 10u;
    }

    uint64_t n = 0;
    while (t > 0 && n + 1 < cap) {
        out[n++] = tmp[--t];
    }
    out[n] = '\0';
}

static int mem_contains(const char *hay, uint64_t hay_len, const char *needle) {
    uint64_t nlen = cstr_len_u64_local(needle);
    if (nlen == 0) return 1;
    if (hay_len < nlen) return 0;

    for (uint64_t i = 0; i + nlen <= hay_len; i++) {
        int ok = 1;
        for (uint64_t j = 0; j < nlen; j++) {
            if (hay[i + j] != needle[j]) {
                ok = 0;
                break;
            }
        }
        if (ok) return 1;
    }
    return 0;
}

static int run_capture(const char *path, const char *const argv[], char *out, uint64_t out_cap) {
    if (!out || out_cap == 0) return 1;
    out[0] = '\0';

    int pfds[2];
    long prc = (long)sys_pipe2(pfds, 0);
    if (prc < 0) {
        sys_puts("[kinit] pipe2 failed\n");
        return 1;
    }

    long pid = (long)sys_fork();
    if (pid == 0) {
        (void)sys_dup2((uint64_t)pfds[1], 1);
        (void)sys_close((uint64_t)pfds[0]);
        (void)sys_close((uint64_t)pfds[1]);
        (void)sys_execve(path, argv, 0);
        sys_puts("[kinit] capture execve failed\n");
        sys_exit_group(127);
    } else if (pid < 0) {
        sys_puts("[kinit] fork failed\n");
        (void)sys_close((uint64_t)pfds[0]);
        (void)sys_close((uint64_t)pfds[1]);
        return 1;
    }

    (void)sys_close((uint64_t)pfds[1]);

    uint64_t pos = 0;
    int read_failed = 0;
    while (pos + 1 < out_cap) {
        long n = (long)sys_read((uint64_t)pfds[0], out + pos, out_cap - pos - 1);
        if (n < 0) {
            /* EAGAIN (11) => retry (used for pipes). */
            if (n == -11) {
                continue;
            }
            sys_puts("[kinit] capture read failed\n");
            read_failed = 1;
            break;
        }
        if (n == 0) break;
        pos += (uint64_t)n;
    }
    out[pos] = '\0';
    (void)sys_close((uint64_t)pfds[0]);

    int status = 0;
    uint64_t wrc = sys_wait4((uint64_t)pid, &status, 0, 0);
    if ((int64_t)wrc < 0) {
        sys_puts("[kinit] capture wait4 failed\n");
        return 1;
    }

    if (read_failed) return 1;

    int exit_code = (status >> 8) & 0xff;
    return (exit_code == 0) ? 0 : 1;
}

static uint64_t ts_to_ns_clamp(linux_timespec_t ts) {
    if (ts.tv_sec < 0) return 0;
    if (ts.tv_nsec < 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void write_i64_dec_local(int64_t v) {
    char buf[32];
    uint64_t n = 0;
    if (v < 0) {
        buf[n++] = '-';
        v = -v;
    }
    if (v == 0) {
        buf[n++] = '0';
    } else {
        char tmp[32];
        uint64_t t = (uint64_t)v;
        uint64_t m = 0;
        while (t > 0 && m < sizeof(tmp)) {
            tmp[m++] = (char)('0' + (t % 10));
            t /= 10;
        }
        while (m > 0) {
            buf[n++] = tmp[--m];
        }
    }
    (void)sys_write(1, buf, n);
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

    /* Tool smoke test: printf + escapes + pipeline into sort. */
    {
        const char *const test_argv[] = {"sh", "-c", "/bin/printf \"b\\na\\n\" | /bin/sort", 0};
        failed |= run_test("/bin/sh -c \"/bin/printf ... | /bin/sort\"", "/bin/sh", test_argv);
    }

    /* Tool smoke test: tee writes to stdout and a file. */
    {
        const char *const test_argv[] = {"sh", "-c", "mkdir -p /tmp; /bin/printf \"x\\n\" | /bin/tee /tmp/tee_test; /bin/cat /tmp/tee_test", 0};
        failed |= run_test("/bin/sh -c \"... tee ...\"", "/bin/sh", test_argv);
    }

    /* Tool smoke test: rev reverses each line. */
    {
        char out[256];
        const char *const rev_argv[] = {"sh", "-c", "/bin/printf \"abc\\n\" | /bin/rev", 0};
        if (run_capture("/bin/sh", rev_argv, out, sizeof(out)) != 0 || !mem_contains(out, cstr_len_u64_local(out), "cba")) {
            sys_puts("[kinit] rev selftest failed\n");
            failed |= 1;
        }
    }

    /* Tool smoke test: env -i prints nothing. */
    {
        char out[256];
        const char *const env_argv[] = {"env", "-i", 0};
        if (run_capture("/bin/env", env_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] env capture failed\n");
            failed |= 1;
        } else if (cstr_len_u64_local(out) != 0) {
            sys_puts("[kinit] env -i output unexpected\n");
            failed |= 1;
        }
    }

    /* Tool smoke test: dirname /bin/sh => /bin. */
    {
        char out[256];
        const char *const dn_argv[] = {"dirname", "/bin/sh", 0};
        if (run_capture("/bin/dirname", dn_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] dirname capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "/bin\n")) {
            sys_puts("[kinit] dirname output unexpected\n");
            failed |= 1;
        }
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

    /* Tool smoke test: awk field printing + pattern filter. */
    {
        char out[512];
        const char *const awk_argv1[] = {"awk", "{print $1}", "/uniq.txt", 0};
        if (run_capture("/bin/awk", awk_argv1, out, sizeof(out)) != 0) {
            sys_puts("[kinit] awk capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "x")) {
            sys_puts("[kinit] awk output missing expected field\n");
            failed |= 1;
        }

        const char *const awk_argv2[] = {"awk", "/y/ {print $1}", "/uniq.txt", 0};
        if (run_capture("/bin/awk", awk_argv2, out, sizeof(out)) != 0) {
            sys_puts("[kinit] awk(pattern) capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "y")) {
            sys_puts("[kinit] awk(pattern) output missing expected match\n");
            failed |= 1;
        }
    }

    /* Tool smoke test: basename. */
    {
        char out[128];
        const char *const bn_argv[] = {"basename", "/a/b/c", 0};
        if (run_capture("/bin/basename", bn_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] basename capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "c\n")) {
            sys_puts("[kinit] basename output unexpected\n");
            failed |= 1;
        }
    }

    /* Tool smoke test: tr. */
    {
        char out[128];
        const char *const tr_argv[] = {"sh", "-c", "echo abc | tr abc ABC", 0};
        if (run_capture("/bin/sh", tr_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] tr capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "ABC")) {
            sys_puts("[kinit] tr output unexpected\n");
            failed |= 1;
        }
    }

    /* Tool smoke test: tr -s (squeeze). */
    {
        char out[128];
        const char *const tr_argv[] = {"sh", "-c", "echo aaabbb | tr -s ab", 0};
        if (run_capture("/bin/sh", tr_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] tr -s capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "ab")) {
            sys_puts("[kinit] tr -s output unexpected\n");
            failed |= 1;
        }
    }

    /* Tool smoke test: tr -c (complement). */
    {
        char out[128];
        const char *const tr_argv[] = {"sh", "-c", "echo abc | tr -c a X", 0};
        if (run_capture("/bin/sh", tr_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] tr -c capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "aXX")) {
            sys_puts("[kinit] tr -c output unexpected\n");
            failed |= 1;
        }
    }

    /* Tool smoke test: du over a known file. */
    {
        char out[256];
        const char *const du_argv[] = {"du", "/uniq.txt", 0};
        if (run_capture("/bin/du", du_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] du capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "/uniq.txt")) {
            sys_puts("[kinit] du output missing path\n");
            failed |= 1;
        }
    }

    /* Tool smoke test: du -a (includes files). */
    {
        enum {
            AT_FDCWD = -100,
            AT_REMOVEDIR = 0x200,
        };

        char out[512];
        const char *const du_argv[] = {"sh", "-c", "mkdir -p /dutest; touch /dutest/x; du -a /dutest", 0};
        if (run_capture("/bin/sh", du_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] du -a capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "/dutest/x")) {
            sys_puts("[kinit] du -a output missing file\n");
            failed |= 1;
        }

        (void)sys_unlinkat((uint64_t)AT_FDCWD, "/dutest/x", 0);
        (void)sys_unlinkat((uint64_t)AT_FDCWD, "/dutest", (uint64_t)AT_REMOVEDIR);
    }

    /* Tool smoke test: ln creates an overlay hardlink; unlink preserves remaining name. */
    {
        enum {
            AT_FDCWD = -100,
            O_RDONLY = 0,
            O_WRONLY = 1,
            O_CREAT = 0100,
            AT_REMOVEDIR = 0x200,
        };

        /* Best-effort cleanup from prior runs. */
        (void)sys_unlinkat((uint64_t)AT_FDCWD, "/lntest/a", 0);
        (void)sys_unlinkat((uint64_t)AT_FDCWD, "/lntest/b", 0);
        (void)sys_unlinkat((uint64_t)AT_FDCWD, "/lntest", (uint64_t)AT_REMOVEDIR);

        (void)sys_mkdirat((uint64_t)AT_FDCWD, "/lntest", 0755);

        uint64_t fd = sys_openat((uint64_t)AT_FDCWD, "/lntest/a", (uint64_t)(O_CREAT | O_WRONLY), 0644);
        if ((int64_t)fd < 0) {
            sys_puts("[kinit] ln setup: openat failed rc=");
            write_i64_dec_local((int64_t)fd);
            sys_puts("\n");
            failed |= 1;
        } else {
            (void)sys_close(fd);

            const char *const ln_argv[] = {"ln", "/lntest/a", "/lntest/b", 0};
            failed |= run_test("/bin/ln /lntest/a /lntest/b", "/bin/ln", ln_argv);

            int64_t urc = (int64_t)sys_unlinkat((uint64_t)AT_FDCWD, "/lntest/a", 0);
            if (urc < 0) {
                sys_puts("[kinit] ln: unlinkat(a) failed rc=");
                write_i64_dec_local(urc);
                sys_puts("\n");
                failed |= 1;
            }

            uint64_t bfd = sys_openat((uint64_t)AT_FDCWD, "/lntest/b", (uint64_t)O_RDONLY, 0);
            if ((int64_t)bfd < 0) {
                sys_puts("[kinit] ln: openat(b) failed rc=");
                write_i64_dec_local((int64_t)bfd);
                sys_puts("\n");
                failed |= 1;
            } else {
                (void)sys_close(bfd);
            }

            (void)sys_unlinkat((uint64_t)AT_FDCWD, "/lntest/b", 0);
            (void)sys_unlinkat((uint64_t)AT_FDCWD, "/lntest", (uint64_t)AT_REMOVEDIR);
        }
    }

    /* Tool smoke test: ln -s + readlink (and open follows symlink). */
    {
        enum {
            AT_FDCWD = -100,
        };

        (void)sys_unlinkat((uint64_t)AT_FDCWD, "/tmp/sy", 0);

        const char *const ln_argv[] = {"ln", "-s", "/uniq.txt", "/tmp/sy", 0};
        failed |= run_test("/bin/ln -s /uniq.txt /tmp/sy", "/bin/ln", ln_argv);

        char out[256];
        const char *const rl_argv[] = {"readlink", "/tmp/sy", 0};
        if (run_capture("/bin/readlink", rl_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] readlink capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "/uniq.txt")) {
            sys_puts("[kinit] readlink output unexpected\n");
            failed |= 1;
        }

        {
            const char *const cat_argv[] = {"cat", "/tmp/sy", 0};
            failed |= run_test("/bin/cat /tmp/sy", "/bin/cat", cat_argv);
        }

        (void)sys_unlinkat((uint64_t)AT_FDCWD, "/tmp/sy", 0);
    }

    /* Tool smoke test: time. */
    {
        char out[256];
        const char *const t_argv[] = {"time", "/bin/true", 0};
        if (run_capture("/bin/time", t_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] time capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "real")) {
            sys_puts("[kinit] time output unexpected\n");
            failed |= 1;
        }
    }

    /* Tool smoke test: readelf. */
    {
        char out[512];
        const char *const re_argv[] = {"readelf", "/bin/sh", 0};
        if (run_capture("/bin/readelf", re_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] readelf capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "ELF Header")) {
            sys_puts("[kinit] readelf output unexpected\n");
            failed |= 1;
        }
    }

    /* Tool smoke test: pipeline into wc. */
    {
        const char *const test_argv[] = {"sh", "-c", "seq 1 10 | wc -l", 0};
        failed |= run_test("/bin/sh -c \"seq 1 10 | wc -l\"", "/bin/sh", test_argv);
    }

    /* Tool smoke test: pstree (non-interactive, should exit 0). */
    {
        const char *const test_argv[] = {"pstree", "-p", 0};
        failed |= run_test("/bin/pstree -p", "/bin/pstree", test_argv);
    }

    /* Tool smoke test: find (basic -name and depth). */
    {
        const char *const prep_argv[] = {"sh", "-c", "mkdir -p /tmp/fd; touch /tmp/fd/hi; mkdir -p /tmp/fd/sub; touch /tmp/fd/sub/lo", 0};
        failed |= run_test("/bin/sh -c \"prepare find tree\"", "/bin/sh", prep_argv);

        char out[512];
        const char *const find_argv[] = {"find", "/tmp/fd", "-name", "hi", 0};
        if (run_capture("/bin/find", find_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] find capture failed\n");
            failed |= 1;
        } else {
            if (!mem_contains(out, cstr_len_u64_local(out), "/tmp/fd/hi")) {
                sys_puts("[kinit] find output missing expected path\n");
                failed |= 1;
            }
        }
    }

    /* Tool smoke test: find can see common /bin tools (directory listing correctness). */
    {
        char out[512];
        const char *const find_argv[] = {"find", "/bin", "-maxdepth", "1", "-name", "sh", 0};
        if (run_capture("/bin/find", find_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] find(/bin) capture failed\n");
            failed |= 1;
        } else {
            if (!mem_contains(out, cstr_len_u64_local(out), "/bin/sh")) {
                sys_puts("[kinit] find(/bin) missing /bin/sh (bad getdents64?)\n");
                failed |= 1;
            }
        }
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

    /* Phase-8 time tests: clock_gettime monotonicity and sleep actually sleeps. */
    {
        sys_puts("[kinit] selftest: clock_gettime monotonic\n");
        linux_timespec_t a;
        linux_timespec_t b;
        if ((int64_t)sys_clock_gettime(1, &a) < 0) {
            sys_puts("[kinit] clock_gettime(CLOCK_MONOTONIC) failed\n");
            failed |= 1;
        } else {
            for (int i = 0; i < 1000; i++) {
                (void)sys_getpid();
            }
            if ((int64_t)sys_clock_gettime(1, &b) < 0) {
                sys_puts("[kinit] clock_gettime(CLOCK_MONOTONIC) failed (2)\n");
                failed |= 1;
            } else {
                if (ts_to_ns_clamp(b) < ts_to_ns_clamp(a)) {
                    sys_puts("[kinit] CLOCK_MONOTONIC went backwards\n");
                    failed |= 1;
                }
            }
        }

        sys_puts("[kinit] selftest: /bin/sleep 1 duration\n");
        linux_timespec_t t0;
        linux_timespec_t t1;
        if ((int64_t)sys_clock_gettime(1, &t0) < 0) {
            sys_puts("[kinit] clock_gettime failed before sleep\n");
            failed |= 1;
        } else {
            const char *const sleep_argv[] = {"sleep", "1", 0};
            failed |= run_test("/bin/sleep 1", "/bin/sleep", sleep_argv);
            if ((int64_t)sys_clock_gettime(1, &t1) < 0) {
                sys_puts("[kinit] clock_gettime failed after sleep\n");
                failed |= 1;
            } else {
                uint64_t dt = ts_to_ns_clamp(t1) - ts_to_ns_clamp(t0);
                if (dt < 900000000ull) {
                    sys_puts("[kinit] sleep returned too early\n");
                    failed |= 1;
                }
            }
        }

        sys_puts("[kinit] selftest: /bin/sleep 0.2 duration\n");
        if ((int64_t)sys_clock_gettime(1, &t0) < 0) {
            sys_puts("[kinit] clock_gettime failed before sleep 0.2\n");
            failed |= 1;
        } else {
            const char *const sleep_argv[] = {"sleep", "0.2", 0};
            failed |= run_test("/bin/sleep 0.2", "/bin/sleep", sleep_argv);
            if ((int64_t)sys_clock_gettime(1, &t1) < 0) {
                sys_puts("[kinit] clock_gettime failed after sleep 0.2\n");
                failed |= 1;
            } else {
                uint64_t dt = ts_to_ns_clamp(t1) - ts_to_ns_clamp(t0);
                if (dt < 150000000ull) {
                    sys_puts("[kinit] sleep 0.2 returned too early\n");
                    failed |= 1;
                }
            }
        }
    }

    /* Tool smoke tests: date + uptime exist and produce plausible output. */
    {
        char out[128];
        const char *const uptime_argv[] = {"uptime", 0};
        if (run_capture("/bin/uptime", uptime_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] uptime capture failed\n");
            failed |= 1;
        } else if (!mem_contains(out, cstr_len_u64_local(out), "up ")) {
            sys_puts("[kinit] uptime output missing prefix\n");
            failed |= 1;
        }

        const char *const date_argv[] = {"date", 0};
        if (run_capture("/bin/date", date_argv, out, sizeof(out)) != 0) {
            sys_puts("[kinit] date capture failed\n");
            failed |= 1;
        } else {
            /* Expect something like YYYY-MM-DD HH:MM:SS */
            if (!mem_contains(out, cstr_len_u64_local(out), "-") ||
                !mem_contains(out, cstr_len_u64_local(out), ":")) {
                sys_puts("[kinit] date output looks wrong\n");
                failed |= 1;
            }
        }
    }

    {
        const char *const test_argv[] = {"compat", 0};
        failed |= run_test("/bin/compat", "/bin/compat", test_argv);
    }

    /* Tool smoke test: ps + kill (fork a child, ensure it shows in ps, kill it, ensure it disappears). */
    {
        sys_puts("[kinit] selftest: /bin/ps + /bin/kill\n");

        long cpid = (long)sys_fork();
        if (cpid == 0) {
            /* Busy loop that yields via syscalls; gets killed by parent. */
            for (;;) {
                (void)sys_getpid();
            }
        }
        if (cpid < 0) {
            sys_puts("[kinit] fork failed\n");
            failed |= 1;
        } else {
            char ps_out[1024];
            const char *const ps_argv[] = {"ps", 0};

            char pid_str[32];
            u64_to_dec((uint64_t)cpid, pid_str, sizeof(pid_str));

            if (run_capture("/bin/ps", ps_argv, ps_out, sizeof(ps_out)) != 0) {
                sys_puts("[kinit] ps capture failed\n");
                failed |= 1;
            } else if (!mem_contains(ps_out, cstr_len_u64_local(ps_out), pid_str)) {
                sys_puts("[kinit] ps output missing child pid\n");
                failed |= 1;
            }

            const char *const kill_argv[] = {"kill", "-9", pid_str, 0};
            failed |= run_test("/bin/kill -9 <pid>", "/bin/kill", kill_argv);

            int st = 0;
            uint64_t wrc = sys_wait4((uint64_t)cpid, &st, 0, 0);
            if ((int64_t)wrc < 0) {
                sys_puts("[kinit] wait4 after kill failed\n");
                failed |= 1;
            } else {
                int exit_code = (st >> 8) & 0xff;
                if (exit_code != 137) {
                    sys_puts("[kinit] unexpected exit code after kill\n");
                    failed |= 1;
                }
            }

            if (run_capture("/bin/ps", ps_argv, ps_out, sizeof(ps_out)) != 0) {
                sys_puts("[kinit] ps capture failed (post-kill)\n");
                failed |= 1;
            } else if (mem_contains(ps_out, cstr_len_u64_local(ps_out), pid_str)) {
                sys_puts("[kinit] ps output still contains killed pid\n");
                failed |= 1;
            }
        }
    }

    if (failed) {
        sys_puts("[kinit] selftests FAILED\n");
        sys_exit_group(1);
    }

    sys_puts("[kinit] selftests OK\n");
    sys_exit_group(0);
    return 0;
}
