#ifndef OC_CAPTURE_H
#define OC_CAPTURE_H

/* oc_capture: the hub-side consumer that persists accepted sensor samples into
 * oc_store's OC_STREAM_TRACE during a bounded capture window, then verifies
 * them by read-back (M3 capture mode, DESIGN.md section 4).
 *
 * STATUS: [REVIEW]. M3-A is SOFTWARE-ONLY: with SIMULATED input a result says
 * nothing about hardware and is never M3 evidence. The same code path is the
 * one a LIVE capture will use; only the origin differs.
 *
 * ---- Where it sits ----
 * The hub (oc_hub) calls oc_capture_on_sample / oc_capture_on_event from its
 * synchronous callbacks; the hub process calls oc_capture_tick at least every
 * OC_CAPTURE_TICK_MS (also when no sample arrives) and oc_capture_finish once
 * the capture ended. Single-threaded, no queue, and no
 * allocation per sample (oc_capture_new allocates once; oc_store allocates its own buffers). Nothing is
 * stored except through oc_store's public API.
 *
 * ---- States ----
 *   ARMED    waiting for BOTH sources to be ACTIVE (SOURCE_UP seen, not DOWN)
 *            and FRESH (a sample received within fresh_ms of "now"). Samples
 *            that arrive meanwhile are counted as pre_window and NOT stored.
 *   RUNNING  the window is open for duration_ms (<= OC_CAPTURE_MAX_MS). Every
 *            accepted sample is appended, including non-OK statuses.
 *   ENDED    the window closed (or arming timed out, or finish was forced).
 *            Later samples are post_window. Ticks still run until finish.
 *   DONE     oc_capture_finish completed; the store is closed.
 * The window starts at the first sample that finds both sources active and
 * fresh (or at a tick that does), so it is exact to that sample.
 *
 * ---- Accounting (per source, inside the window) ----
 *   accepted_window = append_ok + append_fail + accepted_not_stored
 * pre_window and post_window are kept apart and are not part of that identity.
 * oc_store_tick and oc_store_sync failures are counted separately
 * (tick_fail, sync_fail) and are never booked as append failures. After any
 * storage failure the capture STOPS calling the store, keeps receiving, and
 * counts every further accepted sample as accepted_not_stored. Nothing is
 * retried or resubmitted and uncertain records are never reported durable.
 *
 * ---- A capture PASSES only if ALL hold ----
 *   no failed or skipped storage attempt (append_fail, accepted_not_stored,
 *   tick_fail, sync_fail all 0; store state OK);
 *   both sources were ACTIVE and fresh at start, stayed up, contributed at
 *   least one stored sample, were never silent for longer than fresh_ms
 *   inside the window (head, between samples and tail), kept one
 *   session/instance, and never reported a backwards timestamp;
 *   after the final sync the store reports durable == stored with nothing
 *   uncertain and durable_seq >= the last stored record;
 *   read-back (on the same handle AND after close + reopen) finds exactly
 *   append_ok samples per source, contiguous store seqs, only this capture's
 *   records, identical provenance, wrap-safe increasing sample seqs whose
 *   skipped numbers equal the hub's reported gaps, non-decreasing timestamps,
 *   and an unchanged content digest.
 * Anything else is FAIL with flags that say why. A reader disconnecting
 * mid-capture makes the two-source capture FAIL (SOURCE_LOST) even though the
 * remaining source keeps being recorded.
 *
 * ---- Two sequence numbers ----
 * `store_seq` (u64) is oc_store's record number in the trace stream.
 * `sample seq` (u32, wraps) is the reader's. They are never mixed.
 *
 * ---- Digest ----
 * A 64-bit FNV-1a over each stored record's provenance and exact payload bytes
 * is an integrity cross-check between append time and read-back time. It is
 * not a proof of equality (hash collisions exist); the unit tests compare the
 * decoded fields and bytes directly.
 *
 * ---- Latency ----
 * Each hub callback, append, tick and the finish step are timed with
 * CLOCK_MONOTONIC (max and counts over 1/10/100 ms). A rollover is part of the
 * append that triggers it and cannot be isolated from outside oc_store; the
 * append maximum includes it. While the consumer is blocked in the filesystem
 * the readers keep sending without blocking and may drop samples; that is
 * visible as hub gaps and reader bp_drop, never hidden, and nothing here
 * promises that every transmitted sample is received during such a stall.
 *
 * ---- Fallback log ----
 * The supervisor's fallback log does not exist yet. Failures are reported
 * through this module's result and the caller's structured logging only.
 *
 * Errors: 0 or a negative errno unless stated. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "oc_capture_fmt.h"
#include "oc_hub.h"
#include "oc_store.h"

#define OC_CAPTURE_MAX_MS 600000u          /* hard cap: 10 minutes */
#define OC_CAPTURE_DEFAULT_MS 60000u       /* M3 target: 60 s */
#define OC_CAPTURE_DEFAULT_ARM_TIMEOUT_MS 30000u
#define OC_CAPTURE_DEFAULT_FRESH_MS 1000u  /* a configurable policy value, not a measured rate */
#define OC_CAPTURE_TICK_MS 50u             /* recommended tick period for the host loop */

