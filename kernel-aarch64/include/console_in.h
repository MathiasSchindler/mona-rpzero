#pragma once

#include "stdint.h"

/*
 * Console input multiplexer.
 *
 * Today this polls the UART RX FIFO.
 * Soon this will also poll a real keyboard device (e.g. QEMU usb-kbd) so that
 * typing while focused on the QEMU graphics window feeds the framebuffer shell.
 */

void console_in_init(void);

/* Poll all configured input sources and enqueue any newly received characters. */
void console_in_poll(void);

/* Inject a character from a non-UART input backend (e.g. USB keyboard). */
void console_in_inject_char(char c);

/* Non-blocking: returns 1 if a char was read into *out, else 0. */
int console_in_try_getc(char *out);

/* Blocking: spins until a character is available. */
char console_in_getc_blocking(void);
