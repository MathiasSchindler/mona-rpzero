#include "sched.h"

#include "cache.h"
#include "mmu.h"
#include "proc.h"
#include "regs.h"
#include "time.h"

static void sched_wake_sleepers(void) {
    uint64_t now = time_now_ns();
    for (int i = 0; i < (int)MAX_PROCS; i++) {
        if (g_procs[i].state != PROC_SLEEPING) continue;
        if (now >= g_procs[i].sleep_deadline_ns) {
            g_procs[i].state = PROC_RUNNABLE;
            g_procs[i].sleep_deadline_ns = 0;
        }
    }
}

static int sched_any_sleepers(uint64_t *out_earliest_deadline_ns) {
    uint64_t earliest = 0;
    int any = 0;

    for (int i = 0; i < (int)MAX_PROCS; i++) {
        if (g_procs[i].state != PROC_SLEEPING) continue;
        uint64_t d = g_procs[i].sleep_deadline_ns;
        if (!any || d < earliest) {
            earliest = d;
            any = 1;
        }
    }

    if (any && out_earliest_deadline_ns) *out_earliest_deadline_ns = earliest;
    return any;
}

int sched_pick_next_runnable(void) {
    /* Wake any sleepers whose deadline has passed. */
    sched_wake_sleepers();

    for (int step = 1; step <= (int)MAX_PROCS; step++) {
        int idx = (g_last_sched + step) % (int)MAX_PROCS;
        if (g_procs[idx].state == PROC_RUNNABLE) {
            g_last_sched = idx;
            return idx;
        }
    }

    /* No runnable tasks. If we only have sleeping tasks, spin until the earliest
     * deadline and wake them. This keeps nanosleep functional without timer IRQs.
     */
    uint64_t earliest = 0;
    if (sched_any_sleepers(&earliest)) {
        while (time_now_ns() < earliest) {
            __asm__ volatile("nop");
        }
        sched_wake_sleepers();

        for (int step = 1; step <= (int)MAX_PROCS; step++) {
            int idx = (g_last_sched + step) % (int)MAX_PROCS;
            if (g_procs[idx].state == PROC_RUNNABLE) {
                g_last_sched = idx;
                return idx;
            }
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
