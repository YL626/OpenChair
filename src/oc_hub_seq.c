#include "oc_hub_seq.h"

#include <string.h>

#define HALF_RANGE 0x80000000u

void oc_seq_reset(struct oc_seq *s)
{
    if (s != nullptr)
        memset(s, 0, sizeof *s);
}

/* Timestamp bookkeeping for an ACCEPTED sample only. */
static bool note_timestamp(struct oc_seq *s, uint64_t t)
{
    bool backwards = s->have_t && t < s->last_t_ns;
    if (backwards)
        s->ts_backwards++;
    s->have_t = true;
    s->last_t_ns = t;
    return backwards;
}

struct oc_seq_result oc_seq_observe(struct oc_seq *s, uint32_t seq, uint64_t t_mono_ns)
{
    struct oc_seq_result r = {0};
    if (s == nullptr)
        return r;

    if (!s->have) {
        s->have = true;
        s->last = seq;
        s->logical_last = seq;           /* position counted from the instance's seq 0 */
        s->accepted = 1;
        s->unobserved_prefix = seq;      /* not confirmed loss: the hub just was not there */
        r.verdict = OC_SEQ_FIRST;
        r.gap = seq;
        r.ts_backwards = note_timestamp(s, t_mono_ns);
        return r;
    }

    /* Modular distance from the expected next number; never a raw comparison. */
    const uint32_t next = s->last + 1u;
    const uint32_t d = seq - next;

    if (d == 0) {
        r.verdict = OC_SEQ_IN_ORDER;
        s->logical_last += 1;
    } else if (d < HALF_RANGE) {
        r.verdict = OC_SEQ_GAP;
        r.gap = d;
        s->lost += d;
        s->gap_events++;
        s->logical_last += 1u + (uint64_t)d;
    } else if (seq == s->last) {
        r.verdict = OC_SEQ_DUPLICATE;
        s->duplicates++;
        return r;                        /* nothing else moves */
    } else {
        r.verdict = OC_SEQ_REGRESSION;
        s->regressions++;
        return r;
    }

    s->last = seq;
    s->accepted++;
    r.ts_backwards = note_timestamp(s, t_mono_ns);
    return r;
}

bool oc_seq_consistent(const struct oc_seq *s)
{
    if (s == nullptr)
        return false;
    if (!s->have)
        return s->accepted == 0 && s->lost == 0 && s->unobserved_prefix == 0;
    return s->accepted + s->lost + s->unobserved_prefix == s->logical_last + 1 &&
           (uint32_t)s->logical_last == s->last;
}
