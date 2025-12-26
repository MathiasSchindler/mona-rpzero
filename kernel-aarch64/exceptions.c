#include "uart_pl011.h"
#include "exceptions.h"
#include "initramfs.h"
#include "mmu.h"
#include "linux_abi.h"
#include "elf64.h"
#include "cache.h"

/* Linux AArch64 syscall numbers we care about first. */
#define __NR_openat    56ull
#define __NR_close     57ull
#define __NR_getdents64 61ull
#define __NR_lseek     62ull
#define __NR_read      63ull
#define __NR_write      64ull
#define __NR_newfstatat 79ull
#define __NR_clone      220ull
#define __NR_execve     221ull
#define __NR_wait4      260ull
#define __NR_exit       93ull
#define __NR_exit_group 94ull

/* errno values (Linux) */
#define EBADF 9ull
#define ECHILD 10ull
#define EFAULT 14ull
#define ENOENT 2ull
#define ENOSYS 38ull
#define EMFILE 24ull
#define EINVAL 22ull
#define EISDIR 21ull
#define ENOEXEC 8ull
#define E2BIG 7ull

#define AT_FDCWD ((int64_t)-100)

typedef struct {
    const uint8_t *data;
    uint64_t size;
    uint64_t off;
    uint32_t mode;
    uint8_t is_dir;
    char dir_path[128];
    uint8_t used;
} file_desc_t;

static file_desc_t g_fds[16];

typedef enum {
    PROC_UNUSED = 0,
    PROC_RUNNABLE = 1,
    PROC_WAITING = 2,
    PROC_ZOMBIE = 3,
} proc_state_t;

typedef struct {
    uint64_t pid;
    proc_state_t state;
    uint64_t ttbr0_pa;
    trap_frame_t tf;
    uint64_t elr;
    uint64_t exit_code;
    uint64_t wait_target_pid;
    uint64_t wait_status_user;
} proc_t;

static proc_t g_procs[2];
static int g_cur_proc = 0;
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
    p->state = PROC_UNUSED;
    p->ttbr0_pa = 0;
    tf_zero(&p->tf);
    p->elr = 0;
    p->exit_code = 0;
    p->wait_target_pid = 0;
    p->wait_status_user = 0;
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
    if (fd != 1 && fd != 2) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    const volatile char *p = (const volatile char *)buf;
    for (uint64_t i = 0; i < len; i++) {
        uart_putc(p[i]);
    }
    return len;
}

static void proc_init_if_needed(uint64_t elr, trap_frame_t *tf) {
    if (g_proc_inited) return;

    for (uint64_t i = 0; i < (uint64_t)(sizeof(g_fds) / sizeof(g_fds[0])); i++) {
        g_fds[i].used = 0;
    }

    proc_clear(&g_procs[0]);
    g_procs[0].pid = g_next_pid++;
    g_procs[0].state = PROC_RUNNABLE;
    g_procs[0].ttbr0_pa = mmu_ttbr0_read();
    tf_copy(&g_procs[0].tf, tf);
    g_procs[0].elr = elr;

    proc_clear(&g_procs[1]);
    g_cur_proc = 0;
    g_proc_inited = 1;
}

static int proc_find_child_of_init(void) {
    /* For now we only support a single init (proc 0) and at most one child (proc 1). */
    if (g_procs[1].state != PROC_UNUSED) return 1;
    return -1;
}

static void proc_switch_to(int idx, trap_frame_t *tf) {
    g_cur_proc = idx;
    mmu_ttbr0_write(g_procs[idx].ttbr0_pa);
    write_elr_el1(g_procs[idx].elr);
    tf_copy(tf, &g_procs[idx].tf);
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

    for (int fd = 3; fd < (int)(sizeof(g_fds) / sizeof(g_fds[0])); fd++) {
        if (!g_fds[fd].used) {
            g_fds[fd].used = 1;
            g_fds[fd].data = data;
            g_fds[fd].size = size;
            g_fds[fd].off = 0;
            g_fds[fd].mode = imode;
            g_fds[fd].is_dir = ((imode & 0170000u) == 0040000u) ? 1u : 0u;
            g_fds[fd].dir_path[0] = '\0';
            if (g_fds[fd].is_dir) {
                /* Store normalized path (leading slashes stripped, "/" -> ""). */
                const char *pp = path;
                while (*pp == '/') pp++;
                uint64_t i;
                for (i = 0; i + 1 < sizeof(g_fds[fd].dir_path) && pp[i] != '\0'; i++) {
                    g_fds[fd].dir_path[i] = pp[i];
                }
                g_fds[fd].dir_path[i] = '\0';
            }
            return (uint64_t)fd;
        }
    }
    return (uint64_t)(-(int64_t)EMFILE);
}

