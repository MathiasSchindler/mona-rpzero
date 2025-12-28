#include "syscalls.h"

#include "errno.h"
#include "linux_abi.h"
#include "power.h"
#include "proc.h"
#include "sys_util.h"

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
     * Return a deterministic zero time for now.
     */
    if (clockid != 0 && clockid != 1) {
        return (uint64_t)(-(int64_t)EINVAL);
    }
    if (!user_range_ok(tp_user, (uint64_t)sizeof(linux_timespec_t))) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    linux_timespec_t ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    if (write_bytes_to_user(tp_user, &ts, sizeof(ts)) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    return 0;
}

uint64_t sys_nanosleep(uint64_t req_user, uint64_t rem_user) {
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

    /* No timer yet: return immediately. If rem is provided, report 0 remaining. */
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
    return 0;
}

/* sys_reboot is implemented in power.c */
