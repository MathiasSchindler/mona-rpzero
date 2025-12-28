#pragma once

#include "stdint.h"

/* File descriptor layer.
 *
 * - Each process has an fd table mapping fd -> "file description" index.
 * - File descriptions are refcounted and shared across dup/fork.
 */

enum {
    MAX_FDS = 32,
    MAX_FILEDESCS = 64,
};

typedef enum {
    FDESC_UNUSED = 0,
    FDESC_UART = 1,
    FDESC_INITRAMFS = 2,
    FDESC_PIPE = 3,
} fdesc_kind_t;

typedef struct {
    uint32_t kind;
    uint32_t refs;
    union {
        struct {
            const uint8_t *data;
            uint64_t size;
            uint64_t off;
            uint32_t mode;
            uint8_t is_dir;
            char dir_path[128];
        } initramfs;
        struct {
            uint32_t pipe_id;
            uint32_t end;
        } pipe;
        struct {
            uint32_t unused;
        } uart;
    } u;
} file_desc_t;

typedef struct {
    int16_t fd_to_desc[MAX_FDS];
} fd_table_t;

extern file_desc_t g_descs[MAX_FILEDESCS];

void fd_init(void);

void desc_clear(file_desc_t *d);
int desc_alloc(void);
void desc_incref(int didx);
void desc_decref(int didx);

int fd_get_desc_idx(fd_table_t *t, uint64_t fd);
int fd_alloc_into(fd_table_t *t, int min_fd, int didx);
void fd_close(fd_table_t *t, uint64_t fd);
