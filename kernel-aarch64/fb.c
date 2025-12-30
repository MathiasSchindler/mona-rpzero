#include "fb.h"

#include "mailbox.h"
#include "uart_pl011.h"

#define FB_TAG_SET_PHYS_WH   0x00048003u
#define FB_TAG_SET_VIRT_WH   0x00048004u
#define FB_TAG_SET_DEPTH     0x00048005u
#define FB_TAG_SET_PIXEL_ORD 0x00048006u
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

int fb_init_from_mailbox(uint32_t req_w, uint32_t req_h, uint32_t req_bpp) {
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
    msg[i++] = FB_TAG_SET_PHYS_WH;
    msg[i++] = 8;
    msg[i++] = 0;
    msg[i++] = req_w;
    msg[i++] = req_h;

    /* Set virtual width/height */
    msg[i++] = FB_TAG_SET_VIRT_WH;
    msg[i++] = 8;
    msg[i++] = 0;
    msg[i++] = req_w;
    msg[i++] = req_h;

    /* Set depth */
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
    msg[i++] = 16; /* alignment */
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
    uint32_t bus_addr = msg[alloc_tag_idx + 3];
    uint32_t fb_size = msg[alloc_tag_idx + 4];
    uint32_t pitch = msg[pitch_tag_idx + 3];

    if (bus_addr == 0 || fb_size == 0 || pitch == 0) {
        uart_write("fb: invalid response addr/size/pitch\n");
        uart_write("fb: bus_addr="); uart_write_hex_u64(bus_addr); uart_write("\n");
        uart_write("fb: fb_size="); uart_write_hex_u64(fb_size); uart_write("\n");
        uart_write("fb: pitch="); uart_write_hex_u64(pitch); uart_write("\n");
        return -1;
    }

    g_fb.width = req_w;
    g_fb.height = req_h;
    g_fb.bpp = req_bpp;
    g_fb.pitch = pitch;
    g_fb.phys_addr = (uint64_t)rpi_bus_to_phys_u32(bus_addr);
    g_fb.virt = (void *)(uintptr_t)g_fb.phys_addr; /* identity-mapped initially */

    uart_write("fb: initialized ");
    uart_write_hex_u64(g_fb.width);
    uart_write("x");
    uart_write_hex_u64(g_fb.height);
    uart_write(" bpp=");
    uart_write_hex_u64(g_fb.bpp);
    uart_write(" pitch=");
    uart_write_hex_u64(g_fb.pitch);
    uart_write(" addr=");
    uart_write_hex_u64(g_fb.phys_addr);
    uart_write("\n");

    return 0;
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t xrgb8888) {
    if (!g_fb.virt) return;
    if (x >= g_fb.width || y >= g_fb.height) return;
    if (g_fb.bpp != 32) return;

    volatile uint32_t *row = (volatile uint32_t *)((uintptr_t)g_fb.virt + (uintptr_t)y * (uintptr_t)g_fb.pitch);
    row[x] = xrgb8888;
}

void fb_fill(uint32_t xrgb8888) {
    if (!g_fb.virt) return;
    if (g_fb.bpp != 32) return;

    for (uint32_t y = 0; y < g_fb.height; y++) {
        volatile uint32_t *row = (volatile uint32_t *)((uintptr_t)g_fb.virt + (uintptr_t)y * (uintptr_t)g_fb.pitch);
        for (uint32_t x = 0; x < g_fb.width; x++) {
            row[x] = xrgb8888;
        }
    }
}
