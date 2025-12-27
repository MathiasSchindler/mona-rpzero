#include "uart_pl011.h"
#include "exceptions.h"
#include "initramfs.h"
#include "mmu.h"
#include "pmm.h"
#include "linux_abi.h"
#include "elf64.h"
#include "cache.h"

/* Linux AArch64 syscall numbers we care about first. */
#define __NR_dup3      24ull
#define __NR_openat    56ull
#define __NR_close     57ull
#define __NR_pipe2     59ull
#define __NR_getdents64 61ull
#define __NR_lseek     62ull
#define __NR_read      63ull
#define __NR_write      64ull
#define __NR_newfstatat 79ull
#define __NR_clock_gettime 113ull
#define __NR_uname     160ull
#define __NR_getpid     172ull
#define __NR_getppid    173ull
#define __NR_brk        214ull
#define __NR_munmap     215ull
#define __NR_clone      220ull
#define __NR_execve     221ull
#define __NR_mmap       222ull
#define __NR_wait4      260ull
#define __NR_exit       93ull
#define __NR_exit_group 94ull

/* errno values (Linux) */
#define EBADF 9ull
#define ECHILD 10ull
#define EFAULT 14ull
#define ENOENT 2ull
#define EPIPE 32ull
#define ENOSYS 38ull
#define EMFILE 24ull
#define EAGAIN 11ull
#define EINVAL 22ull
#define EISDIR 21ull
#define ENOEXEC 8ull
#define E2BIG 7ull
#define ENOMEM 12ull

#define AT_FDCWD ((int64_t)-100)

enum {
    MAX_PROCS = 16,
    MAX_FDS = 32,
    MAX_FILEDESCS = 64,
    MAX_PIPES = 16,
    PIPE_BUF = 1024,
    MAX_VMAS = 32,
};

typedef struct {
    uint8_t used;
    uint64_t base;
    uint64_t len;
} vma_t;

typedef enum {
    FDESC_UNUSED = 0,
    FDESC_UART = 1,
    FDESC_INITRAMFS = 2,
    FDESC_PIPE = 3,
} fdesc_kind_t;

typedef enum {
    PIPE_END_READ = 0,
    PIPE_END_WRITE = 1,
} pipe_end_t;

typedef struct {
    uint8_t used;
    uint8_t buf[PIPE_BUF];
    uint32_t rpos;
    uint32_t wpos;
    uint32_t count;
    uint32_t read_refs;
    uint32_t write_refs;
} pipe_t;

typedef struct {
    uint32_t kind;
    uint32_t refs;
    union {
        struct {
            const uint8_t *data;
            uint64_t size;
            uint64_t off;
            uint32_t mode;
            uint8_t is_dir;
            char dir_path[128];
        } initramfs;
        struct {
            uint32_t pipe_id;
            uint32_t end;
        } pipe;
        struct {
            uint32_t unused;
        } uart;
    } u;
} file_desc_t;

typedef struct {
    int16_t fd_to_desc[MAX_FDS];
} fd_table_t;

typedef enum {
    PROC_UNUSED = 0,
    PROC_RUNNABLE = 1,
    PROC_WAITING = 2,
    PROC_ZOMBIE = 3,
} proc_state_t;

typedef struct {
    uint64_t pid;
    uint64_t ppid;
    proc_state_t state;
    uint64_t ttbr0_pa;
    uint64_t user_pa_base;
    uint64_t heap_base;
    uint64_t heap_end;
    uint64_t stack_low;
    uint64_t mmap_next;
    vma_t vmas[MAX_VMAS];
    trap_frame_t tf;
    uint64_t elr;
    uint64_t exit_code;
    int64_t wait_target_pid;
    uint64_t wait_status_user;
    fd_table_t fdt;
} proc_t;

static pipe_t g_pipes[MAX_PIPES];
static file_desc_t g_descs[MAX_FILEDESCS];
static proc_t g_procs[MAX_PROCS];
static int g_cur_proc = 0;
static int g_last_sched = 0;
static uint64_t g_next_pid = 1;
static int g_proc_inited = 0;

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

static inline void write_elr_el1(uint64_t v);
static inline void write_sp_el0(uint64_t v);

static void tf_copy(trap_frame_t *dst, const trap_frame_t *src) {
    for (uint64_t i = 0; i < 31; i++) {
        dst->x[i] = src->x[i];
    }
    dst->sp_el0 = src->sp_el0;
}

static void tf_zero(trap_frame_t *tf) {
    for (uint64_t i = 0; i < 31; i++) {
        tf->x[i] = 0;
    }
    tf->sp_el0 = 0;
}

static void proc_clear(proc_t *p) {
    p->pid = 0;
    p->ppid = 0;
    p->state = PROC_UNUSED;
    p->ttbr0_pa = 0;
    p->user_pa_base = 0;
    p->heap_base = 0;
    p->heap_end = 0;
    p->stack_low = 0;
    p->mmap_next = 0;
    for (uint64_t i = 0; i < MAX_VMAS; i++) {
        p->vmas[i].used = 0;
        p->vmas[i].base = 0;
        p->vmas[i].len = 0;
    }
    tf_zero(&p->tf);
    p->elr = 0;
    p->exit_code = 0;
    p->wait_target_pid = 0;
    p->wait_status_user = 0;
    for (uint64_t i = 0; i < MAX_FDS; i++) {
        p->fdt.fd_to_desc[i] = -1;
    }
}

static void desc_clear(file_desc_t *d) {
    d->kind = FDESC_UNUSED;
    d->refs = 0;
    d->u.initramfs.data = 0;
    d->u.initramfs.size = 0;
    d->u.initramfs.off = 0;
    d->u.initramfs.mode = 0;
    d->u.initramfs.is_dir = 0;
    d->u.initramfs.dir_path[0] = '\0';
    d->u.pipe.pipe_id = 0;
    d->u.pipe.end = 0;
}

static int desc_alloc(void) {
    for (int i = 0; i < (int)MAX_FILEDESCS; i++) {
        if (g_descs[i].refs == 0) {
            /* Reserve immediately so subsequent desc_alloc() calls cannot return the same slot. */
            desc_clear(&g_descs[i]);
            g_descs[i].refs = 1;
            return i;
        }
    }
    return -1;
}

static void desc_incref(int didx) {
    if (didx < 0 || didx >= (int)MAX_FILEDESCS) return;
    file_desc_t *d = &g_descs[didx];
    if (d->refs == 0) return;
    d->refs++;
    if (d->kind == FDESC_PIPE) {
        uint32_t pid = d->u.pipe.pipe_id;
        if (pid < MAX_PIPES && g_pipes[pid].used) {
            if (d->u.pipe.end == PIPE_END_READ) g_pipes[pid].read_refs++;
            else g_pipes[pid].write_refs++;
        }
    }
}

