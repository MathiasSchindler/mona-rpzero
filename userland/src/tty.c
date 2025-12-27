#include "syscall.h"

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} linux_winsize_t;

int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;

    const uint64_t TCGETS = 0x5401u;
    const uint64_t TIOCGWINSZ = 0x5413u;

    uint8_t termios[60];
    for (uint64_t i = 0; i < sizeof(termios); i++) termios[i] = 0xAA;

    uint64_t rc = sys_ioctl(0, TCGETS, termios);
    sys_puts("ioctl(TCGETS) rc=");
    {
        char hx[16];
        const char *d = "0123456789abcdef";
        for (int i = 0; i < 16; i++) hx[15 - i] = d[(rc >> (i * 4)) & 0xf];
        (void)sys_write(1, hx, 16);
    }
    sys_puts("\n");

    linux_winsize_t ws;
    ws.ws_row = 0;
    ws.ws_col = 0;
    ws.ws_xpixel = 0;
    ws.ws_ypixel = 0;

    rc = sys_ioctl(1, TIOCGWINSZ, &ws);
    sys_puts("ioctl(TIOCGWINSZ) rc=");
    {
        char hx[16];
        const char *d = "0123456789abcdef";
        for (int i = 0; i < 16; i++) hx[15 - i] = d[(rc >> (i * 4)) & 0xf];
        (void)sys_write(1, hx, 16);
    }
    sys_puts(" rows=");
    {
        char buf[8];
        uint64_t v = ws.ws_row;
        uint64_t n = 0;
        if (v == 0) buf[n++] = '0';
        else {
            char tmp[8];
            uint64_t m = 0;
            while (v > 0 && m < sizeof(tmp)) { tmp[m++] = (char)('0' + (v % 10)); v /= 10; }
            while (m > 0) buf[n++] = tmp[--m];
        }
        (void)sys_write(1, buf, n);
    }
    sys_puts(" cols=");
    {
        char buf[8];
        uint64_t v = ws.ws_col;
        uint64_t n = 0;
        if (v == 0) buf[n++] = '0';
        else {
            char tmp[8];
            uint64_t m = 0;
            while (v > 0 && m < sizeof(tmp)) { tmp[m++] = (char)('0' + (v % 10)); v /= 10; }
            while (m > 0) buf[n++] = tmp[--m];
        }
        (void)sys_write(1, buf, n);
    }
    sys_puts("\n");

    sys_puts("tty: OK\n");
    return 0;
}
