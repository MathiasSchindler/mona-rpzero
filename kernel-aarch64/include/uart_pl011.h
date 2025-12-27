#pragma once

#include "stdint.h"

/*
 * PL011 UART base address for BCM283x in the 0x3Fxxxxxx peripheral window.
 * For QEMU raspi3b and Pi Zero 2 W compatibility, this is the commonly used base.
 */
#ifndef UART_PL011_BASE
#define UART_PL011_BASE 0x3F201000u
#endif

void uart_init(void);
void uart_putc(char c);
void uart_write(const char *s);
void uart_write_hex_u64(uint64_t v);

/* Blocking / non-blocking receive helpers (for stdin via read(0)). */
int uart_try_getc(char *out);
char uart_getc_blocking(void);
