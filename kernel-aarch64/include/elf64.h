#pragma once

#include "stddef.h"
#include "stdint.h"

/* Minimal ELF64 definitions for an AArch64 ET_EXEC loader (initramfs-backed). */

#define EI_NIDENT 16

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
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

#define ELF_MAGIC0 0x7f
#define ELF_MAGIC1 'E'
#define ELF_MAGIC2 'L'
#define ELF_MAGIC3 'F'

#define ELFCLASS64 2
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define EM_AARCH64 183

#define PT_LOAD 1
#define PT_PHDR 6

/* Returns 0 on success, -1 on invalid ELF or out-of-range segments. */
int elf64_load_etexec(const uint8_t *img,
                      size_t img_size,
                      uint64_t user_va_base,
                      uint64_t user_size,
                      uint64_t user_pa_base,
                      uint64_t *entry_out,
                      uint64_t *min_loaded_va_out,
                      uint64_t *max_loaded_va_out);