static void desc_decref(int didx) {
    if (didx < 0 || didx >= (int)MAX_FILEDESCS) return;
    file_desc_t *d = &g_descs[didx];
    if (d->refs == 0) return;

    if (d->kind == FDESC_PIPE) {
        uint32_t pid = d->u.pipe.pipe_id;
        if (pid < MAX_PIPES && g_pipes[pid].used) {
            if (d->u.pipe.end == PIPE_END_READ) {
                if (g_pipes[pid].read_refs > 0) g_pipes[pid].read_refs--;
            } else {
                if (g_pipes[pid].write_refs > 0) g_pipes[pid].write_refs--;
            }
            if (g_pipes[pid].read_refs == 0 && g_pipes[pid].write_refs == 0) {
                g_pipes[pid].used = 0;
                g_pipes[pid].rpos = g_pipes[pid].wpos = g_pipes[pid].count = 0;
            }
        }
    }

    d->refs--;
    if (d->refs == 0) {
        desc_clear(d);
    }
}

static int fd_get_desc_idx(proc_t *p, uint64_t fd) {
    if (!p) return -1;
    if (fd >= MAX_FDS) return -1;
    int didx = p->fdt.fd_to_desc[fd];
    if (didx < 0 || didx >= (int)MAX_FILEDESCS) return -1;
    if (g_descs[didx].refs == 0) return -1;
    return didx;
}

static int fd_alloc_into(proc_t *p, int min_fd, int didx) {
    if (!p) return -1;
    if (min_fd < 0) min_fd = 0;
    for (int fd = min_fd; fd < (int)MAX_FDS; fd++) {
        if (p->fdt.fd_to_desc[fd] < 0) {
            p->fdt.fd_to_desc[fd] = (int16_t)didx;
            desc_incref(didx);
            return fd;
        }
    }
    return -1;
}

static void fd_close(proc_t *p, uint64_t fd) {
    if (!p) return;
    if (fd >= MAX_FDS) return;
    int didx = p->fdt.fd_to_desc[fd];
    if (didx >= 0) {
        p->fdt.fd_to_desc[fd] = -1;
        desc_decref(didx);
    }
}

static void proc_close_all_fds(proc_t *p) {
    if (!p) return;
    for (uint64_t fd = 0; fd < MAX_FDS; fd++) {
        fd_close(p, fd);
    }
}

static int user_range_ok(uint64_t user_ptr, uint64_t len) {
    if (len == 0) return 1;
    if (user_ptr < USER_REGION_BASE) return 0;
    if (user_ptr >= USER_REGION_BASE + USER_REGION_SIZE) return 0;
    if (user_ptr + len < user_ptr) return 0; /* overflow */
    if (user_ptr + len > USER_REGION_BASE + USER_REGION_SIZE) return 0;
    return 1;
}

static int copy_cstr_from_user(char *dst, uint64_t dstsz, uint64_t user_ptr) {
    if (dstsz == 0) return -1;
    for (uint64_t i = 0; i < dstsz; i++) {
        if (!user_range_ok(user_ptr + i, 1)) return -1;
        char c = *(const volatile char *)(uintptr_t)(user_ptr + i);
        dst[i] = c;
        if (c == '\0') return 0;
    }
    dst[dstsz - 1] = '\0';
    return -1;
}

static int read_u64_from_user(uint64_t user_ptr, uint64_t *out) {
    if (!user_range_ok(user_ptr, 8)) return -1;
    *out = *(const volatile uint64_t *)(uintptr_t)user_ptr;
    return 0;
}

static uint64_t cstr_len(const char *s) {
    uint64_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

static int write_bytes_to_user(uint64_t user_dst, const void *src, uint64_t len) {
    if (!user_range_ok(user_dst, len)) return -1;
    volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)user_dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < len; i++) d[i] = s[i];
    return 0;
}

static int write_u64_to_user(uint64_t user_dst, uint64_t v) {
    if (!user_range_ok(user_dst, 8)) return -1;
    *(volatile uint64_t *)(uintptr_t)user_dst = v;
    return 0;
}

static uint64_t align_down_u64(uint64_t x, uint64_t a) {
    return x & ~(a - 1u);
}

static uint64_t align_up_u64(uint64_t x, uint64_t a) {
    return (x + (a - 1u)) & ~(a - 1u);
}

static const char *exc_kind_name(unsigned long kind) {
    switch (kind) {
        case 0: return "sync el1t";
        case 1: return "irq  el1t";
        case 2: return "fiq  el1t";
        case 3: return "serr el1t";
        case 4: return "sync el1h";
        case 5: return "irq  el1h";
        case 6: return "fiq  el1h";
        case 7: return "serr el1h";
        case 8: return "sync el0 64";
        case 9: return "irq  el0 64";
        case 10: return "fiq  el0 64";
        case 11: return "serr el0 64";
        case 12: return "sync el0 32";
        case 13: return "irq  el0 32";
        case 14: return "fiq  el0 32";
        case 15: return "serr el0 32";
        default: return "unknown";
    }
}

void exception_report(unsigned long kind,
                      unsigned long esr,
                      unsigned long elr,
                      unsigned long far,
                      unsigned long spsr) {
    uart_write("\n*** EXCEPTION ***\n");
    uart_write("kind: ");
    uart_write(exc_kind_name(kind));
    uart_write(" (id=");
    uart_write_hex_u64(kind);
    uart_write(")\n");

    uart_write("ESR_EL1: ");
    uart_write_hex_u64(esr);
    uart_write("\n");

    uart_write("ELR_EL1: ");
    uart_write_hex_u64(elr);
    uart_write("\n");

    uart_write("FAR_EL1: ");
    uart_write_hex_u64(far);
    uart_write("\n");

    uart_write("SPSR_EL1:");
    uart_write_hex_u64(spsr);
    uart_write("\n");

    uart_write("Halting.\n");
}

