#include "syscalls.h"

#include "errno.h"
#include "klog.h"
#include "sys_util.h"

/* flags */
enum { DMESG_F_CLEAR = 1u };

uint64_t sys_mona_dmesg(uint64_t buf_user, uint64_t len, uint64_t flags) {
    uint64_t cur_len = klog_len();

    /* Query length. */
    if (buf_user == 0) {
        if (flags & DMESG_F_CLEAR) {
            klog_clear();
        }
        return cur_len;
    }

    if (len == 0) {
        if (flags & DMESG_F_CLEAR) {
            klog_clear();
        }
        return 0;
    }

    if (!user_range_ok(buf_user, len)) {
        return (uint64_t)(-(int64_t)EFAULT);
    }

    uint64_t n = (cur_len < len) ? cur_len : len;
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)buf_user;
    for (uint64_t i = 0; i < n; i++) {
        dst[i] = (uint8_t)klog_at(i);
    }

    if (flags & DMESG_F_CLEAR) {
        klog_clear();
    }

    return n;
}
