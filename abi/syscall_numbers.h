#pragma once

/*
 * Linux AArch64 syscall numbers used by this repo.
 *
 * Single source of truth included by both kernel and userland.
 */

#define __NR_getcwd        17ull
#define __NR_dup3          24ull
#define __NR_ioctl         29ull
#define __NR_mkdirat       34ull
#define __NR_unlinkat      35ull
#define __NR_symlinkat     36ull
#define __NR_linkat        37ull
#define __NR_chdir         49ull
#define __NR_fchmodat      53ull
#define __NR_openat        56ull
#define __NR_close         57ull
#define __NR_pipe2         59ull
#define __NR_getdents64    61ull
#define __NR_lseek         62ull
#define __NR_read          63ull
#define __NR_write         64ull
#define __NR_readlinkat    78ull
#define __NR_newfstatat    79ull
#define __NR_exit          93ull
#define __NR_exit_group    94ull
#define __NR_set_tid_address 96ull
#define __NR_set_robust_list  99ull
#define __NR_nanosleep     101ull
#define __NR_clock_gettime 113ull
#define __NR_kill          129ull
#define __NR_rt_sigaction  134ull
#define __NR_rt_sigprocmask 135ull
#define __NR_reboot        142ull
#define __NR_uname         160ull
#define __NR_getpid        172ull
#define __NR_getppid       173ull
#define __NR_getuid        174ull
#define __NR_geteuid       175ull
#define __NR_getgid        176ull
#define __NR_getegid       177ull
#define __NR_gettid        178ull
#define __NR_brk           214ull
#define __NR_munmap        215ull
#define __NR_clone         220ull
#define __NR_execve        221ull
#define __NR_mmap          222ull
#define __NR_wait4         260ull
#define __NR_prlimit64     261ull
#define __NR_getrandom     278ull

/*
 * mona-specific syscalls (non-Linux).
 * Keep these well above the Linux AArch64 range we currently use.
 */
#define __NR_mona_dmesg    4096ull
#define __NR_mona_ping6    4097ull