static uint64_t sys_write(uint64_t fd, const void *buf, uint64_t len) {
    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(cur, fd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    file_desc_t *d = &g_descs[didx];
    if (d->kind == FDESC_UART) {
        const volatile char *p = (const volatile char *)buf;
        for (uint64_t i = 0; i < len; i++) {
            uart_putc(p[i]);
        }
        return len;
    }

    if (d->kind == FDESC_PIPE && d->u.pipe.end == PIPE_END_WRITE) {
        uint32_t pid = d->u.pipe.pipe_id;
        if (pid >= MAX_PIPES || !g_pipes[pid].used) {
            return (uint64_t)(-(int64_t)EBADF);
        }
        pipe_t *pp = &g_pipes[pid];
        if (pp->read_refs == 0) {
            return (uint64_t)(-(int64_t)EPIPE);
        }
        if (len == 0) return 0;
        if (pp->count == PIPE_BUF) {
            return (uint64_t)(-(int64_t)EAGAIN);
        }

        uint64_t space = (uint64_t)(PIPE_BUF - pp->count);
        uint64_t n = (len < space) ? len : space;
        const volatile uint8_t *src = (const volatile uint8_t *)buf;
        for (uint64_t i = 0; i < n; i++) {
            pp->buf[pp->wpos] = src[i];
            pp->wpos = (pp->wpos + 1u) % PIPE_BUF;
        }
        pp->count += (uint32_t)n;
        return n;
    }

    return (uint64_t)(-(int64_t)EBADF);
}

static void proc_init_if_needed(uint64_t elr, trap_frame_t *tf) {
    if (g_proc_inited) return;

    for (uint64_t i = 0; i < MAX_PIPES; i++) {
        g_pipes[i].used = 0;
        g_pipes[i].rpos = g_pipes[i].wpos = g_pipes[i].count = 0;
        g_pipes[i].read_refs = g_pipes[i].write_refs = 0;
    }
    for (uint64_t i = 0; i < MAX_FILEDESCS; i++) {
        desc_clear(&g_descs[i]);
    }
    for (uint64_t i = 0; i < MAX_PROCS; i++) {
        proc_clear(&g_procs[i]);
    }

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

static uint64_t sys_brk(uint64_t newbrk) {
    proc_t *cur = &g_procs[g_cur_proc];

    /* Lazy init for pre-execve callers: keep it within user region. */
    if (cur->heap_base == 0) {
        cur->heap_base = align_up_u64(USER_REGION_BASE, 16);
        cur->heap_end = cur->heap_base;
    }
    if (cur->stack_low == 0) {
        cur->stack_low = cur->tf.sp_el0;
    }

    if (newbrk == 0) {
        return cur->heap_end;
    }

    /* Simple safety gap to reduce heap/stack collisions without full VM. */
    const uint64_t STACK_GUARD = 256u * 1024u;
    uint64_t max_brk = USER_REGION_BASE + USER_REGION_SIZE;
    if (cur->stack_low > USER_REGION_BASE + STACK_GUARD) {
        uint64_t lim = cur->stack_low - STACK_GUARD;
        if (lim < max_brk) max_brk = lim;
    }

    newbrk = align_up_u64(newbrk, 16);
    if (newbrk < cur->heap_base || newbrk > max_brk) {
        /* Linux brk returns the current program break on failure. */
        return cur->heap_end;
    }

    cur->heap_end = newbrk;
    return cur->heap_end;
}

static int vma_overlaps(proc_t *p, uint64_t base, uint64_t len, uint64_t *out_overlap_base) {
    uint64_t end = base + len;
    for (uint64_t i = 0; i < MAX_VMAS; i++) {
        if (!p->vmas[i].used) continue;
        uint64_t b = p->vmas[i].base;
        uint64_t e = b + p->vmas[i].len;
        if (!(end <= b || base >= e)) {
            if (out_overlap_base) *out_overlap_base = b;
            return 1;
        }
    }
    return 0;
}

static uint64_t proc_mmap_hi(proc_t *p) {
    const uint64_t PAGE = 4096u;
    const uint64_t STACK_GUARD = 256u * 1024u;
    uint64_t hi = USER_REGION_BASE + USER_REGION_SIZE;
    if (p->stack_low > USER_REGION_BASE + STACK_GUARD) {
        uint64_t lim = p->stack_low - STACK_GUARD;
        if (lim < hi) hi = lim;
    }
    hi = align_down_u64(hi, PAGE);
    if (hi < USER_REGION_BASE) hi = USER_REGION_BASE;
    return hi;
}

static uint64_t proc_recompute_mmap_next(proc_t *p) {
    uint64_t top = proc_mmap_hi(p);
    for (;;) {
        int found = 0;
        for (uint64_t i = 0; i < MAX_VMAS; i++) {
            if (!p->vmas[i].used) continue;
            uint64_t end = p->vmas[i].base + p->vmas[i].len;
            if (end == top) {
                top = p->vmas[i].base;
                found = 1;
                break;
            }
        }
        if (!found) break;
    }
    return top;
}

static uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off) {
    (void)prot;
    (void)off;

    const uint64_t PAGE = 4096u;
    const uint64_t MAP_PRIVATE = 0x02u;
    const uint64_t MAP_ANONYMOUS = 0x20u;
    const uint64_t HEAP_GUARD = 64u * 1024u;

    if (len == 0) return (uint64_t)(-(int64_t)EINVAL);
    if (addr != 0) return (uint64_t)(-(int64_t)ENOSYS);
    if (fd != -1) return (uint64_t)(-(int64_t)ENOSYS);

    /* Support only anonymous private mappings for now. */
    if ((flags & (MAP_PRIVATE | MAP_ANONYMOUS)) != (MAP_PRIVATE | MAP_ANONYMOUS)) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if ((flags & ~(MAP_PRIVATE | MAP_ANONYMOUS)) != 0) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }

    proc_t *p = &g_procs[g_cur_proc];
    uint64_t alen = align_up_u64(len, PAGE);

    /* Lazy init. */
    if (p->mmap_next == 0) {
        p->mmap_next = proc_mmap_hi(p);
    } else {
        /* Stack may have moved; tighten the ceiling if needed. */
        uint64_t hi = proc_mmap_hi(p);
        if (p->mmap_next > hi) p->mmap_next = hi;
    }

    uint64_t end = align_down_u64(p->mmap_next, PAGE);
    for (;;) {
        if (end < USER_REGION_BASE + alen) {
            return (uint64_t)(-(int64_t)ENOMEM);
        }
        uint64_t base = align_down_u64(end - alen, PAGE);

        /* Keep mmap area above the current brk heap (simple split). */
        uint64_t heap_lim = p->heap_end;
        if (heap_lim < p->heap_base) heap_lim = p->heap_base;
        heap_lim = align_up_u64(heap_lim, PAGE);
        if (base < heap_lim + HEAP_GUARD) {
            return (uint64_t)(-(int64_t)ENOMEM);
        }

        uint64_t ovb = 0;
        if (vma_overlaps(p, base, alen, &ovb)) {
            /* Move below the overlapping region and retry. */
            end = align_down_u64(ovb, PAGE);
            continue;
        }

        int slot = -1;
        for (uint64_t i = 0; i < MAX_VMAS; i++) {
            if (!p->vmas[i].used) {
                slot = (int)i;
                break;
            }
        }
        if (slot < 0) {
            return (uint64_t)(-(int64_t)ENOMEM);
        }

        p->vmas[slot].used = 1;
        p->vmas[slot].base = base;
        p->vmas[slot].len = alen;
        p->mmap_next = base;
        return base;
    }
}

static uint64_t sys_munmap(uint64_t addr, uint64_t len) {
    const uint64_t PAGE = 4096u;
    if ((addr & (PAGE - 1u)) != 0) return (uint64_t)(-(int64_t)EINVAL);
    if (len == 0) return (uint64_t)(-(int64_t)EINVAL);

    uint64_t alen = align_up_u64(len, PAGE);
    proc_t *p = &g_procs[g_cur_proc];

    for (uint64_t i = 0; i < MAX_VMAS; i++) {
        if (!p->vmas[i].used) continue;
        if (p->vmas[i].base == addr && p->vmas[i].len == alen) {
            p->vmas[i].used = 0;
            p->vmas[i].base = 0;
            p->vmas[i].len = 0;
            p->mmap_next = proc_recompute_mmap_next(p);
            return 0;
        }
    }

    return (uint64_t)(-(int64_t)EINVAL);
}

