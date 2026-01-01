#pragma once

#include "exceptions.h"
#include "stdint.h"

#define SYSCALL_SWITCHED 0xFFFFFFFFFFFFFFFFull

uint64_t sys_getcwd(uint64_t buf_user, uint64_t size);
uint64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t argp_user);
uint64_t sys_brk(uint64_t newbrk);
uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off);
uint64_t sys_munmap(uint64_t addr, uint64_t len);

uint64_t sys_getuid(void);
uint64_t sys_geteuid(void);
uint64_t sys_getgid(void);
uint64_t sys_getegid(void);
uint64_t sys_gettid(void);

uint64_t sys_uname(uint64_t buf_user);
uint64_t sys_clock_gettime(uint64_t clockid, uint64_t tp_user);
uint64_t sys_kill(trap_frame_t *tf, int64_t pid, uint64_t sig, uint64_t elr);
uint64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd, uint64_t arg);
uint64_t sys_set_tid_address(uint64_t tidptr_user);
uint64_t sys_set_robust_list(uint64_t head_user, uint64_t len);
uint64_t sys_rt_sigaction(uint64_t sig, uint64_t act_user, uint64_t oldact_user, uint64_t sigsetsize);
uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_user, uint64_t oldset_user, uint64_t sigsetsize);
uint64_t sys_nanosleep(trap_frame_t *tf, uint64_t req_user, uint64_t rem_user, uint64_t elr);
uint64_t sys_getrandom(uint64_t buf_user, uint64_t len, uint64_t flags);

uint64_t sys_chdir(uint64_t path_user);
uint64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags);
uint64_t sys_mkdirat(int64_t dirfd, uint64_t pathname_user, uint64_t mode);
uint64_t sys_openat(int64_t dirfd, uint64_t pathname_user, uint64_t flags, uint64_t mode);
uint64_t sys_symlinkat(uint64_t target_user, int64_t newdirfd, uint64_t linkpath_user);
uint64_t sys_linkat(int64_t olddirfd, uint64_t oldpath_user, int64_t newdirfd, uint64_t newpath_user, uint64_t flags);
uint64_t sys_unlinkat(int64_t dirfd, uint64_t pathname_user, uint64_t flags);
uint64_t sys_close(uint64_t fd);
uint64_t sys_pipe2(uint64_t pipefd_user, uint64_t flags);
uint64_t sys_read(trap_frame_t *tf, uint64_t fd, uint64_t buf_user, uint64_t len, uint64_t elr);
uint64_t sys_getdents64(uint64_t fd, uint64_t dirp_user, uint64_t count);
uint64_t sys_lseek(uint64_t fd, int64_t off, uint64_t whence);
uint64_t sys_write(uint64_t fd, const void *buf, uint64_t len);
uint64_t sys_readlinkat(int64_t dirfd, uint64_t pathname_user, uint64_t buf_user, uint64_t bufsiz);
uint64_t sys_newfstatat(int64_t dirfd, uint64_t pathname_user, uint64_t statbuf_user, uint64_t flags);
uint64_t sys_fchmodat(int64_t dirfd, uint64_t pathname_user, uint64_t mode, uint64_t flags);

uint64_t sys_execve(trap_frame_t *tf, uint64_t pathname_user, uint64_t argv_user, uint64_t envp_user);
uint64_t sys_clone(trap_frame_t *tf, uint64_t flags, uint64_t child_stack, uint64_t ptid, uint64_t ctid, uint64_t tls, uint64_t elr);
uint64_t sys_wait4(trap_frame_t *tf, int64_t pid_req, uint64_t wstatus_user, uint64_t options, uint64_t rusage_user, uint64_t elr);
int handle_exit_and_maybe_switch(trap_frame_t *tf, uint64_t code);

/* mona-specific: read kernel log ring buffer (dmesg). */
uint64_t sys_mona_dmesg(uint64_t buf_user, uint64_t len, uint64_t flags);