static uint64_t sys_close(uint64_t fd) {
    if (fd >= (uint64_t)(sizeof(g_fds) / sizeof(g_fds[0]))) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (fd < 3 || !g_fds[fd].used) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    g_fds[fd].used = 0;
    g_fds[fd].data = 0;
    g_fds[fd].size = 0;
    g_fds[fd].off = 0;
    g_fds[fd].mode = 0;
    g_fds[fd].is_dir = 0;
    g_fds[fd].dir_path[0] = '\0';
    return 0;
}

static uint64_t sys_read(uint64_t fd, uint64_t buf_user, uint64_t len) {
    /* stdin (UART) */
    if (fd == 0) {
        if (!user_range_ok(buf_user, len)) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        if (len == 0) return 0;

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

    if (fd >= (uint64_t)(sizeof(g_fds) / sizeof(g_fds[0]))) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (fd < 3 || !g_fds[fd].used) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (g_fds[fd].is_dir) {
        return (uint64_t)(-(int64_t)EINVAL);
    }
    if (!user_range_ok(buf_user, len)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    file_desc_t *f = &g_fds[fd];
    if (f->off >= f->size) return 0;

    uint64_t remain = f->size - f->off;
    uint64_t n = (len < remain) ? len : remain;

    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)buf_user;
    const uint8_t *src = f->data + f->off;
    for (uint64_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }

    f->off += n;
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
    if (fd >= (uint64_t)(sizeof(g_fds) / sizeof(g_fds[0]))) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (fd < 3 || !g_fds[fd].used || !g_fds[fd].is_dir) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (!user_range_ok(dirp_user, count)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    dents_ctx_t dc;
    dc.skip = g_fds[fd].off;
    dc.emitted = 0;
    dc.buf_user = dirp_user;
    dc.buf_len = count;
    dc.pos = 0;

    int rc = initramfs_list_dir(g_fds[fd].dir_path, dents_emit_cb, &dc);
    if (rc != 0) {
        return (uint64_t)(-(int64_t)ENOENT);
    }

    /* Advance directory position by entries emitted in this call. */
    if (dc.emitted > dc.skip) {
        g_fds[fd].off = dc.emitted;
    }

    return dc.pos;
}

static uint64_t sys_lseek(uint64_t fd, int64_t off, uint64_t whence) {
    if (fd >= (uint64_t)(sizeof(g_fds) / sizeof(g_fds[0]))) {
        return (uint64_t)(-(int64_t)EBADF);
    }
    if (fd < 3 || !g_fds[fd].used || g_fds[fd].is_dir) {
        return (uint64_t)(-(int64_t)EBADF);
    }

    uint64_t newoff;
    switch (whence) {
        case 0: /* SEEK_SET */
            if (off < 0) return (uint64_t)(-(int64_t)EINVAL);
            newoff = (uint64_t)off;
            break;
        case 1: /* SEEK_CUR */
            if (off < 0 && (uint64_t)(-off) > g_fds[fd].off) return (uint64_t)(-(int64_t)EINVAL);
            newoff = (uint64_t)((int64_t)g_fds[fd].off + off);
            break;
        case 2: /* SEEK_END */
            if (off < 0 && (uint64_t)(-off) > g_fds[fd].size) return (uint64_t)(-(int64_t)EINVAL);
            newoff = (uint64_t)((int64_t)g_fds[fd].size + off);
            break;
        default:
            return (uint64_t)(-(int64_t)EINVAL);
    }

    if (newoff > g_fds[fd].size) return (uint64_t)(-(int64_t)EINVAL);
    g_fds[fd].off = newoff;
    return newoff;
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

    /* Align down for pointer writes. */
    sp = align_down_u64(sp, 8);

    /* auxv: AT_NULL (type=0,val=0) */
    sp -= 16u;
    if (write_u64_to_user(sp + 0, 0) != 0) return (uint64_t)(-(int64_t)EFAULT);
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

    return 0;
}

static uint64_t sys_clone(trap_frame_t *tf, uint64_t flags, uint64_t child_stack, uint64_t ptid, uint64_t ctid, uint64_t tls, uint64_t elr) {
    (void)child_stack;
    (void)ptid;
    (void)ctid;
    (void)tls;

    /* Reject complex clone() uses; allow only the low-byte exit-signal (e.g. SIGCHLD). */
    const uint64_t forbidden = 0x00000100ull /* CLONE_VM */ |
                              0x00000200ull /* CLONE_FS */ |
                              0x00000400ull /* CLONE_FILES */ |
                              0x00000800ull /* CLONE_SIGHAND */ |
                              0x00010000ull /* CLONE_THREAD */;
    if ((flags & forbidden) != 0) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }

    if (g_procs[1].state != PROC_UNUSED) {
        return (uint64_t)(-(int64_t)EMFILE);
    }

    uint64_t child_ttbr0 = mmu_ttbr0_create_with_user_pa(USER_REGION_CHILD_PA);
    if (child_ttbr0 == 0) {
        return (uint64_t)(-(int64_t)EMFILE);
    }

    /* Copy current user image (VA USER_REGION_BASE) into child backing physical region. */
    const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)USER_REGION_BASE;
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)USER_REGION_CHILD_PA;
    for (uint64_t i = 0; i < USER_REGION_SIZE; i++) {
        dst[i] = src[i];
    }

    uint64_t pid = g_next_pid++;
    proc_clear(&g_procs[1]);
    g_procs[1].pid = pid;
    g_procs[1].state = PROC_RUNNABLE;
    g_procs[1].ttbr0_pa = child_ttbr0;
    tf_copy(&g_procs[1].tf, tf);
    g_procs[1].elr = elr;

    /* In the child, clone returns 0. */
    g_procs[1].tf.x[0] = 0;

    /* Parent sees child's pid as return value. */
    return pid;
}

