#pragma once

#include <stdint.h>

/* Implemented in src/syscall_asm.S */
uint64_t __syscall0(uint64_t nr);
uint64_t __syscall1(uint64_t nr, uint64_t a0);
uint64_t __syscall2(uint64_t nr, uint64_t a0, uint64_t a1);
uint64_t __syscall3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t __syscall4(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
uint64_t __syscall5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);

/* Typed-pointer variants to preserve aliasing information (avoid uintptr_t casts). */
uint64_t __syscall3_p(uint64_t nr, uint64_t a0, void *p1, uint64_t a2);
uint64_t __syscall3_ppp(uint64_t nr, const void *p0, const void *p1, const void *p2);
uint64_t __syscall4_upuu(uint64_t nr, uint64_t a0, const void *p1, uint64_t a2, uint64_t a3);
uint64_t __syscall4_uppu(uint64_t nr, uint64_t a0, const void *p1, void *p2, uint64_t a3);
uint64_t __syscall4_upup(uint64_t nr, uint64_t a0, void *p1, uint64_t a2, void *p3);

/* Linux AArch64 syscall numbers */
#define __NR_dup3      24ull
#define __NR_openat    56ull
#define __NR_close     57ull
#define __NR_pipe2     59ull
#define __NR_getdents64 61ull
#define __NR_lseek     62ull
#define __NR_read      63ull
#define __NR_write      64ull
#define __NR_newfstatat 79ull
#define __NR_clone      220ull
#define __NR_execve     221ull
#define __NR_wait4      260ull
#define __NR_exit       93ull
#define __NR_exit_group 94ull

static inline uint64_t sys_openat(uint64_t dirfd, const char *pathname, uint64_t flags, uint64_t mode) {
    return __syscall4_upuu(__NR_openat, dirfd, pathname, flags, mode);
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
