#pragma once

#include "stddef.h"
#include "stdint.h"

/*
 * Minimal framebuffer text console.
 *
 * - Assumes fb_init_from_mailbox() has already succeeded.
 * - Currently supports 32bpp XRGB8888.
 * - Keeps state internally (single global console).
 */

int termfb_init(uint32_t fg_xrgb8888, uint32_t bg_xrgb8888);

void termfb_set_colors(uint32_t fg_xrgb8888, uint32_t bg_xrgb8888);
void termfb_clear(void);

void termfb_putc(char c);
void termfb_write(const char *s);

void termfb_write_hex_u64(uint64_t v);