static int proc_find_free_slot(void) {
    for (int i = 0; i < (int)MAX_PROCS; i++) {
        if (g_procs[i].state == PROC_UNUSED) return i;
    }
    return -1;
}

static int sched_pick_next_runnable(void) {
    for (int step = 1; step <= (int)MAX_PROCS; step++) {
        int idx = (g_last_sched + step) % (int)MAX_PROCS;
        if (g_procs[idx].state == PROC_RUNNABLE) {
            g_last_sched = idx;
            return idx;
        }
    }
    return -1;
}

static void proc_switch_to(int idx, trap_frame_t *tf) {
    g_cur_proc = idx;
    mmu_ttbr0_write(g_procs[idx].ttbr0_pa);
    write_elr_el1(g_procs[idx].elr);
    tf_copy(tf, &g_procs[idx].tf);
}

static void sched_maybe_switch(trap_frame_t *tf) {
    int next = sched_pick_next_runnable();
    if (next >= 0 && next != g_cur_proc) {
        proc_switch_to(next, tf);
    }
}

static uint64_t sys_openat(int64_t dirfd, uint64_t pathname_user, uint64_t flags, uint64_t mode) {
    (void)flags;
    (void)mode;

    if (dirfd != AT_FDCWD) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }

    char path[256];
    if (copy_cstr_from_user(path, sizeof(path), pathname_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    const uint8_t *data = 0;
    uint64_t size = 0;
    uint32_t imode = 0;
    if (initramfs_lookup(path, &data, &size, &imode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }

    int didx = desc_alloc();
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EMFILE);
    }

    file_desc_t *d = &g_descs[didx];
    desc_clear(d);
    d->kind = FDESC_INITRAMFS;
    d->refs = 1;
    d->u.initramfs.data = data;
    d->u.initramfs.size = size;
    d->u.initramfs.off = 0;
    d->u.initramfs.mode = imode;
    d->u.initramfs.is_dir = ((imode & 0170000u) == 0040000u) ? 1u : 0u;
    d->u.initramfs.dir_path[0] = '\0';
    if (d->u.initramfs.is_dir) {
        /* Store normalized path (leading slashes stripped, "/" -> ""). */
        const char *pp = path;
        while (*pp == '/') pp++;
        uint64_t i;
        for (i = 0; i + 1 < sizeof(d->u.initramfs.dir_path) && pp[i] != '\0'; i++) {
            d->u.initramfs.dir_path[i] = pp[i];
        }
        d->u.initramfs.dir_path[i] = '\0';
    }

    proc_t *cur = &g_procs[g_cur_proc];
    int fd = fd_alloc_into(cur, 3, didx);
    /* fd_alloc_into() takes a ref; drop our creation ref. */
    desc_decref(didx);
    if (fd < 0) {
        return (uint64_t)(-(int64_t)EMFILE);
    }
    return (uint64_t)fd;
}

static uint64_t sys_close(uint64_t fd) {
    proc_t *cur = &g_procs[g_cur_proc];
    if (fd >= MAX_FDS) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (fd_get_desc_idx(cur, fd) < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    fd_close(cur, fd);
    return 0;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf_user, uint64_t len) {
    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(cur, fd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (!user_range_ok(buf_user, len)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }
    if (len == 0) return 0;

    file_desc_t *d = &g_descs[didx];
    if (d->kind == FDESC_UART) {
        volatile char *dst = (volatile char *)(uintptr_t)buf_user;

        /* Block for the first byte, then drain any immediately available bytes. */
        char c = uart_getc_blocking();
        if (c == '\r') c = '\n';
        dst[0] = c;

        uint64_t n = 1;
        for (; n < len; n++) {
            char t;
            if (!uart_try_getc(&t)) break;
            if (t == '\r') t = '\n';
            dst[n] = t;
        }
        return n;
    }

    if (d->kind == FDESC_PIPE && d->u.pipe.end == PIPE_END_READ) {
        uint32_t pid = d->u.pipe.pipe_id;
        if (pid >= MAX_PIPES || !g_pipes[pid].used) {
            return (uint64_t)(-(int64_t)EBADF);
        }
        pipe_t *pp = &g_pipes[pid];
        if (pp->count == 0) {
            if (pp->write_refs == 0) {
                return 0; /* EOF */
            }
            return (uint64_t)(-(int64_t)EAGAIN);
        }

        uint64_t n = (len < pp->count) ? len : (uint64_t)pp->count;
        volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)buf_user;
        for (uint64_t i = 0; i < n; i++) {
            dst[i] = pp->buf[pp->rpos];
            pp->rpos = (pp->rpos + 1u) % PIPE_BUF;
        }
        pp->count -= (uint32_t)n;
        return n;
    }

    if (d->kind != FDESC_INITRAMFS) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    if (d->u.initramfs.is_dir) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    if (d->u.initramfs.off >= d->u.initramfs.size) return 0;

    uint64_t remain = d->u.initramfs.size - d->u.initramfs.off;
    uint64_t n = (len < remain) ? len : remain;

    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)buf_user;
    const uint8_t *src = d->u.initramfs.data + d->u.initramfs.off;
    for (uint64_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }

    d->u.initramfs.off += n;
    return n;
}

typedef struct {
    uint64_t skip;
    uint64_t emitted;
    uint64_t buf_user;
    uint64_t buf_len;
    uint64_t pos;
} dents_ctx_t;

static uint64_t align8_u64(uint64_t x) {
    return (x + 7u) & ~7u;
}

static int dents_emit_cb(const char *name, uint32_t mode, void *ctx) {
    dents_ctx_t *dc = (dents_ctx_t *)ctx;
    if (dc->emitted < dc->skip) {
        dc->emitted++;
        return 0;
    }

    /* linux_dirent64 record: ino,u64; off,s64; reclen,u16; type,u8; name,NUL */
    uint64_t name_len = 0;
    while (name[name_len] != '\0') name_len++;

    uint64_t reclen = 8 + 8 + 2 + 1 + (name_len + 1);
    reclen = align8_u64(reclen);
    if (dc->pos + reclen > dc->buf_len) {
        return 1; /* stop */
    }

    uint64_t dst = dc->buf_user + dc->pos;

    /* d_ino */
    *(volatile uint64_t *)(uintptr_t)(dst + 0) = 1;
    /* d_off */
    *(volatile int64_t *)(uintptr_t)(dst + 8) = (int64_t)(dc->emitted + 1);
    /* d_reclen */
    *(volatile uint16_t *)(uintptr_t)(dst + 16) = (uint16_t)reclen;
    /* d_type */
    uint8_t dtype = LINUX_DT_UNKNOWN;
    if ((mode & 0170000u) == 0040000u) dtype = LINUX_DT_DIR;
    else if ((mode & 0170000u) == 0100000u) dtype = LINUX_DT_REG;
    *(volatile uint8_t *)(uintptr_t)(dst + 18) = dtype;

    /* name */
    volatile char *np = (volatile char *)(uintptr_t)(dst + 19);
    for (uint64_t i = 0; i < name_len; i++) np[i] = name[i];
    np[name_len] = '\0';

    /* zero pad remainder */
    for (uint64_t i = 19 + name_len + 1; i < reclen; i++) {
        *(volatile uint8_t *)(uintptr_t)(dst + i) = 0;
    }

    dc->pos += reclen;
    dc->emitted++;
    return 0;
}

