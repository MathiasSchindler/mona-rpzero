#include "exceptions.h"

#include "errno.h"
#include "mmu.h"
#include "proc.h"
#include "sched.h"
#include "syscalls.h"
#include "syscall_numbers.h"
#include "uart_pl011.h"

#define PROC_TRACE 0

static void proc_trace(const char *msg, uint64_t a, uint64_t b) {
#if PROC_TRACE
    uart_write("[proc] ");
    uart_write(msg);
    uart_write(" a=");
    uart_write_hex_u64(a);
    uart_write(" b=");
    uart_write_hex_u64(b);
    uart_write("\n");
#else
    (void)msg;
    (void)a;
    (void)b;
#endif
}

static const char *exc_kind_name(uint64_t kind) {
    switch (kind) {
        case 0: return "SYNC_EL1t";
        case 1: return "IRQ_EL1t";
        case 2: return "FIQ_EL1t";
        case 3: return "SError_EL1t";
        case 4: return "SYNC_EL1h";
        case 5: return "IRQ_EL1h";
        case 6: return "FIQ_EL1h";
        case 7: return "SError_EL1h";
        case 8: return "SYNC_EL0_64";
        case 9: return "IRQ_EL0_64";
        case 10: return "FIQ_EL0_64";
        case 11: return "SError_EL0_64";
        case 12: return "SYNC_EL0_32";
        case 13: return "IRQ_EL0_32";
        case 14: return "FIQ_EL0_32";
        case 15: return "SError_EL0_32";
        default: return "UNKNOWN";
    }
}

void exception_report(uint64_t kind,
                      uint64_t esr,
                      uint64_t elr,
                      uint64_t far,
                      uint64_t spsr) {
    uint64_t ec = (esr >> 26) & 0x3Full;
    uint64_t il = (esr >> 25) & 0x1ull;
    uint64_t iss = esr & 0x01FFFFFFull;

    uart_write("\n[exception] kind=");
    uart_write(exc_kind_name(kind));
    uart_write(" esr=");
    uart_write_hex_u64(esr);
    uart_write(" ec=");
    uart_write_hex_u64(ec);
    uart_write(" il=");
    uart_write_hex_u64(il);
    uart_write(" iss=");
    uart_write_hex_u64(iss);
    uart_write(" elr=");
    uart_write_hex_u64(elr);
    uart_write(" far=");
    uart_write_hex_u64(far);
    uart_write(" spsr=");
    uart_write_hex_u64(spsr);

    if (kind == 8 && elr >= USER_REGION_BASE && (elr + 4u) <= (USER_REGION_BASE + USER_REGION_SIZE)) {
        uint32_t insn = *(volatile uint32_t *)(uintptr_t)elr;
        uart_write(" insn=");
        uart_write_hex_u64((uint64_t)insn);
    }
    uart_write("\n");
}

