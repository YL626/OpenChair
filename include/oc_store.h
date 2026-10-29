#ifndef OC_STORE_H
#define OC_STORE_H

/* oc_store: crash-consistent append-only storage (Mechanism D).
 *
 * STATUS: [REVIEW] interface following DESIGN.md §4 and
 * docs/IMPLEMENTATION_NOTES.md §5-7, implemented in src/oc_store.c.
 * File format, sync policy, recovery rules and the decisions awaiting
 * partner review: docs/oc_store.md. Tests: docs/oc_store_tests.md.
 *
 * Concurrency: single writer, not thread-safe. Only the hub uses it. A read
 * visitor must not call back into the store.
 *
 * Errors: every int-returning call returns 0 on success or a negative errno.
 * An invalid argument (-EINVAL) never changes store state. Any I/O failure
 * (-EIO, -ENOSPC, -EFBIG, ...) moves the store to OC_STORE_FAILED: records
 * not yet durable become uncertain, nothing is acknowledged as durable
 * again, and every later append/tick/sync returns -EBADFD. status, read and
 * close still work. Other codes:
 *   -EDQUOT   append: the evidence stream is at its max_segments cap. The
 *             record is refused; the store stays OK and trace is unaffected.
 *   -ENODATA  sync: the stream holds no records.
 *   -EBADMSG  read: a segment changed on disk after open; nothing invented.
 * Output parameters (*seq_out, *durable_seq) are written only on success.
 *
 * Record data states (DESIGN.md §4):
 *   buffered  - in this process's memory; lost if the process dies
 *   written   - write() returned; lost on power loss
 *   durable   - a sync succeeded after the record was written
 *   uncertain - a sync failed; validity is decided by the next recovery
 */

#include <stddef.h>
#include <stdint.h>

/* Largest payload accepted by oc_store_append, in bytes. */
#define OC_STORE_MAX_PAYLOAD 65536u

enum oc_stream {
    OC_STREAM_EVIDENCE = 0,   /* bounded, never deleted before the cap */
    OC_STREAM_TRACE = 1,      /* ring: oldest segment removed when full */
    OC_STREAM_COUNT = 2
};

enum oc_rec_type {
    OC_REC_EVENT = 1,
    OC_REC_HEARTBEAT = 2,
    OC_REC_RESULT = 3,
    OC_REC_SAMPLES = 4
};

enum oc_kind {
    OC_KIND_SENSOR = 1,
    OC_KIND_DERIVED = 2,
    OC_KIND_SYSTEM = 3
};

enum oc_origin {
    OC_ORIGIN_LIVE = 1,
    OC_ORIGIN_SIMULATED = 2,
    OC_ORIGIN_REPLAYED = 3,
    OC_ORIGIN_MIXED = 4,
    OC_ORIGIN_NONE = 5
};

/* oc_prov.flags bits */
#define OC_PROV_INJECTED 0x01u   /* record describes a deliberately injected fault */

/* Provenance stored with every record and returned unchanged by read. */
struct oc_prov {
    uint8_t kind;          /* enum oc_kind */
    uint8_t origin;        /* enum oc_origin */
    uint8_t emitter;       /* which process/module produced the record */
    uint8_t flags;         /* OC_PROV_* bits; unknown bits are rejected */
    uint32_t instance;     /* reader spawn number from the supervisor */
    uint64_t session_id;   /* random per supervisor start */
};

/* oc_store_append flags */
#define OC_STORE_FORCE_SYNC 0x01u   /* write and sync before returning */

struct oc_store_stream_cfg {
    size_t segment_bytes;    /* max size of one segment file */
    uint32_t max_segments;   /* evidence: hard cap; trace: ring length */
};

struct oc_store_cfg {
    const char *dir;   /* existing directory; streams live in dir/evidence, dir/trace */
    struct oc_store_stream_cfg stream[OC_STREAM_COUNT];
    uint32_t batch_records;   /* sync after this many unsynced records */
    uint32_t batch_ms;        /* ...or after this long, checked by tick */
};

