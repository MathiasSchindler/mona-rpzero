#include "console_in.h"

#include "uart_pl011.h"

#ifdef ENABLE_USB_KBD
#include "usb_kbd.h"
#endif

/* Keep it simple: single-producer/single-consumer under a cooperative kernel. */
#define CONSOLE_IN_RING_SIZE 1024u

static char g_ring[CONSOLE_IN_RING_SIZE];
static uint32_t g_r; /* read index */
static uint32_t g_w; /* write index */

static inline uint32_t ring_next(uint32_t idx) {
    idx++;
    if (idx >= CONSOLE_IN_RING_SIZE) idx = 0;
    return idx;
}

static void ring_push(char c) {
    uint32_t next = ring_next(g_w);
    if (next == g_r) {
        /* full: drop newest (safe default for console input) */
        return;
    }
    g_ring[g_w] = c;
    g_w = next;
}

void console_in_inject_char(char c) {
    ring_push(c);
}

static int ring_pop(char *out) {
    if (!out) return 0;
    if (g_r == g_w) return 0;
    *out = g_ring[g_r];
    g_r = ring_next(g_r);
    return 1;
}

void console_in_init(void) {
    g_r = 0;
    g_w = 0;
}

void console_in_poll(void) {
    /* UART RX */
    for (;;) {
        char c;
        if (!uart_try_getc(&c)) break;
        if (c == '\r') c = '\n';
        ring_push(c);
    }

#ifdef ENABLE_USB_KBD
    usb_kbd_poll();
#endif

    /* Future: additional input backends enqueue into the same ring. */
}

int console_in_has_data(void) {
    return g_r != g_w;
}

int console_in_pop(char *out) {
    return ring_pop(out);
}

int console_in_try_getc(char *out) {
    console_in_poll();
    return ring_pop(out);
}

char console_in_getc_blocking(void) {
    char c;
    while (!console_in_try_getc(&c)) {
        /* spin */
    }
    return c;
}

int console_in_needs_polling(void) {
#ifdef ENABLE_USB_KBD
    return 1;
#else
    return 0;
#endif
}
