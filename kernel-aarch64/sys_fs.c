#include "syscalls.h"

#include "errno.h"
#include "fd.h"
#include "initramfs.h"
#include "linux_abi.h"
#include "pipe.h"
#include "proc.h"
#include "sys_util.h"
#include "stat_bits.h"
#include "uart_pl011.h"
#include "console_in.h"
#include "vfs.h"

#define AT_FDCWD ((int64_t)-100)

/* openat(2) flags (minimal subset). */
#define O_RDONLY 0u
#define O_WRONLY 1u
#define O_RDWR 2u
#define O_ACCMODE 3u
#define O_CREAT 0100u
#define O_EXCL 0200u
#define O_TRUNC 01000u

/* unlinkat(2) flags (subset). */
#define AT_REMOVEDIR 0x200u

#define MAX_SYMLINK_HOPS 8

static int follow_symlink_data(const char *link_abs, const uint8_t *tgt, uint64_t tgt_len, char *out_abs, uint64_t outsz) {
    if (!link_abs || !tgt || !out_abs || outsz == 0) return -1;

    /* Copy target into a temporary NUL-terminated buffer. */
    char target[MAX_PATH];
    if (tgt_len + 1 > sizeof(target)) return -1;
    for (uint64_t i = 0; i < tgt_len; i++) target[i] = (char)tgt[i];
    target[tgt_len] = '\0';

    /* Determine parent directory of link_abs. */
    char parent[MAX_PATH];
    if (abs_path_parent_dir(link_abs, parent, sizeof(parent)) != 0) return -1;

    if (target[0] == '/') {
        return normalize_abs_path(target, out_abs, outsz);
    }

    char tmp[MAX_PATH];
    uint64_t pn = cstr_len_u64(parent);
    uint64_t tn = cstr_len_u64(target);
    uint64_t o = 0;
    if (pn == 0) return -1;
    if (pn + 1 + tn + 1 > sizeof(tmp)) return -1;
    for (uint64_t i = 0; i < pn; i++) tmp[o++] = parent[i];
    if (o == 0 || tmp[o - 1] != '/') tmp[o++] = '/';
    for (uint64_t i = 0; i < tn; i++) tmp[o++] = target[i];
    tmp[o] = '\0';
    return normalize_abs_path(tmp, out_abs, outsz);
}

static int resolve_final_symlink(char *abs_io, uint64_t cap) {
    if (!abs_io || cap == 0) return -1;

    for (int hop = 0; hop < MAX_SYMLINK_HOPS; hop++) {
        const uint8_t *data = 0;
        uint64_t size = 0;
        uint32_t mode = 0;
        if (vfs_lookup_abs(abs_io, &data, &size, &mode) != 0) {
            return 0;
        }
        if ((mode & S_IFMT) != S_IFLNK) {
            return 0;
        }

        char next[MAX_PATH];
        if (follow_symlink_data(abs_io, data, size, next, sizeof(next)) != 0) {
            return -1;
        }

        uint64_t n = cstr_len_u64(next);
        if (n + 1 > cap) return -1;
        for (uint64_t i = 0; i <= n; i++) abs_io[i] = next[i];
    }

    return -1;
}

/* /proc helpers (used by /proc/ps). */
static char proc_state_char(proc_state_t st) {
    switch (st) {
        case PROC_RUNNABLE: return 'R';
        case PROC_WAITING: return 'W';
        case PROC_ZOMBIE: return 'Z';
        case PROC_SLEEPING: return 'S';
        case PROC_UNUSED: return 'U';
        default: return '?';
    }
}

static void buf_putc(char *buf, uint64_t cap, uint64_t *pos, char c) {
    if (!buf || cap == 0 || !pos) return;
    if (*pos + 1 >= cap) return;
    buf[*pos] = c;
    *pos = *pos + 1;
}

static void buf_puts(char *buf, uint64_t cap, uint64_t *pos, const char *s) {
    if (!buf || cap == 0 || !pos || !s) return;
    for (uint64_t i = 0; s[i] != '\0'; i++) {
        if (*pos + 1 >= cap) return;
        buf[*pos] = s[i];
        *pos = *pos + 1;
    }
}