static uint64_t sys_wait4(trap_frame_t *tf, int64_t pid_req, uint64_t wstatus_user, uint64_t options, uint64_t rusage_user, uint64_t elr) {
    (void)options;
    (void)rusage_user;

    /* Only init (proc 0) may wait for the single child. */
    if (g_cur_proc != 0) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }

    int cidx = proc_find_child_of_init();
    if (cidx < 0) {
        return (uint64_t)(-(int64_t)ECHILD);
    }

    uint64_t cpid = g_procs[cidx].pid;
    if (pid_req > 0 && (uint64_t)pid_req != cpid) {
        return (uint64_t)(-(int64_t)ECHILD);
    }

    if (g_procs[cidx].state == PROC_ZOMBIE) {
        if (wstatus_user != 0) {
            if (!user_range_ok(wstatus_user, 4)) {
                return (uint64_t)(-(int64_t)EFAULT);
            }
            /* Linux wait status encoding: (exit_code & 0xff) << 8 */
            uint32_t st = (uint32_t)((g_procs[cidx].exit_code & 0xffu) << 8);
            *(volatile uint32_t *)(uintptr_t)wstatus_user = st;
        }
        proc_clear(&g_procs[cidx]);
        return cpid;
    }

    /* Block parent and switch to child. */
    g_procs[0].state = PROC_WAITING;
    g_procs[0].wait_target_pid = (pid_req == -1) ? 0 : (uint64_t)pid_req;
    g_procs[0].wait_status_user = wstatus_user;
    tf_copy(&g_procs[0].tf, tf);
    g_procs[0].elr = elr;

    proc_switch_to(cidx, tf);
    return 0; /* return value for the *child* will be in its saved frame */
}

