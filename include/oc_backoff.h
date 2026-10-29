#ifndef OC_BACKOFF_H
#define OC_BACKOFF_H

/* oc_backoff: pure reconnect-delay calculator (Notes section 3, step 4:
 * 100 ms doubling to a 2 s cap). No I/O, no clock, no allocation, no jitter.
 *
 * oc_backoff_next returns the delay to wait before the next attempt and then
 * doubles the stored delay, saturating at the cap. The caller decides when a
 * connection counts as stable enough to oc_backoff_reset (S3C readers: after
 * the connection stayed up for 1 s, so a hub that ACKs and then cuts a reader
 * off cannot make it reconnect at the initial rate forever).
 *
 * Arguments are clamped: initial_ms >= 1, cap_ms >= initial_ms. */
#include <stdint.h>

struct oc_backoff {
    uint32_t initial_ms;
    uint32_t cap_ms;
    uint32_t cur_ms;
};

void oc_backoff_init(struct oc_backoff *b, uint32_t initial_ms, uint32_t cap_ms);
uint32_t oc_backoff_next(struct oc_backoff *b);
void oc_backoff_reset(struct oc_backoff *b);

#endif
