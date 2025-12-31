#pragma once

#include "stdint.h"

typedef struct fb_info {
    /* Visible (physical) dimensions */
    uint32_t width;
    uint32_t height;

    /* Virtual buffer dimensions (may be larger than visible). */
    uint32_t virt_width;
    uint32_t virt_height;

    /* Current viewport offset inside the virtual buffer. */
    uint32_t x_offset;
    uint32_t y_offset;

    uint32_t pitch;
    uint32_t bpp;
    uint32_t size_bytes;
    uint64_t phys_addr;
    void *virt;
} fb_info_t;

/* Returns 0 on success, negative on error. */
int fb_init_from_mailbox(uint32_t req_w, uint32_t req_h, uint32_t req_bpp);

/* Extended init that allows requesting a larger virtual buffer (for fast scrolling). */
int fb_init_from_mailbox_ex(uint32_t phys_w, uint32_t phys_h,
                            uint32_t virt_w, uint32_t virt_h,
                            uint32_t req_bpp);

/* Set virtual viewport offset. Returns 0 on success, negative on error. */
int fb_set_virtual_offset(uint32_t x_off, uint32_t y_off);

const fb_info_t *fb_get_info(void);

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t xrgb8888);
void fb_fill(uint32_t xrgb8888);
