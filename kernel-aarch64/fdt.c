#include "fdt.h"
#include "uart_pl011.h"

#define FDT_MAGIC 0xD00DFEEDu

#define FDT_BEGIN_NODE 0x1u
#define FDT_END_NODE   0x2u
#define FDT_PROP       0x3u
#define FDT_NOP        0x4u
#define FDT_END        0x9u

typedef struct {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
} fdt_header_t;

static inline uint32_t be32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

static inline uint32_t be32v(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) | ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
}

static inline uint64_t be64_from_cells(const uint32_t *cells, int n_cells) {
    if (n_cells <= 0) {
        return 0;
    }
    if (n_cells == 1) {
        return (uint64_t)be32(cells);
    }
    /* n_cells >= 2: take first two 32-bit cells */
    uint64_t hi = (uint64_t)be32(&cells[0]);
    uint64_t lo = (uint64_t)be32(&cells[1]);
    return (hi << 32) | lo;
}

static inline uintptr_t align4(uintptr_t x) {
    return (x + 3u) & ~((uintptr_t)3u);
}

static const char *fdt_str(const char *strings, uint32_t strings_size, uint32_t off) {
    if (off >= strings_size) {
        return 0;
    }
    return strings + off;
}

int fdt_read_info(const void *dtb, fdt_info_t *out) {
    if (!dtb || !out) {
        return -1;
    }

    *out = (fdt_info_t){0};

    const uint8_t *base = (const uint8_t *)dtb;
    const fdt_header_t *hdr = (const fdt_header_t *)base;

    uint32_t magic = be32(&hdr->magic);
    if (magic != FDT_MAGIC) {
        return -2;
    }

    uint32_t totalsize = be32(&hdr->totalsize);
    uint32_t off_struct = be32(&hdr->off_dt_struct);
    uint32_t off_strings = be32(&hdr->off_dt_strings);
    uint32_t size_strings = be32(&hdr->size_dt_strings);
    uint32_t size_struct = be32(&hdr->size_dt_struct);

    if (totalsize < sizeof(fdt_header_t)) {
        return -3;
    }
    if (off_struct >= totalsize || off_strings >= totalsize) {
        return -4;
    }
    if ((off_struct + size_struct) > totalsize) {
        return -5;
    }
    if ((off_strings + size_strings) > totalsize) {
        return -6;
    }

    const uint8_t *struct_base = base + off_struct;
    const char *strings = (const char *)(base + off_strings);

    int depth = 0;
    int in_root = 0;
    int in_memory = 0;
    int addr_cells = 2;
    int size_cells = 2;

    const uint8_t *p = struct_base;
    const uint8_t *struct_end = struct_base + size_struct;

    while (p + 4 <= struct_end) {
        uint32_t token = be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            /* node name is NUL-terminated string */
            while (p < struct_end && *p != 0) {
                p++;
            }
            if (p >= struct_end) {
                return -7;
            }
            p++; /* consume NUL */
            p = (const uint8_t *)align4((uintptr_t)p);

            depth++;
            if (depth == 1) {
                in_root = 1;
                in_memory = 0;
            } else {
                in_root = 0;
                if (name && name[0] != 0) {
                    /* Heuristic: memory node starts with "memory" */
                    in_memory = (name[0] == 'm' && name[1] == 'e' && name[2] == 'm' && name[3] == 'o' && name[4] == 'r' && name[5] == 'y');
                } else {
                    in_memory = 0;
                }
            }
        } else if (token == FDT_END_NODE) {
            if (depth > 0) {
                depth--;
            }
            if (depth < 2) {
                in_memory = 0;
            }
            if (depth == 0) {
                in_root = 0;
            }
        } else if (token == FDT_PROP) {
            if (p + 8 > struct_end) {
                return -8;
            }
            uint32_t len = be32(p);
            uint32_t nameoff = be32(p + 4);
            p += 8;

            const char *pname = fdt_str(strings, size_strings, nameoff);
            if (!pname) {
                return -9;
            }
            const uint8_t *val = p;
            const uint8_t *val_end = p + len;
            if (val_end > struct_end) {
                return -10;
            }

            if (in_root) {
                if (!out->has_model) {
                    /* /model is a NUL-terminated string */
                    if (pname[0] == 'm' && pname[1] == 'o' && pname[2] == 'd' && pname[3] == 'e' && pname[4] == 'l' && pname[5] == 0) {
                        out->model = (const char *)val;
                        out->has_model = 1;
                    }
                }

                if (pname[0] == '#' && pname[1] == 'a' && pname[2] == 'd' && pname[3] == 'd' && pname[4] == 'r' && pname[5] == 'e' && pname[6] == 's' && pname[7] == 's' && pname[8] == '-' && pname[9] == 'c' && pname[10] == 'e' && pname[11] == 'l' && pname[12] == 'l' && pname[13] == 's' && pname[14] == 0) {
                    if (len == 4) {
                        addr_cells = (int)be32(val);
                        if (addr_cells < 1) addr_cells = 1;
                        if (addr_cells > 2) addr_cells = 2;
                    }
                }
                if (pname[0] == '#' && pname[1] == 's' && pname[2] == 'i' && pname[3] == 'z' && pname[4] == 'e' && pname[5] == '-' && pname[6] == 'c' && pname[7] == 'e' && pname[8] == 'l' && pname[9] == 'l' && pname[10] == 's' && pname[11] == 0) {
                    if (len == 4) {
                        size_cells = (int)be32(val);
                        if (size_cells < 1) size_cells = 1;
                        if (size_cells > 2) size_cells = 2;
                    }
                }
            }

            if (in_memory) {
                /* Prefer device_type="memory" if present */
                if (pname[0] == 'd' && pname[1] == 'e' && pname[2] == 'v' && pname[3] == 'i' && pname[4] == 'c' && pname[5] == 'e' && pname[6] == '_' && pname[7] == 't' && pname[8] == 'y' && pname[9] == 'p' && pname[10] == 'e' && pname[11] == 0) {
                    const char *s = (const char *)val;
                    if (len >= 6 && s[0] == 'm' && s[1] == 'e' && s[2] == 'm' && s[3] == 'o' && s[4] == 'r' && s[5] == 'y') {
                        /* keep in_memory = 1 */
                    }
                }

                if (!out->has_mem) {
                    if (pname[0] == 'r' && pname[1] == 'e' && pname[2] == 'g' && pname[3] == 0) {
                        int entry_cells = addr_cells + size_cells;
                        if (entry_cells < 2) {
                            entry_cells = 2;
                        }
                        uint32_t entry_bytes = (uint32_t)entry_cells * 4u;
                        if (len >= entry_bytes) {
                            const uint32_t *cells = (const uint32_t *)val;
                            out->mem_base = be64_from_cells(cells, addr_cells);
                            out->mem_size = be64_from_cells(cells + addr_cells, size_cells);
                            out->has_mem = 1;
                        }
                    }
                }
            }

            p = (const uint8_t *)align4((uintptr_t)val_end);
        } else if (token == FDT_NOP) {
            /* ignore */
        } else if (token == FDT_END) {
            break;
        } else {
            /* Unknown token */
            return -11;
        }
    }

    return 0;
}

void fdt_print_info(const void *dtb) {
    fdt_info_t info;
    int rc = fdt_read_info(dtb, &info);
    if (rc != 0) {
        uart_write("fdt: invalid (rc=");
        uart_write_hex_u64((uint64_t)(unsigned long long)rc);
        uart_write(")\n");
        return;
    }

    if (info.has_model) {
        uart_write("fdt model: ");
        uart_write(info.model);
        uart_write("\n");
    } else {
        uart_write("fdt model: (unknown)\n");
    }

    if (info.has_mem) {
        uart_write("fdt mem: base=");
        uart_write_hex_u64(info.mem_base);
        uart_write(" size=");
        uart_write_hex_u64(info.mem_size);
        uart_write("\n");
    } else {
        uart_write("fdt mem: (unknown)\n");
    }
}
