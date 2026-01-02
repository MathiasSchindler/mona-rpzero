#include "sched.h"

#include "cache.h"
#include "console_in.h"
#include "irq.h"
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

static int sched_wake_one_console_reader_if_ready(void) {
    if (!console_in_has_data()) {
        return -1;
    }

    /* Wake at most one blocked reader per pass to avoid stampedes. */
    for (int step = 1; step <= (int)MAX_PROCS; step++) {
        int idx = (g_last_sched + step) % (int)MAX_PROCS;
        if (g_procs[idx].state == PROC_BLOCKED_IO && g_procs[idx].pending_console_read) {
            g_procs[idx].state = PROC_RUNNABLE;
            return idx;
        }
    }
    return -1;
}

static void sched_complete_console_read_if_needed(proc_t *p) {
    if (!p) return;
    if (!p->pending_console_read) return;
    if (p->pending_read_len == 0) {
        p->tf.x[0] = 0;
        p->pending_console_read = 0;
        p->pending_read_fd = 0;
        p->pending_read_buf_user = 0;
        p->pending_read_len = 0;
        return;
    }

    volatile char *dst = (volatile char *)(uintptr_t)p->pending_read_buf_user;
    uint64_t len = p->pending_read_len;

    char c;
    if (!console_in_pop(&c)) {
        /* Nothing to complete yet: keep it pending and block again. */
        p->state = PROC_BLOCKED_IO;
        return;
    }

    dst[0] = c;
    uint64_t n = 1;
    for (; n < len; n++) {
        char t;
        if (!console_in_pop(&t)) break;
        dst[n] = t;
    }

    p->tf.x[0] = n;
    p->pending_console_read = 0;
    p->pending_read_fd = 0;
    p->pending_read_buf_user = 0;
    p->pending_read_len = 0;
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
    for (;;) {
        /* Bring in any new input (UART, optional USB kbd). */
        console_in_poll();

        /* Wake any sleepers whose deadline has passed. */
        sched_wake_sleepers();

        /* Wake one console reader if buffered input exists. */
        int woke = sched_wake_one_console_reader_if_ready();
        if (woke >= 0 && g_procs[woke].state == PROC_RUNNABLE) {
            g_last_sched = woke;
            return woke;
        }

        for (int step = 1; step <= (int)MAX_PROCS; step++) {
            int idx = (g_last_sched + step) % (int)MAX_PROCS;
            if (g_procs[idx].state == PROC_RUNNABLE) {
                g_last_sched = idx;
                return idx;
            }
        }

        /* No runnable tasks. If there are sleepers or blocked IO, enter low-power
         * idle and wait for the periodic timer tick to wake us.
         */
        uint64_t earliest = 0;
        int has_sleepers = sched_any_sleepers(&earliest);
        int has_blocked_io = 0;
        for (int i = 0; i < (int)MAX_PROCS; i++) {
            if (g_procs[i].state == PROC_BLOCKED_IO) {
                has_blocked_io = 1;
                break;
            }
        }

        if (!has_sleepers && !has_blocked_io) {
            return -1;
        }

        /* Tickless idle policy:
         * - If sleepers exist: program a one-shot tick for the next deadline.
         * - If only blocked console I/O exists:
         *   - if input backends require polling, keep periodic ticks.
         *   - else, disable the tick and wait for IRQ-driven input.
         */
        if (has_sleepers) {
            uint64_t now = time_now_ns();
            if (earliest > now) {
                time_tick_schedule_oneshot_ns(earliest - now);
            } else {
                /* Deadline already passed; loop will wake sleepers. */
                time_tick_schedule_oneshot_ns(1);
            }
        } else if (has_blocked_io) {
            if (!console_in_needs_polling()) {
                /* IRQ-driven input (e.g. UART RX) will wake us without a tick. */
                time_tick_disable();
            } else {
                /* Polling backends (USB kbd): wake up only when polling is due. */
                uint64_t now = time_now_ns();
                uint64_t next = console_in_next_poll_deadline_ns();
                if (now != 0 && next != 0) {
                    if (next > now) {
                        time_tick_schedule_oneshot_ns(next - now);
                    } else {
                        time_tick_schedule_oneshot_ns(1);
                    }
                } else {
                    /* Fallback: if we can't compute deadlines, keep a periodic tick. */
                    time_tick_enable_periodic();
                }
            }
        }

        irq_enable();
        cpu_wfi();
        irq_disable();
    }
}

void proc_switch_to(int idx, trap_frame_t *tf) {
    g_cur_proc = idx;

    /* With per-process TTBR0 but no ASIDs, user VA caching can alias across
     * processes. Flush caches on switch to avoid stale instructions/data.
     */
    cache_clean_invalidate_all();

    mmu_ttbr0_write(g_procs[idx].ttbr0_pa);

    /* Complete any pending console read now that this process address space is active. */
    sched_complete_console_read_if_needed(&g_procs[idx]);

    write_elr_el1(g_procs[idx].elr);
    tf_copy(tf, &g_procs[idx].tf);
}

void sched_maybe_switch(trap_frame_t *tf) {
    int next = sched_pick_next_runnable();
    if (next >= 0 && next != g_cur_proc) {
        proc_switch_to(next, tf);
    }
}