static uint64_t sys_getdents64(uint64_t fd, uint64_t dirp_user, uint64_t count) {
    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(cur, fd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    file_desc_t *d = &g_descs[didx];
    if (d->kind != FDESC_INITRAMFS || !d->u.initramfs.is_dir) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (!user_range_ok(dirp_user, count)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    dents_ctx_t dc;
    dc.skip = d->u.initramfs.off;
    dc.emitted = 0;
    dc.buf_user = dirp_user;
    dc.buf_len = count;
    dc.pos = 0;

    int rc = initramfs_list_dir(d->u.initramfs.dir_path, dents_emit_cb, &dc);
    if (rc != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }

    /* Advance directory position by entries emitted in this call. */
    if (dc.emitted > dc.skip) {
        d->u.initramfs.off = dc.emitted;
    }

    return dc.pos;
}

static uint64_t sys_lseek(uint64_t fd, int64_t off, uint64_t whence) {
    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(cur, fd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    file_desc_t *d = &g_descs[didx];
    if (d->kind != FDESC_INITRAMFS || d->u.initramfs.is_dir) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    uint64_t newoff;
    switch (whence) {
        case 0: /* SEEK_SET */
            if (off < 0) return (uint64_t)(-(int64_t)EINVAL);
            newoff = (uint64_t)off;
            break;
        case 1: /* SEEK_CUR */
            if (off < 0 && (uint64_t)(-off) > d->u.initramfs.off) return (uint64_t)(-(int64_t)EINVAL);
            newoff = (uint64_t)((int64_t)d->u.initramfs.off + off);
            break;
        case 2: /* SEEK_END */
            if (off < 0 && (uint64_t)(-off) > d->u.initramfs.size) return (uint64_t)(-(int64_t)EINVAL);
            newoff = (uint64_t)((int64_t)d->u.initramfs.size + off);
            break;
        default:
            return (uint64_t)(-(int64_t)EINVAL);
    }

    if (newoff > d->u.initramfs.size) return (uint64_t)(-(int64_t)EINVAL);
    d->u.initramfs.off = newoff;
    return newoff;
}

static uint64_t sys_dup3(uint64_t oldfd, uint64_t newfd, uint64_t flags) {
    if (flags != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }
    if (oldfd >= MAX_FDS || newfd >= MAX_FDS) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    proc_t *cur = &g_procs[g_cur_proc];
    int didx = fd_get_desc_idx(cur, oldfd);
    if (didx < 0) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    if (oldfd == newfd) {
        return newfd;
    }

    /* Close destination if open. */
    fd_close(cur, newfd);

    cur->fdt.fd_to_desc[newfd] = (int16_t)didx;
    desc_incref(didx);
    return newfd;
}

static uint64_t sys_pipe2(uint64_t pipefd_user, uint64_t flags) {
    if (flags != 0) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (!user_range_ok(pipefd_user, 8)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    int pid = -1;
    for (int i = 0; i < (int)MAX_PIPES; i++) {
        if (!g_pipes[i].used) {
            pid = i;
            break;
        }
    }
    if (pid < 0) {
        return (uint64_t)(-(int64_t)EMFILE);
    }

    g_pipes[pid].used = 1;
    g_pipes[pid].rpos = 0;
    g_pipes[pid].wpos = 0;
    g_pipes[pid].count = 0;
    g_pipes[pid].read_refs = 0;
    g_pipes[pid].write_refs = 0;

    int rdesc = desc_alloc();
    int wdesc = desc_alloc();
    if (rdesc < 0 || wdesc < 0) {
        if (rdesc >= 0) desc_clear(&g_descs[rdesc]);
        if (wdesc >= 0) desc_clear(&g_descs[wdesc]);
        g_pipes[pid].used = 0;
        return (uint64_t)(-(int64_t)EMFILE);
    }

    desc_clear(&g_descs[rdesc]);
    g_descs[rdesc].kind = FDESC_PIPE;
    g_descs[rdesc].refs = 1;
    g_descs[rdesc].u.pipe.pipe_id = (uint32_t)pid;
    g_descs[rdesc].u.pipe.end = PIPE_END_READ;

    desc_clear(&g_descs[wdesc]);
    g_descs[wdesc].kind = FDESC_PIPE;
    g_descs[wdesc].refs = 1;
    g_descs[wdesc].u.pipe.pipe_id = (uint32_t)pid;
    g_descs[wdesc].u.pipe.end = PIPE_END_WRITE;

    /* Initial references for the two pipe ends. Further dup/fork will adjust via desc_{inc,dec}ref(). */
    g_pipes[pid].read_refs = 1;
    g_pipes[pid].write_refs = 1;

    /* Install into current process FD table. */
    proc_t *cur = &g_procs[g_cur_proc];
    int rfd = fd_alloc_into(cur, 0, rdesc);
    int wfd = fd_alloc_into(cur, 0, wdesc);
    /* fd_alloc_into() increments refs; drop our creation refs. */
    desc_decref(rdesc);
    desc_decref(wdesc);

    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) fd_close(cur, (uint64_t)rfd);
        if (wfd >= 0) fd_close(cur, (uint64_t)wfd);
        g_pipes[pid].used = 0;
        return (uint64_t)(-(int64_t)EMFILE);
    }

    /* Write pipe fds back to user. */
    uint32_t out[2];
    out[0] = (uint32_t)rfd;
    out[1] = (uint32_t)wfd;

    if (write_bytes_to_user(pipefd_user, out, sizeof(out)) != 0) {
        fd_close(cur, (uint64_t)rfd);
        fd_close(cur, (uint64_t)wfd);
        return (uint64_t)(-(int64_t)EFAULT);
    }

    return 0;
}

static uint64_t sys_newfstatat(int64_t dirfd, uint64_t pathname_user, uint64_t statbuf_user, uint64_t flags) {
    (void)flags;

    if (dirfd != AT_FDCWD) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }
    if (!user_range_ok(statbuf_user, (uint64_t)sizeof(linux_stat_t))) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    char path[256];
    if (copy_cstr_from_user(path, sizeof(path), pathname_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    const uint8_t *data = 0;
    uint64_t size = 0;
    uint32_t mode = 0;
    if (initramfs_lookup(path, &data, &size, &mode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }

    (void)data;
    linux_stat_t st;
    /* Zero-init without libc */
    volatile uint8_t *zp = (volatile uint8_t *)&st;
    for (uint64_t i = 0; i < sizeof(st); i++) zp[i] = 0;

    st.st_dev = 0;
    st.st_ino = 1;
    st.st_nlink = 1;
    st.st_mode = mode;
    st.st_uid = 0;
    st.st_gid = 0;
    st.st_rdev = 0;
    st.st_size = (int64_t)size;
    st.st_blksize = 4096;
    st.st_blocks = (int64_t)((size + 511u) / 512u);

    /* Copy out */
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)statbuf_user;
    const volatile uint8_t *src = (const volatile uint8_t *)&st;
    for (uint64_t i = 0; i < sizeof(st); i++) dst[i] = src[i];
    return 0;
}

static uint64_t sys_uname(uint64_t buf_user) {
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

static uint64_t sys_clock_gettime(uint64_t clockid, uint64_t tp_user) {
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

static inline void write_elr_el1(uint64_t v) {
    __asm__ volatile("msr ELR_EL1, %0" :: "r"(v));
}

static inline void write_sp_el0(uint64_t v) {
    __asm__ volatile("msr SP_EL0, %0" :: "r"(v));
}

static uint64_t sys_execve(trap_frame_t *tf, uint64_t pathname_user, uint64_t argv_user, uint64_t envp_user) {
    enum {
        MAX_ARGS = 32,
        MAX_ENVP = 32,
        MAX_STR = 256,
    };

    /* Minimal Linux auxv types we care about for static binaries. */
    enum {
        AT_NULL = 0,
        AT_PHDR = 3,
        AT_PHENT = 4,
        AT_PHNUM = 5,
        AT_PAGESZ = 6,
        AT_ENTRY = 9,
        AT_UID = 11,
        AT_EUID = 12,
        AT_GID = 13,
        AT_EGID = 14,
        AT_PLATFORM = 15,
        AT_EXECFN = 31,
        AT_RANDOM = 25,
        AT_SECURE = 23,
    };

    /* Snapshot argv/envp strings from the *current* user image before loading the new one. */
    char arg_strs[MAX_ARGS][MAX_STR];
    char env_strs[MAX_ENVP][MAX_STR];
    uint64_t argc = 0;
    uint64_t envc = 0;

    if (argv_user != 0) {
        for (; argc < MAX_ARGS; argc++) {
            uint64_t p = 0;
            if (read_u64_from_user(argv_user + argc * 8u, &p) != 0) {
                return (uint64_t)(-(int64_t)EFAULT);
            }
            if (p == 0) break;
            if (copy_cstr_from_user(arg_strs[argc], MAX_STR, p) != 0) {
                return (uint64_t)(-(int64_t)EFAULT);
            }
        }
        if (argc == MAX_ARGS) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
    }

    if (envp_user != 0) {
        for (; envc < MAX_ENVP; envc++) {
            uint64_t p = 0;
            if (read_u64_from_user(envp_user + envc * 8u, &p) != 0) {
                return (uint64_t)(-(int64_t)EFAULT);
            }
            if (p == 0) break;
            if (copy_cstr_from_user(env_strs[envc], MAX_STR, p) != 0) {
                return (uint64_t)(-(int64_t)EFAULT);
            }
        }
        if (envc == MAX_ENVP) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
    }

    char path[256];
    if (copy_cstr_from_user(path, sizeof(path), pathname_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    /* If argv is missing, provide a sensible argv[0] for compatibility. */
    if (argv_user == 0) {
        const char *p = path;
        for (uint64_t i = 0; path[i] != '\0'; i++) {
            if (path[i] == '/') p = &path[i + 1];
        }
        /* If path ends with '/', fall back to the full path. */
        if (*p == '\0') p = path;

        argc = 1;
        /* Copy into snapshot buffer.
         * (Avoid libc; cap at MAX_STR-1.)
         */
        uint64_t j = 0;
        for (; j + 1 < MAX_STR && p[j] != '\0'; j++) {
            arg_strs[0][j] = p[j];
        }
        arg_strs[0][j] = '\0';
    }

    const uint8_t *img = 0;
    uint64_t img_size = 0;
    uint32_t mode = 0;
    if (initramfs_lookup(path, &img, &img_size, &mode) != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }
    if ((mode & 0170000u) == 0040000u) {
        return (uint64_t)(-(int64_t)EISDIR);
    }

    uint64_t entry = 0;
    uint64_t minva = 0;
    uint64_t maxva = 0;
    if (elf64_load_etexec(img, (size_t)img_size, USER_REGION_BASE, USER_REGION_SIZE, &entry, &minva, &maxva) != 0) {
        return (uint64_t)(-(int64_t)ENOEXEC);
    }

    /* Compute auxiliary vectors derived from the ELF header.
     * Best-effort: if we can't derive AT_PHDR safely, we omit it.
     */
    uint64_t at_phdr = 0;
    uint64_t at_phent = 0;
    uint64_t at_phnum = 0;
    {
        if (img_size >= sizeof(elf64_ehdr_t)) {
            const elf64_ehdr_t *eh = (const elf64_ehdr_t *)img;
            at_phent = (uint64_t)eh->e_phentsize;
            at_phnum = (uint64_t)eh->e_phnum;

            uint64_t ph_end = eh->e_phoff + (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
            if (ph_end >= eh->e_phoff && ph_end <= img_size && eh->e_phentsize == sizeof(elf64_phdr_t)) {
                /* Prefer PT_PHDR if present. */
                for (uint16_t i = 0; i < eh->e_phnum; i++) {
                    const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff + (uint64_t)i * sizeof(elf64_phdr_t));
                    if (ph->p_type == PT_PHDR) {
                        at_phdr = ph->p_vaddr;
                        break;
                    }
                }

                /* Fallback: program headers are typically within the first PT_LOAD segment. */
                if (at_phdr == 0) {
                    for (uint16_t i = 0; i < eh->e_phnum; i++) {
                        const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff + (uint64_t)i * sizeof(elf64_phdr_t));
                        if (ph->p_type != PT_LOAD) continue;
                        if (ph->p_offset != 0) continue;

                        /* Ensure ELF header + phdr table are within this LOAD in file image. */
                        uint64_t need_end = eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t);
                        if (need_end <= ph->p_filesz) {
                            at_phdr = ph->p_vaddr + eh->e_phoff;
                            break;
                        }
                    }
                }

                /* Validate computed address is within the loaded user range. */
                if (at_phdr != 0) {
                    uint64_t need = at_phnum * at_phent;
                    if (!user_range_ok(at_phdr, need)) {
                        at_phdr = 0;
                    }
                }
            }
        }
    }

    if (maxva > minva) {
        cache_sync_icache_for_range(minva, maxva - minva);
    }

    /* Build an initial user stack containing argc/argv/envp/auxv (minimal).
     * Layout (lowest addr = SP):
     *   argc (u64)
     *   argv[0..argc-1] (u64 pointers)
     *   NULL
     *   envp[0..envc-1] (u64 pointers)
     *   NULL
     *   auxv: (AT_NULL=0,0)
     *   strings...
     */
    uint64_t sp = USER_REGION_BASE + USER_REGION_SIZE;
    uint64_t argv_addrs[MAX_ARGS];
    uint64_t envp_addrs[MAX_ENVP];

    /* Extra auxv-backed strings/blobs placed on the user stack. */
    uint64_t execfn_addr = 0;
    uint64_t platform_addr = 0;
    uint64_t random_addr = 0;

    /* Copy strings near the top of the stack so pointers can reference them. */
    for (uint64_t i = 0; i < argc; i++) {
        uint64_t len = cstr_len(arg_strs[i]) + 1u;
        sp -= len;
        if (!user_range_ok(sp, len)) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
        if (write_bytes_to_user(sp, arg_strs[i], len) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        argv_addrs[i] = sp;
    }

    for (uint64_t i = 0; i < envc; i++) {
        uint64_t len = cstr_len(env_strs[i]) + 1u;
        sp -= len;
        if (!user_range_ok(sp, len)) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
        if (write_bytes_to_user(sp, env_strs[i], len) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        envp_addrs[i] = sp;
    }

    /* execfn (full path) */
    {
        uint64_t len = cstr_len(path) + 1u;
        sp -= len;
        if (!user_range_ok(sp, len)) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
        if (write_bytes_to_user(sp, path, len) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        execfn_addr = sp;
    }

    /* platform */
    {
        static const char platform[] = "aarch64";
        uint64_t len = sizeof(platform);
        sp -= len;
        if (!user_range_ok(sp, len)) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
        if (write_bytes_to_user(sp, platform, len) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        platform_addr = sp;
    }

    /* AT_RANDOM: 16 bytes */
    {
        uint8_t rnd[16];
        /* Deterministic placeholder; replace with real entropy if/when available. */
        for (uint64_t i = 0; i < sizeof(rnd); i++) rnd[i] = (uint8_t)(0xA5u ^ (uint8_t)i);
        sp -= sizeof(rnd);
        if (!user_range_ok(sp, sizeof(rnd))) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
        if (write_bytes_to_user(sp, rnd, sizeof(rnd)) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        random_addr = sp;
    }

    /* Align down for pointer writes. */
    sp = align_down_u64(sp, 16);

    /* auxv terminator first so it ends up last in memory order. */
    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_NULL) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, 0) != 0) return (uint64_t)(-(int64_t)EFAULT);

    /* Minimal auxv surface (best-effort). */
    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_SECURE) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, 0) != 0) return (uint64_t)(-(int64_t)EFAULT);

    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_RANDOM) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, random_addr) != 0) return (uint64_t)(-(int64_t)EFAULT);

    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_PLATFORM) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, platform_addr) != 0) return (uint64_t)(-(int64_t)EFAULT);

    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_EXECFN) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, execfn_addr) != 0) return (uint64_t)(-(int64_t)EFAULT);

    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_PAGESZ) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, 4096) != 0) return (uint64_t)(-(int64_t)EFAULT);

    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_ENTRY) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, entry) != 0) return (uint64_t)(-(int64_t)EFAULT);

    if (at_phent != 0 && at_phnum != 0) {
        sp -= 16u;
        if (write_u64_to_user(sp + 0, AT_PHENT) != 0) return (uint64_t)(-(int64_t)EFAULT);
        if (write_u64_to_user(sp + 8, at_phent) != 0) return (uint64_t)(-(int64_t)EFAULT);

        sp -= 16u;
        if (write_u64_to_user(sp + 0, AT_PHNUM) != 0) return (uint64_t)(-(int64_t)EFAULT);
        if (write_u64_to_user(sp + 8, at_phnum) != 0) return (uint64_t)(-(int64_t)EFAULT);
    }

    if (at_phdr != 0) {
        sp -= 16u;
        if (write_u64_to_user(sp + 0, AT_PHDR) != 0) return (uint64_t)(-(int64_t)EFAULT);
        if (write_u64_to_user(sp + 8, at_phdr) != 0) return (uint64_t)(-(int64_t)EFAULT);
    }

    /* Identity values for ids (single-user environment). */
    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_UID) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, 0) != 0) return (uint64_t)(-(int64_t)EFAULT);

    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_EUID) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, 0) != 0) return (uint64_t)(-(int64_t)EFAULT);

    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_GID) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, 0) != 0) return (uint64_t)(-(int64_t)EFAULT);

    sp -= 16u;
    if (write_u64_to_user(sp + 0, AT_EGID) != 0) return (uint64_t)(-(int64_t)EFAULT);
    if (write_u64_to_user(sp + 8, 0) != 0) return (uint64_t)(-(int64_t)EFAULT);

    /* envp NULL */
    sp -= 8u;
    if (write_u64_to_user(sp, 0) != 0) return (uint64_t)(-(int64_t)EFAULT);

    /* envp pointers */
    for (uint64_t i = envc; i > 0; i--) {
        sp -= 8u;
        if (write_u64_to_user(sp, envp_addrs[i - 1]) != 0) return (uint64_t)(-(int64_t)EFAULT);
    }
    uint64_t envp_ptr = sp;

    /* argv NULL */
    sp -= 8u;
    if (write_u64_to_user(sp, 0) != 0) return (uint64_t)(-(int64_t)EFAULT);

    /* argv pointers */
    for (uint64_t i = argc; i > 0; i--) {
        sp -= 8u;
        if (write_u64_to_user(sp, argv_addrs[i - 1]) != 0) return (uint64_t)(-(int64_t)EFAULT);
    }
    uint64_t argv_ptr = sp;

    /* argc */
    sp -= 8u;
    if (write_u64_to_user(sp, argc) != 0) return (uint64_t)(-(int64_t)EFAULT);

    tf->sp_el0 = sp;
    tf->x[0] = argc;
    tf->x[1] = argv_ptr;
    tf->x[2] = envp_ptr;

    write_sp_el0(sp);
    write_elr_el1(entry);

    /* Persist entry point for later reschedules (we may time-slice after execve). */
    g_procs[g_cur_proc].elr = entry;

    return 0;
}

