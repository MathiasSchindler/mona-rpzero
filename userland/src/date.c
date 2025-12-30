#include "syscall.h"

static void putc1(char c) {
    (void)sys_write(1, &c, 1);
}

static void put_u64_dec(uint64_t v) {
    char tmp[32];
    uint64_t t = 0;

    if (v == 0) {
        putc1('0');
        return;
    }
    while (v != 0 && t < sizeof(tmp)) {
        tmp[t++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (t > 0) {
        putc1(tmp[--t]);
    }
}

static void put_u64_dec_pad2(uint64_t v) {
    putc1((char)('0' + (char)((v / 10u) % 10u)));
    putc1((char)('0' + (char)(v % 10u)));
}

static void put_i64_dec(int64_t v) {
    if (v < 0) {
        putc1('-');
        /* avoid overflow for INT64_MIN by printing via uint64 */
        uint64_t u = (uint64_t)(-(v + 1)) + 1u;
        put_u64_dec(u);
        return;
    }
    put_u64_dec((uint64_t)v);
}

/* Convert days since 1970-01-01 to civil date (UTC).
 * Algorithm: Howard Hinnant's civil_from_days.
 */
static void civil_from_days(int64_t z, int64_t *out_y, uint64_t *out_m, uint64_t *out_d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);                           /* [0, 146096] */
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  /* [0, 399] */
    int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                /* [0, 365] */
    uint64_t mp = (5 * doy + 2) / 153;                                     /* [0, 11] */
    uint64_t d = doy - (153 * mp + 2) / 5 + 1;                             /* [1, 31] */
    uint64_t m = mp + (mp < 10 ? 3 : (uint64_t)-9);                        /* [1, 12] */
    y += (m <= 2);

    *out_y = y;
    *out_m = m;
    *out_d = d;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;

    if (argc >= 2 && argv[1] && argv[1][0] == '-' && argv[1][1] == 'h') {
        sys_puts("usage: date\n");
        sys_puts("note: CLOCK_REALTIME is boot-relative (no RTC yet)\n");
        return 0;
    }

    linux_timespec_t ts;
    uint64_t rc = sys_clock_gettime(0, &ts); /* CLOCK_REALTIME */
    if ((int64_t)rc < 0) {
        sys_puts("date: clock_gettime failed\n");
        return 1;
    }

    int64_t sec = ts.tv_sec;
    if (sec < 0) sec = 0;

    int64_t days = sec / 86400;
    int64_t rem = sec - days * 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }

    uint64_t hh = (uint64_t)(rem / 3600);
    uint64_t mm = (uint64_t)((rem % 3600) / 60);
    uint64_t ss = (uint64_t)(rem % 60);

    int64_t y;
    uint64_t m, d;
    civil_from_days(days, &y, &m, &d);

    /* YYYY-MM-DD HH:MM:SS */
    put_i64_dec(y);
    putc1('-');
    put_u64_dec_pad2(m);
    putc1('-');
    put_u64_dec_pad2(d);
    putc1(' ');
    put_u64_dec_pad2(hh);
    putc1(':');
    put_u64_dec_pad2(mm);
    putc1(':');
    put_u64_dec_pad2(ss);
    putc1('\n');

    return 0;
}
