#pragma once

#include "stdint.h"

#include "usb_host.h"

/*
 * Experimental: USB HID boot keyboard input.
 *
 * QEMU-first goal: allow typing while focused in the QEMU graphics window
 * (not via UART/stdio).
 *
 * Implementation targets the Pi's integrated Synopsys DWC2 controller.
 */

void usb_kbd_init(void);

/* Attempt to bind a keyboard device discovered during USB enumeration.
 * Returns 0 if claimed.
 */
int usb_kbd_try_bind(const usb_device_t *dev);

/* Poll USB controller; if a key was pressed, emit ASCII via console_in. */
void usb_kbd_poll(void);

/* Returns non-zero once a keyboard is configured and reports are flowing. */
int usb_kbd_is_ready(void);