enum oc_capture_state {
    OC_CAP_ARMED = 1,
    OC_CAP_RUNNING,
    OC_CAP_ENDED,
    OC_CAP_DONE
};

enum oc_capture_verdict {
    OC_CAP_PASS = 1,
    OC_CAP_FAIL = 2
};

/* Why a capture failed (bit set). OC_CAPF_NONE == PASS. */
enum oc_capture_flag {
    OC_CAPF_NONE = 0,
    OC_CAPF_OPEN_FAILED = 1u << 0,     /* oc_store_open failed: nothing could be stored */
    OC_CAPF_ARM_TIMEOUT = 1u << 1,     /* both sources never became active and fresh */
    OC_CAPF_INTERRUPTED = 1u << 2,     /* finish before the window ended */
    OC_CAPF_SOURCE_LOST = 1u << 3,     /* a source went down inside the window */
    OC_CAPF_SOURCE_MISSING = 1u << 4,  /* a source stored no sample */
    OC_CAPF_INSTANCE_CHANGED = 1u << 5,
    OC_CAPF_TS_BACKWARDS = 1u << 6,
    OC_CAPF_ORIGIN_MISMATCH = 1u << 7, /* a sample's origin was not the expected one */
    OC_CAPF_APPEND_FAILED = 1u << 8,
    OC_CAPF_TICK_FAILED = 1u << 9,
    OC_CAPF_SYNC_FAILED = 1u << 10,
    OC_CAPF_STORE_FAILED = 1u << 11,   /* store state FAILED (uncertain records exist) */
    OC_CAPF_NOT_STORED = 1u << 12,     /* accepted samples were not stored */
    OC_CAPF_NOT_DURABLE = 1u << 13,    /* final status: durable != stored, or uncertain != 0 */
    OC_CAPF_READBACK = 1u << 14,       /* read-back counts / content / provenance mismatch */
    OC_CAPF_GAP_MISMATCH = 1u << 15,   /* stored seq gaps != hub-reported gaps */
    OC_CAPF_RING_WRAP = 1u << 16,      /* this capture's oldest records were rotated out */
    OC_CAPF_CLOSE_FAILED = 1u << 17,
    OC_CAPF_REOPEN_FAILED = 1u << 18,
    OC_CAPF_ACCOUNTING = 1u << 19,     /* the accounting identity did not hold (a bug) */
    OC_CAPF_SOURCE_STALLED = 1u << 20  /* a source was silent for more than fresh_ms inside the window */
};

struct oc_lat {
    uint64_t n, total_ns, max_ns;
    uint64_t over_1ms, over_10ms, over_100ms;
};

struct oc_capture_cfg {
    const char *dir;               /* existing directory for oc_store (NOT the socket directory) */
    uint32_t duration_ms;          /* 0 -> OC_CAPTURE_DEFAULT_MS; > OC_CAPTURE_MAX_MS -> -EINVAL */
    uint32_t arm_timeout_ms;       /* 0 -> default; how long to wait for both sources */
    uint32_t fresh_ms;             /* 0 -> default; a source is fresh if it sent within this */
    uint8_t expected_origin;       /* LIVE, SIMULATED or REPLAYED */
    size_t trace_segment_bytes;    /* 0 -> 2 MiB */
    uint32_t trace_segments;       /* 0 -> 16 (ring length) */
    size_t evidence_segment_bytes; /* 0 -> 4 MiB */
    uint32_t evidence_segments;    /* 0 -> 16 */
};

/* Per-source counters (all inside the window unless named otherwise). */
struct oc_capture_src {
    uint64_t pre_window, post_window;
    uint64_t accepted_window;
    uint64_t append_ok, append_fail, accepted_not_stored;
    uint64_t non_ok;                /* accepted in the window with status != OK */
    uint64_t gaps;                  /* sample seqs the hub reported skipped between window samples */
    uint64_t ts_backwards;
    bool went_down;
    bool instance_changed;
    bool have_first;                /* first window sample seen */
    uint32_t instance;              /* identity of the first window sample */
    uint64_t session_id;
    uint32_t first_seq, last_seq;   /* SAMPLE seq (u32) of the first / last window sample */
    uint64_t last_store_seq;        /* STORE seq (u64) of this source's last stored record */
    uint64_t digest;                /* FNV-1a over what was appended */
    uint64_t last_rx_window_ns;     /* hub receive time of the latest window sample (or the window start) */
    uint64_t max_silence_ns;        /* longest silence of this source inside the window */
};

