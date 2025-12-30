#pragma once

#include "stdint.h"

/*
 * Minimal stat(2)-style mode/type bits used throughout the kernel.
 *
 * These match the traditional Unix layout (and Linux userspace expectations):
 * - top bits are the file type (masked by S_IFMT)
 * - low 9 bits are permissions
 */

#define S_IFMT 0170000u
#define S_IFSOCK 0140000u
#define S_IFLNK 0120000u
#define S_IFREG 0100000u
#define S_IFBLK 0060000u
#define S_IFDIR 0040000u
#define S_IFCHR 0020000u
#define S_IFIFO 0010000u

#define S_ISDIR(m) ((((uint32_t)(m)) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) ((((uint32_t)(m)) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) ((((uint32_t)(m)) & S_IFMT) == S_IFLNK)
