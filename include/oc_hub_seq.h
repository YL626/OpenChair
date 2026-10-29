#ifndef OC_HUB_SEQ_H
#define OC_HUB_SEQ_H

/* oc_hub_seq: per-source, per-instance sequence and timestamp tracker.
 *
 * STATUS: [REVIEW] follows docs/IMPLEMENTATION_NOTES.md section 3 ("Hub rules").
 * Pure: no I/O, no allocation, no clock. One tracker per (source, instance);
 * the hub resets it when a supervisor-authorized NEW instance is accepted.
 *
 * Sequence semantics. A reader numbers every sample ATTEMPT 0, 1, 2, ... per
 * instance (u32, wrapping). A failed acquisition still consumes its number and
 * arrives as a sample with a non-OK status; a sample the reader could not
 * transmit leaves a hole. The tracker therefore reports:
 *   FIRST        first sample seen for this instance. If its seq is nonzero the
 *                numbers before it are `unobserved_prefix`: the hub did not
 *                see them, which is NOT confirmed loss (a restarted hub meets
 *                a reader that is already at seq 5000).
 *   IN_ORDER     seq == last + 1
 *   GAP          0 < (seq - next) < 2^31 : accepted; the skipped numbers are
 *                counted in `lost` (reader-side drops and transport loss are
 *                indistinguishable here)
 *   DUPLICATE    seq == last : rejected
 *   REGRESSION   anything else behind `last` : rejected (stale / out of order)
 * Rejected samples change NOTHING except their own counter: `last`, the
 * logical position and the timestamp state do not move.
 *
 * Wrap safety. All comparisons use modular u32 distance. In addition the
 * tracker keeps `logical_last`, a u64 position counted from the instance's
 * seq 0 and extended across u32 wraps, so that for every state
 *     accepted + lost + unobserved_prefix == logical_last + 1
 * (when `have`), with no raw u32 subtraction involved.
 *
 * Timestamps. For ACCEPTED samples only, t_mono_ns is compared with the
 * previous accepted sample's. A smaller value is a backward step: counted in
 * `ts_backwards` and flagged in the result. The sample is not modified or
 * rejected, and the state follows the new value (one step back is flagged
 * once). Equal timestamps are not backward. A consumer that needs a clean
 * capture must reject any capture during which ts_backwards (or `lost`, or the
 * instance generation) changed. */

#include <stdbool.h>
#include <stdint.h>

enum oc_seq_verdict {
    OC_SEQ_FIRST = 1,
    OC_SEQ_IN_ORDER,
    OC_SEQ_GAP,
    OC_SEQ_DUPLICATE,
    OC_SEQ_REGRESSION
};

struct oc_seq {
    bool have;                  /* a first sample has been accepted */
    uint32_t last;              /* raw seq of the last accepted sample */
    uint64_t logical_last;      /* wrap-extended position of `last`, from seq 0 */
    uint64_t accepted;
    uint64_t lost;              /* numbers skipped by GAPs */
    uint64_t gap_events;
    uint64_t duplicates;
    uint64_t regressions;
    uint64_t unobserved_prefix; /* numbers before the first observed sample */
    bool have_t;
    uint64_t last_t_ns;         /* t_mono_ns of the last accepted sample */
    uint64_t ts_backwards;
};

struct oc_seq_result {
    enum oc_seq_verdict verdict;
    uint32_t gap;               /* GAP: numbers skipped. FIRST: the unobserved prefix. Else 0 */
    bool ts_backwards;          /* only for accepted samples */
};

/* Clears all history (new instance). */
void oc_seq_reset(struct oc_seq *s);

/* Classifies one sample and updates the tracker according to the rules above.
 * A NULL tracker returns verdict 0 and changes nothing. */
struct oc_seq_result oc_seq_observe(struct oc_seq *s, uint32_t seq, uint64_t t_mono_ns);

/* True if the invariant above holds (always true when !have). For tests and
 * assertions. */
bool oc_seq_consistent(const struct oc_seq *s);

#endif
