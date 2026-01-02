#include "proc.h"

#include "mmu.h"
#include "pipe.h"
#include "vfs.h"

uint64_t g_next_pid = 1;
proc_t g_procs[MAX_PROCS];
int g_cur_proc = 0;
int g_last_sched = 0;

static int g_proc_inited = 0;

void tf_copy(trap_frame_t *dst, const trap_frame_t *src) {
    for (uint64_t i = 0; i < 31; i++) {
        dst->x[i] = src->x[i];
    }
    dst->sp_el0 = src->sp_el0;
}

uint64_t proc_current_elr_value(void) {
    return g_procs[g_cur_proc].elr;
}

void tf_zero(trap_frame_t *tf) {
    for (uint64_t i = 0; i < 31; i++) {
        tf->x[i] = 0;
    }
    tf->sp_el0 = 0;
}

void proc_clear(proc_t *p) {
    p->pid = 0;
    p->ppid = 0;
    p->state = PROC_UNUSED;
    p->ttbr0_pa = 0;
    p->user_pa_base = 0;
    p->heap_base = 0;
    p->heap_end = 0;
    p->stack_low = 0;
    p->cwd[0] = '/';
    p->cwd[1] = '\0';
    p->mmap_next = 0;
    for (uint64_t i = 0; i < MAX_VMAS; i++) {
        p->vmas[i].used = 0;
        p->vmas[i].base = 0;
        p->vmas[i].len = 0;
    }
    tf_zero(&p->tf);
    p->elr = 0;
    p->exit_code = 0;
    p->clear_child_tid_user = 0;
    p->wait_target_pid = 0;
    p->wait_status_user = 0;
    p->sleep_deadline_ns = 0;
    p->pending_console_read = 0;
    p->pending_read_buf_user = 0;
    p->pending_read_len = 0;
    p->pending_read_fd = 0;

    p->pending_udp6_recv = 0;
    p->pending_udp6_sock_id = 0;
    p->pending_udp6_fd = 0;
    p->pending_udp6_buf_user = 0;
    p->pending_udp6_len = 0;
    p->pending_udp6_src_ip_user = 0;
    p->pending_udp6_src_port_user = 0;
    p->pending_udp6_ret = 0;

    p->pending_ping6 = 0;
    p->ping6_done = 0;
    p->ping6_ident = 0;
    p->ping6_seq = 0;
    for (uint64_t i = 0; i < 16; i++) p->ping6_dst_ip[i] = 0;
    p->ping6_start_ns = 0;
    p->ping6_rtt_ns = 0;
    p->ping6_rtt_user = 0;
    p->ping6_ret = 0;
    for (uint64_t i = 0; i < MAX_FDS; i++) {
        p->fdt.fd_to_desc[i] = -1;
    }
}

void proc_close_all_fds(proc_t *p) {
    if (!p) return;
    for (uint64_t fd = 0; fd < MAX_FDS; fd++) {
        fd_close(&p->fdt, fd);
    }
}

int proc_find_free_slot(void) {
    for (int i = 0; i < (int)MAX_PROCS; i++) {
        if (g_procs[i].state == PROC_UNUSED) return i;
    }
    return -1;
}

void proc_init_if_needed(uint64_t elr, trap_frame_t *tf) {
    if (g_proc_inited) return;

    pipe_init();
    fd_init();
    for (uint64_t i = 0; i < MAX_PROCS; i++) {
        proc_clear(&g_procs[i]);
    }

    vfs_init();

    /* Init process (pid 1) runs in the initial identity TTBR0. */
    g_cur_proc = 0;
    g_last_sched = 0;
    proc_clear(&g_procs[0]);
    g_procs[0].pid = g_next_pid++;
    g_procs[0].ppid = 0;
    g_procs[0].state = PROC_RUNNABLE;
    g_procs[0].ttbr0_pa = mmu_ttbr0_read();
    g_procs[0].user_pa_base = USER_REGION_BASE;
    /* Heap is initialized on first execve(). */
    g_procs[0].heap_base = 0;
    g_procs[0].heap_end = 0;
    g_procs[0].stack_low = tf->sp_el0;
    tf_copy(&g_procs[0].tf, tf);
    g_procs[0].elr = elr;

    /* Create a shared UART file description and install it as fd 0/1/2. */
    int uart_desc = desc_alloc();
    if (uart_desc >= 0) {
        g_descs[uart_desc].kind = FDESC_UART;
        g_descs[uart_desc].refs = 1;

        for (int i = 0; i < 3; i++) {
            g_procs[0].fdt.fd_to_desc[i] = (int16_t)uart_desc;
            desc_incref(uart_desc);
        }
        /* Balance initial refs=1 + 3 incref calls. */
        desc_decref(uart_desc);
    }

    g_proc_inited = 1;
}
