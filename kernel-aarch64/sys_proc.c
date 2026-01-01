#include "syscalls.h"

#include "cache.h"
#include "elf64.h"
#include "errno.h"
#include "initramfs.h"
#include "linux_abi.h"
#include "mmu.h"
#include "pmm.h"
#include "power.h"
#include "proc.h"
#include "regs.h"
#include "sched.h"
#include "stat_bits.h"
#include "sys_util.h"
#include "uart_pl011.h"

static void byte_copy(void *dst, const uint8_t *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = src[i];
    }
}

uint64_t sys_brk(uint64_t newbrk) {
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

uint64_t sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off) {
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

uint64_t sys_munmap(uint64_t addr, uint64_t len) {
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

uint64_t sys_execve(trap_frame_t *tf, uint64_t pathname_user, uint64_t argv_user, uint64_t envp_user) {
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

    char in[MAX_PATH];
    if (copy_cstr_from_user(in, sizeof(in), pathname_user) != 0) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    proc_t *cur = &g_procs[g_cur_proc];
    char path[MAX_PATH];
    if (resolve_path(cur, in, path, sizeof(path)) != 0) {
        return (uint64_t)(-(int64_t)EINVAL);
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
    if (S_ISDIR(mode)) {
        return (uint64_t)(-(int64_t)EISDIR);
    }

    uint64_t entry = 0;
    uint64_t minva = 0;
    uint64_t maxva = 0;
    uint64_t user_pa_base = cur->user_pa_base;
    if (user_pa_base == 0) {
        user_pa_base = USER_REGION_BASE;
    }
    if (elf64_load_etexec(img, (size_t)img_size, USER_REGION_BASE, USER_REGION_SIZE, user_pa_base, &entry, &minva, &maxva) != 0) {
        return (uint64_t)(-(int64_t)ENOEXEC);
    }

    /*
     * Touch a few words of the freshly loaded image via both the user VA and
     * the backing physical alias. This helps avoid occasional stale/garbled
     * instruction fetches observed under QEMU when execve() replaces the image.
     */
    if (maxva > minva) {
        volatile uint32_t touch = 0;
        uint64_t va0 = minva;
        uint64_t pa0 = user_pa_base + (va0 - USER_REGION_BASE);
        touch ^= *(volatile uint32_t *)(uintptr_t)va0;
        touch ^= *(volatile uint32_t *)(uintptr_t)pa0;

        uint64_t va1 = minva + 256u;
        if (va1 + 4u <= maxva) {
            uint64_t pa1 = user_pa_base + (va1 - USER_REGION_BASE);
            touch ^= *(volatile uint32_t *)(uintptr_t)va1;
            touch ^= *(volatile uint32_t *)(uintptr_t)pa1;
        }
        (void)touch;
    }

    /* Compute auxiliary vectors derived from the ELF header.
     * Best-effort: if we can't derive AT_PHDR safely, we omit it.
     */
    uint64_t at_phdr = 0;
    uint64_t at_phent = 0;
    uint64_t at_phnum = 0;
    {
        if (img_size >= sizeof(elf64_ehdr_t)) {
            elf64_ehdr_t eh;
            byte_copy(&eh, img, sizeof(eh));

            at_phent = (uint64_t)eh.e_phentsize;
            at_phnum = (uint64_t)eh.e_phnum;

            uint64_t ph_end = eh.e_phoff + (uint64_t)eh.e_phnum * (uint64_t)eh.e_phentsize;
            if (ph_end >= eh.e_phoff && ph_end <= img_size && eh.e_phentsize == sizeof(elf64_phdr_t)) {
                /* Prefer PT_PHDR if present. */
                for (uint16_t i = 0; i < eh.e_phnum; i++) {
                    elf64_phdr_t ph;
                    uint64_t ph_off = eh.e_phoff + (uint64_t)i * sizeof(elf64_phdr_t);
                    if (ph_off + sizeof(ph) > img_size) break;
                    byte_copy(&ph, img + ph_off, sizeof(ph));

                    if (ph.p_type == PT_PHDR) {
                        at_phdr = ph.p_vaddr;
                        break;
                    }
                }

                /* Fallback: program headers are typically within the first PT_LOAD segment. */
                if (at_phdr == 0) {
                    for (uint16_t i = 0; i < eh.e_phnum; i++) {
                        elf64_phdr_t ph;
                        uint64_t ph_off = eh.e_phoff + (uint64_t)i * sizeof(elf64_phdr_t);
                        if (ph_off + sizeof(ph) > img_size) break;
                        byte_copy(&ph, img + ph_off, sizeof(ph));

                        if (ph.p_type != PT_LOAD) continue;
                        if (ph.p_offset != 0) continue;

                        /* Ensure ELF header + phdr table are within this LOAD in file image. */
                        uint64_t need_end = eh.e_phoff + (uint64_t)eh.e_phnum * sizeof(elf64_phdr_t);
                        if (need_end <= ph.p_filesz) {
                            at_phdr = ph.p_vaddr + eh.e_phoff;
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

    /* Build an initial user stack containing argc/argv/envp/auxv (minimal). */
    uint64_t sp = USER_REGION_BASE + USER_REGION_SIZE;
    uint64_t argv_addrs[MAX_ARGS];
    uint64_t envp_addrs[MAX_ENVP];

    /* Extra auxv-backed strings/blobs placed on the user stack. */
    uint64_t execfn_addr = 0;
    uint64_t platform_addr = 0;
    uint64_t random_addr = 0;

    /* Copy strings near the top of the stack so pointers can reference them. */
    for (uint64_t i = 0; i < argc; i++) {
        uint64_t len2 = cstr_len(arg_strs[i]) + 1u;
        sp -= len2;
        if (!user_range_ok(sp, len2)) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
        if (write_bytes_to_user(sp, arg_strs[i], len2) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        argv_addrs[i] = sp;
    }

    for (uint64_t i = 0; i < envc; i++) {
        uint64_t len2 = cstr_len(env_strs[i]) + 1u;
        sp -= len2;
        if (!user_range_ok(sp, len2)) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
        if (write_bytes_to_user(sp, env_strs[i], len2) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        envp_addrs[i] = sp;
    }

    /* execfn (full path) */
    {
        uint64_t len2 = cstr_len(path) + 1u;
        sp -= len2;
        if (!user_range_ok(sp, len2)) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
        if (write_bytes_to_user(sp, path, len2) != 0) {
            return (uint64_t)(-(int64_t)EFAULT);
        }
        execfn_addr = sp;
    }

    /* platform */
    {
        static const char platform[] = "aarch64";
        uint64_t len2 = sizeof(platform);
        sp -= len2;
        if (!user_range_ok(sp, len2)) {
            return (uint64_t)(-(int64_t)E2BIG);
        }
        if (write_bytes_to_user(sp, platform, len2) != 0) {
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

    /*
     * execve() replaces the current process image without switching processes.
     * Since we don't use ASIDs, stale VA-tagged cache lines can survive across
     * the image replacement. Flush here to ensure EL0 always fetches the newly
     * loaded instructions/data.
     */
    cache_clean_invalidate_all();

    return 0;
}

uint64_t sys_clone(trap_frame_t *tf, uint64_t flags, uint64_t child_stack, uint64_t ptid, uint64_t ctid, uint64_t tls, uint64_t elr) {
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

    /* Inherit cwd. */
    for (uint64_t i = 0; i < MAX_PATH; i++) {
        g_procs[slot].cwd[i] = parent->cwd[i];
        if (parent->cwd[i] == '\0') {
            break;
        }
        if (i == MAX_PATH - 1) {
            g_procs[slot].cwd[i] = '\0';
        }
    }

    /* In the child, clone returns 0. */
    g_procs[slot].tf.x[0] = 0;

    /* Parent sees child's pid as return value. */
    return pid;
}

uint64_t sys_wait4(trap_frame_t *tf, int64_t pid_req, uint64_t wstatus_user, uint64_t options, uint64_t rusage_user, uint64_t elr) {
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

int handle_exit_and_maybe_switch(trap_frame_t *tf, uint64_t code) {
    /* Mark current as zombie and wake its parent if waiting; otherwise keep zombie until reaped. */
    if (g_cur_proc == 0) {
        uart_write("\n[el0] exit_group status=");
        uart_write_hex_u64(code);
        uart_write("\n");
        kernel_poweroff_with_code((uint32_t)(code & 0xffu));
    }

    int cidx = g_cur_proc;
    /* Close open file descriptors (important for pipes reaching EOF). */
    proc_close_all_fds(&g_procs[cidx]);

    /* Best-effort thread-lib compatibility: clear *clear_child_tid on exit. */
    if (g_procs[cidx].clear_child_tid_user != 0 && user_range_ok(g_procs[cidx].clear_child_tid_user, 4)) {
        *(volatile uint32_t *)(uintptr_t)g_procs[cidx].clear_child_tid_user = 0;
    }

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

        /* Parent was already blocked in wait4; reap the child now. */
        if (g_procs[cidx].user_pa_base != 0 && g_procs[cidx].user_pa_base != USER_REGION_BASE) {
            pmm_free_2mib_aligned(g_procs[cidx].user_pa_base);
        }
        proc_clear(&g_procs[cidx]);
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

static int proc_find_idx_by_pid(uint64_t pid) {
    for (int i = 0; i < (int)MAX_PROCS; i++) {
        if (g_procs[i].state == PROC_UNUSED) continue;
        if (g_procs[i].pid == pid) return i;
    }
    return -1;
}

uint64_t sys_kill(trap_frame_t *tf, int64_t pid, uint64_t sig, uint64_t elr) {
    (void)elr;

    if (pid <= 0) {
        return (uint64_t)(-(int64_t)EINVAL);
    }

    /* Minimal support: sig=0 (existence check), SIGKILL(9), SIGTERM(15). */
    if (sig != 0 && sig != 9 && sig != 15) {
        return (uint64_t)(-(int64_t)ENOSYS);
    }

    int idx = proc_find_idx_by_pid((uint64_t)pid);
    if (idx < 0) {
        return (uint64_t)(-(int64_t)ESRCH);
    }

    if (sig == 0) {
        return 0;
    }

    /* If already dead, treat as success. */
    if (g_procs[idx].state == PROC_ZOMBIE) {
        return 0;
    }

    uint64_t code = 128u + (sig & 0xffu);

    /* Self-kill: reuse the normal exit path so we switch properly. */
    if (idx == g_cur_proc) {
        int switched = handle_exit_and_maybe_switch(tf, code);
        return switched ? SYSCALL_SWITCHED : 0;
    }

    /* Kill another process: mark zombie and wake a waiting parent if present. */
    proc_close_all_fds(&g_procs[idx]);

    /* Best-effort thread-lib compatibility: clear *clear_child_tid on exit. */
    if (g_procs[idx].clear_child_tid_user != 0) {
        mmu_ttbr0_write(g_procs[idx].ttbr0_pa);
        if (user_range_ok(g_procs[idx].clear_child_tid_user, 4)) {
            *(volatile uint32_t *)(uintptr_t)g_procs[idx].clear_child_tid_user = 0;
        }
    }

    g_procs[idx].state = PROC_ZOMBIE;
    g_procs[idx].exit_code = code;
    uint64_t cpid = g_procs[idx].pid;

    for (int i = 0; i < (int)MAX_PROCS; i++) {
        if (g_procs[i].state != PROC_WAITING) continue;
        if (g_procs[i].pid != g_procs[idx].ppid) continue;

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

        /* Parent was blocked in wait4; reap the child now. */
        if (g_procs[idx].user_pa_base != 0 && g_procs[idx].user_pa_base != USER_REGION_BASE) {
            pmm_free_2mib_aligned(g_procs[idx].user_pa_base);
        }
        proc_clear(&g_procs[idx]);
        break;
    }

    /* Restore current process address space before returning to user. */
    mmu_ttbr0_write(g_procs[g_cur_proc].ttbr0_pa);
    return 0;
}
