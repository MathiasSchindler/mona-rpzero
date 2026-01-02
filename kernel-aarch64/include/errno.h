#pragma once

/* errno values (Linux) */
#define EPERM 1ull
#define E2BIG 7ull
#define ENOEXEC 8ull
#define EBADF 9ull
#define ECHILD 10ull
#define EAGAIN 11ull
#define ENOMEM 12ull
#define EFAULT 14ull
#define EEXIST 17ull
#define ENOTDIR 20ull
#define EISDIR 21ull
#define EINVAL 22ull
#define EMFILE 24ull
#define ENOTTY 25ull
#define EROFS 30ull
#define EPIPE 32ull
#define ERANGE 34ull
#define ENAMETOOLONG 36ull
#define ENOENT 2ull
#define ESRCH 3ull
#define EIO 5ull
#define ENOSYS 38ull
#define ENOTEMPTY 39ull

/* Additional errno values used by networking bring-up. */
#define ENODEV 19ull
#define EBUSY 16ull
#define EMSGSIZE 90ull
#define EAFNOSUPPORT 97ull
#define EADDRINUSE 98ull
#define ETIMEDOUT 110ull
#define ENETUNREACH 101ull
