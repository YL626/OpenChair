#ifndef OC_CLOCK_H
#define OC_CLOCK_H

/* oc_clock: injectable monotonic clock, so readers can be tested without
 * real time passing.
 *
 * Both calls return 0 or a negative errno. now_ns writes the time only on
 * success. sleep_until_ns returns once the clock has reached abs_ns (or
 * immediately if it already has); the system implementation resumes after
 * EINTR against the same absolute time, so a signal never extends the sleep.
 *
 * Ownership: the struct holds no resources; ctx belongs to the caller. */

#include <stdint.h>

struct oc_clock {
    int (*now_ns)(void *ctx, uint64_t *out);
    int (*sleep_until_ns)(void *ctx, uint64_t abs_ns);
    void *ctx;
};

/* Fills *c with the CLOCK_MONOTONIC implementation. Cannot fail. */
void oc_clock_system(struct oc_clock *c);

#endif
