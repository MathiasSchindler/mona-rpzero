#include "fb.h"

#include "mailbox.h"
#include "mmu.h"
#include "pmm.h"
#include "uart_pl011.h"

#define FB_TAG_SET_PHYS_WH   0x00048003u
#define FB_TAG_SET_VIRT_WH   0x00048004u
#define FB_TAG_SET_DEPTH     0x00048005u
#define FB_TAG_SET_PIXEL_ORD 0x00048006u
#define FB_TAG_SET_VIRT_OFF  0x00048009u
#define FB_TAG_ALLOC_BUFFER  0x00040001u
#define FB_TAG_GET_PITCH     0x00040008u

#define FB_PIXEL_ORDER_BGR 0u
#define FB_PIXEL_ORDER_RGB 1u

static fb_info_t g_fb;

static inline uint32_t rpi_bus_to_phys_u32(uint32_t bus_addr) {
    /* Common Pi firmware convention: clear top bits to obtain physical address. */
    return bus_addr & 0x3FFFFFFFu;
}

const fb_info_t *fb_get_info(void) {
    return &g_fb;
}

int fb_set_virtual_offset(uint32_t x_off, uint32_t y_off) {
    if (!g_fb.virt) return -1;
    if (g_fb.virt_width == 0 || g_fb.virt_height == 0) return -1;
    if (x_off >= g_fb.virt_width) return -1;
    if (y_off >= g_fb.virt_height) return -1;
    /* Viewport origin must fit fully within the virtual framebuffer. */
    if (g_fb.width != 0 && x_off > (g_fb.virt_width - g_fb.width)) return -1;
    if (g_fb.height != 0 && y_off > (g_fb.virt_height - g_fb.height)) return -1;

    static uint32_t msg[16] __attribute__((aligned(16)));
    uint32_t i = 2;
    msg[i++] = FB_TAG_SET_VIRT_OFF;
    msg[i++] = 8;
    msg[i++] = 0;
    msg[i++] = x_off;
    msg[i++] = y_off;
    msg[i++] = 0;

    uint32_t msg_bytes = i * 4u;
    msg_bytes = (msg_bytes + 15u) & ~15u;

    if (mailbox_property_call(msg, msg_bytes) != 0) {
        return -1;
    }

    /* Firmware may clamp; read back what it returned. */
    g_fb.x_offset = msg[2 + 3];
    g_fb.y_offset = msg[2 + 4];
    return 0;
}

int fb_init_from_mailbox(uint32_t req_w, uint32_t req_h, uint32_t req_bpp) {
    return fb_init_from_mailbox_ex(req_w, req_h, req_w, req_h, req_bpp);
}

