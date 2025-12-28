#include "pipe.h"

/* Keep errno values consistent with exceptions.c. */
#define EBADF 9
#define EAGAIN 11
#define EPIPE 32
#define ENOMEM 12

enum {
    MAX_PIPES = 16,
    PIPE_BUF = 1024,
};

typedef struct {
    uint8_t used;
    uint8_t buf[PIPE_BUF];
    uint32_t rpos;
    uint32_t wpos;
    uint32_t count;
    uint32_t read_refs;
    uint32_t write_refs;
} pipe_t;

static pipe_t g_pipes[MAX_PIPES];

void pipe_init(void) {
    for (uint32_t i = 0; i < (uint32_t)MAX_PIPES; i++) {
        g_pipes[i].used = 0;
        g_pipes[i].rpos = 0;
        g_pipes[i].wpos = 0;
        g_pipes[i].count = 0;
        g_pipes[i].read_refs = 0;
        g_pipes[i].write_refs = 0;
    }
}

int pipe_create(uint32_t *out_pipe_id) {
    if (!out_pipe_id) return -(int)EBADF;

    int pid = -1;
    for (int i = 0; i < (int)MAX_PIPES; i++) {
        if (!g_pipes[i].used) {
            pid = i;
            break;
        }
    }
    if (pid < 0) return -(int)ENOMEM;

    g_pipes[pid].used = 1;
    g_pipes[pid].rpos = 0;
    g_pipes[pid].wpos = 0;
    g_pipes[pid].count = 0;
    g_pipes[pid].read_refs = 0;
    g_pipes[pid].write_refs = 0;

    *out_pipe_id = (uint32_t)pid;
    return 0;
}

void pipe_abort(uint32_t pipe_id) {
    if (pipe_id >= (uint32_t)MAX_PIPES) return;
    g_pipes[pipe_id].used = 0;
    g_pipes[pipe_id].rpos = 0;
    g_pipes[pipe_id].wpos = 0;
    g_pipes[pipe_id].count = 0;
    g_pipes[pipe_id].read_refs = 0;
    g_pipes[pipe_id].write_refs = 0;
}

static inline void pipe_maybe_free(uint32_t pipe_id) {
    if (pipe_id >= (uint32_t)MAX_PIPES) return;
    pipe_t *pp = &g_pipes[pipe_id];
    if (!pp->used) return;
    if (pp->read_refs == 0 && pp->write_refs == 0) {
        pp->used = 0;
        pp->rpos = 0;
        pp->wpos = 0;
        pp->count = 0;
    }
}

void pipe_on_desc_incref(uint32_t pipe_id, uint32_t end) {
    if (pipe_id >= (uint32_t)MAX_PIPES) return;
    pipe_t *pp = &g_pipes[pipe_id];
    if (!pp->used) return;

    if (end == PIPE_END_READ) pp->read_refs++;
    else if (end == PIPE_END_WRITE) pp->write_refs++;
}

void pipe_on_desc_decref(uint32_t pipe_id, uint32_t end) {
    if (pipe_id >= (uint32_t)MAX_PIPES) return;
    pipe_t *pp = &g_pipes[pipe_id];
    if (!pp->used) return;

    if (end == PIPE_END_READ) {
        if (pp->read_refs > 0) pp->read_refs--;
    } else if (end == PIPE_END_WRITE) {
        if (pp->write_refs > 0) pp->write_refs--;
    }

    pipe_maybe_free(pipe_id);
}

int64_t pipe_read(uint32_t pipe_id, volatile uint8_t *dst, uint64_t len) {
    if (pipe_id >= (uint32_t)MAX_PIPES) return -(int64_t)EBADF;
    pipe_t *pp = &g_pipes[pipe_id];
    if (!pp->used) return -(int64_t)EBADF;

    if (len == 0) return 0;
    if (!dst) return -(int64_t)EBADF;

    if (pp->count == 0) {
        if (pp->write_refs == 0) return 0; /* EOF */
        return -(int64_t)EAGAIN;
    }

    uint64_t n = (len < (uint64_t)pp->count) ? len : (uint64_t)pp->count;
    for (uint64_t i = 0; i < n; i++) {
        dst[i] = pp->buf[pp->rpos];
        pp->rpos = (pp->rpos + 1u) % PIPE_BUF;
    }
    pp->count -= (uint32_t)n;
    return (int64_t)n;
}

int64_t pipe_write(uint32_t pipe_id, const volatile uint8_t *src, uint64_t len) {
    if (pipe_id >= (uint32_t)MAX_PIPES) return -(int64_t)EBADF;
    pipe_t *pp = &g_pipes[pipe_id];
    if (!pp->used) return -(int64_t)EBADF;

    if (len == 0) return 0;
    if (!src) return -(int64_t)EBADF;

    if (pp->read_refs == 0) {
        return -(int64_t)EPIPE;
    }

    if (pp->count == PIPE_BUF) {
        return -(int64_t)EAGAIN;
    }

    uint64_t space = (uint64_t)(PIPE_BUF - pp->count);
    uint64_t n = (len < space) ? len : space;

    for (uint64_t i = 0; i < n; i++) {
        pp->buf[pp->wpos] = src[i];
        pp->wpos = (pp->wpos + 1u) % PIPE_BUF;
    }
    pp->count += (uint32_t)n;
    return (int64_t)n;
}
