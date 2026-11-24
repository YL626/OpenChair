#ifndef OC_POSITION_H
#define OC_POSITION_H

/* oc_position: placement and stability verdict from the VL6180X distance
 * stream (CLAUDE.md: "verify sensor placement and detect movement during
 * capture").
 *
 * [REVIEW: Alan] Written by Kacper as part of the distance reader. DESIGN.md
 * section 7 gives sensor fusion to Alan, so this module only *computes* a
 * verdict from distance samples; it is not called by the hub, and how fusion
 * uses it (which verdicts accept a capture, when to ask the user to
 * reposition) is Alan's decision. Until he has reviewed it, treat the API and
 * the defaults as a proposal. See docs/vl6180x.md section 5.
 *
 * Pure: no I/O, no allocation, no globals, no clock reads (the caller passes
 * "now"). Not thread-safe; one instance per consumer.
 *
 * Window: the last window_n consecutive OK samples of ONE unbroken stream.
 * The window is emptied by
 *   - any non-OK sample (the stream says nothing about placement then);
 *   - a seq discontinuity (gap, duplicate or regression, wrap-safe u32), which
 *     means lost samples or a restarted reader (DESIGN.md section 1: a capture
 *     spanning a gap is rejected);
 *   - a timestamp that goes backwards.
 * Each emptying is counted in discontinuities (non-OK samples are not).
 *
 * Verdict (oc_position_eval), first match wins:
 *   UNAVAILABLE   no sample yet, the newest is older than max_age_ns, or it is
 *                 NACK / TIMEOUT / IO_ERR / any status other than OK/RANGE_ERR
 *   FAULT         the newest is RANGE_ERR with a system error code (1-5)
 *   NO_TARGET     the newest is RANGE_ERR with any other code (nothing usable
 *                 in front of the sensor: too far, too close, weak signal)
 *   SETTLING      fewer than window_n OK samples since the window was emptied
 *   OUT_OF_RANGE  median outside [min_mm, max_mm]
 *   MOVING        max - min over the window exceeds max_spread_mm
 *   STABLE        none of the above
 * Only STABLE says "placement validated". Every other verdict must stop an
 * automatic diagnosis from being called validated (DESIGN.md failure table:
 * "Positioning ... Reject; ask to reposition").
 *
 * Provenance: the verdict's origin is the common origin of the window's
 * samples, or OC_ORIGIN_MIXED if they differ (DESIGN.md section 4), so a
 * SIMULATED or REPLAYED window can never yield a LIVE verdict. For verdicts
 * decided by the newest sample alone, it is that sample's origin; with no
 * sample, OC_ORIGIN_NONE.
 *
 * Defaults are placeholders [MEASURE]: they depend on the shroud geometry and
 * the real sample rate, neither of which is fixed yet. */

#include <stdint.h>

#include "oc_sample.h"

#define OC_POSITION_WINDOW_MAX 32u

enum oc_position_state : uint8_t {
    OC_POS_UNAVAILABLE = 1,
    OC_POS_FAULT = 2,
    OC_POS_NO_TARGET = 3,
    OC_POS_SETTLING = 4,
    OC_POS_OUT_OF_RANGE = 5,
    OC_POS_MOVING = 6,
    OC_POS_STABLE = 7,
};

struct oc_position_cfg {
    int32_t min_mm;          /* accepted placement window, inclusive */
    int32_t max_mm;
    int32_t max_spread_mm;   /* largest max - min still called steady */
    uint32_t window_n;       /* OK samples needed, 1..OC_POSITION_WINDOW_MAX */
    uint64_t max_age_ns;     /* newest sample older than this: UNAVAILABLE */
};

/* [MEASURE] placeholders: 15-120 mm, 5 mm spread, 5 samples (about 0.75 s at
 * the reader's current 150 ms LIVE period), 500 ms freshness (more than
 * three periods). */
#define OC_POSITION_CFG_DEFAULT                                              \
    { .min_mm = 15, .max_mm = 120, .max_spread_mm = 5, .window_n = 5,       \
      .max_age_ns = 500000000ull }

struct oc_position_verdict {
    uint8_t state;        /* enum oc_position_state */
    uint8_t origin;       /* enum oc_origin (see Provenance) */
    uint8_t last_status;  /* newest sample's status, 0 if none */
    uint8_t range_error;  /* its VL6180X error code if RANGE_ERR, else 0 */
    uint32_t n;           /* OK samples in the window */
    int32_t median_mm;    /* window median (mean of the two middle values,
                             rounded down, for even n); 0 if n == 0 */
    int32_t spread_mm;    /* max - min over the window; 0 if n == 0 */
    uint64_t age_ns;      /* now - newest sample's timestamp; 0 if none or
                             if now is earlier than that timestamp */
};

struct oc_position {
    struct oc_position_cfg cfg;
    int32_t mm[OC_POSITION_WINDOW_MAX];      /* ring of window values */
    uint8_t origin[OC_POSITION_WINDOW_MAX];
    uint32_t head;      /* next slot to write */
    uint32_t n;         /* values in the ring, <= cfg.window_n */
    bool have_last;
    uint32_t last_seq;
    uint64_t last_t_ns;
    uint8_t last_status;
    uint8_t last_origin;
    uint32_t last_aux;
    uint32_t discontinuities;   /* window emptied by a seq or time break */
};

/* -EINVAL for NULL, min_mm > max_mm, a negative max_spread_mm, window_n
 * outside 1..OC_POSITION_WINDOW_MAX, or max_age_ns == 0. */
int oc_position_init(struct oc_position *p, const struct oc_position_cfg *cfg);

/* Forgets every sample (e.g. on a new reader instance); keeps cfg and the
 * discontinuity count. */
void oc_position_reset(struct oc_position *p);

/* Adds one sample. -EINVAL (and no change) for NULL, a sample that fails
 * oc_sample_validate, or a source other than DISTANCE. */
int oc_position_push(struct oc_position *p, const struct oc_sample *s);

/* Computes the verdict as of now_ns (CLOCK_MONOTONIC, same clock as the
 * samples). -EINVAL for NULL. Does not change *p. */
int oc_position_eval(const struct oc_position *p, uint64_t now_ns,
                     struct oc_position_verdict *out);

/* "UNAVAILABLE", "FAULT", ..., "STABLE"; "?" for anything else. */
const char *oc_position_state_name(uint8_t state);

#endif
