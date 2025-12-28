#pragma once

#include "stdint.h"

/* Implemented in src/syscall_asm.S */
uint64_t __syscall0(uint64_t nr);
uint64_t __syscall1(uint64_t nr, uint64_t a0);
uint64_t __syscall2(uint64_t nr, uint64_t a0, uint64_t a1);
uint64_t __syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t __syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t __syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
uint64_t __syscall6(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

/* Typed-pointer variants to preserve aliasing information (avoid uintptr_t casts). */
uint64_t __syscall3_p(uint64_t nr, uint64_t a0, void *p1, uint64_t a2);
uint64_t __syscall3_ppp(uint64_t nr, const void *p0, const void *p1, const void *p2);
uint64_t __syscall4_upuu(uint64_t nr, uint64_t a0, const void *p1, uint64_t a2, uint64_t a3);
uint64_t __syscall4_uppu(uint64_t nr, uint64_t a0, const void *p1, void *p2, uint64_t a3);
uint64_t __syscall4_upup(uint64_t nr, uint64_t a0, void *p1, uint64_t a2, void *p3);

/* Linux AArch64 syscall numbers */
#define __NR_getcwd    17ull
#define __NR_ioctl     29ull
#define __NR_dup3      24ull
#define __NR_mkdirat   34ull
#define __NR_chdir     49ull
#define __NR_openat    56ull
#define __NR_close     57ull
#define __NR_pipe2     59ull
#define __NR_getdents64 61ull
#define __NR_lseek     62ull
#define __NR_read      63ull
#define __NR_write      64ull
#define __NR_newfstatat 79ull
#define __NR_nanosleep 101ull
#define __NR_set_tid_address 96ull
#define __NR_set_robust_list 99ull
#define __NR_clock_gettime 113ull
#define __NR_rt_sigaction 134ull
#define __NR_rt_sigprocmask 135ull
#define __NR_reboot    142ull
#define __NR_uname     160ull
#define __NR_getpid     172ull
#define __NR_getppid    173ull
#define __NR_getuid     174ull
#define __NR_geteuid    175ull
#define __NR_getgid     176ull
#define __NR_getegid    177ull
#define __NR_gettid     178ull
#define __NR_brk        214ull
#define __NR_munmap     215ull
#define __NR_clone      220ull
#define __NR_execve     221ull
#define __NR_mmap       222ull
#define __NR_wait4      260ull
#define __NR_prlimit64  261ull
#define __NR_getrandom  278ull
#define __NR_exit       93ull
#define __NR_exit_group 94ull

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} linux_timespec_t;

enum { LINUX_UTSNAME_LEN = 65 };

typedef struct {
    char sysname[LINUX_UTSNAME_LEN];
    char nodename[LINUX_UTSNAME_LEN];
    char release[LINUX_UTSNAME_LEN];
    char version[LINUX_UTSNAME_LEN];
    char machine[LINUX_UTSNAME_LEN];
    char domainname[LINUX_UTSNAME_LEN];
} linux_utsname_t;

static inline uint64_t sys_getpid(void) {
    return __syscall0(__NR_getpid);
}

static inline uint64_t sys_getppid(void) {
    return __syscall0(__NR_getppid);
}

static inline uint64_t sys_getuid(void) {
    return __syscall0(__NR_getuid);
}

static inline uint64_t sys_geteuid(void) {
    return __syscall0(__NR_geteuid);
}

static inline uint64_t sys_getgid(void) {
    return __syscall0(__NR_getgid);
}

static inline uint64_t sys_getegid(void) {
    return __syscall0(__NR_getegid);
}

static inline uint64_t sys_gettid(void) {
    return __syscall0(__NR_gettid);
}

static inline uint64_t sys_uname(linux_utsname_t *buf) {
    return __syscall1(__NR_uname, (uint64_t)(uintptr_t)buf);
}

static inline uint64_t sys_clock_gettime(uint64_t clockid, linux_timespec_t *tp) {
    return __syscall2(__NR_clock_gettime, clockid, (uint64_t)(uintptr_t)tp);
}

static inline uint64_t sys_brk(void *addr) {
    return __syscall1(__NR_brk, (uint64_t)(uintptr_t)addr);
}

static inline uint64_t sys_getcwd(char *buf, uint64_t size) {
    return __syscall2(__NR_getcwd, (uint64_t)(uintptr_t)buf, size);
}

static inline uint64_t sys_chdir(const char *path) {
    return __syscall1(__NR_chdir, (uint64_t)(uintptr_t)path);
}

static inline uint64_t sys_nanosleep(const linux_timespec_t *req, linux_timespec_t *rem) {
    return __syscall2(__NR_nanosleep, (uint64_t)(uintptr_t)req, (uint64_t)(uintptr_t)rem);
}

static inline uint64_t sys_set_tid_address(uint32_t *tidptr) {
    return __syscall1(__NR_set_tid_address, (uint64_t)(uintptr_t)tidptr);
}

