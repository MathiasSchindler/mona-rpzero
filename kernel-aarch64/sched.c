#include "sched.h"

#include "cache.h"
#include "console_in.h"
#include "errno.h"
#include "irq.h"
#include "mmu.h"
#include "net_udp6.h"
#include "proc.h"
#include "regs.h"
#include "sys_util.h"
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

static void sched_complete_ping6_if_needed(proc_t *p) {
    if (!p) return;
    if (!p->pending_ping6) return;

    if (!p->ping6_done) {
        /* If we're runnable but still marked pending, the sleep deadline fired. */
        uint64_t now = time_now_ns();
        if (p->sleep_deadline_ns != 0 && now >= p->sleep_deadline_ns) {
            p->ping6_done = 1;
            p->ping6_ret = (uint64_t)(-(int64_t)ETIMEDOUT);
            p->ping6_rtt_ns = 0;
            p->sleep_deadline_ns = 0;
        } else {
            /* Still waiting; keep it pending. */
            return;
        }
    }

    /* Complete the syscall return value and optional RTT writeback. */
    if (p->ping6_ret == 0 && p->ping6_rtt_user != 0) {
        (void)write_u64_to_user(p->ping6_rtt_user, p->ping6_rtt_ns);
    }

    p->tf.x[0] = p->ping6_ret;

    p->pending_ping6 = 0;
    p->ping6_done = 0;
    p->ping6_ident = 0;
    p->ping6_seq = 0;
    for (uint64_t i = 0; i < 16; i++) p->ping6_dst_ip[i] = 0;
    p->ping6_start_ns = 0;
    p->ping6_rtt_ns = 0;
    p->ping6_rtt_user = 0;
    p->ping6_ret = 0;
}

static void sched_complete_udp6_recv_if_needed(proc_t *p) {
    if (!p) return;
    if (!p->pending_udp6_recv) return;

    udp6_dgram_t dg;
    int rc = net_udp6_try_recv(p->pending_udp6_sock_id, &dg);
    if (rc == 0) {
        uint64_t n = p->pending_udp6_len;
        if (n > (uint64_t)dg.len) n = (uint64_t)dg.len;

        if (n != 0) {
            if (write_bytes_to_user(p->pending_udp6_buf_user, dg.data, n) != 0) {
                p->tf.x[0] = (uint64_t)(-(int64_t)EFAULT);
            } else {
                p->tf.x[0] = n;
            }
        } else {
            p->tf.x[0] = 0;
        }

        if (p->pending_udp6_src_ip_user != 0) {
            (void)write_bytes_to_user(p->pending_udp6_src_ip_user, dg.src_ip, 16);
        }
        if (p->pending_udp6_src_port_user != 0) {
            (void)write_u16_to_user(p->pending_udp6_src_port_user, dg.src_port);
        }

        p->pending_udp6_recv = 0;
        p->pending_udp6_sock_id = 0;
        p->pending_udp6_fd = 0;
        p->pending_udp6_buf_user = 0;
        p->pending_udp6_len = 0;
        p->pending_udp6_src_ip_user = 0;
        p->pending_udp6_src_port_user = 0;
        p->pending_udp6_ret = 0;
        p->sleep_deadline_ns = 0;
        return;
    }

    if (rc == -(int)EAGAIN) {
        uint64_t now = time_now_ns();
        if (p->sleep_deadline_ns != 0 && now >= p->sleep_deadline_ns) {
            p->tf.x[0] = (uint64_t)(-(int64_t)ETIMEDOUT);
            p->pending_udp6_recv = 0;
            p->pending_udp6_sock_id = 0;
            p->pending_udp6_fd = 0;
            p->pending_udp6_buf_user = 0;
            p->pending_udp6_len = 0;
            p->pending_udp6_src_ip_user = 0;
            p->pending_udp6_src_port_user = 0;
            p->pending_udp6_ret = 0;
            p->sleep_deadline_ns = 0;
            return;
        }

        /* Still waiting; keep blocked. */
        if (p->sleep_deadline_ns != 0) p->state = PROC_SLEEPING;
        else p->state = PROC_BLOCKED_IO;
        return;
    }

    /* Error: complete with errno. */
    p->tf.x[0] = (uint64_t)(int64_t)rc;
    p->pending_udp6_recv = 0;
    p->pending_udp6_sock_id = 0;
    p->pending_udp6_fd = 0;
    p->pending_udp6_buf_user = 0;
    p->pending_udp6_len = 0;
    p->pending_udp6_src_ip_user = 0;
    p->pending_udp6_src_port_user = 0;
    p->pending_udp6_ret = 0;
    p->sleep_deadline_ns = 0;
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
         * - If sleepers exist: wake at the earliest sleep deadline.
         * - If blocked console I/O exists:
         *   - for IRQ-driven input, no tick is needed (UART RX IRQ wakes us).
         *   - for polling input (USB kbd), also wake at the next poll deadline.
         *
         * When both sleepers and polling I/O exist, we must wake for the
         * earliest of (sleep deadline, poll deadline) to keep input responsive.
         */
        if (has_blocked_io && !console_in_needs_polling()) {
            if (!has_sleepers) {
                /* Only IRQ-driven input can wake us; no tick required. */
                time_tick_disable();
            }
        } else {
            uint64_t now = time_now_ns();
            if (now == 0) {
                /* If we can't compute deadlines, keep a periodic tick. */
                time_tick_enable_periodic();
            } else {
                uint64_t wake_ns = 0;
                if (has_sleepers) {
                    wake_ns = earliest;
                }

                if (has_blocked_io && console_in_needs_polling()) {
                    uint64_t next_poll = console_in_next_poll_deadline_ns();
                    if (next_poll == 0) {
                        /* No deadline yet; wake immediately to establish one. */
                        next_poll = now;
                    }
                    if (wake_ns == 0 || next_poll < wake_ns) {
                        wake_ns = next_poll;
                    }
                }

                if (wake_ns > now) {
                    time_tick_schedule_oneshot_ns(wake_ns - now);
                } else {
                    /* Deadline already passed; handle it on the next loop. */
                    time_tick_schedule_oneshot_ns(1);
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

    /* Complete any pending ping6 syscall now that this process address space is active. */
    sched_complete_ping6_if_needed(&g_procs[idx]);

    /* Complete any pending udp6 recvfrom syscall now that this process address space is active. */
    sched_complete_udp6_recv_if_needed(&g_procs[idx]);

    write_elr_el1(g_procs[idx].elr);
    tf_copy(tf, &g_procs[idx].tf);
}

void sched_maybe_switch(trap_frame_t *tf) {
    int next = sched_pick_next_runnable();
    if (next >= 0 && next != g_cur_proc) {
        proc_switch_to(next, tf);
    }
}
