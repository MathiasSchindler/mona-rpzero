#pragma once

#include <stdint.h>

/* Cache maintenance helpers for AArch64 EL1 bring-up. */

void cache_invalidate_all(void);

/* Clean D-cache to PoU and invalidate I-cache for a VA range. */
void cache_sync_icache_for_range(uint64_t start, uint64_t size);