/* What recovery found during oc_store_open, per stream. */
struct oc_store_stream_report {
    uint64_t records_kept;
    uint64_t torn_bytes;          /* bytes truncated from the newest segment's tail */
    uint32_t damaged_segments;    /* sealed segments that failed validation, left unmodified */
    uint32_t removed_segments;    /* newest segments removed because their header was invalid */
    uint64_t seq_gaps;            /* missing record seqs between kept records */
    uint64_t next_seq;            /* seq the next append to this stream will receive */
};

struct oc_store_report {
    struct oc_store_stream_report stream[OC_STREAM_COUNT];
};

enum oc_store_state {
    OC_STORE_OK = 0,
    OC_STORE_FAILED = 1
};

/* The four counts cover records appended through this handle since open.
 * has_durable/durable_seq also cover records kept by recovery at open. */
struct oc_store_stream_status {
    uint64_t buffered;
    uint64_t written;
    uint64_t durable;
    uint64_t uncertain;
    int has_durable;           /* nonzero once any record is durable */
    uint64_t durable_seq;      /* every seq <= this is durable; valid if has_durable */
};

struct oc_store_status {
    enum oc_store_state state;
    int last_errno;            /* errno of the failure that caused FAILED, else 0 */
    struct oc_store_stream_status stream[OC_STREAM_COUNT];
};

/* A record handed to an oc_store_read visitor. payload is owned by the
 * store and valid only for the duration of the visitor call. */
struct oc_record_view {
    enum oc_stream stream;
    uint64_t seq;
    uint8_t type;            /* enum oc_rec_type */
    struct oc_prov prov;
    const void *payload;
    size_t len;
};

/* Return 0 to continue; any nonzero value stops the read and is returned by
 * oc_store_read. */
typedef int (*oc_store_visit_fn)(const struct oc_record_view *rec, void *ctx);

struct oc_store;

/* Opens or creates both streams under cfg->dir and runs recovery.
 * On success *out owns the store (release with oc_store_close) and *report,
 * if non-NULL, describes what recovery did. On failure *out is NULL. */
int oc_store_open(const struct oc_store_cfg *cfg, struct oc_store **out,
                  struct oc_store_report *report);

/* Copies payload and assigns the next seq of the stream to *seq_out (may be
 * NULL). The record is only buffered unless flags has OC_STORE_FORCE_SYNC,
 * in which case it is durable when 0 is returned. len may be 0, and then
 * payload may be NULL. */
int oc_store_append(struct oc_store *st, enum oc_stream stream,
                    enum oc_rec_type type, const struct oc_prov *prov,
                    const void *payload, size_t len, unsigned flags,
                    uint64_t *seq_out);

/* Applies the batching policy; call at least every 100 ms with
 * CLOCK_MONOTONIC nanoseconds. */
int oc_store_tick(struct oc_store *st, uint64_t now_mono_ns);

/* Writes and syncs everything pending in one stream. On success every record
 * of that stream with seq <= *durable_seq is durable. Returns -ENODATA if the
 * stream holds no records. */
int oc_store_sync(struct oc_store *st, enum oc_stream stream,
                  uint64_t *durable_seq);

int oc_store_status(const struct oc_store *st, struct oc_store_status *out);

/* Calls visit for each valid stored record of the stream with seq >= from_seq,
 * in seq order. Records still buffered are not visible; written records are,
 * including, after a failure, records whose sync failed (uncertain). */
int oc_store_read(struct oc_store *st, enum oc_stream stream, uint64_t from_seq,
                  oc_store_visit_fn visit, void *ctx);

/* Syncs, seals the active segments and frees the store. Returns the first
 * error encountered (on a FAILED store: the failure's -errno, with no I/O);
 * the store is freed either way. NULL is a no-op. */
int oc_store_close(struct oc_store *st);

#endif
