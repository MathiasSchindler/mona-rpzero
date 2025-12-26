#include "elf64.h"

static int range_ok(uint64_t base, uint64_t size, uint64_t p, uint64_t n) {
    if (n == 0) return 1;
    if (p < base) return 0;
    if (p >= base + size) return 0;
    if (p + n < p) return 0;
    if (p + n > base + size) return 0;
    return 1;
}

int elf64_load_etexec(const uint8_t *img,
                      size_t img_size,
                      uint64_t user_base,
                      uint64_t user_size,
                      uint64_t *entry_out,
                      uint64_t *min_loaded_va_out,
                      uint64_t *max_loaded_va_out) {
    if (!img || img_size < sizeof(elf64_ehdr_t)) return -1;

    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)img;
    if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
        eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) {
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64) return -1;
    if (eh->e_ident[5] != ELFDATA2LSB) return -1;
    if (eh->e_type != ET_EXEC) return -1;
    if (eh->e_machine != EM_AARCH64) return -1;

    if (eh->e_phentsize != sizeof(elf64_phdr_t)) return -1;
    if (eh->e_phnum == 0) return -1;

    uint64_t ph_end = eh->e_phoff + (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
    if (ph_end < eh->e_phoff) return -1;
    if (ph_end > (uint64_t)img_size) return -1;

    uint64_t min_va = ~0ull;
    uint64_t max_va = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(img + eh->e_phoff + (uint64_t)i * sizeof(elf64_phdr_t));
        if (ph->p_type != PT_LOAD) continue;

        /* Ignore empty load segments. */
        if (ph->p_memsz == 0) continue;

        if (ph->p_memsz < ph->p_filesz) return -1;
        if (ph->p_offset + ph->p_filesz < ph->p_offset) return -1;
        if (ph->p_offset + ph->p_filesz > (uint64_t)img_size) return -1;

        if (!range_ok(user_base, user_size, ph->p_vaddr, ph->p_memsz)) return -1;

        volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)ph->p_vaddr;
        const uint8_t *src = img + ph->p_offset;

        for (uint64_t j = 0; j < ph->p_filesz; j++) {
            dst[j] = src[j];
        }
        for (uint64_t j = ph->p_filesz; j < ph->p_memsz; j++) {
            dst[j] = 0;
        }

        if (ph->p_vaddr < min_va) min_va = ph->p_vaddr;
        if (ph->p_vaddr + ph->p_memsz > max_va) max_va = ph->p_vaddr + ph->p_memsz;
    }

    if (min_va == ~0ull) return -1;

    if (entry_out) *entry_out = eh->e_entry;
    if (min_loaded_va_out) *min_loaded_va_out = min_va;
    if (max_loaded_va_out) *max_loaded_va_out = max_va;
    return 0;
}
