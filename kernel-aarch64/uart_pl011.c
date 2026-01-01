#include "uart_pl011.h"

#include "klog.h"

#define UART_DR   0x00u
#define UART_FR   0x18u
#define UART_IBRD 0x24u
#define UART_FBRD 0x28u
#define UART_LCRH 0x2Cu
#define UART_CR   0x30u
#define UART_IMSC 0x38u
#define UART_ICR  0x44u

#define FR_TXFF (1u << 5)
#define FR_RXFE (1u << 4)

static void (*g_uart_mirror_putc)(char c) = 0;

static inline volatile uint32_t *uart_reg(uint32_t off) {
    return (volatile uint32_t *)((uintptr_t)UART_PL011_BASE + off);
}

void uart_init(void) {
    /*
     * Keep init conservative: QEMU usually tolerates writes here; if firmware/QEMU
     * already configured UART, this won't break it.
     */
    *uart_reg(UART_CR) = 0;
    *uart_reg(UART_ICR) = 0x7FF;

    /* 8N1, FIFO off for simplicity */
    *uart_reg(UART_LCRH) = (3u << 5);

    /* Mask interrupts */
    *uart_reg(UART_IMSC) = 0;

    /* Enable UART, TX, RX */
    *uart_reg(UART_CR) = (1u << 0) | (1u << 8) | (1u << 9);

    (void)FR_RXFE;
}

void uart_set_mirror(void (*mirror_putc)(char c)) {
    g_uart_mirror_putc = mirror_putc;
}

static void uart_putc_hw(char c) {
    while ((*uart_reg(UART_FR) & FR_TXFF) != 0) {
        /* spin */
    }
    *uart_reg(UART_DR) = (uint32_t)c;
}

void uart_putc(char c) {
    if (c == '\n') {
        uart_putc_hw('\r');
        uart_putc_hw('\n');

        if (g_uart_mirror_putc) {
            g_uart_mirror_putc('\n');
        }
        return;
    }

    uart_putc_hw(c);

    if (g_uart_mirror_putc) {
        g_uart_mirror_putc(c);
    }
}

void uart_write(const char *s) {
    while (*s) {
        char c = *s++;
        if (c == '\n') {
            /* Store a single '\n' in the kernel log (avoid CRLF duplication). */
            klog_putc('\n');
        } else {
            klog_putc(c);
        }
        uart_putc(c);
    }
}

void uart_write_hex_u64(uint64_t v) {
    static const char *hex = "0123456789abcdef";
    uart_write("0x");
    for (int i = 60; i >= 0; i -= 4) {
        char c = hex[(v >> (unsigned)i) & 0xFu];
        klog_putc(c);
        uart_putc(c);
    }
}

int uart_try_getc(char *out) {
    if (!out) return 0;
    if ((*uart_reg(UART_FR) & FR_RXFE) != 0) {
        return 0;
    }
    *out = (char)(*uart_reg(UART_DR) & 0xFFu);
    return 1;
}

char uart_getc_blocking(void) {
    char c;
    while (!uart_try_getc(&c)) {
        /* spin */
    }
    return c;
}