static void buf_put_u64(char *buf, uint64_t cap, uint64_t *pos, uint64_t v) {
    if (!buf || cap == 0 || !pos) return;
    if (*pos + 1 >= cap) return;

    if (v == 0) {
        buf_putc(buf, cap, pos, '0');
        return;
    }

    char tmp[32];
    uint64_t t = 0;
    while (v != 0 && t < sizeof(tmp)) {
        tmp[t++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (t > 0) {
        buf_putc(buf, cap, pos, tmp[--t]);
    }
}

uint64_t sys_getcwd(uint64_t buf_user, uint64_t size) {
    proc_t *cur = &g_procs[g_cur_proc];
    uint64_t n = cstr_len_u64(cur->cwd);
    if (size == 0) return (uint64_t)(-(int64_t)EINVAL);
    if (n + 1 > size) return (uint64_t)(-(int64_t)ERANGE);
    if (!user_range_ok(buf_user, n + 1)) return (uint64_t)(-(int64_t)EFAULT);
    if (write_bytes_to_user(buf_user, cur->cwd, n + 1) != 0) return (uint64_t)(-(int64_t)EFAULT);
    return buf_user;
}

uint64_t sys_chdir(uint64_t path_user) {
    char in[MAX_PATH];
    if (copy_cstr_from_user(in, sizeof(in), path_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    proc_t *cur = &g_procs[g_cur_proc];
    char path[MAX_PATH];
    if (resolve_path(cur, in, path, sizeof(path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    if (resolve_final_symlink(path, sizeof(path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    const uint8_t *data = 0;
    uint64_t size = 0;
    uint32_t mode = 0;
    if (vfs_lookup_abs(path, &data, &size, &mode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }
        if (!S_ISDIR(mode)) {
        return (uint64_t)(-(int64_t)ENOTDIR);
    }

    uint64_t n = cstr_len_u64(path);
    if (n + 1 > sizeof(cur->cwd)) {
        return (uint64_t)(-(int64_t)ENAMETOOLONG);
    }
    for (uint64_t i = 0; i <= n; i++) {
        cur->cwd[i] = path[i];
    }
    return 0;
}

uint64_t sys_symlinkat(uint64_t target_user, int64_t newdirfd, uint64_t linkpath_user) {
    if (newdirfd != AT_FDCWD) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (target_user == 0 || linkpath_user == 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    char target_in[MAX_PATH];
    if (copy_cstr_from_user(target_in, sizeof(target_in), target_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    char link_in[MAX_PATH];
    if (copy_cstr_from_user(link_in, sizeof(link_in), linkpath_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    proc_t *cur = &g_procs[g_cur_proc];

    char link_abs[MAX_PATH];
    if (resolve_path(cur, link_in, link_abs, sizeof(link_abs)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    char link_no_slash[MAX_PATH];
    {
        int prc = abs_path_to_no_slash_trim(link_abs, link_no_slash, sizeof(link_no_slash));
        if (prc != 0) return (uint64_t)(int64_t)prc;
    }

    if (cstr_eq_u64(link_abs, "/")) {
        return (uint64_t)(-(int64_t)EPERM);
    }

    /* New path must not exist. */
    uint32_t tmp_mode = 0;
    if (vfs_lookup_abs(link_abs, 0, 0, &tmp_mode) == 0) {
        return (uint64_t)(-(int64_t)EEXIST);
    }

    /* Parent must exist and be a directory. */
    char parent_abs[MAX_PATH];
    {
        int prc = abs_path_parent_dir(link_abs, parent_abs, sizeof(parent_abs));
        if (prc != 0) return (uint64_t)(int64_t)prc;
    }

    uint32_t pmode = 0;
    if (vfs_lookup_abs(parent_abs, 0, 0, &pmode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }
    if (!S_ISDIR(pmode)) {
        return (uint64_t)(-(int64_t)ENOTDIR);
    }

    int crc = vfs_ramfile_create(link_no_slash, S_IFLNK | 0777u);
    if (crc != 0) return (uint64_t)(int64_t)crc;

    uint32_t file_id = 0;
    if (vfs_ramfile_find_abs(link_abs, &file_id) != 0) {
        (void)vfs_ramfile_unlink(link_no_slash);
        return (uint64_t)(-(int64_t)ENOENT);
    }

    uint8_t *data = 0;
    uint64_t size = 0;
    uint64_t cap = 0;
    uint32_t mode = 0;
    if (vfs_ramfile_get(file_id, &data, &size, &cap, &mode) != 0) {
        (void)vfs_ramfile_unlink(link_no_slash);
        return (uint64_t)(-(int64_t)ENOENT);
    }

    uint64_t tlen = cstr_len_u64(target_in);
    if (tlen > cap) {
        (void)vfs_ramfile_unlink(link_no_slash);
        return (uint64_t)(-(int64_t)ENAMETOOLONG);
    }
    for (uint64_t i = 0; i < tlen; i++) data[i] = (uint8_t)target_in[i];
    if (vfs_ramfile_set_size(file_id, tlen) != 0) {
        (void)vfs_ramfile_unlink(link_no_slash);
        return (uint64_t)(-(int64_t)EINVAL);
    }
    return 0;
}

uint64_t sys_mkdirat(int64_t dirfd, uint64_t pathname_user, uint64_t mode) {
    if (dirfd != AT_FDCWD) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (pathname_user == 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    char in[MAX_PATH];
    if (copy_cstr_from_user(in, sizeof(in), pathname_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    proc_t *cur = &g_procs[g_cur_proc];
    char abs_path[MAX_PATH];
    if (resolve_path(cur, in, abs_path, sizeof(abs_path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    char p[MAX_PATH];
    {
        int prc = abs_path_to_no_slash_trim(abs_path, p, sizeof(p));
        if (prc != 0) return (uint64_t)(int64_t)prc;
    }

    /* mkdir("/") => EEXIST */
    if (cstr_eq_u64(abs_path, "/")) {
        return (uint64_t)(-(int64_t)EEXIST);
    }

    /* If it already exists in initramfs or ramdir overlay, fail with EEXIST. */
    {
        uint32_t em = 0;
        if (vfs_lookup_abs(abs_path, 0, 0, &em) == 0) {
            return (uint64_t)(-(int64_t)EEXIST);
        }
    }

    /* Parent must exist and be a directory. */
    char parent_abs[MAX_PATH];
    {
        int prc = abs_path_parent_dir(abs_path, parent_abs, sizeof(parent_abs));
        if (prc != 0) return (uint64_t)(int64_t)prc;
    }

    uint32_t pmode = 0;
    if (vfs_lookup_abs(parent_abs, 0, 0, &pmode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }
    if (!S_ISDIR(pmode)) {
        return (uint64_t)(-(int64_t)ENOTDIR);
    }

    uint64_t m = mode & 0777u;
    int rc = vfs_ramdir_create(p, S_IFDIR | (uint32_t)m);
    if (rc != 0) {
        return (uint64_t)(int64_t)rc;
    }
    return 0;
}

uint64_t sys_linkat(int64_t olddirfd, uint64_t oldpath_user, int64_t newdirfd, uint64_t newpath_user, uint64_t flags) {
    if (olddirfd != AT_FDCWD || newdirfd != AT_FDCWD) {
        return (uint64_t)(-(int64_t)EINVAL);
    }
    if (flags != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }
    if (oldpath_user == 0 || newpath_user == 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    char old_in[MAX_PATH];
    if (copy_cstr_from_user(old_in, sizeof(old_in), oldpath_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    char new_in[MAX_PATH];
    if (copy_cstr_from_user(new_in, sizeof(new_in), newpath_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    proc_t *cur = &g_procs[g_cur_proc];

    char old_abs[MAX_PATH];
    if (resolve_path(cur, old_in, old_abs, sizeof(old_abs)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }
    char new_abs[MAX_PATH];
    if (resolve_path(cur, new_in, new_abs, sizeof(new_abs)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    char old_no_slash[MAX_PATH];
    {
        int prc = abs_path_to_no_slash_trim(old_abs, old_no_slash, sizeof(old_no_slash));
        if (prc != 0) return (uint64_t)(int64_t)prc;
    }
    char new_no_slash[MAX_PATH];
    {
        int prc = abs_path_to_no_slash_trim(new_abs, new_no_slash, sizeof(new_no_slash));
        if (prc != 0) return (uint64_t)(int64_t)prc;
    }

    /* Special cases. */
    if (cstr_eq_u64(old_abs, "/") || cstr_eq_u64(new_abs, "/")) {
        return (uint64_t)(-(int64_t)EPERM);
    }

    /* Old path must exist and be a regular file in the overlay ramfile layer. */
    uint32_t old_mode = 0;
    if (vfs_lookup_abs(old_abs, 0, 0, &old_mode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }
    if (S_ISDIR(old_mode)) {
        /* Hardlinking directories is forbidden. */
        return (uint64_t)(-(int64_t)EPERM);
    }

    uint32_t old_ramfile_id = 0;
    if (vfs_ramfile_find_abs(old_abs, &old_ramfile_id) != 0) {
        /* Exists in initramfs (read-only) or non-overlay => reject as read-only. */
        return (uint64_t)(-(int64_t)EROFS);
    }
    (void)old_ramfile_id;

    /* New path must not exist. */
    uint32_t tmp_mode = 0;
    if (vfs_lookup_abs(new_abs, 0, 0, &tmp_mode) == 0) {
        return (uint64_t)(-(int64_t)EEXIST);
    }

    /* Parent of new path must exist and be a directory. */
    char parent_abs[MAX_PATH];
    {
        int prc = abs_path_parent_dir(new_abs, parent_abs, sizeof(parent_abs));
        if (prc != 0) return (uint64_t)(int64_t)prc;
    }

    uint32_t pmode = 0;
    if (vfs_lookup_abs(parent_abs, 0, 0, &pmode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }
    if (!S_ISDIR(pmode)) {
        return (uint64_t)(-(int64_t)ENOTDIR);
    }

    /* Create the overlay hardlink. */
    int rc = vfs_ramfile_link(old_no_slash, new_no_slash);
    if (rc != 0) return (uint64_t)(int64_t)rc;
    return 0;
}

uint64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t argp_user) {
    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(&cur->fdt, fd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    file_desc_t *d = &g_descs[didx];
    if (d->kind != FDESC_UART) {
        return (uint64_t)(-(int64_t)ENOTTY);
    }

    /* Common tty requests used for isatty() / shell probing. */
    const uint64_t TCGETS = 0x5401u;
    const uint64_t TIOCGWINSZ = 0x5413u;
    const uint64_t TIOCGPGRP = 0x540Fu;

    if (req == TCGETS) {
        /* struct termios is 60 bytes on AArch64. Return zeros (reasonable defaults). */
        if (argp_user == 0) return (uint64_t)(-(int64_t)EFAULT);
        if (!user_range_ok(argp_user, 60)) return (uint64_t)(-(int64_t)EFAULT);
        uint8_t zero[60];
        for (uint64_t i = 0; i < sizeof(zero); i++) zero[i] = 0;
        if (write_bytes_to_user(argp_user, zero, sizeof(zero)) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        return 0;
    }

    if (req == TIOCGWINSZ) {
        /* struct winsize { u16 row,col,xpixel,ypixel } */
        if (argp_user == 0) return (uint64_t)(-(int64_t)EFAULT);
        if (!user_range_ok(argp_user, 8)) return (uint64_t)(-(int64_t)EFAULT);

        uint16_t ws[4];
        ws[0] = 24; /* rows */
        ws[1] = 80; /* cols */
        ws[2] = 0;
        ws[3] = 0;
        if (write_bytes_to_user(argp_user, ws, sizeof(ws)) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        return 0;
    }

    if (req == TIOCGPGRP) {
        if (argp_user == 0) return (uint64_t)(-(int64_t)EFAULT);
        if (!user_range_ok(argp_user, 4)) return (uint64_t)(-(int64_t)EFAULT);
        uint32_t pg = (uint32_t)cur->pid;
        if (write_bytes_to_user(argp_user, &pg, sizeof(pg)) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        return 0;
    }

    return (uint64_t)(-(int64_t)ENOTTY);
}

uint64_t sys_openat(int64_t dirfd, uint64_t pathname_user, uint64_t flags, uint64_t mode) {
    if (dirfd != AT_FDCWD) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }

    char in[MAX_PATH];
    if (copy_cstr_from_user(in, sizeof(in), pathname_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    proc_t *cur = &g_procs[g_cur_proc];
    char path[MAX_PATH];
    if (resolve_path(cur, in, path, sizeof(path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    if (resolve_final_symlink(path, sizeof(path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    /* Minimal procfs: /proc (dir) and /proc/ps (file). */
    if (cstr_eq_u64(path, "/proc") || cstr_eq_u64(path, "/proc/")) {
        uint64_t acc = flags & (uint64_t)O_ACCMODE;
        if (acc != (uint64_t)O_RDONLY) {
            return (uint64_t)(-(int64_t)EROFS);
        }

        int didx = desc_alloc();
        if (didx < 0) {
            return (uint64_t)(-(int64_t)EMFILE);
        }
        file_desc_t *d = &g_descs[didx];
        desc_clear(d);
        d->kind = FDESC_PROC;
        d->refs = 1;
        d->u.proc.node = 1u;
        d->u.proc.off = 0;

        int fd = fd_alloc_into(&cur->fdt, 3, didx);
        desc_decref(didx);
        if (fd < 0) {
            return (uint64_t)(-(int64_t)EMFILE);
        }
        return (uint64_t)fd;
    }

    if (cstr_eq_u64(path, "/proc/ps")) {
        uint64_t acc = flags & (uint64_t)O_ACCMODE;
        if (acc != (uint64_t)O_RDONLY) {
            return (uint64_t)(-(int64_t)EROFS);
        }

        int didx = desc_alloc();
        if (didx < 0) {
            return (uint64_t)(-(int64_t)EMFILE);
        }
        file_desc_t *d = &g_descs[didx];
        desc_clear(d);
        d->kind = FDESC_PROC;
        d->refs = 1;
        d->u.proc.node = 2u;
        d->u.proc.off = 0;

        int fd = fd_alloc_into(&cur->fdt, 3, didx);
        desc_decref(didx);
        if (fd < 0) {
            return (uint64_t)(-(int64_t)EMFILE);
        }
        return (uint64_t)fd;
    }

    /* First: if a ramfile already exists at this path, open it. */
    uint32_t ramfile_id = 0;
    if (vfs_ramfile_find_abs(path, &ramfile_id) == 0) {
        /* O_EXCL only matters with O_CREAT. */
        if ((flags & (uint64_t)(O_CREAT | O_EXCL)) == (uint64_t)(O_CREAT | O_EXCL)) {
            return (uint64_t)(-(int64_t)EEXIST);
        }

        if ((flags & (uint64_t)O_TRUNC) != 0) {
            (void)vfs_ramfile_set_size(ramfile_id, 0);
        }

        int didx = desc_alloc();
        if (didx < 0) {
            return (uint64_t)(-(int64_t)EMFILE);
        }

        file_desc_t *d = &g_descs[didx];
        desc_clear(d);
        d->kind = FDESC_RAMFILE;
        d->refs = 1;
        d->u.ramfile.file_id = ramfile_id;
        d->u.ramfile.off = 0;

        int fd = fd_alloc_into(&cur->fdt, 3, didx);
        desc_decref(didx);
        if (fd < 0) {
            return (uint64_t)(-(int64_t)EMFILE);
        }
        return (uint64_t)fd;
    }

    /* Second: resolve via initramfs + ramdir overlay.
     * If it doesn't exist and O_CREAT is set, create a new ramfile entry.
     */

    const uint8_t *data = 0;
    uint64_t size = 0;
    uint32_t imode = 0;

    if (vfs_lookup_abs(path, &data, &size, &imode) != 0) {
        if ((flags & (uint64_t)O_CREAT) == 0) {
            return (uint64_t)(-(int64_t)ENOENT);
        }

        /* Create a new overlay file. Parent must exist and be a directory. */
        if (cstr_eq_u64(path, "/")) {
            return (uint64_t)(-(int64_t)EISDIR);
        }

        char p[MAX_PATH];
        {
            int prc = abs_path_to_no_slash_trim(path, p, sizeof(p));
            if (prc != 0) return (uint64_t)(int64_t)prc;
        }

        /* Parent must exist and be a directory. */
        char parent_abs[MAX_PATH];
        {
            int prc = abs_path_parent_dir(path, parent_abs, sizeof(parent_abs));
            if (prc != 0) return (uint64_t)(int64_t)prc;
        }

        uint32_t pmode = 0;
        if (vfs_lookup_abs(parent_abs, 0, 0, &pmode) != 0) {
            return (uint64_t)(-(int64_t)ENOENT);
        }
        if (!S_ISDIR(pmode)) {
            return (uint64_t)(-(int64_t)ENOTDIR);
        }

        uint32_t file_mode = S_IFREG | (uint32_t)(mode & 0777u);
        int crc = vfs_ramfile_create(p, file_mode);
        if (crc != 0) {
            return (uint64_t)(int64_t)crc;
        }

        /* Re-open as ramfile. */
        if (vfs_ramfile_find_abs(path, &ramfile_id) != 0) {
            return (uint64_t)(-(int64_t)ENOENT);
        }

        int didx = desc_alloc();
        if (didx < 0) {
            return (uint64_t)(-(int64_t)EMFILE);
        }
        file_desc_t *d = &g_descs[didx];
        desc_clear(d);
        d->kind = FDESC_RAMFILE;
        d->refs = 1;
        d->u.ramfile.file_id = ramfile_id;
        d->u.ramfile.off = 0;

        int fd = fd_alloc_into(&cur->fdt, 3, didx);
        desc_decref(didx);
        if (fd < 0) {
            return (uint64_t)(-(int64_t)EMFILE);
        }
        return (uint64_t)fd;
    }

    /* Exists: handle O_EXCL|O_CREAT. */
    if ((flags & (uint64_t)(O_CREAT | O_EXCL)) == (uint64_t)(O_CREAT | O_EXCL)) {
        return (uint64_t)(-(int64_t)EEXIST);
    }

    /* Initramfs is read-only: reject opens requesting write access. */
    if (S_ISREG(imode)) {
        uint64_t acc = flags & (uint64_t)O_ACCMODE;
        if (acc == (uint64_t)O_WRONLY || acc == (uint64_t)O_RDWR) {
            return (uint64_t)(-(int64_t)EROFS);
        }
    }

    int didx = desc_alloc();
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EMFILE);
    }

    file_desc_t *d = &g_descs[didx];
    desc_clear(d);
    d->kind = FDESC_INITRAMFS;
    d->refs = 1;
    d->u.initramfs.data = data;
    d->u.initramfs.size = size;
    d->u.initramfs.off = 0;
    d->u.initramfs.mode = imode;
    d->u.initramfs.is_dir = S_ISDIR(imode) ? 1u : 0u;
    d->u.initramfs.dir_path[0] = '\0';
    if (d->u.initramfs.is_dir) {
        /* Store normalized path (leading slashes stripped, "/" -> ""). */
        const char *pp = path;
        while (*pp == '/') pp++;
        uint64_t i;
        for (i = 0; i + 1 < sizeof(d->u.initramfs.dir_path) && pp[i] != '\0'; i++) {
            d->u.initramfs.dir_path[i] = pp[i];
        }
        d->u.initramfs.dir_path[i] = '\0';
    }

    int fd = fd_alloc_into(&cur->fdt, 3, didx);
    /* fd_alloc_into() takes a ref; drop our creation ref. */
    desc_decref(didx);
    if (fd < 0) {
        return (uint64_t)(-(int64_t)EMFILE);
    }
    return (uint64_t)fd;
}

uint64_t sys_close(uint64_t fd) {
    proc_t *cur = &g_procs[g_cur_proc];
    if (fd >= MAX_FDS) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (fd_get_desc_idx(&cur->fdt, fd) < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    fd_close(&cur->fdt, fd);
    return 0;
}

uint64_t sys_read(uint64_t fd, uint64_t buf_user, uint64_t len) {
    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(&cur->fdt, fd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (!user_range_ok(buf_user, len)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    if (len == 0) return 0;

    file_desc_t *d = &g_descs[didx];
    if (d->kind == FDESC_UART) {
        volatile char *dst = (volatile char *)(uintptr_t)buf_user;

        /* Block for the first byte, then drain any immediately available bytes. */
        char c = console_in_getc_blocking();
        dst[0] = c;

        uint64_t n = 1;
        for (; n < len; n++) {
            char t;
            if (!console_in_try_getc(&t)) break;
            dst[n] = t;
        }
        return n;
    }

    if (d->kind == FDESC_PIPE && d->u.pipe.end == PIPE_END_READ) {
        int64_t rc = pipe_read(d->u.pipe.pipe_id, (volatile uint8_t *)(uintptr_t)buf_user, len);
        if (rc < 0) return (uint64_t)rc;
        return (uint64_t)rc;
    }

    if (d->kind == FDESC_RAMFILE) {
        uint8_t *data = 0;
        uint64_t size = 0;
        uint64_t cap = 0;
        uint32_t mode = 0;
        if (vfs_ramfile_get(d->u.ramfile.file_id, &data, &size, &cap, &mode) != 0) {
            return (uint64_t)(-(int64_t)EBADF);
        }
        (void)cap;
        (void)mode;

        if (d->u.ramfile.off >= size) return 0;
        uint64_t remain = size - d->u.ramfile.off;
        uint64_t n = (len < remain) ? len : remain;

        volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)buf_user;
        const uint8_t *src = data + d->u.ramfile.off;
        for (uint64_t i = 0; i < n; i++) {
            dst[i] = src[i];
        }

        d->u.ramfile.off += n;
        return n;
    }

    if (d->kind == FDESC_PROC && d->u.proc.node == 2u) {
        /* /proc/ps: generate a small text snapshot each read and slice by offset. */
        char out[1024];
        uint64_t pos = 0;

        /* Tiny helpers */
        for (int i = 0; i < (int)MAX_PROCS; i++) {
            if (g_procs[i].state == PROC_UNUSED) continue;
            buf_put_u64(out, sizeof(out), &pos, g_procs[i].pid);
            buf_putc(out, sizeof(out), &pos, ' ');
            buf_put_u64(out, sizeof(out), &pos, g_procs[i].ppid);
            buf_putc(out, sizeof(out), &pos, ' ');
            buf_putc(out, sizeof(out), &pos, proc_state_char(g_procs[i].state));
            buf_putc(out, sizeof(out), &pos, ' ');
            buf_puts(out, sizeof(out), &pos, g_procs[i].cwd);
            buf_putc(out, sizeof(out), &pos, '\n');
        }

        /* NUL-terminate for safety (not counted). */
        if (pos < sizeof(out)) out[pos] = '\0';

        if (d->u.proc.off >= pos) return 0;
        uint64_t remain = pos - d->u.proc.off;
        uint64_t n = (len < remain) ? len : remain;
        volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)buf_user;
        const uint8_t *src = (const uint8_t *)out + d->u.proc.off;
        for (uint64_t i = 0; i < n; i++) {
            dst[i] = src[i];
        }
        d->u.proc.off += n;
        return n;
    }

    if (d->kind != FDESC_INITRAMFS) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    if (d->u.initramfs.is_dir) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    if (d->u.initramfs.off >= d->u.initramfs.size) return 0;

    uint64_t remain = d->u.initramfs.size - d->u.initramfs.off;
    uint64_t n = (len < remain) ? len : remain;

    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)buf_user;
    const uint8_t *src = d->u.initramfs.data + d->u.initramfs.off;
    for (uint64_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }

    d->u.initramfs.off += n;
    return n;
}

uint64_t sys_write(uint64_t fd, const void *buf, uint64_t len) {
    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(&cur->fdt, fd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    file_desc_t *d = &g_descs[didx];
    if (d->kind == FDESC_UART) {
        const volatile char *p = (const volatile char *)buf;
        for (uint64_t i = 0; i < len; i++) {
            uart_putc(p[i]);
        }
        return len;
    }

    if (d->kind == FDESC_PIPE && d->u.pipe.end == PIPE_END_WRITE) {
        int64_t rc = pipe_write(d->u.pipe.pipe_id, (const volatile uint8_t *)buf, len);
        if (rc < 0) return (uint64_t)rc;
        return (uint64_t)rc;
    }

    if (d->kind == FDESC_RAMFILE) {
        uint64_t src_user = (uint64_t)(uintptr_t)buf;
        if (!user_range_ok(src_user, len)) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        if (len == 0) return 0;

        uint8_t *data = 0;
        uint64_t size = 0;
        uint64_t cap = 0;
        uint32_t mode = 0;
        if (vfs_ramfile_get(d->u.ramfile.file_id, &data, &size, &cap, &mode) != 0) {
            return (uint64_t)(-(int64_t)EBADF);
        }
        (void)mode;

        if (d->u.ramfile.off >= cap) {
            return (uint64_t)(-(int64_t)EINVAL);
        }
        uint64_t space = cap - d->u.ramfile.off;
        uint64_t n = (len < space) ? len : space;

        const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)src_user;
        uint8_t *dst = data + d->u.ramfile.off;
        for (uint64_t i = 0; i < n; i++) {
            dst[i] = src[i];
        }

        d->u.ramfile.off += n;
        if (d->u.ramfile.off > size) {
            (void)vfs_ramfile_set_size(d->u.ramfile.file_id, d->u.ramfile.off);
        }
        return n;
    }

    return (uint64_t)(-(int64_t)EBADF);
}

typedef struct {
    uint64_t skip;
    uint64_t emitted;
    uint64_t buf_user;
    uint64_t buf_len;
    uint64_t pos;
} dents_ctx_t;

static uint64_t align8_u64(uint64_t x) {
    return (x + 7u) & ~7u;
}

static int dents_emit_cb(const char *name, uint32_t mode, void *ctx) {
    dents_ctx_t *dc = (dents_ctx_t *)ctx;
    if (dc->emitted < dc->skip) {
        dc->emitted++;
        return 0;
    }

    /* linux_dirent64 record: ino,u64; off,s64; reclen,u16; type,u8; name,NUL */
    uint64_t name_len = 0;
    while (name[name_len] != '\0') name_len++;

    uint64_t reclen = 8 + 8 + 2 + 1 + (name_len + 1);
    reclen = align8_u64(reclen);
    if (dc->pos + reclen > dc->buf_len) {
        return 1; /* stop */
    }

    uint64_t dst = dc->buf_user + dc->pos;

    /* d_ino */
    *(volatile uint64_t *)(uintptr_t)(dst + 0) = 1;
    /* d_off */
    *(volatile int64_t *)(uintptr_t)(dst + 8) = (int64_t)(dc->emitted + 1);
    /* d_reclen */
    *(volatile uint16_t *)(uintptr_t)(dst + 16) = (uint16_t)reclen;
    /* d_type */
    uint8_t dtype = LINUX_DT_UNKNOWN;
    if (S_ISDIR(mode)) dtype = LINUX_DT_DIR;
    else if (S_ISREG(mode)) dtype = LINUX_DT_REG;
    *(volatile uint8_t *)(uintptr_t)(dst + 18) = dtype;

    /* name */
    volatile char *np = (volatile char *)(uintptr_t)(dst + 19);
    for (uint64_t i = 0; i < name_len; i++) np[i] = name[i];
    np[name_len] = '\0';

    /* zero pad remainder */
    for (uint64_t i = 19 + name_len + 1; i < reclen; i++) {
        *(volatile uint8_t *)(uintptr_t)(dst + i) = 0;
    }

    dc->pos += reclen;
    dc->emitted++;
    return 0;
}

uint64_t sys_getdents64(uint64_t fd, uint64_t dirp_user, uint64_t count) {
    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(&cur->fdt, fd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    file_desc_t *d = &g_descs[didx];
    if (d->kind == FDESC_PROC && d->u.proc.node == 1u) {
        if (!user_range_ok(dirp_user, count)) {
            return (uint64_t)(-(int64_t)EFAULT);
        }

        dents_ctx_t dc;
        dc.skip = d->u.proc.off;
        dc.emitted = 0;
        dc.buf_user = dirp_user;
        dc.buf_len = count;
        dc.pos = 0;

        (void)dents_emit_cb(".", S_IFDIR, &dc);
        (void)dents_emit_cb("..", S_IFDIR, &dc);
        (void)dents_emit_cb("ps", S_IFREG, &dc);

        if (dc.emitted > dc.skip) {
            d->u.proc.off = dc.emitted;
        }
        return dc.pos;
    }

    if (d->kind != FDESC_INITRAMFS || !d->u.initramfs.is_dir) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (!user_range_ok(dirp_user, count)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    dents_ctx_t dc;
    dc.skip = d->u.initramfs.off;
    dc.emitted = 0;
    dc.buf_user = dirp_user;
    dc.buf_len = count;
    dc.pos = 0;

    int rc = vfs_list_dir(d->u.initramfs.dir_path, dents_emit_cb, &dc);
    if (rc != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }

    /* Advance directory position by entries emitted in this call. */
    if (dc.emitted > dc.skip) {
        d->u.initramfs.off = dc.emitted;
    }

    return dc.pos;
}

uint64_t sys_lseek(uint64_t fd, int64_t off, uint64_t whence) {
    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(&cur->fdt, fd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    file_desc_t *d = &g_descs[didx];
    if (d->kind == FDESC_PROC) {
        /* Minimal support: SEEK_SET/SEEK_CUR only (used rarely). */
        uint64_t cur_off = d->u.proc.off;
        uint64_t newoff;
        switch (whence) {
            case 0: /* SEEK_SET */
                if (off < 0) return (uint64_t)(-(int64_t)EINVAL);
                newoff = (uint64_t)off;
                break;
            case 1: /* SEEK_CUR */
                if (off < 0 && (uint64_t)(-off) > cur_off) return (uint64_t)(-(int64_t)EINVAL);
                newoff = (uint64_t)((int64_t)cur_off + off);
                break;
            default:
                return (uint64_t)(-(int64_t)EINVAL);
        }
        d->u.proc.off = newoff;
        return newoff;
    }

    if (d->kind == FDESC_RAMFILE) {
        uint8_t *data = 0;
        uint64_t size = 0;
        uint64_t cap = 0;
        uint32_t mode = 0;
        if (vfs_ramfile_get(d->u.ramfile.file_id, &data, &size, &cap, &mode) != 0) {
            return (uint64_t)(-(int64_t)EBADF);
        }
        (void)data;
        (void)cap;
        (void)mode;

        uint64_t newoff;
        switch (whence) {
            case 0: /* SEEK_SET */
                if (off < 0) return (uint64_t)(-(int64_t)EINVAL);
                newoff = (uint64_t)off;
                break;
            case 1: /* SEEK_CUR */
                if (off < 0 && (uint64_t)(-off) > d->u.ramfile.off) return (uint64_t)(-(int64_t)EINVAL);
                newoff = (uint64_t)((int64_t)d->u.ramfile.off + off);
                break;
            case 2: /* SEEK_END */
                if (off < 0 && (uint64_t)(-off) > size) return (uint64_t)(-(int64_t)EINVAL);
                newoff = (uint64_t)((int64_t)size + off);
                break;
            default:
                return (uint64_t)(-(int64_t)EINVAL);
        }
        if (newoff > size) return (uint64_t)(-(int64_t)EINVAL);
        d->u.ramfile.off = newoff;
        return newoff;
    }

    if (d->kind != FDESC_INITRAMFS || d->u.initramfs.is_dir) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    uint64_t newoff;
    switch (whence) {
        case 0: /* SEEK_SET */
            if (off < 0) return (uint64_t)(-(int64_t)EINVAL);
            newoff = (uint64_t)off;
            break;
        case 1: /* SEEK_CUR */
            if (off < 0 && (uint64_t)(-off) > d->u.initramfs.off) return (uint64_t)(-(int64_t)EINVAL);
            newoff = (uint64_t)((int64_t)d->u.initramfs.off + off);
            break;
        case 2: /* SEEK_END */
            if (off < 0 && (uint64_t)(-off) > d->u.initramfs.size) return (uint64_t)(-(int64_t)EINVAL);
            newoff = (uint64_t)((int64_t)d->u.initramfs.size + off);
            break;
        default:
            return (uint64_t)(-(int64_t)EINVAL);
    }

    if (newoff > d->u.initramfs.size) return (uint64_t)(-(int64_t)EINVAL);
    d->u.initramfs.off = newoff;
    return newoff;
}

uint64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags) {
    if (flags != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }
    if (oldfd >= MAX_FDS || newfd >= MAX_FDS) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(&cur->fdt, oldfd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    if (oldfd == newfd) {
        return newfd;
    }

    /* Close destination if open. */
    fd_close(&cur->fdt, newfd);

    cur->fdt.fd_to_desc[newfd] = (int16_t)didx;
    desc_incref(didx);
    return newfd;
}

uint64_t sys_pipe2(uint64_t pipefd_user, uint64_t flags) {
    if (flags != 0) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (!user_range_ok(pipefd_user, 8)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    uint32_t pid_u32 = 0;
    int prc = pipe_create(&pid_u32);
    if (prc != 0) {
        /* Keep behavior: report as EMFILE when we cannot allocate a pipe slot. */
        return (uint64_t)(-(int64_t)EMFILE);
    }
    uint32_t pid = pid_u32;

    int rdesc = desc_alloc();
    int wdesc = desc_alloc();
    if (rdesc < 0 || wdesc < 0) {
        if (rdesc >= 0) desc_clear(&g_descs[rdesc]);
        if (wdesc >= 0) desc_clear(&g_descs[wdesc]);
        pipe_abort(pid);
        return (uint64_t)(-(int64_t)EMFILE);
    }

    desc_clear(&g_descs[rdesc]);
    g_descs[rdesc].kind = FDESC_PIPE;
    g_descs[rdesc].refs = 1;
    g_descs[rdesc].u.pipe.pipe_id = (uint32_t)pid;
    g_descs[rdesc].u.pipe.end = PIPE_END_READ;
    pipe_on_desc_incref((uint32_t)pid, PIPE_END_READ);

    desc_clear(&g_descs[wdesc]);
    g_descs[wdesc].kind = FDESC_PIPE;
    g_descs[wdesc].refs = 1;
    g_descs[wdesc].u.pipe.pipe_id = (uint32_t)pid;
    g_descs[wdesc].u.pipe.end = PIPE_END_WRITE;
    pipe_on_desc_incref((uint32_t)pid, PIPE_END_WRITE);

    /* Install into current process FD table. */
    proc_t *cur = &g_procs[g_cur_proc];
    int rfd = fd_alloc_into(&cur->fdt, 0, rdesc);
    int wfd = fd_alloc_into(&cur->fdt, 0, wdesc);
    /* fd_alloc_into() increments refs; drop our creation refs. */
    desc_decref(rdesc);
    desc_decref(wdesc);

    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) fd_close(&cur->fdt, (uint64_t)rfd);
        if (wfd >= 0) fd_close(&cur->fdt, (uint64_t)wfd);
        pipe_abort((uint32_t)pid);
        return (uint64_t)(-(int64_t)EMFILE);
    }

    /* Write pipe fds back to user. */
    uint32_t out[2];
    out[0] = (uint32_t)rfd;
    out[1] = (uint32_t)wfd;

    if (write_bytes_to_user(pipefd_user, out, sizeof(out)) != 0) {
        fd_close(&cur->fdt, (uint64_t)rfd);
        fd_close(&cur->fdt, (uint64_t)wfd);
        return (uint64_t)(-(int64_t)EFAULT);
    }

    return 0;
}

uint64_t sys_newfstatat(int64_t dirfd, uint64_t pathname_user, uint64_t statbuf_user, uint64_t flags) {
    (void)flags;

    if (dirfd != AT_FDCWD) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (!user_range_ok(statbuf_user, (uint64_t)sizeof(linux_stat_t))) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    char in[MAX_PATH];
    if (copy_cstr_from_user(in, sizeof(in), pathname_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    proc_t *cur = &g_procs[g_cur_proc];
    char path[MAX_PATH];
    if (resolve_path(cur, in, path, sizeof(path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    if (resolve_final_symlink(path, sizeof(path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    if (cstr_eq_u64(path, "/proc") || cstr_eq_u64(path, "/proc/")) {
        linux_stat_t *st = (linux_stat_t *)(uintptr_t)statbuf_user;
        st->st_mode = S_IFDIR | 0555u;
        st->st_nlink = 1;
        st->st_size = 0;
        return 0;
    }
    if (cstr_eq_u64(path, "/proc/ps")) {
        linux_stat_t *st = (linux_stat_t *)(uintptr_t)statbuf_user;
        st->st_mode = S_IFREG | 0444u;
        st->st_nlink = 1;
        st->st_size = 0;
        return 0;
    }

    const uint8_t *data = 0;
    uint64_t size = 0;
    uint32_t mode = 0;
    if (vfs_lookup_abs(path, &data, &size, &mode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }

    (void)data;
    linux_stat_t st;
    /* Zero-init without libc */
    volatile uint8_t *zp = (volatile uint8_t *)&st;
    for (uint64_t i = 0; i < sizeof(st); i++) zp[i] = 0;

    st.st_dev = 0;
    st.st_ino = 1;
    st.st_nlink = 1;
    st.st_mode = mode;
    st.st_uid = 0;
    st.st_gid = 0;
    st.st_rdev = 0;
    st.st_size = (int64_t)size;
    st.st_blksize = 4096;
    st.st_blocks = (int64_t)((size + 511u) / 512u);

    /* Copy out */
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)statbuf_user;
    const volatile uint8_t *src = (const volatile uint8_t *)&st;
    for (uint64_t i = 0; i < sizeof(st); i++) dst[i] = src[i];
    return 0;
}

uint64_t sys_fchmodat(int64_t dirfd, uint64_t pathname_user, uint64_t mode, uint64_t flags) {
    if (dirfd != AT_FDCWD) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (flags != 0) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (pathname_user == 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    char in[MAX_PATH];
    if (copy_cstr_from_user(in, sizeof(in), pathname_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    proc_t *cur = &g_procs[g_cur_proc];
    char path[MAX_PATH];
    if (resolve_path(cur, in, path, sizeof(path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    if (resolve_final_symlink(path, sizeof(path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    /* procfs is read-only. */
    if (cstr_eq_u64(path, "/proc") || cstr_eq_u64(path, "/proc/") || cstr_eq_u64(path, "/proc/ps")) {
        return (uint64_t)(-(int64_t)EROFS);
    }

    /* Must exist. */
    uint32_t old_mode = 0;
    if (vfs_lookup_abs(path, 0, 0, &old_mode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }

    /* chmod updates permission bits (lowest 9 bits) while preserving the file type. */
    uint32_t new_mode = (old_mode & ~0777u) | (uint32_t)(mode & 0777u);

    /* Only overlay entries are mutable. */
    if (vfs_ramfile_set_mode_abs(path, new_mode) == 0) {
        return 0;
    }
    if (vfs_ramdir_set_mode_abs(path, new_mode) == 0) {
        return 0;
    }

    return (uint64_t)(-(int64_t)EROFS);
}

uint64_t sys_readlinkat(int64_t dirfd, uint64_t pathname_user, uint64_t buf_user, uint64_t bufsiz) {
    if (dirfd != AT_FDCWD) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (pathname_user == 0 || buf_user == 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    if (bufsiz == 0) {
        return 0;
    }
    if (!user_range_ok(buf_user, bufsiz)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    char in[MAX_PATH];
    if (copy_cstr_from_user(in, sizeof(in), pathname_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    proc_t *cur = &g_procs[g_cur_proc];
    char abs_path[MAX_PATH];
    if (resolve_path(cur, in, abs_path, sizeof(abs_path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    const uint8_t *data = 0;
    uint64_t size = 0;
    uint32_t mode = 0;
    if (vfs_lookup_abs(abs_path, &data, &size, &mode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }
    if ((mode & S_IFMT) != S_IFLNK) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    uint64_t n = (size < bufsiz) ? size : bufsiz;
    if (write_bytes_to_user(buf_user, data, n) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    return n;
}

uint64_t sys_unlinkat(int64_t dirfd, uint64_t pathname_user, uint64_t flags) {
    if (dirfd != AT_FDCWD) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (flags != 0 && flags != (uint64_t)AT_REMOVEDIR) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (pathname_user == 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    char in[MAX_PATH];
    if (copy_cstr_from_user(in, sizeof(in), pathname_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    proc_t *cur = &g_procs[g_cur_proc];
    char abs_path[MAX_PATH];
    if (resolve_path(cur, in, abs_path, sizeof(abs_path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    if (cstr_eq_u64(abs_path, "/")) {
        return (uint64_t)(-(int64_t)EISDIR);
    }

    const char *p = abs_path;
    while (*p == '/') p++;
    if (*p == '\0') {
        return (uint64_t)(-(int64_t)EISDIR);
    }

    if (flags == (uint64_t)AT_REMOVEDIR) {
        int drc = vfs_ramdir_remove(p);
        if (drc == 0) return 0;

        /* Propagate meaningful overlay errors (e.g. non-empty). */
        if (drc == -(int)ENOTEMPTY) {
            return (uint64_t)(int64_t)drc;
        }

        /* If it exists but is not a removable overlay dir, translate errors. */
        uint32_t mode = 0;
        if (vfs_lookup_abs(abs_path, 0, 0, &mode) == 0) {
            if (!S_ISDIR(mode)) {
                return (uint64_t)(-(int64_t)ENOTDIR);
            }
            return (uint64_t)(-(int64_t)EROFS);
        }
        return (uint64_t)(-(int64_t)ENOENT);
    }

    int rc = vfs_ramfile_unlink(p);
    if (rc == 0) return 0;

    /* If it exists but is not a ramfile, reject (read-only initramfs or directory). */
    uint32_t mode = 0;
    if (vfs_lookup_abs(abs_path, 0, 0, &mode) == 0) {
        if (S_ISDIR(mode)) {
            return (uint64_t)(-(int64_t)EISDIR);
        }
        return (uint64_t)(-(int64_t)EROFS);
    }

    return (uint64_t)(-(int64_t)ENOENT);
}
