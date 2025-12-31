#include "uart_pl011.h"
#include "arch.h"
#include "fdt.h"
#include "pmm.h"
#include "mmu.h"
#include "cache.h"
#include "initramfs.h"
#include "time.h"
#include "fb.h"
#include "termfb.h"

#ifndef FB_REQ_W
#define FB_REQ_W 1920u
#endif
#ifndef FB_REQ_H
#define FB_REQ_H 1080u
#endif
#ifndef FB_REQ_BPP
#define FB_REQ_BPP 32u
#endif

#ifndef FB_REQ_VIRT_H
#define FB_REQ_VIRT_H (FB_REQ_H * 2u)
#endif

extern unsigned char __kernel_start[];
extern unsigned char __kernel_end[];

static volatile uint64_t g_mmu_test = 0x1122334455667788ull;

extern void enter_el0(uint64_t entry, uint64_t user_sp);
extern unsigned char user_payload_start[];
extern unsigned char user_payload_end[];

extern unsigned char initramfs_start[];
extern unsigned char initramfs_end[];

void kmain(unsigned long dtb_ptr) {
    uart_init();

    time_init();

    uart_write("mona-rpzero aarch64 kernel\n");

    uart_write("current EL: ");
    uart_write_hex_u64(arch_current_el());
    uart_write("\n");

    uart_write("dtb: ");
    uart_write_hex_u64((unsigned long long)dtb_ptr);
    uart_write("\n");

    fdt_info_t info;
    if (dtb_ptr != 0 && fdt_read_info((const void *)dtb_ptr, &info) == 0) {
        fdt_print_info((const void *)dtb_ptr);

        uint64_t ks = (uint64_t)(unsigned long long)__kernel_start;
        uint64_t ke = (uint64_t)(unsigned long long)__kernel_end;

        pmm_init(info.mem_base, info.mem_size, ks, ke, (uint64_t)dtb_ptr);

        mmu_init_identity(info.mem_base, info.mem_size);

    #ifdef ENABLE_FB
        /* Best-effort framebuffer bring-up (QEMU-first). */
        if (fb_init_from_mailbox_ex(FB_REQ_W, FB_REQ_H, FB_REQ_W, FB_REQ_VIRT_H, FB_REQ_BPP) == 0) {
            /* Bring up a very small framebuffer console for early gfx testing. */
            (void)termfb_init(0x00ffffffu, 0x00203040u);
            uart_set_mirror(termfb_putc_ansi);
            termfb_write("mona-rpzero framebuffer console\n");

            const fb_info_t *fb = fb_get_info();
            termfb_write("fb: w="); termfb_write_hex_u64(fb->width);
            termfb_write(" h="); termfb_write_hex_u64(fb->height);
            termfb_write(" vh="); termfb_write_hex_u64(fb->virt_height);
            termfb_write(" bpp="); termfb_write_hex_u64(fb->bpp);
            termfb_write(" pitch="); termfb_write_hex_u64(fb->pitch);
            termfb_write("\n");
            termfb_write("(UART still active)\n\n");

            /* Quick ANSI smoke test (colors + reset). */
            termfb_write_ansi("\x1b[32mANSI ok\x1b[0m\n\n");
        }
    #endif

        uart_write("mmu: higher-half test\n");
        uint64_t low = g_mmu_test;
        uint64_t high = *(volatile uint64_t *)(uintptr_t)(KERNEL_VA_BASE + (uint64_t)(uintptr_t)&g_mmu_test);
        uart_write("  low ="); uart_write_hex_u64(low); uart_write("\n");
        uart_write("  high="); uart_write_hex_u64(high); uart_write("\n");

        uart_write("pmm: selftest alloc 3 pages\n");
        uint64_t a = pmm_alloc_page();
        uint64_t b = pmm_alloc_page();
        uint64_t c = pmm_alloc_page();
        uart_write("  a="); uart_write_hex_u64(a); uart_write("\n");
        uart_write("  b="); uart_write_hex_u64(b); uart_write("\n");
        uart_write("  c="); uart_write_hex_u64(c); uart_write("\n");
        pmm_free_page(b);
        pmm_free_page(a);
        pmm_free_page(c);
        uart_write("pmm: selftest done\n");
        pmm_dump();

        uart_write("el0: staging user payload\n");
        uint64_t blob_sz = (uint64_t)(uintptr_t)(user_payload_end - user_payload_start);
        volatile unsigned char *dst = (volatile unsigned char *)(uintptr_t)USER_REGION_BASE;
        for (uint64_t i = 0; i < blob_sz; i++) {
            dst[i] = user_payload_start[i];
        }
        cache_sync_icache_for_range(USER_REGION_BASE, blob_sz);

        uint64_t initramfs_sz = (uint64_t)(uintptr_t)(initramfs_end - initramfs_start);
        uart_write("initramfs: embedded size=");
        uart_write_hex_u64(initramfs_sz);
        uart_write("\n");
        initramfs_init(initramfs_start, (size_t)initramfs_sz);

        uart_write("el0: entering\n");
        enter_el0(USER_REGION_BASE, USER_REGION_BASE + USER_REGION_SIZE - 0x10ull);

        uart_write("el0: returned unexpectedly\n");
    } else {
        uart_write("fdt: unavailable; skipping pmm init\n");
    }

#if TEST_FAULT
    uart_write("TEST_FAULT=1: triggering data abort...\n");
    *(volatile unsigned long long *)0 = 0;
#endif

    uart_write("halting (wfe loop)\n");
    for (;;) {
        __asm__ volatile("wfe");
    }
}
