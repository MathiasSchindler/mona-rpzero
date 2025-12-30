#pragma once

#include "stdint.h"

/*
 * Raspberry Pi mailbox interface (minimal).
 *
 * Phase 0/1 framebuffer work uses the "property" mailbox channel to talk to the
 * firmware (request framebuffer allocation, pitch, etc).
 *
 * This implementation is intentionally tiny and QEMU-first.
 */

#ifndef RPI_PERIPH_BASE
#define RPI_PERIPH_BASE 0x3F000000ull
#endif

/* Mailbox registers (BCM283x/BCM2710-style). */
#ifndef RPI_MBOX_BASE
#define RPI_MBOX_BASE (RPI_PERIPH_BASE + 0x0000B880ull)
#endif

/* Mailbox channels */
#define MBOX_CH_PROP 8u

/* Returns 0 on success, negative on error. */
int mailbox_property_call(uint32_t *msg, uint32_t msg_bytes);
