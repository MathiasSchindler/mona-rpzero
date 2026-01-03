#include "fd.h"

#include "net_tcp6.h"
#include "net_udp6.h"
#include "pipe.h"

/* errno values (match exceptions.c) */
#define EBADF 9
#define EMFILE 24

typedef struct {
    fd_table_t fdt;
} fd_table_wrapper_t;

file_desc_t g_descs[MAX_FILEDESCS];

void desc_clear(file_desc_t *d) {
    d->kind = FDESC_UNUSED;
    d->refs = 0;
    d->u.initramfs.data = 0;
    d->u.initramfs.size = 0;
    d->u.initramfs.off = 0;
    d->u.initramfs.mode = 0;
    d->u.initramfs.is_dir = 0;
    d->u.initramfs.dir_path[0] = '\0';
    d->u.pipe.pipe_id = 0;
    d->u.pipe.end = 0;
    d->u.ramfile.file_id = 0;
    d->u.ramfile._pad = 0;
    d->u.ramfile.off = 0;
    d->u.proc.node = 0;
    d->u.proc._pad = 0;
    d->u.proc.off = 0;
    d->u.udp6.sock_id = 0;
    d->u.udp6._pad = 0;
    d->u.tcp6.conn_id = 0;
    d->u.tcp6._pad = 0;
}

void fd_init(void) {
    for (uint64_t i = 0; i < (uint64_t)MAX_FILEDESCS; i++) {
        desc_clear(&g_descs[i]);
    }
}

int desc_alloc(void) {
    for (int i = 0; i < (int)MAX_FILEDESCS; i++) {
        if (g_descs[i].refs == 0) {
            /* Reserve immediately so subsequent desc_alloc() calls cannot return the same slot. */
            desc_clear(&g_descs[i]);
            g_descs[i].refs = 1;
            return i;
        }
    }
    return -1;
}

void desc_incref(int didx) {
    if (didx < 0 || didx >= (int)MAX_FILEDESCS) return;
    file_desc_t *d = &g_descs[didx];
    if (d->refs == 0) return;
    d->refs++;

    if (d->kind == FDESC_PIPE) {
        pipe_on_desc_incref(d->u.pipe.pipe_id, d->u.pipe.end);
    }

    if (d->kind == FDESC_UDP6) {
        net_udp6_on_desc_incref(d->u.udp6.sock_id);
    }

    if (d->kind == FDESC_TCP6) {
        net_tcp6_on_desc_incref(d->u.tcp6.conn_id);
    }
}

void desc_decref(int didx) {
    if (didx < 0 || didx >= (int)MAX_FILEDESCS) return;
    file_desc_t *d = &g_descs[didx];
    if (d->refs == 0) return;

    if (d->kind == FDESC_PIPE) {
        pipe_on_desc_decref(d->u.pipe.pipe_id, d->u.pipe.end);
    }

    if (d->kind == FDESC_UDP6) {
        net_udp6_on_desc_decref(d->u.udp6.sock_id);
    }

    if (d->kind == FDESC_TCP6) {
        net_tcp6_on_desc_decref(d->u.tcp6.conn_id);
    }

    d->refs--;
    if (d->refs == 0) {
        desc_clear(d);
    }
}

int fd_get_desc_idx(fd_table_t *t, uint64_t fd) {
    if (!t) return -1;
    if (fd >= MAX_FDS) return -1;
    int didx = t->fd_to_desc[fd];
    if (didx < 0 || didx >= (int)MAX_FILEDESCS) return -1;
    if (g_descs[didx].refs == 0) return -1;
    return didx;
}

int fd_alloc_into(fd_table_t *t, int min_fd, int didx) {
    if (!t) return -1;
    if (min_fd < 0) min_fd = 0;
    for (int fd = min_fd; fd < (int)MAX_FDS; fd++) {
        if (t->fd_to_desc[fd] < 0) {
            t->fd_to_desc[fd] = (int16_t)didx;
            desc_incref(didx);
            return fd;
        }
    }
    return -1;
}

void fd_close(fd_table_t *t, uint64_t fd) {
    if (!t) return;
    if (fd >= MAX_FDS) return;
    int didx = t->fd_to_desc[fd];
    if (didx >= 0) {
        t->fd_to_desc[fd] = -1;
        desc_decref(didx);
    }
}
