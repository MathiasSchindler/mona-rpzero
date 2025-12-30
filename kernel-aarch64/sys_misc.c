#include "syscalls.h"

#include "errno.h"
#include "linux_abi.h"
#include "power.h"
#include "proc.h"
#include "sched.h"
#include "sys_util.h"
#include "time.h"

uint64_t sys_getuid(void) { return 0; }
uint64_t sys_geteuid(void) { return 0; }
uint64_t sys_getgid(void) { return 0; }
uint64_t sys_getegid(void) { return 0; }
uint64_t sys_gettid(void) { return g_procs[g_cur_proc].pid; }

uint64_t sys_set_tid_address(uint64_t tidptr_user) {
    proc_t *cur = &g_procs[g_cur_proc];
    if (tidptr_user != 0) {
        if (!user_range_ok(tidptr_user, 4)) return (uint64_t)(-(int64_t)EFAULT);
        cur->clear_child_tid_user = tidptr_user;
    } else {
        cur->clear_child_tid_user = 0;
    }
    return cur->pid;
}

uint64_t sys_set_robust_list(uint64_t head_user, uint64_t len) {
    (void)head_user;
    (void)len;
    return 0;
}

static uint64_t g_rand_state = 0x9e3779b97f4a7c15ull;

uint64_t sys_getrandom(uint64_t buf_user, uint64_t len, uint64_t flags) {
    (void)flags;
    if (len == 0) return 0;
    if (!user_range_ok(buf_user, len)) return (uint64_t)(-(int64_t)EFAULT);

    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)buf_user;
    uint64_t s = g_rand_state ^ (g_procs[g_cur_proc].pid << 1);
    for (uint64_t i = 0; i < len; i++) {
        /* xorshift64* */
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        uint64_t x = s * 2685821657736338717ull;
        dst[i] = (uint8_t)(x & 0xffu);
    }
    g_rand_state = s;
    return len;
}

uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_user, uint64_t oldset_user, uint64_t sigsetsize) {
    (void)how;
    (void)set_user;
    if (sigsetsize == 0 || sigsetsize > 128) return (uint64_t)(-(int64_t)EINVAL);
    if (oldset_user != 0) {
        if (!user_range_ok(oldset_user, sigsetsize)) return (uint64_t)(-(int64_t)EFAULT);
        /* Report empty mask. */
        for (uint64_t i = 0; i < sigsetsize; i++) {
            *(volatile uint8_t *)(uintptr_t)(oldset_user + i) = 0;
        }
    }
    return 0;
}

uint64_t sys_rt_sigaction(uint64_t sig, uint64_t act_user, uint64_t oldact_user, uint64_t sigsetsize) {
    (void)sig;
    (void)act_user;
    if (sigsetsize == 0 || sigsetsize > 128) return (uint64_t)(-(int64_t)EINVAL);

    /* Best-effort: if oldact is requested, return a zeroed structure. */
    if (oldact_user != 0) {
        uint64_t need = 24u + sigsetsize; /* handler + flags + restorer + mask */
        if (!user_range_ok(oldact_user, need)) return (uint64_t)(-(int64_t)EFAULT);
        for (uint64_t i = 0; i < need; i++) {
            *(volatile uint8_t *)(uintptr_t)(oldact_user + i) = 0;
        }
    }
    return 0;
}