/* One read-back pass. */
struct oc_cap_rb_src {
    uint64_t samples, gaps, ts_backwards, non_ok;
    uint64_t digest;
    bool provenance_ok;             /* every record: kind/origin/emitter/session/instance as expected */
};
struct oc_cap_rb {
    bool done;
    int rc;                         /* oc_store_read return */
    uint64_t records, decode_errors, foreign_records;
    uint64_t order_errors;          /* duplicate or regressed sample seq (wrap-safe compare) */
    uint64_t first_store_seq_seen, last_store_seq_seen;
    bool store_seq_contiguous;
    struct oc_cap_rb_src src[OC_SOURCE_DISTANCE + 1];
};

struct oc_capture_result {
    enum oc_capture_verdict verdict;
    uint32_t flags;                 /* enum oc_capture_flag bits */
    uint8_t origin;                 /* expected origin of this capture */
    uint64_t window_ns;             /* length of the window actually open (0 if never opened) */
    struct oc_capture_src src[OC_SOURCE_DISTANCE + 1];
    bool have_store_seq;
    uint64_t first_store_seq, last_store_seq, records_appended;
    bool store_failed;
    int store_errno;                /* first storage failure's errno, else 0 */
    uint64_t tick_fail, sync_fail;
    int sync_rc, close_rc, reopen_rc;
    bool status_valid;
    struct oc_store_status status;  /* after the final sync, before close */
    struct oc_store_report rec_open;     /* recovery at oc_capture_open */
    struct oc_store_report rec_reopen;   /* recovery at the verification reopen */
    bool reopen_done;
    struct oc_cap_rb rb[2];         /* [0] same handle after sync, [1] after close + reopen */
    struct oc_lat lat_cb, lat_append, lat_tick, lat_sync;
    uint64_t finish_ns;             /* wall time spent in oc_capture_finish */
};

struct oc_capture;                  /* opaque; allocate with oc_capture_new */

/* Allocates and initializes a capture (state ARMED) and opens the store under
 * cfg->dir (recovery runs; *rec, if non-NULL, receives the report). Returns:
 *   0       ready, store open
 *   -EINVAL bad configuration (NULL dir, duration > OC_CAPTURE_MAX_MS, bad origin)
 *   -ENOMEM
 *   other   oc_store_open failed: *out is still a valid capture in the ENDED
 *           state with OC_CAPF_OPEN_FAILED, which finish reports as FAIL; the
 *           host keeps sensing. (This is the only non-zero return that sets *out.) */
int oc_capture_new(const struct oc_capture_cfg *cfg, struct oc_capture **out,
                   struct oc_store_report *rec);

/* Hub callbacks. `ctx` is the struct oc_capture *. They never block beyond
 * oc_store_append and never call back into the hub. */
void oc_capture_on_sample(void *ctx, const struct oc_hub_sample_info *info);
void oc_capture_on_event(void *ctx, const struct oc_hub_event *ev);

/* Advances arming / the window and drives oc_store_tick with now (CLOCK_MONOTONIC
 * ns, the same clock as info->hub_rx_ns). Returns 0, or the first storage error
 * (already recorded; informational). A no-op once DONE. */
int oc_capture_tick(struct oc_capture *c, uint64_t now_ns);

/* Tells the capture what time it is without touching the store. The hub loop
 * calls this right after poll() returns and BEFORE oc_hub_dispatch, so a window
 * that has run out is closed before the events and samples of that dispatch are
 * applied: a reader that leaves just after the window is not blamed for it.
 * Time never moves backwards here; an older value is ignored. */
void oc_capture_clock(struct oc_capture *c, uint64_t now_ns);

enum oc_capture_state oc_capture_state(const struct oc_capture *c);
uint32_t oc_capture_flags(const struct oc_capture *c);
const struct oc_capture_src *oc_capture_src(const struct oc_capture *c, uint8_t source);
/* Live store status; -ENODEV once the store is closed or never opened. */
int oc_capture_store_status(const struct oc_capture *c, struct oc_store_status *out);
/* The store handle, for inspection and fault-injection tests ONLY. NULL once closed. */
struct oc_store *oc_capture_store(const struct oc_capture *c);

/* Ends the capture if still ARMED/RUNNING (flag INTERRUPTED / ARM_TIMEOUT as
 * appropriate), syncs, checks status, reads back, closes the store, reopens it
 * and reads back again, then closes. Fills *res (may not be NULL) and moves to
 * DONE. Idempotent: a second call returns the same result. Always releases the
 * store; the capture object itself is freed by oc_capture_free. */
int oc_capture_finish(struct oc_capture *c, struct oc_capture_result *res);
const struct oc_capture_result *oc_capture_result(const struct oc_capture *c);
void oc_capture_free(struct oc_capture *c);   /* finishes first if needed; NULL is a no-op */

/* Writes the flag names, comma separated ("-" for none), truncating safely. */
void oc_capture_flags_str(uint32_t flags, char *out, size_t cap);

#endif