static inline uint64_t sys_set_robust_list(void *head, uint64_t len) {
    return __syscall2(__NR_set_robust_list, (uint64_t)(uintptr_t)head, len);
}

static inline uint64_t sys_rt_sigaction(uint64_t sig, const void *act, void *oldact, uint64_t sigsetsize) {
    return __syscall4(__NR_rt_sigaction, sig, (uint64_t)(uintptr_t)act, (uint64_t)(uintptr_t)oldact, sigsetsize);
}

static inline uint64_t sys_rt_sigprocmask(uint64_t how, const void *set, void *oldset, uint64_t sigsetsize) {
    return __syscall4(__NR_rt_sigprocmask, how, (uint64_t)(uintptr_t)set, (uint64_t)(uintptr_t)oldset, sigsetsize);
}

static inline uint64_t sys_getrandom(void *buf, uint64_t len, uint64_t flags) {
    return __syscall3(__NR_getrandom, (uint64_t)(uintptr_t)buf, len, flags);
}

static inline uint64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd, void *arg) {
    return __syscall4(__NR_reboot, magic1, magic2, cmd, (uint64_t)(uintptr_t)arg);
}

static inline uint64_t sys_ioctl(uint64_t fd, uint64_t req, void *argp) {
    return __syscall3(__NR_ioctl, fd, req, (uint64_t)(uintptr_t)argp);
}

static inline uint64_t sys_mmap(void *addr,
                               uint64_t len,
                               uint64_t prot,
                               uint64_t flags,
                               int64_t fd,
                               uint64_t off) {
    return __syscall6(
        __NR_mmap,
        (uint64_t)(uintptr_t)addr,
        len,
        prot,
        flags,
        (uint64_t)fd,
        off);
}

static inline uint64_t sys_munmap(void *addr, uint64_t len) {
    return __syscall2(__NR_munmap, (uint64_t)(uintptr_t)addr, len);
}

static inline uint64_t sys_openat(uint64_t dirfd, const char *pathname, uint64_t flags, uint64_t mode) {
    return __syscall4_upuu(__NR_openat, dirfd, pathname, flags, mode);
}

static inline uint64_t sys_mkdirat(uint64_t dirfd, const char *pathname, uint64_t mode) {
    return __syscall3_p(__NR_mkdirat, dirfd, (void *)pathname, mode);
}

static inline uint64_t sys_close(uint64_t fd) {
    return __syscall1(__NR_close, fd);
}

static inline uint64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags) {
    return __syscall3(__NR_dup3, oldfd, newfd, flags);
}

static inline uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd) {
    return sys_dup3(oldfd, newfd, 0);
}

static inline uint64_t sys_pipe2(int pipefd[2], uint64_t flags) {
    return __syscall2(__NR_pipe2, (uint64_t)(uintptr_t)pipefd, flags);
}

static inline uint64_t sys_read(uint64_t fd, void *buf, uint64_t len) {
    return __syscall3_p(__NR_read, fd, buf, len);
}

static inline uint64_t sys_getdents64(uint64_t fd, void *dirp, uint64_t count) {
    return __syscall3_p(__NR_getdents64, fd, dirp, count);
}

static inline uint64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence) {
    return __syscall3(__NR_lseek, fd, (uint64_t)offset, whence);
}

static inline uint64_t sys_newfstatat(uint64_t dirfd, const char *pathname, void *statbuf, uint64_t flags) {
    return __syscall4_uppu(__NR_newfstatat, dirfd, pathname, statbuf, flags);
}

static inline uint64_t sys_execve(const char *pathname, const char *const *argv, const char *const *envp) {
    return __syscall3_ppp(__NR_execve, pathname, (const void *)argv, (const void *)envp);
}

static inline uint64_t sys_clone(uint64_t flags, void *child_stack, void *ptid, void *ctid, uint64_t tls) {
    return __syscall5(
        __NR_clone,
        flags,
        (uint64_t)(uintptr_t)child_stack,
        (uint64_t)(uintptr_t)ptid,
        (uint64_t)(uintptr_t)ctid,
        tls);
}

static inline uint64_t sys_wait4(int64_t pid, int *wstatus, int options, void *rusage) {
    return __syscall4_upup(__NR_wait4, (uint64_t)pid, (void *)wstatus, (uint64_t)options, rusage);
}

static inline uint64_t sys_fork(void) {
    /* clone(flags=SIGCHLD, ...) like a classic fork. */
    return sys_clone(17, 0, 0, 0, 0);
}

static inline uint64_t sys_write(uint64_t fd, const void *buf, uint64_t len) {
    return __syscall3_p(__NR_write, fd, (void *)buf, len);
}

__attribute__((noreturn)) static inline void sys_exit_group(uint64_t status) {
    (void)__syscall1(__NR_exit_group, status);
    for (;;) { }
}

static inline void sys_puts(const char *s) {
    const char *p = s;
    uint64_t n = 0;
    while (p[n] != '\0') n++;
    (void)sys_write(1, s, n);
}
