#pragma once

#include "stdint.h"

typedef struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint64_t phys_addr;
    void *virt;
} fb_info_t;

/* Returns 0 on success, negative on error. */
int fb_init_from_mailbox(uint32_t req_w, uint32_t req_h, uint32_t req_bpp);

const fb_info_t *fb_get_info(void);

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t xrgb8888);
void fb_fill(uint32_t xrgb8888);
