#pragma once

#include "stdint.h"

/*
 * Very small in-kernel log ring buffer (dmesg).
 *
 * This is intentionally simple: single-writer (UART path), single-reader (syscall).
 */

void klog_putc(char c);

/* Current number of bytes retained in the buffer (<= capacity). */
uint64_t klog_len(void);

/* Return the i-th byte from the oldest retained byte (0 <= i < klog_len()). */
char klog_at(uint64_t i);

/* Clear the buffer. */
void klog_clear(void);
