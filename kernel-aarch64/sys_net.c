#include "syscalls.h"

#include "errno.h"
#include "net.h"
#include "net_ipv6.h"
#include "proc.h"
#include "sched.h"
#include "sys_util.h"
#include "time.h"

/* mona-specific: synchronous ICMPv6 echo (ping6).
 *
 * Kernel starts NDP if needed, then sends an echo request and blocks the calling
 * task until an echo reply arrives or timeout fires.
 */

static int read_bytes_from_user(uint8_t *dst, uint64_t dst_len, uint64_t user_src) {
    if (!dst || dst_len == 0) return -1;
    if (!user_range_ok(user_src, dst_len)) return -1;
    for (uint64_t i = 0; i < dst_len; i++) {
        dst[i] = *(const volatile uint8_t *)(uintptr_t)(user_src + i);
    }
    return 0;
}

uint64_t sys_mona_ping6(trap_frame_t *tf,
                        uint64_t dst_ip_user,
                        uint64_t ident,
                        uint64_t seq,
                        uint64_t timeout_ms,
                        uint64_t rtt_ns_user,
                        uint64_t elr) {
    proc_t *cur = &g_procs[g_cur_proc];

    if (cur->pending_ping6) {
        return (uint64_t)(-(int64_t)EBUSY);
    }

    uint8_t dst_ip[16];
    if (read_bytes_from_user(dst_ip, sizeof(dst_ip), dst_ip_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    if (rtt_ns_user != 0 && !user_range_ok(rtt_ns_user, 8)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    netif_t *nif = netif_get(0);
    if (!nif) {
        return (uint64_t)(-(int64_t)ENODEV);
    }

    uint64_t now = time_now_ns();
    uint64_t timeout_ns = timeout_ms * 1000000ull;
    if (timeout_ns == 0) timeout_ns = 1000000000ull;

    /* Save user return state in case we block/switch. */
    tf_copy(&cur->tf, tf);
    cur->elr = elr;

    /* Arm pending ping6 state. */
    cur->pending_ping6 = 1;
    cur->ping6_done = 0;
    cur->ping6_ident = (uint16_t)ident;
    cur->ping6_seq = (uint16_t)seq;
    for (int i = 0; i < 16; i++) cur->ping6_dst_ip[i] = dst_ip[i];
    cur->ping6_start_ns = 0;
    cur->ping6_rtt_ns = 0;
    cur->ping6_rtt_user = rtt_ns_user;
    cur->ping6_ret = 0;

    cur->sleep_deadline_ns = (now == 0) ? 0 : (now + timeout_ns);
    cur->state = PROC_SLEEPING;

    int rc = net_ipv6_ping6_start(g_cur_proc, nif, dst_ip, (uint16_t)ident, (uint16_t)seq);
    if (rc < 0) {
        /* Fail fast, don't block. */
        cur->pending_ping6 = 0;
        cur->state = PROC_RUNNABLE;
        cur->sleep_deadline_ns = 0;
        return (uint64_t)(int64_t)rc;
    }

retry_wait:
    int next = sched_pick_next_runnable();
    if (next >= 0 && next != g_cur_proc) {
        proc_switch_to(next, tf);
        return SYSCALL_SWITCHED;
    }

    /* No other runnable tasks: sched_pick_next_runnable may have idled until
     * a packet or the deadline made us runnable again.
     */
    if (cur->state == PROC_SLEEPING) {
        cur->state = PROC_RUNNABLE;
    }

    if (!cur->pending_ping6) {
        /* Completed via scheduler completion hook.
         * In the "no other runnable" case, we didn't actually switch, so the
         * completion hook didn't run; complete inline.
         */
        return cur->tf.x[0];
    }

    /* Inline completion path (no context switch happened): check done/timeout. */
    if (!cur->ping6_done) {
        uint64_t tnow = time_now_ns();
        if (cur->sleep_deadline_ns != 0 && tnow >= cur->sleep_deadline_ns) {
            cur->ping6_done = 1;
            cur->ping6_ret = (uint64_t)(-(int64_t)ETIMEDOUT);
            cur->ping6_rtt_ns = 0;
        } else {
            /* Still pending: keep waiting. */
            goto retry_wait;
        }
    }

    if (cur->ping6_ret == 0 && cur->ping6_rtt_user != 0) {
        (void)write_u64_to_user(cur->ping6_rtt_user, cur->ping6_rtt_ns);
    }

    uint64_t ret = cur->ping6_ret;

    cur->pending_ping6 = 0;
    cur->ping6_done = 0;
    cur->ping6_ident = 0;
    cur->ping6_seq = 0;
    for (int i = 0; i < 16; i++) cur->ping6_dst_ip[i] = 0;
    cur->ping6_start_ns = 0;
    cur->ping6_rtt_ns = 0;
    cur->ping6_rtt_user = 0;
    cur->ping6_ret = 0;
    cur->sleep_deadline_ns = 0;

    return ret;
}
