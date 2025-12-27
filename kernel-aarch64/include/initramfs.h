#pragma once

#include "stddef.h"
#include "stdint.h"

void initramfs_init(const void *archive, size_t archive_size);

/* Returns 0 on success, -1 if not found. */
int initramfs_lookup(const char *path, const uint8_t **out_data, uint64_t *out_size, uint32_t *out_mode);

/*
 * Enumerate direct children of a directory path.
 * The callback receives each child name (NUL-terminated) and a mode (S_IFDIR/S_IFREG + perms).
 */
typedef int (*initramfs_dir_cb_t)(const char *name, uint32_t mode, void *ctx);
int initramfs_list_dir(const char *dir_path, initramfs_dir_cb_t cb, void *ctx);
