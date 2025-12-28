#pragma once

#include "exceptions.h"
#include "fd.h"
#include "stdint.h"

enum {
    MAX_PROCS = 16,
    MAX_VMAS = 32,
    MAX_PATH = 256,
};

typedef struct {
    uint8_t used;
    uint64_t base;
    uint64_t len;
} vma_t;

typedef enum {
    PROC_UNUSED = 0,
    PROC_RUNNABLE = 1,
    PROC_WAITING = 2,
    PROC_ZOMBIE = 3,
} proc_state_t;

typedef struct {
    uint64_t pid;
    uint64_t ppid;
    proc_state_t state;
    uint64_t ttbr0_pa;

    uint64_t user_pa_base;
    uint64_t heap_base;
    uint64_t heap_end;
    uint64_t stack_low;
    char cwd[MAX_PATH];
    uint64_t mmap_next;
    vma_t vmas[MAX_VMAS];
    trap_frame_t tf;
    uint64_t elr;
    uint64_t exit_code;
    uint64_t clear_child_tid_user;
    int64_t wait_target_pid;
    uint64_t wait_status_user;
    fd_table_t fdt;
} proc_t;

extern proc_t g_procs[MAX_PROCS];
extern int g_cur_proc;
extern int g_last_sched;
extern uint64_t g_next_pid;

static inline proc_t *proc_current(void) {
    return &g_procs[g_cur_proc];
}

void tf_copy(trap_frame_t *dst, const trap_frame_t *src);
void tf_zero(trap_frame_t *tf);

void proc_clear(proc_t *p);
void proc_close_all_fds(proc_t *p);
void proc_init_if_needed(uint64_t elr, trap_frame_t *tf);
int proc_find_free_slot(void);
