#include "usb_kbd.h"

#include "console_in.h"
#include "uart_pl011.h"
#include "usb_host.h"

#ifdef ENABLE_USB_KBD_DEBUG
#define USB_KBD_DEBUG 1
#else
#define USB_KBD_DEBUG 0
#endif

typedef struct {
    int bound;
    int ready;

    uint8_t addr;
    int low_speed;

    usb_ep_t intr_in;
    uint32_t intr_in_pid;

    uint8_t last_report[8];
} usb_kbd_state_t;

static usb_kbd_state_t g;

static char hid_keycode_to_ascii(uint8_t keycode, int shift) {
    if (keycode >= 0x04 && keycode <= 0x1d) {
        char c = (char)('a' + (keycode - 0x04));
        if (shift) c = (char)('A' + (keycode - 0x04));
        return c;
    }
    if (keycode >= 0x1e && keycode <= 0x27) {
        static const char *no = "1234567890";
        static const char *sh = "!@#$%^&*()";
        return shift ? sh[keycode - 0x1e] : no[keycode - 0x1e];
    }
    switch (keycode) {
        case 0x28: return '\n';
        case 0x2a: return '\b';
        case 0x2b: return '\t';
        case 0x2c: return ' ';
        case 0x2d: return shift ? '_' : '-';
        case 0x2e: return shift ? '+' : '=';
        case 0x2f: return shift ? '{' : '[';
        case 0x30: return shift ? '}' : ']';
        case 0x31: return shift ? '|' : '\\';
        case 0x33: return shift ? ':' : ';';
        case 0x34: return shift ? '"' : '\'';
        case 0x35: return shift ? '~' : '`';
        case 0x36: return shift ? '<' : ',';
        case 0x37: return shift ? '>' : '.';
        case 0x38: return shift ? '?' : '/';
        default: return 0;
    }
}

void usb_kbd_init(void) {
    /* Kept for compatibility: actual bus enumeration happens in usb_init(). */
    g.bound = 0;
    g.ready = 0;
    g.addr = 0;
    g.low_speed = 0;
    g.intr_in.ep_num = 0;
    g.intr_in.ep_type = USB_EPTYP_INTR;
    g.intr_in.ep_in = 1;
    g.intr_in.mps = 8;
    g.intr_in_pid = USB_PID_DATA0;
    for (int i = 0; i < 8; i++) g.last_report[i] = 0;
}

int usb_kbd_try_bind(const usb_device_t *dev) {
    if (!dev) return -1;
    if (g.bound) return -1;

    usb_ep_t intr_in;
    if (usb_host_find_hid_kbd_intr_in(dev, &intr_in) != 0) {
        return -1;
    }

    g.bound = 1;
    g.ready = 1;
    g.addr = dev->addr;
    g.low_speed = (int)dev->low_speed;
    g.intr_in = intr_in;
    g.intr_in_pid = USB_PID_DATA0;
    for (int i = 0; i < 8; i++) g.last_report[i] = 0;

#if USB_KBD_DEBUG
    uart_write("usb-kbd: bound dev addr=");
    uart_write_hex_u64(g.addr);
    uart_write(" ep=");
    uart_write_hex_u64(g.intr_in.ep_num);
    uart_write(" mps=");
    uart_write_hex_u64(g.intr_in.mps);
    uart_write("\n");
#endif

    return 0;
}

int usb_kbd_is_ready(void) {
    return g.ready;
}

void usb_kbd_poll(void) {
    if (!g.bound || !g.ready) return;

    uint8_t report[8];
    uint32_t got = 0;

    int xrc = usb_host_in_xfer(g.addr, g.low_speed, g.intr_in, g.intr_in_pid,
                               report, sizeof(report), &got, /*nak_ok=*/1);
    if (xrc != 0 && xrc != USB_XFER_NODATA) {
        return;
    }

    if (xrc == USB_XFER_NODATA) {
        return;
    }

    /* got==0 with xrc==0 is a valid ZLP completion; advance PID and return. */
    if (got == 0) {
        g.intr_in_pid = (g.intr_in_pid == USB_PID_DATA0) ? USB_PID_DATA1 : USB_PID_DATA0;
        return;
    }

    if (got < 8) return;

    g.intr_in_pid = (g.intr_in_pid == USB_PID_DATA0) ? USB_PID_DATA1 : USB_PID_DATA0;

    uint8_t mods = report[0];
    int shift = (mods & 0x22u) != 0;

    for (int i = 2; i < 8; i++) {
        uint8_t kc = report[i];
        if (kc == 0) continue;

        int already = 0;
        for (int j = 2; j < 8; j++) {
            if (g.last_report[j] == kc) { already = 1; break; }
        }
        if (already) continue;

        char c = hid_keycode_to_ascii(kc, shift);
        if (c) {
            console_in_inject_char(c);
        }
    }

    for (int i = 0; i < 8; i++) g.last_report[i] = report[i];
}