uint64_t sys_uname(uint64_t buf_user) {
    if (!user_range_ok(buf_user, (uint64_t)sizeof(linux_utsname_t))) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    linux_utsname_t u;
    /* Zero-init without libc. */
    {
        volatile uint8_t *zp = (volatile uint8_t *)&u;
        for (uint64_t i = 0; i < sizeof(u); i++) zp[i] = 0;
    }

    /* Keep these short and stable; many user programs only probe for presence. */
    const char *sysname = "Linux";
    const char *nodename = "mona";
    const char *release = "0.0";
    const char *version = "mona-rpzero";
    const char *machine = "aarch64";
    const char *domainname = "";

    const struct {
        const char *src;
        char *dst;
    } fields[] = {
        {sysname, u.sysname},
        {nodename, u.nodename},
        {release, u.release},
        {version, u.version},
        {machine, u.machine},
        {domainname, u.domainname},
    };

    for (uint64_t f = 0; f < (uint64_t)(sizeof(fields) / sizeof(fields[0])); f++) {
        const char *s = fields[f].src;
        char *d = fields[f].dst;
        uint64_t i = 0;
        while (s[i] != '\0' && i + 1 < LINUX_UTSNAME_LEN) {
            d[i] = s[i];
            i++;
        }
        d[i] = '\0';
    }

    if (write_bytes_to_user(buf_user, &u, sizeof(u)) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    return 0;
}

uint64_t sys_clock_gettime(uint64_t clockid, uint64_t tp_user) {
    /* Support common ids: 0=CLOCK_REALTIME, 1=CLOCK_MONOTONIC.
     *
     * For now, CLOCK_REALTIME is "boot-relative" (same as monotonic) because
     * we don't have RTC/NTP yet.
     */
    if (clockid != 0 && clockid != 1) {
        return (uint64_t)(-(int64_t)EINVAL);
    }
    if (!user_range_ok(tp_user, (uint64_t)sizeof(linux_timespec_t))) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    uint64_t ns = time_now_ns();
    linux_timespec_t ts;
    ts.tv_sec = (int64_t)(ns / 1000000000ull);
    ts.tv_nsec = (int64_t)(ns % 1000000000ull);
    if (write_bytes_to_user(tp_user, &ts, sizeof(ts)) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    return 0;
}

uint64_t sys_nanosleep(trap_frame_t *tf, uint64_t req_user, uint64_t rem_user, uint64_t elr) {
    if (req_user == 0) return (uint64_t)(-(int64_t)EFAULT);
    if (!user_range_ok(req_user, (uint64_t)sizeof(linux_timespec_t))) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    linux_timespec_t req;
    {
        volatile const uint8_t *src = (volatile const uint8_t *)(uintptr_t)req_user;
        volatile uint8_t *dst = (volatile uint8_t *)&req;
        for (uint64_t i = 0; i < sizeof(req); i++) dst[i] = src[i];
    }

    if (req.tv_nsec < 0 || req.tv_nsec >= 1000000000ll) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    if (req.tv_sec < 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    __uint128_t total_ns_128 = (__uint128_t)(uint64_t)req.tv_sec * 1000000000ull + (__uint128_t)(uint64_t)req.tv_nsec;
    if (total_ns_128 > (__uint128_t)0xFFFFFFFFFFFFFFFFull) {
        return (uint64_t)(-(int64_t)EINVAL);
    }
    uint64_t total_ns = (uint64_t)total_ns_128;

    /* If rem is provided, on successful completion report 0 remaining. */
    if (rem_user != 0) {
        if (!user_range_ok(rem_user, (uint64_t)sizeof(linux_timespec_t))) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        linux_timespec_t rem;
        rem.tv_sec = 0;
        rem.tv_nsec = 0;
        if (write_bytes_to_user(rem_user, &rem, sizeof(rem)) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
    }

    if (total_ns == 0) {
        return 0;
    }

    proc_t *cur = &g_procs[g_cur_proc];
    uint64_t now = time_now_ns();
    uint64_t deadline = now + total_ns;
    if (deadline < now) {
        deadline = 0xFFFFFFFFFFFFFFFFull;
    }

    /* Save current state and mark the task sleeping. When it resumes, nanosleep
     * should return 0.
     */
    tf_copy(&cur->tf, tf);
    cur->elr = elr;
    cur->tf.x[0] = 0;
    cur->state = PROC_SLEEPING;
    cur->sleep_deadline_ns = deadline;

    int next = sched_pick_next_runnable();
    if (next >= 0 && next != g_cur_proc) {
        proc_switch_to(next, tf);
        return SYSCALL_SWITCHED;
    }

    /* No other runnable tasks; sched_pick_next_runnable may have waited for our
     * own deadline and woken us. Ensure we're runnable again and return.
     */
    if (cur->state == PROC_SLEEPING) {
        cur->state = PROC_RUNNABLE;
        cur->sleep_deadline_ns = 0;
    }

    return 0;
}

/* sys_reboot is implemented in power.c */