static uint64_t sys_clone(trap_frame_t *tf, uint64_t flags, uint64_t child_stack, uint64_t ptid, uint64_t ctid, uint64_t tls, uint64_t elr) {
    (void)child_stack;
    (void)ptid;
    (void)ctid;
    (void)tls;

    /* Minimal fork-style clone(): allow *only* the low-byte exit signal (e.g. SIGCHLD).
     * This keeps userland simple and avoids Linux clone() complexity.
     */
    if ((flags & ~0xffull) != 0) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }

    int slot = proc_find_free_slot();
    if (slot < 0) {
        return (uint64_t)(-(int64_t)EMFILE);
    }

    uint64_t child_user_pa = pmm_alloc_2mib_aligned();
    if (child_user_pa == 0) {
        return (uint64_t)(-(int64_t)EMFILE);
    }

    uint64_t child_ttbr0 = mmu_ttbr0_create_with_user_pa(child_user_pa);
    if (child_ttbr0 == 0) {
        pmm_free_2mib_aligned(child_user_pa);
        return (uint64_t)(-(int64_t)EMFILE);
    }

    /* Copy current user image (VA USER_REGION_BASE) into the child's backing physical region. */
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)USER_REGION_BASE;
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)child_user_pa;
    for (uint64_t i = 0; i < USER_REGION_SIZE; i++) {
        dst[i] = src[i];
    }

    proc_t *parent = &g_procs[g_cur_proc];
    uint64_t pid = g_next_pid++;
    proc_clear(&g_procs[slot]);
    g_procs[slot].pid = pid;
    g_procs[slot].ppid = parent->pid;
    g_procs[slot].state = PROC_RUNNABLE;
    g_procs[slot].ttbr0_pa = child_ttbr0;
    g_procs[slot].user_pa_base = child_user_pa;
    tf_copy(&g_procs[slot].tf, tf);
    g_procs[slot].elr = elr;

    /* Inherit FD table (shared file descriptions). */
    for (uint64_t i = 0; i < MAX_FDS; i++) {
        int16_t didx = parent->fdt.fd_to_desc[i];
        g_procs[slot].fdt.fd_to_desc[i] = didx;
        if (didx >= 0) {
            desc_incref(didx);
        }
    }

    /* In the child, clone returns 0. */
    g_procs[slot].tf.x[0] = 0;

    /* Parent sees child's pid as return value. */
    return pid;
}