int fb_init_from_mailbox_ex(uint32_t phys_w, uint32_t phys_h,
                            uint32_t virt_w, uint32_t virt_h,
                            uint32_t req_bpp) {
    /* Message is in 32-bit words and must be 16-byte aligned. */
    static uint32_t msg[64] __attribute__((aligned(16)));

    /*
     * Property message layout:
     *  msg[0] = total bytes
     *  msg[1] = request/response code
     *  tags...
     *  msg[n] = 0 end tag
     */
    uint32_t i = 2;

    /* Set physical width/height */
    uint32_t phys_wh_tag_idx = i;
    msg[i++] = FB_TAG_SET_PHYS_WH;
    msg[i++] = 8;
    msg[i++] = 0;
    msg[i++] = phys_w;
    msg[i++] = phys_h;

    /* Set virtual width/height */
    uint32_t virt_wh_tag_idx = i;
    msg[i++] = FB_TAG_SET_VIRT_WH;
    msg[i++] = 8;
    msg[i++] = 0;
    msg[i++] = virt_w;
    msg[i++] = virt_h;

    /* Set depth */
    uint32_t depth_tag_idx = i;
    msg[i++] = FB_TAG_SET_DEPTH;
    msg[i++] = 4;
    msg[i++] = 0;
    msg[i++] = req_bpp;

    /* Pixel order */
    msg[i++] = FB_TAG_SET_PIXEL_ORD;
    msg[i++] = 4;
    msg[i++] = 0;
    msg[i++] = FB_PIXEL_ORDER_RGB;

    /* Allocate buffer (alignment, returns bus addr + size) */
    uint32_t alloc_tag_idx = i;
    msg[i++] = FB_TAG_ALLOC_BUFFER;
    msg[i++] = 8;
    msg[i++] = 0;
    msg[i++] = 0x200000u; /* 2MiB alignment (matches our coarse MMU blocks) */
    msg[i++] = 0;  /* will be overwritten with size */

    /* Get pitch */
    uint32_t pitch_tag_idx = i;
    msg[i++] = FB_TAG_GET_PITCH;
    msg[i++] = 4;
    msg[i++] = 0;
    msg[i++] = 0;

    msg[i++] = 0; /* end tag */

    /* Round message size up to 16 bytes as required by mailbox_property_call. */
    uint32_t msg_bytes = i * 4u;
    msg_bytes = (msg_bytes + 15u) & ~15u;

    if (mailbox_property_call(msg, msg_bytes) != 0) {
        uart_write("fb: mailbox_property_call failed\n");
        return -1;
    }

    /* Parse responses. Offsets are fixed by construction above. */
    uint32_t phys_w_ret = msg[phys_wh_tag_idx + 3];
    uint32_t phys_h_ret = msg[phys_wh_tag_idx + 4];
    uint32_t virt_w_ret = msg[virt_wh_tag_idx + 3];
    uint32_t virt_h_ret = msg[virt_wh_tag_idx + 4];
    uint32_t depth = msg[depth_tag_idx + 3];

    uint32_t bus_addr = msg[alloc_tag_idx + 3];
    uint32_t fb_size = msg[alloc_tag_idx + 4];
    uint32_t pitch = msg[pitch_tag_idx + 3];

    if (phys_w_ret == 0 || phys_h_ret == 0 || virt_w_ret == 0 || virt_h_ret == 0 || depth == 0 || bus_addr == 0 || fb_size == 0 || pitch == 0) {
        uart_write("fb: invalid response addr/size/pitch\n");
        uart_write("fb: phys="); uart_write_hex_u64(phys_w_ret); uart_write("x"); uart_write_hex_u64(phys_h_ret); uart_write("\n");
        uart_write("fb: virt="); uart_write_hex_u64(virt_w_ret); uart_write("x"); uart_write_hex_u64(virt_h_ret); uart_write("\n");
        uart_write("fb: depth="); uart_write_hex_u64(depth); uart_write("\n");
        uart_write("fb: bus_addr="); uart_write_hex_u64(bus_addr); uart_write("\n");
        uart_write("fb: fb_size="); uart_write_hex_u64(fb_size); uart_write("\n");
        uart_write("fb: pitch="); uart_write_hex_u64(pitch); uart_write("\n");
        return -1;
    }

    /* Use the values returned by firmware/QEMU (it may clamp/adjust). */
    g_fb.width = phys_w_ret;
    g_fb.height = phys_h_ret;
    g_fb.virt_width = virt_w_ret;
    g_fb.virt_height = virt_h_ret;
    g_fb.x_offset = 0;
    g_fb.y_offset = 0;
    g_fb.bpp = depth;
    g_fb.pitch = pitch;
    g_fb.size_bytes = fb_size;
    g_fb.phys_addr = (uint64_t)rpi_bus_to_phys_u32(bus_addr);

    /* Prevent the allocator from handing out framebuffer RAM. */
    pmm_reserve_range(g_fb.phys_addr, g_fb.phys_addr + (uint64_t)g_fb.size_bytes);

    /*
     * Memory type for the framebuffer:
     * - On real hardware, mapping it as DEVICE avoids cache coherency issues (we don't clean caches on every draw).
     * - Under QEMU, DEVICE mappings are extremely slow for per-pixel text rendering; keep it NORMAL for speed.
     */
#ifndef QEMU_SEMIHOSTING
    if (mmu_mark_region_device(g_fb.phys_addr, (uint64_t)g_fb.size_bytes) != 0) {
        uart_write("fb: warning: failed to mark fb region as device\n");
    }
#endif

    /* Access via higher-half mapping (same shared L2 table). */
    g_fb.virt = (void *)(uintptr_t)(KERNEL_VA_BASE + g_fb.phys_addr);

    /* Ensure viewport is at (0,0) if supported. */
    (void)fb_set_virtual_offset(0, 0);

    uart_write("fb: initialized w=");
    uart_write_hex_u64(g_fb.width);
    uart_write(" h=");
    uart_write_hex_u64(g_fb.height);
    uart_write(" vw=");
    uart_write_hex_u64(g_fb.virt_width);
    uart_write(" vh=");
    uart_write_hex_u64(g_fb.virt_height);
    uart_write(" bpp=");
    uart_write_hex_u64(g_fb.bpp);
    uart_write(" pitch=");
    uart_write_hex_u64(g_fb.pitch);
    uart_write(" addr=");
    uart_write_hex_u64(g_fb.phys_addr);
    uart_write(" size=");
    uart_write_hex_u64(g_fb.size_bytes);
    uart_write("\n");

    return 0;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t xrgb8888) {
    if (!g_fb.virt) return;
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (g_fb.bpp != 32) return;

    uint32_t yy = y;
    if (g_fb.virt_height != 0) {
        yy = (uint32_t)((uint64_t)yy + (uint64_t)g_fb.y_offset) % g_fb.virt_height;
    }

    volatile uint32_t *row = (volatile uint32_t *)((uintptr_t)g_fb.virt + (uintptr_t)yy * (uintptr_t)g_fb.pitch);
    row[x] = xrgb8888;
}

void fb_fill(uint32_t xrgb8888) {
    if (!g_fb.virt) return;
    if (g_fb.bpp != 32) return;

    for (uint32_t y = 0; y < g_fb.height; y++) {
        uint32_t yy = y;
        if (g_fb.virt_height != 0) {
            yy = (uint32_t)((uint64_t)yy + (uint64_t)g_fb.y_offset) % g_fb.virt_height;
        }
        volatile uint32_t *row = (volatile uint32_t *)((uintptr_t)g_fb.virt + (uintptr_t)yy * (uintptr_t)g_fb.pitch);
        for (uint32_t x = 0; x < g_fb.width; x++) {
            row[x] = xrgb8888;
        }
    }
}
