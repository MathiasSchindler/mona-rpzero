#pragma once

#include "stdint.h"

/*
 * Minimal MMU bring-up for AArch64 EL1.
 *
 * Current design: identity-map low memory in TTBR0.
 * - Normal memory for most of RAM
 * - Device memory for peripheral window (0x3F000000..0x40000000)
 */

void mmu_init_identity(uint64_t ram_base, uint64_t ram_size);
int mmu_is_enabled(void);

/* Per-process TTBR0 support (early, minimal).
 *
 * The kernel keeps a fixed user VA region at USER_REGION_BASE.
 * Each process gets its own TTBR0 tables which map USER_REGION_BASE
 * to a chosen physical 2MiB backing region.
 */
uint64_t mmu_ttbr0_read(void);
void mmu_ttbr0_write(uint64_t ttbr0_pa);
uint64_t mmu_ttbr0_create_with_user_pa(uint64_t user_pa_base);

/*
 * Mark a physical range as device memory in the shared identity mapping.
 *
 * The current MMU setup uses 2MiB block mappings; this helper rounds the range
 * to 2MiB and updates the corresponding block descriptors, then flushes TLBs.
 */
int mmu_mark_region_device(uint64_t phys_start, uint64_t size_bytes);

/* For 39-bit VA, the upper canonical half begins at 0xFFFFFFC000000000. */
#define KERNEL_VA_BASE 0xFFFFFFC000000000ull

/* Coarse EL0 sandbox region (identity-mapped) used for Phase 3 bring-up. */
#define USER_REGION_BASE 0x0000000000400000ull
#define USER_REGION_SIZE 0x0000000000200000ull
