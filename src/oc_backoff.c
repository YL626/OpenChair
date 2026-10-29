#include "oc_backoff.h"

void oc_backoff_init(struct oc_backoff *b, uint32_t initial_ms, uint32_t cap_ms)
{
    if (initial_ms < 1)
        initial_ms = 1;
    if (cap_ms < initial_ms)
        cap_ms = initial_ms;
    b->initial_ms = initial_ms;
    b->cap_ms = cap_ms;
    b->cur_ms = initial_ms;
}

uint32_t oc_backoff_next(struct oc_backoff *b)
{
    uint32_t d = b->cur_ms;
    /* Doubling is done in 64 bits so it cannot wrap; then clamped to the cap. */
    uint64_t dbl = (uint64_t)d * 2u;
    b->cur_ms = dbl > b->cap_ms ? b->cap_ms : (uint32_t)dbl;
    return d;
}

void oc_backoff_reset(struct oc_backoff *b)
{
    b->cur_ms = b->initial_ms;
}
