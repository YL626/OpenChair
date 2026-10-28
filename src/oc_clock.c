#define _POSIX_C_SOURCE 200809L

#include "oc_clock.h"

#include <errno.h>
#include <time.h>

static int sys_now_ns(void *ctx, uint64_t *out)
{
    (void)ctx;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -errno;
    *out = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return 0;
}

/* Absolute-time sleep: after EINTR the same target is used again, so a
 * signal can neither shorten the wait below abs_ns nor extend it past it. */
static int sys_sleep_until_ns(void *ctx, uint64_t abs_ns)
{
    (void)ctx;
    const struct timespec ts = {
        .tv_sec = (time_t)(abs_ns / 1000000000ull),
        .tv_nsec = (long)(abs_ns % 1000000000ull),
    };
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
    } while (rc == EINTR);
    return rc == 0 ? 0 : -rc;   /* clock_nanosleep returns the errno, not -1 */
}

void oc_clock_system(struct oc_clock *c)
{
    *c = (struct oc_clock){.now_ns = sys_now_ns,
                           .sleep_until_ns = sys_sleep_until_ns,
                           .ctx = nullptr};
}
