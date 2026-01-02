#pragma once

#include "stdint.h"

/* Cache maintenance helpers for AArch64 EL1 bring-up. */

void cache_invalidate_all(void);

/* Clean+invalidate D-cache and invalidate I-cache (global).
 * Useful when switching between distinct EL0 address spaces without ASIDs.
 */
void cache_clean_invalidate_all(void);

/* Clean D-cache to PoU and invalidate I-cache for a VA range. */
void cache_sync_icache_for_range(uint64_t start, uint64_t size);

/* DMA helpers (D-cache maintenance for a VA range).
 * These are needed when device DMA reads/writes memory while caches are enabled.
 */
void cache_clean_dcache_for_range(uint64_t start, uint64_t size);
void cache_invalidate_dcache_for_range(uint64_t start, uint64_t size);
