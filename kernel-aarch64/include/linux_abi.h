#pragma once

#include <stdint.h>

/* Minimal Linux ABI structs/constants we use for syscalls. */

/* newfstatat(2) uses this struct stat on AArch64 (glibc-compatible layout). */

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} linux_timespec_t;

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;

    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;

    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;

    linux_timespec_t st_atim;
    linux_timespec_t st_mtim;
    linux_timespec_t st_ctim;

    int64_t __glibc_reserved[3];
} linux_stat_t;

/* getdents64(2) entry header (variable-length record). */
#define LINUX_DT_UNKNOWN 0
#define LINUX_DT_DIR 4
#define LINUX_DT_REG 8

/* uname(2) uses struct utsname. */
enum { LINUX_UTSNAME_LEN = 65 };

typedef struct {
    char sysname[LINUX_UTSNAME_LEN];
    char nodename[LINUX_UTSNAME_LEN];
    char release[LINUX_UTSNAME_LEN];
    char version[LINUX_UTSNAME_LEN];
    char machine[LINUX_UTSNAME_LEN];
    char domainname[LINUX_UTSNAME_LEN];
} linux_utsname_t;
