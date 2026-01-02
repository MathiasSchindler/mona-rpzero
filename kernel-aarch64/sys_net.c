#include "syscalls.h"

#include "errno.h"
#include "fd.h"
#include "net.h"
#include "net_ipv6.h"
#include "net_udp6.h"
#include "proc.h"
#include "sched.h"
#include "sys_util.h"
#include "usb.h"
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

static int get_udp6_sock_id_from_fd(proc_t *p, uint64_t fd, uint32_t *out_sock_id) {
    if (!p || !out_sock_id) return -(int)EINVAL;
    int didx = fd_get_desc_idx(&p->fdt, fd);
    if (didx < 0) return -(int)EBADF;
    file_desc_t *d = &g_descs[didx];
    if (d->kind != FDESC_UDP6) return -(int)EBADF;
    *out_sock_id = d->u.udp6.sock_id;
    return 0;
}

uint64_t sys_mona_udp6_socket(void) {
    proc_t *cur = &g_procs[g_cur_proc];

    uint32_t sock_id = 0;
    int rc = net_udp6_socket_alloc(&sock_id);
    if (rc < 0) return (uint64_t)(int64_t)rc;

    int didx = desc_alloc();
    if (didx < 0) {
        net_udp6_on_desc_decref(sock_id);
        return (uint64_t)(-(int64_t)EMFILE);
    }

    desc_clear(&g_descs[didx]);
    g_descs[didx].kind = FDESC_UDP6;
    g_descs[didx].refs = 1;
    g_descs[didx].u.udp6.sock_id = sock_id;

    int fd = fd_alloc_into(&cur->fdt, 0, didx);
    /* fd_alloc_into() increments refs; drop our creation ref. */
    desc_decref(didx);

    if (fd < 0) {
        /* Dropping the desc ref above may already free the socket; ensure no leak. */
        return (uint64_t)(-(int64_t)EMFILE);
    }

    return (uint64_t)fd;
}

uint64_t sys_mona_udp6_bind(uint64_t fd, uint64_t port) {
    proc_t *cur = &g_procs[g_cur_proc];
    uint32_t sock_id = 0;
    int rc = get_udp6_sock_id_from_fd(cur, fd, &sock_id);
    if (rc < 0) return (uint64_t)(int64_t)rc;
    if (port > 65535ull) return (uint64_t)(-(int64_t)EINVAL);
    rc = net_udp6_bind(sock_id, (uint16_t)port);
    return (rc < 0) ? (uint64_t)(int64_t)rc : 0;
}

