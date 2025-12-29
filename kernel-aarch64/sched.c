#include "sched.h"

#include "cache.h"
#include "mmu.h"
#include "proc.h"
#include "regs.h"

int sched_pick_next_runnable(void) {
    for (int step = 1; step <= (int)MAX_PROCS; step++) {
        int idx = (g_last_sched + step) % (int)MAX_PROCS;
        if (g_procs[idx].state == PROC_RUNNABLE) {
            g_last_sched = idx;
            return idx;
        }
    }
    return -1;
}

void proc_switch_to(int idx, trap_frame_t *tf) {
    g_cur_proc = idx;

    /* With per-process TTBR0 but no ASIDs, user VA caching can alias across
     * processes. Flush caches on switch to avoid stale instructions/data.
     */
    cache_clean_invalidate_all();

    mmu_ttbr0_write(g_procs[idx].ttbr0_pa);
    write_elr_el1(g_procs[idx].elr);
    tf_copy(tf, &g_procs[idx].tf);
}

void sched_maybe_switch(trap_frame_t *tf) {
    int next = sched_pick_next_runnable();
    if (next >= 0 && next != g_cur_proc) {
        proc_switch_to(next, tf);
    }
}
