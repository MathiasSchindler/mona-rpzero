#include "syscall.h"

#define AT_FDCWD ((int64_t)-100)

enum {
    EI_NIDENT = 16,
};

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
}

static void write_u64_hex(uint64_t v) {
    char buf[16];
    for (int i = 0; i < 16; i++) {
        uint8_t d = (uint8_t)((v >> ((15 - i) * 4)) & 0xfull);
        buf[i] = (d < 10) ? (char)('0' + d) : (char)('a' + (d - 10));
    }
    sys_puts("0x");
    (void)sys_write(1, buf, 16);
}

static void write_u64_dec(uint64_t v) {
    char tmp[32];
    uint64_t t = 0;
    if (v == 0) {
        tmp[t++] = '0';
    } else {
        char rev[32];
        uint64_t r = 0;
        while (v > 0 && r < sizeof(rev)) {
            rev[r++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (r > 0) tmp[t++] = rev[--r];
    }
    (void)sys_write(1, tmp, t);
}

static void usage(void) {
    sys_puts("usage: readelf FILE\n");
}

static int read_exact(uint64_t fd, void *buf, uint64_t n) {
    uint8_t *p = (uint8_t *)buf;
    uint64_t got = 0;
    while (got < n) {
        int64_t rc = (int64_t)sys_read(fd, p + got, n - got);
        if (rc < 0) return -1;
        if (rc == 0) return -1;
        got += (uint64_t)rc;
    }
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc != 2) {
        usage();
        return 1;
    }

    const char *path = argv[1];

    int64_t fd = (int64_t)sys_openat((uint64_t)AT_FDCWD, path, 0, 0);
    if (fd < 0) {
        sys_puts("readelf: open failed\n");
        return 1;
    }

    elf64_ehdr_t eh;
    if (read_exact((uint64_t)fd, &eh, sizeof(eh)) != 0) {
        sys_puts("readelf: short read\n");
        (void)sys_close((uint64_t)fd);
        return 1;
    }

    if (!(eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E' && eh.e_ident[2] == 'L' && eh.e_ident[3] == 'F')) {
        sys_puts("readelf: not an ELF file\n");
        (void)sys_close((uint64_t)fd);
        return 1;
    }

    /* Print basic header info. */
    sys_puts("ELF Header:\n");
    sys_puts("  Type: ");
    write_u64_dec((uint64_t)eh.e_type);
    sys_puts("\n  Machine: ");
    write_u64_dec((uint64_t)eh.e_machine);
    sys_puts("\n  Entry: ");
    write_u64_hex(eh.e_entry);
    sys_puts("\n  Program header offset: ");
    write_u64_hex(eh.e_phoff);
    sys_puts("\n  Program header count: ");
    write_u64_dec((uint64_t)eh.e_phnum);
    sys_puts("\n");

    /* Program headers. */
    if (eh.e_phoff != 0 && eh.e_phnum != 0 && eh.e_phentsize == (uint16_t)sizeof(elf64_phdr_t)) {
        if ((int64_t)sys_lseek((uint64_t)fd, (int64_t)eh.e_phoff, 0) < 0) {
            sys_puts("readelf: lseek phoff failed\n");
            (void)sys_close((uint64_t)fd);
            return 1;
        }

        sys_puts("Program Headers:\n");
        for (uint64_t i = 0; i < (uint64_t)eh.e_phnum; i++) {
            elf64_phdr_t ph;
            if (read_exact((uint64_t)fd, &ph, sizeof(ph)) != 0) {
                sys_puts("readelf: short read phdr\n");
                (void)sys_close((uint64_t)fd);
                return 1;
            }

            sys_puts("  [");
            write_u64_dec(i);
            sys_puts("] type=");
            write_u64_dec((uint64_t)ph.p_type);
            sys_puts(" off=");
            write_u64_hex(ph.p_offset);
            sys_puts(" vaddr=");
            write_u64_hex(ph.p_vaddr);
            sys_puts(" filesz=");
            write_u64_hex(ph.p_filesz);
            sys_puts(" memsz=");
            write_u64_hex(ph.p_memsz);
            sys_puts("\n");
        }
    }

    (void)sys_close((uint64_t)fd);
    (void)cstr_len_u64_local;
    return 0;
}