uint64_t sys_mona_udp6_sendto(uint64_t fd,
                              uint64_t dst_ip_user,
                              uint64_t dst_port,
                              uint64_t buf_user,
                              uint64_t len) {
    proc_t *cur = &g_procs[g_cur_proc];
    uint32_t sock_id = 0;
    int rc = get_udp6_sock_id_from_fd(cur, fd, &sock_id);
    if (rc < 0) return (uint64_t)(int64_t)rc;

    if (dst_port > 65535ull) return (uint64_t)(-(int64_t)EINVAL);
    if (len > (uint64_t)UDP6_MAX_PAYLOAD) return (uint64_t)(-(int64_t)EMSGSIZE);

    uint8_t dst_ip[16];
    if (read_bytes_from_user(dst_ip, sizeof(dst_ip), dst_ip_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    uint8_t payload[UDP6_MAX_PAYLOAD];
    if (len != 0) {
        if (read_bytes_from_user(payload, len, buf_user) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
    }

    rc = net_udp6_sendto(sock_id, dst_ip, (uint16_t)dst_port, payload, (size_t)len);
    return (uint64_t)(int64_t)rc;
}

uint64_t sys_mona_udp6_recvfrom(trap_frame_t *tf,
                                uint64_t fd,
                                uint64_t buf_user,
                                uint64_t len,
                                uint64_t src_ip_user,
                                uint64_t src_port_user,
                                uint64_t timeout_ms,
                                uint64_t elr) {
    proc_t *cur = &g_procs[g_cur_proc];

    if (cur->pending_udp6_recv) {
        return (uint64_t)(-(int64_t)EBUSY);
    }

    if (len != 0 && !user_range_ok(buf_user, len)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    if (src_ip_user != 0 && !user_range_ok(src_ip_user, 16)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    if (src_port_user != 0 && !user_range_ok(src_port_user, 2)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    uint32_t sock_id = 0;
    int rc = get_udp6_sock_id_from_fd(cur, fd, &sock_id);
    if (rc < 0) return (uint64_t)(int64_t)rc;

    udp6_dgram_t dg;
    rc = net_udp6_try_recv(sock_id, &dg);
    if (rc == 0) {
        uint64_t n = len;
        if (n > (uint64_t)dg.len) n = (uint64_t)dg.len;
        if (n != 0) {
            if (write_bytes_to_user(buf_user, dg.data, n) != 0) {
                return (uint64_t)(-(int64_t)EFAULT);
            }
        }
        if (src_ip_user != 0) {
            (void)write_bytes_to_user(src_ip_user, dg.src_ip, 16);
        }
        if (src_port_user != 0) {
            (void)write_u16_to_user(src_port_user, dg.src_port);
        }
        return n;
    }

    if (rc != -(int)EAGAIN) {
        return (uint64_t)(int64_t)rc;
    }

    /* Arm pending recv state and block. */
    tf_copy(&cur->tf, tf);
    cur->elr = elr;

    cur->pending_udp6_recv = 1;
    cur->pending_udp6_sock_id = sock_id;
    cur->pending_udp6_fd = fd;
    cur->pending_udp6_buf_user = buf_user;
    cur->pending_udp6_len = len;
    cur->pending_udp6_src_ip_user = src_ip_user;
    cur->pending_udp6_src_port_user = src_port_user;
    cur->pending_udp6_ret = 0;

    if (timeout_ms != 0) {
        uint64_t now = time_now_ns();
        uint64_t timeout_ns = timeout_ms * 1000000ull;
        uint64_t deadline = now + timeout_ns;
        if (deadline < now) deadline = 0xFFFFFFFFFFFFFFFFull;
        cur->sleep_deadline_ns = deadline;
        cur->state = PROC_SLEEPING;
    } else {
        cur->sleep_deadline_ns = 0;
        cur->state = PROC_BLOCKED_IO;
    }

    /* Like ping6, ensure forward progress even if the system is otherwise idle.
     * USB net RX is polled; without explicit polling here, a task blocked in
     * recvfrom can time out even though the host replied.
     */
    uint64_t start_ns = time_now_ns();
    uint64_t deadline_ns = cur->sleep_deadline_ns;
    if (timeout_ms != 0) {
        /* If time isn't available, we can't enforce a deadline; treat as blocking. */
        if (start_ns == 0) deadline_ns = 0;
    }

retry_wait:
    /* Pull in any pending USB net traffic before trying to sleep/yield.
     * This reduces the chance of missing a fast DNS reply.
     */
#if defined(ENABLE_USB_KBD) || defined(ENABLE_USB_NET)
    usb_poll();
#endif

    int next = sched_pick_next_runnable();
    if (next >= 0 && next != g_cur_proc) {
        proc_switch_to(next, tf);
        return SYSCALL_SWITCHED;
    }

    /* No other runnable tasks: wait (idle) and retry inline completion. */
    udp6_dgram_t dg2;
    int trc = net_udp6_try_recv(sock_id, &dg2);
    if (trc == 0) {
        uint64_t n = len;
        if (n > (uint64_t)dg2.len) n = (uint64_t)dg2.len;
        if (n != 0) {
            if (write_bytes_to_user(buf_user, dg2.data, n) != 0) {
                n = 0;
                trc = -(int)EFAULT;
            }
        }
        if (trc == 0) {
            if (src_ip_user != 0) {
                (void)write_bytes_to_user(src_ip_user, dg2.src_ip, 16);
            }
            if (src_port_user != 0) {
                (void)write_u16_to_user(src_port_user, dg2.src_port);
            }
        }

        cur->pending_udp6_recv = 0;
        cur->pending_udp6_sock_id = 0;
        cur->pending_udp6_fd = 0;
        cur->pending_udp6_buf_user = 0;
        cur->pending_udp6_len = 0;
        cur->pending_udp6_src_ip_user = 0;
        cur->pending_udp6_src_port_user = 0;
        cur->pending_udp6_ret = 0;
        cur->sleep_deadline_ns = 0;
        cur->state = PROC_RUNNABLE;

        if (trc != 0) return (uint64_t)(int64_t)trc;
        return n;
    }

    if (trc == -(int)EAGAIN) {
        if (deadline_ns != 0) {
            uint64_t now = time_now_ns();
            if (now != 0 && now >= deadline_ns) {
                cur->pending_udp6_recv = 0;
                cur->pending_udp6_sock_id = 0;
                cur->pending_udp6_fd = 0;
                cur->pending_udp6_buf_user = 0;
                cur->pending_udp6_len = 0;
                cur->pending_udp6_src_ip_user = 0;
                cur->pending_udp6_src_port_user = 0;
                cur->pending_udp6_ret = 0;
                cur->sleep_deadline_ns = 0;
                cur->state = PROC_RUNNABLE;
                return (uint64_t)(-(int64_t)ETIMEDOUT);
            }
        }
        goto retry_wait;
    }

    /* Error. */
    cur->pending_udp6_recv = 0;
    cur->pending_udp6_sock_id = 0;
    cur->pending_udp6_fd = 0;
    cur->pending_udp6_buf_user = 0;
    cur->pending_udp6_len = 0;
    cur->pending_udp6_src_ip_user = 0;
    cur->pending_udp6_src_port_user = 0;
    cur->pending_udp6_ret = 0;
    cur->sleep_deadline_ns = 0;
    cur->state = PROC_RUNNABLE;
    return (uint64_t)(int64_t)trc;
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

    uint64_t deadline = now + timeout_ns;
    if (deadline < now) deadline = 0xFFFFFFFFFFFFFFFFull;
    cur->sleep_deadline_ns = deadline;
    cur->state = PROC_SLEEPING;

    int rc = net_ipv6_ping6_start(g_cur_proc, nif, dst_ip, (uint16_t)ident, (uint16_t)seq);
    if (rc < 0) {
        /* If the network isn't configured yet (SLAAC/RA pending), wait/retry within the timeout. */
        if (rc != -(int)EAGAIN && rc != -(int)EBUSY) {
            cur->pending_ping6 = 0;
            cur->state = PROC_RUNNABLE;
            cur->sleep_deadline_ns = 0;
            return (uint64_t)(int64_t)rc;
        }
    }

    /* Busy-wait polling path:
     * Userland runs with IRQs masked, so timer-driven USB polling may not run.
     * For early bring-up and tests, explicitly poll USB here until completion
     * or timeout.
     */
    for (;;) {
        if (!cur->pending_ping6) {
            return cur->tf.x[0];
        }

        if (!cur->ping6_done && cur->ping6_start_ns == 0) {
            int trc = net_ipv6_ping6_start(g_cur_proc, nif, dst_ip, (uint16_t)ident, (uint16_t)seq);
            if (trc < 0 && trc != -(int)EAGAIN && trc != -(int)EBUSY) {
                cur->ping6_done = 1;
                cur->ping6_ret = (uint64_t)(int64_t)trc;
                net_ipv6_ping6_cancel(g_cur_proc);
            }
        }

#if defined(ENABLE_USB_KBD) || defined(ENABLE_USB_NET)
        usb_poll();
#endif

        if (cur->ping6_done) {
            break;
        }

        uint64_t tnow = time_now_ns();
        if (cur->sleep_deadline_ns != 0 && tnow != 0 && tnow >= cur->sleep_deadline_ns) {
            cur->ping6_done = 1;
            cur->ping6_ret = (uint64_t)(-(int64_t)ETIMEDOUT);
            cur->ping6_rtt_ns = 0;
            net_ipv6_ping6_cancel(g_cur_proc);
            break;
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

uint64_t sys_mona_net6_get_dns(uint64_t out_ip_user) {
    if (!user_range_ok(out_ip_user, 16)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    netif_t *nif = netif_get(0);
    if (!nif) {
        return (uint64_t)(-(int64_t)ENODEV);
    }

    if (!nif->ipv6_dns_valid) {
        return (uint64_t)(-(int64_t)ENOENT);
    }

    if (write_bytes_to_user(out_ip_user, nif->ipv6_dns, 16) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    return 0;
}
