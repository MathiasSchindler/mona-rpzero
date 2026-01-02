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
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
} elf64_shdr_t;

static int streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static uint64_t cstr_len_u64_local(const char *s) {
    uint64_t n = 0;
    while (s && s[n] != '\0') n++;
    return n;
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

static void write_u64_hex(uint64_t v, int width) {
    const char *hex = "0123456789abcdef";
    char buf[32];
    int n = width;
    if (n < 1) n = 1;
    if (n > (int)sizeof(buf)) n = (int)sizeof(buf);

    for (int i = 0; i < n; i++) {
        int shift = (n - 1 - i) * 4;
        buf[i] = hex[(v >> shift) & 0xf];
    }
    (void)sys_write(1, buf, (uint64_t)n);
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

static int seek_set(uint64_t fd, uint64_t off) {
    int64_t rc = (int64_t)sys_lseek(fd, (int64_t)off, 0 /* SEEK_SET */);
    return (rc < 0) ? -1 : 0;
}

static const char *sec_name(const char *shstr, uint64_t shstr_len, uint32_t off) {
    if (!shstr) return "";
    if ((uint64_t)off >= shstr_len) return "";
    return &shstr[off];
}

static void usage(void) {
    sys_puts("usage: objdump [-h] FILE\n");
    sys_puts("  Minimal ELF64 section header dumper (similar to 'objdump -h').\n");
}

static void print_sections(const elf64_ehdr_t *eh, const elf64_shdr_t *shdrs, const char *shstr, uint64_t shstr_len) {
    sys_puts("Sections:\n");
    sys_puts("Idx Name                 Size      VMA               File off\n");

    for (uint64_t i = 0; i < (uint64_t)eh->e_shnum; i++) {
        const elf64_shdr_t *sh = &shdrs[i];
        const char *name = sec_name(shstr, shstr_len, sh->sh_name);

        sys_puts(" ");
        write_u64_dec(i);
        sys_puts("  ");

        /* Name padded/truncated to 20. */
        char namebuf[21];
        uint64_t n = 0;
        while (name && name[n] && n < 20) {
            namebuf[n] = name[n];
            n++;
        }
        while (n < 20) namebuf[n++] = ' ';
        namebuf[20] = '\0';
        (void)sys_write(1, namebuf, 20);

        sys_puts(" ");
        write_u64_hex(sh->sh_size, 8);
        sys_puts(" ");
        write_u64_hex(sh->sh_addr, 16);
        sys_puts(" ");
        write_u64_hex(sh->sh_offset, 8);
        sys_puts("\n");
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    int want_sections = 0;
    const char *path = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!a) break;

        if (streq(a, "-h") || streq(a, "--section-headers")) {
            want_sections = 1;
            continue;
        }
        if (streq(a, "--help")) {
            usage();
            return 0;
        }
        if (a[0] == '-') {
            usage();
            return 2;
        }
        if (!path) {
            path = a;
            continue;
        }
        usage();
        return 2;
    }

    if (!path) {
        usage();
        return 2;
    }

    /* Default to -h behavior (most common for bringup/debug). */
    if (!want_sections) want_sections = 1;

    int64_t fd = (int64_t)sys_openat((uint64_t)AT_FDCWD, path, 0 /* O_RDONLY */, 0);
    if (fd < 0) {
        sys_puts("objdump: open failed\n");
        return 1;
    }

    elf64_ehdr_t eh;
    if (read_exact((uint64_t)fd, &eh, sizeof(eh)) != 0) {
        sys_puts("objdump: short read\n");
        (void)sys_close((uint64_t)fd);
        return 1;
    }

    if (!(eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E' && eh.e_ident[2] == 'L' && eh.e_ident[3] == 'F')) {
        sys_puts("objdump: not an ELF file\n");
        (void)sys_close((uint64_t)fd);
        return 1;
    }

    if (eh.e_shoff == 0 || eh.e_shnum == 0 || eh.e_shentsize != (uint16_t)sizeof(elf64_shdr_t)) {
        sys_puts("objdump: no section headers\n");
        (void)sys_close((uint64_t)fd);
        return 1;
    }

    if (eh.e_shnum > 256) {
        sys_puts("objdump: too many sections\n");
        (void)sys_close((uint64_t)fd);
        return 1;
    }

    elf64_shdr_t shdrs[256];
    if (seek_set((uint64_t)fd, eh.e_shoff) != 0) {
        sys_puts("objdump: lseek shoff failed\n");
        (void)sys_close((uint64_t)fd);
        return 1;
    }

    for (uint64_t i = 0; i < (uint64_t)eh.e_shnum; i++) {
        if (read_exact((uint64_t)fd, &shdrs[i], sizeof(elf64_shdr_t)) != 0) {
            sys_puts("objdump: short read shdr\n");
            (void)sys_close((uint64_t)fd);
            return 1;
        }
    }

    /* Read section header string table (best-effort). */
    char shstr[8192];
    uint64_t shstr_len = 0;
    if (eh.e_shstrndx < eh.e_shnum) {
        const elf64_shdr_t *ss = &shdrs[eh.e_shstrndx];
        if (ss->sh_offset != 0 && ss->sh_size != 0) {
            if (ss->sh_size < sizeof(shstr)) shstr_len = ss->sh_size;
            else shstr_len = sizeof(shstr) - 1;
            if (seek_set((uint64_t)fd, ss->sh_offset) == 0) {
                if (read_exact((uint64_t)fd, shstr, shstr_len) == 0) {
                    shstr[shstr_len] = '\0';
                } else {
                    shstr_len = 0;
                }
            } else {
                shstr_len = 0;
            }
        }
    }

    if (want_sections) {
        print_sections(&eh, shdrs, (shstr_len ? shstr : 0), shstr_len);
    }

    (void)sys_close((uint64_t)fd);
    (void)cstr_len_u64_local;
    return 0;
}
