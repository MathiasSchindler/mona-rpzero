#pragma once

#include "stdint.h"

/* Pipe implementation used by FDESC_PIPE.
 * The pipe buffer and refcounts are kernel-internal.
 */

#define PIPE_END_READ 0u
#define PIPE_END_WRITE 1u

void pipe_init(void);

/* Allocate a new pipe. On success returns 0 and writes *out_pipe_id.
 * On failure returns -errno.
 */
int pipe_create(uint32_t *out_pipe_id);

/* Abort and free a pipe created by pipe_create() if descriptor setup fails.
 * Safe to call multiple times.
 */
void pipe_abort(uint32_t pipe_id);

/* Hook for file-desc refcount changes (dup/fork/close paths). */
void pipe_on_desc_incref(uint32_t pipe_id, uint32_t end);
void pipe_on_desc_decref(uint32_t pipe_id, uint32_t end);

/* Data movement primitives used by sys_read/sys_write.
 * Return >=0 bytes, or -errno.
 */
int64_t pipe_read(uint32_t pipe_id, volatile uint8_t *dst, uint64_t len);
int64_t pipe_write(uint32_t pipe_id, const volatile uint8_t *src, uint64_t len);
