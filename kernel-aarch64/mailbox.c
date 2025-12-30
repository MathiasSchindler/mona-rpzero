#include "mailbox.h"

#include "stddef.h"

#define MBOX_READ   0x00u
#define MBOX_STATUS 0x18u
#define MBOX_WRITE  0x20u

#define MBOX_STATUS_FULL  (1u << 31)
#define MBOX_STATUS_EMPTY (1u << 30)

static inline volatile uint32_t *mbox_reg(uint32_t off) {
    return (volatile uint32_t *)((uintptr_t)RPI_MBOX_BASE + off);
}

static inline uint32_t mbox_pack(uint32_t channel, uint32_t data_aligned) {
    return (data_aligned & ~0xFu) | (channel & 0xFu);
}

/*
 * Convert a CPU physical address to a bus address.
 *
 * For many Pi firmware mailbox interactions, the GPU expects a bus address.
 * On BCM2710-class systems this is commonly represented by OR-ing 0x40000000.
 *
 * QEMU setups vary; keeping this isolated makes it easy to adjust later.
 */
static inline uint32_t rpi_bus_addr(uintptr_t phys) {
    return (uint32_t)((uint64_t)phys | 0x40000000ull);
}

static int mailbox_call(uint32_t channel, uintptr_t msg_phys) {
    if ((channel & ~0xFu) != 0) return -1;
    if ((msg_phys & 0xFu) != 0) return -1; /* 16-byte alignment required */

    uint32_t data = rpi_bus_addr(msg_phys);

    /* Wait until mailbox not full */
    while ((*mbox_reg(MBOX_STATUS) & MBOX_STATUS_FULL) != 0) {
        /* spin */
    }

    *mbox_reg(MBOX_WRITE) = mbox_pack(channel, data);

    for (;;) {
        /* Wait for response */
        while ((*mbox_reg(MBOX_STATUS) & MBOX_STATUS_EMPTY) != 0) {
            /* spin */
        }

        uint32_t v = *mbox_reg(MBOX_READ);
        if ((v & 0xFu) != (channel & 0xFu)) {
            continue;
        }
        if ((v & ~0xFu) != (data & ~0xFu)) {
            continue;
        }
        return 0;
    }
}

int mailbox_property_call(uint32_t *msg, uint32_t msg_bytes) {
    if (!msg) return -1;
    if ((msg_bytes & 0xFu) != 0) return -1; /* message size must be 16-byte multiple */

    /* The message buffer must be 16-byte aligned for the mailbox. */
    if ((((uintptr_t)msg) & 0xFu) != 0) return -1;

    /*
     * Message format (property interface):
     *  msg[0] = total size in bytes
     *  msg[1] = request/response code
     *  ... tags ...
     *  end tag = 0
     */
    msg[0] = msg_bytes;
    msg[1] = 0; /* request */

    if (mailbox_call(MBOX_CH_PROP, (uintptr_t)msg) != 0) {
        return -1;
    }

    /* msg[1] bit31 set indicates response; exact semantics handled by callers. */
    return 0;
}