#define SYSCALL_SWITCHED 0xFFFFFFFFFFFFFFFFull

static uint64_t sys_wait4(trap_frame_t *tf, int64_t pid_req, uint64_t wstatus_user, uint64_t options, uint64_t rusage_user, uint64_t elr) {
    const uint64_t WNOHANG = 1ull;
    (void)rusage_user;

    proc_t *parent = &g_procs[g_cur_proc];
    uint64_t ppid = parent->pid;

    /* Find a zombie child matching pid_req. */
    int found = -1;
    for (int i = 0; i < (int)MAX_PROCS; i++) {
        if (g_procs[i].state != PROC_ZOMBIE) continue;
        if (g_procs[i].ppid != ppid) continue;
        if (pid_req > 0 && g_procs[i].pid != (uint64_t)pid_req) continue;
        found = i;
        break;
    }

    if (found >= 0) {
        uint64_t cpid = g_procs[found].pid;
        if (wstatus_user != 0) {
            if (!user_range_ok(wstatus_user, 4)) {
                return (uint64_t)(-(int64_t)EFAULT);
            }
            uint32_t st = (uint32_t)((g_procs[found].exit_code & 0xffu) << 8);
            *(volatile uint32_t *)(uintptr_t)wstatus_user = st;
        }

        /* Close child's resources, free backing, then reap. */
        proc_close_all_fds(&g_procs[found]);
        if (g_procs[found].user_pa_base != 0 && g_procs[found].user_pa_base != USER_REGION_BASE) {
            pmm_free_2mib_aligned(g_procs[found].user_pa_base);
        }
        proc_clear(&g_procs[found]);
        return cpid;
    }

    /* No children at all? */
    int any_child = 0;
    for (int i = 0; i < (int)MAX_PROCS; i++) {
        if (g_procs[i].state == PROC_UNUSED) continue;
        if (g_procs[i].ppid == ppid) {
            any_child = 1;
            break;
        }
    }
    if (!any_child) {
        return (uint64_t)(-(int64_t)ECHILD);
    }

    if ((options & WNOHANG) != 0) {
        return 0;
    }

    /* Block parent: it will be woken by child exit. */
    parent->state = PROC_WAITING;
    parent->wait_target_pid = pid_req;
    parent->wait_status_user = wstatus_user;
    tf_copy(&parent->tf, tf);
    parent->elr = elr;

    /* Switch to another runnable task. */
    int next = sched_pick_next_runnable();
    if (next >= 0 && next != g_cur_proc) {
        proc_switch_to(next, tf);
        return SYSCALL_SWITCHED;
    }

    /* No runnable tasks; keep running (busy) for now. */
    parent->state = PROC_RUNNABLE;
    parent->wait_target_pid = 0;
    parent->wait_status_user = 0;
    return (uint64_t)(-(int64_t)EAGAIN);
}