static int handle_exit_and_maybe_switch(trap_frame_t *tf, uint64_t code) {
    /* Mark current as zombie and switch back to init if waiting; otherwise halt. */
    if (g_cur_proc == 0) {
        uart_write("\n[el0] exit_group status=");
        uart_write_hex_u64(code);
        uart_write("\n");
        return 0; /* no init process to return to */
    }


    int cidx = g_cur_proc;
    g_procs[cidx].state = PROC_ZOMBIE;
    g_procs[cidx].exit_code = code;

    if (g_procs[0].state == PROC_WAITING) {
        uint64_t wstatus_user = g_procs[0].wait_status_user;
        uint64_t cpid = g_procs[cidx].pid;

        /* Switch to parent's address space before writing its status/return value. */
        mmu_ttbr0_write(g_procs[0].ttbr0_pa);

        if (wstatus_user != 0) {
            if (user_range_ok(wstatus_user, 4)) {
                uint32_t st = (uint32_t)((code & 0xffu) << 8);
                *(volatile uint32_t *)(uintptr_t)wstatus_user = st;
            }
        }

        g_procs[0].state = PROC_RUNNABLE;
        g_procs[0].wait_target_pid = 0;
        g_procs[0].wait_status_user = 0;

        /* Wake parent: wait4 returns child's pid. */
        g_procs[0].tf.x[0] = cpid;

        /* Reap child now. */
        proc_clear(&g_procs[cidx]);

        /* Return to parent. */
        g_cur_proc = 0;
        write_elr_el1(g_procs[0].elr);
        tf_copy(tf, &g_procs[0].tf);
        return 1;
    }

    /* Nobody is waiting; just switch to init if runnable. */
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

    uint64_t nr = tf->x[8];
    uint64_t a0 = tf->x[0];
    uint64_t a1 = tf->x[1];
    uint64_t a2 = tf->x[2];
    uint64_t a3 = tf->x[3];
    uint64_t a4 = tf->x[4];

    switch (nr) {
        case __NR_openat:
            tf->x[0] = sys_openat((int64_t)a0, a1, a2, a3);
            return 1;

        case __NR_close:
            tf->x[0] = sys_close(a0);
            return 1;

        case __NR_read:
            tf->x[0] = sys_read(a0, a1, a2);
            return 1;

        case __NR_getdents64:
            tf->x[0] = sys_getdents64(a0, a1, a2);
            return 1;

        case __NR_lseek:
            tf->x[0] = sys_lseek(a0, (int64_t)a1, a2);
            return 1;

        case __NR_write:
            tf->x[0] = sys_write(a0, (const void *)(uintptr_t)a1, a2);
            return 1;

        case __NR_newfstatat:
            tf->x[0] = sys_newfstatat((int64_t)a0, a1, a2, a3);
            return 1;

        case __NR_execve:
            proc_trace("execve", g_procs[g_cur_proc].pid, a0);
            tf->x[0] = sys_execve(tf, a0, a1, a2);
            tf_copy(&g_procs[g_cur_proc].tf, tf);
            return 1;

        case __NR_clone:
            proc_trace("clone", g_procs[g_cur_proc].pid, a0);
            tf->x[0] = sys_clone(tf, a0, a1, a2, a3, a4, elr);
            tf_copy(&g_procs[g_cur_proc].tf, tf);
            return 1;

        case __NR_wait4:
            proc_trace("wait4", g_procs[g_cur_proc].pid, (uint64_t)(int64_t)a0);
            tf->x[0] = sys_wait4(tf, (int64_t)a0, a1, a2, a3, elr);
            /* sys_wait4 may have switched to child; sync saved state. */
            tf_copy(&g_procs[g_cur_proc].tf, tf);
            return 1;

        case __NR_exit:
        case __NR_exit_group:
            proc_trace("exit", g_procs[g_cur_proc].pid, a0);
            return handle_exit_and_maybe_switch(tf, a0) ? 1 : 0;

        default:
            tf->x[0] = (uint64_t)(-(int64_t)ENOSYS);
            return 1;
    }
}