uint64_t exception_handle(trap_frame_t *tf,
                          uint64_t kind,
                          uint64_t esr,
                          uint64_t elr,
                          uint64_t far,
                          uint64_t spsr) {
    (void)esr;
    (void)far;
    (void)spsr;

    /* Only support EL0 AArch64 sync (SVC) for now. */
    if (kind != 8) {
        return 0;
    }

    proc_init_if_needed(elr, tf);
    g_procs[g_cur_proc].elr = elr;
    tf_copy(&g_procs[g_cur_proc].tf, tf);
    if (g_procs[g_cur_proc].stack_low == 0 || tf->sp_el0 < g_procs[g_cur_proc].stack_low) {
        g_procs[g_cur_proc].stack_low = tf->sp_el0;
    }

    uint64_t nr = tf->x[8];
    uint64_t a0 = tf->x[0];
    uint64_t a1 = tf->x[1];
    uint64_t a2 = tf->x[2];
    uint64_t a3 = tf->x[3];
    uint64_t a4 = tf->x[4];
    uint64_t a5 = tf->x[5];

    uint64_t ret = 0;
    int set_x0_ret = 1;
    int update_saved_elr = 1;

    switch (nr) {
        case __NR_getcwd:
            ret = sys_getcwd(a0, a1);
            break;

        case __NR_ioctl:
            ret = sys_ioctl(a0, a1, a2);
            break;

        case __NR_brk:
            ret = sys_brk(a0);
            break;

        case __NR_mmap:
            ret = sys_mmap(a0, a1, a2, a3, (int64_t)a4, a5);
            break;

        case __NR_munmap:
            ret = sys_munmap(a0, a1);
            break;

        case __NR_getpid:
            ret = g_procs[g_cur_proc].pid;
            break;

        case __NR_getppid:
            ret = g_procs[g_cur_proc].ppid;
            break;

        case __NR_getuid:
            ret = sys_getuid();
            break;

        case __NR_geteuid:
            ret = sys_geteuid();
            break;

        case __NR_getgid:
            ret = sys_getgid();
            break;

        case __NR_getegid:
            ret = sys_getegid();
            break;

        case __NR_gettid:
            ret = sys_gettid();
            break;

        case __NR_uname:
            ret = sys_uname(a0);
            break;

        case __NR_clock_gettime:
            ret = sys_clock_gettime(a0, a1);
            break;

        case __NR_kill:
            ret = sys_kill(tf, (int64_t)a0, a1, elr);
            if (ret == SYSCALL_SWITCHED) {
                /* sys_kill already switched contexts. */
                tf_copy(&g_procs[g_cur_proc].tf, tf);
                return 1;
            }
            break;

        case __NR_set_tid_address:
            ret = sys_set_tid_address(a0);
            break;

        case __NR_set_robust_list:
            ret = sys_set_robust_list(a0, a1);
            break;

        case __NR_rt_sigaction:
            ret = sys_rt_sigaction(a0, a1, a2, a3);
            break;

        case __NR_rt_sigprocmask:
            ret = sys_rt_sigprocmask(a0, a1, a2, a3);
            break;

        case __NR_nanosleep:
            ret = sys_nanosleep(tf, a0, a1, elr);
            if (ret == SYSCALL_SWITCHED) {
                /* sys_nanosleep already switched contexts. */
                tf_copy(&g_procs[g_cur_proc].tf, tf);
                return 1;
            }
            break;

        case __NR_chdir:
            ret = sys_chdir(a0);
            break;

        case __NR_dup3:
            ret = sys_dup3(a0, a1, a2);
            break;

        case __NR_mkdirat:
            ret = sys_mkdirat((int64_t)a0, a1, a2);
            break;

        case __NR_symlinkat:
            ret = sys_symlinkat(a0, (int64_t)a1, a2);
            break;

        case __NR_linkat:
            ret = sys_linkat((int64_t)a0, a1, (int64_t)a2, a3, a4);
            break;

        case __NR_unlinkat:
            ret = sys_unlinkat((int64_t)a0, a1, a2);
            break;

        case __NR_openat:
            ret = sys_openat((int64_t)a0, a1, a2, a3);
            break;

        case __NR_close:
            ret = sys_close(a0);
            break;

        case __NR_pipe2:
            ret = sys_pipe2(a0, a1);
            break;

        case __NR_read:
            ret = sys_read(a0, a1, a2);
            break;

        case __NR_getdents64:
            ret = sys_getdents64(a0, a1, a2);
            break;

        case __NR_lseek:
            ret = sys_lseek(a0, (int64_t)a1, a2);
            break;

        case __NR_write:
            ret = sys_write(a0, (const void *)(uintptr_t)a1, a2);
            break;

        case __NR_readlinkat:
            ret = sys_readlinkat((int64_t)a0, a1, a2, a3);
            break;

        case __NR_newfstatat:
            ret = sys_newfstatat((int64_t)a0, a1, a2, a3);
            break;

        case __NR_prlimit64:
            /* Not needed yet; pretend "no limit" and succeed for basic runtimes. */
            ret = 0;
            break;

        case __NR_getrandom:
            ret = sys_getrandom(a0, a1, a2);
            break;

        case __NR_reboot:
            ret = sys_reboot(a0, a1, a2, a3);
            break;

        case __NR_execve:
            proc_trace("execve", g_procs[g_cur_proc].pid, a0);
            ret = sys_execve(tf, a0, a1, a2);
            if (ret == 0) {
                /* Success: sys_execve prepared initial user register state (argc/argv/envp).
                 * execve does not return to the caller.
                 */
                set_x0_ret = 0;
                update_saved_elr = 0;
            }
            break;

        case __NR_clone:
            proc_trace("clone", g_procs[g_cur_proc].pid, a0);
            ret = sys_clone(tf, a0, a1, a2, a3, a4, elr);
            break;

        case __NR_wait4:
            proc_trace("wait4", g_procs[g_cur_proc].pid, (uint64_t)(int64_t)a0);
            ret = sys_wait4(tf, (int64_t)a0, a1, a2, a3, elr);
            if (ret == SYSCALL_SWITCHED) {
                /* sys_wait4 already switched contexts. */
                tf_copy(&g_procs[g_cur_proc].tf, tf);
                return 1;
            }
            break;

        case __NR_mona_dmesg:
            ret = sys_mona_dmesg(a0, a1, a2);
            break;

        case __NR_exit:
        case __NR_exit_group:
            proc_trace("exit", g_procs[g_cur_proc].pid, a0);
            return handle_exit_and_maybe_switch(tf, a0) ? 1 : 0;

        default:
            ret = (uint64_t)(-(int64_t)ENOSYS);
            break;
    }

    /* Write return value into the current process, then optionally time-slice. */
    if (set_x0_ret) {
        tf->x[0] = ret;
    }
    tf_copy(&g_procs[g_cur_proc].tf, tf);
    if (update_saved_elr) {
        g_procs[g_cur_proc].elr = elr;
    }
    sched_maybe_switch(tf);
    return 1;
}