static int handle_exit_and_maybe_switch(trap_frame_t *tf, uint64_t code) {
    /* Mark current as zombie and wake its parent if waiting; otherwise keep zombie until reaped. */
    if (g_cur_proc == 0) {
        uart_write("\n[el0] exit_group status=");
        uart_write_hex_u64(code);
        uart_write("\n");
        return 0; /* no init process to return to */
    }

    int cidx = g_cur_proc;
    /* Close open file descriptors (important for pipes reaching EOF). */
    proc_close_all_fds(&g_procs[cidx]);
    g_procs[cidx].state = PROC_ZOMBIE;
    g_procs[cidx].exit_code = code;
    uint64_t cpid = g_procs[cidx].pid;

    /* Try to wake parent if it is waiting. */
    for (int i = 0; i < (int)MAX_PROCS; i++) {
        if (g_procs[i].state != PROC_WAITING) continue;
        if (g_procs[i].pid != g_procs[cidx].ppid) continue;

        int64_t want = g_procs[i].wait_target_pid;
        if (want > 0 && (uint64_t)want != cpid) {
            break;
        }

        /* Switch to parent's address space before writing status/return value. */
        mmu_ttbr0_write(g_procs[i].ttbr0_pa);

        uint64_t wstatus_user = g_procs[i].wait_status_user;
        if (wstatus_user != 0 && user_range_ok(wstatus_user, 4)) {
            uint32_t st = (uint32_t)((code & 0xffu) << 8);
            *(volatile uint32_t *)(uintptr_t)wstatus_user = st;
        }

        g_procs[i].state = PROC_RUNNABLE;
        g_procs[i].wait_target_pid = 0;
        g_procs[i].wait_status_user = 0;
        g_procs[i].tf.x[0] = cpid;
        break;
    }

    /* Switch to another runnable task. */
    int next = sched_pick_next_runnable();
    if (next >= 0 && next != g_cur_proc) {
        proc_switch_to(next, tf);
        return 1;
    }

    /* If nothing else runs, fall back to pid1 if runnable. */
    if (g_procs[0].state == PROC_RUNNABLE) {
        proc_switch_to(0, tf);
        return 1;
    }

    uart_write("\n[el0] exit_group status=");
    uart_write_hex_u64(code);
    uart_write("\n");
    return 0;
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

        case __NR_uname:
            ret = sys_uname(a0);
            break;

        case __NR_clock_gettime:
            ret = sys_clock_gettime(a0, a1);
            break;

        case __NR_dup3:
            ret = sys_dup3(a0, a1, a2);
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

        case __NR_newfstatat:
            ret = sys_newfstatat((int64_t)a0, a1, a2, a3);
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