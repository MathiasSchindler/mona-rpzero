#pragma once

#include "initramfs.h"
#include "stdint.h"

/* initramfs + overlay VFS helpers.
 * Paths passed to vfs_lookup_abs must be normalized absolute paths.
 * dir_path_no_slash is normalized, no leading slash; "" represents root.
 */

void vfs_init(void);

int vfs_lookup_abs(const char *abs_path,
                   const uint8_t **out_data,
                   uint64_t *out_size,
                   uint32_t *out_mode);

int vfs_list_dir(const char *dir_path_no_slash, initramfs_dir_cb_t cb, void *ctx);

/* Create a directory entry in the in-memory overlay.
 * path_no_slash must be normalized, no leading slash, no trailing slash.
 * mode should include S_IFDIR bits (e.g. 0040000|0755).
 * Returns 0 on success, or -errno.
 */
int vfs_ramdir_create(const char *path_no_slash, uint32_t mode);

/* Remove an overlay directory. Directory must be empty (no overlay children). */
int vfs_ramdir_remove(const char *path_no_slash);

/* Regular file entries in the in-memory overlay.
 * path_no_slash must be normalized, no leading slash, no trailing slash.
 * mode should include S_IFREG bits (e.g. 0100000|0644).
 */
int vfs_ramfile_create(const char *path_no_slash, uint32_t mode);
int vfs_ramfile_unlink(const char *path_no_slash);

/* Lookup ramfile by absolute path. Returns 0 and sets out_id on success. */
int vfs_ramfile_find_abs(const char *abs_path, uint32_t *out_id);

/* Access ramfile storage by id (index). Returns 0 on success, or -errno. */
int vfs_ramfile_get(uint32_t id, uint8_t **out_data, uint64_t *out_size, uint64_t *out_cap, uint32_t *out_mode);
int vfs_ramfile_set_size(uint32_t id, uint64_t new_size);
