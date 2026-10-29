#define _GNU_SOURCE

#include "oc_capture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MS_NS 1000000ull
#define FNV_INIT 0xcbf29ce484222325ull
#define DIR_MAX 512u

struct live {
    bool active;                 /* SOURCE_UP seen and not DOWN since */
    bool seen;
    uint64_t last_rx_ns;
};

struct oc_capture {
    struct oc_capture_cfg cfg;   /* resolved (defaults applied); dir points at dir_copy */
    char dir_copy[DIR_MAX];
    struct oc_store_cfg scfg;
    struct oc_store *st;
    enum oc_capture_state state;
    uint32_t flags;
    bool clock_started, started;
    uint64_t armed_ns, start_ns, end_ns, last_now_ns;
    struct live live[OC_SOURCE_DISTANCE + 1];
    bool fail_latched;
    bool finished;
    struct oc_capture_result res;   /* counters accumulate here; finish completes it */
};

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void lat_add(struct oc_lat *l, uint64_t ns)
{
    l->n++;
    l->total_ns += ns;
    if (ns > l->max_ns)
        l->max_ns = ns;
    if (ns > 1 * MS_NS)
        l->over_1ms++;
    if (ns > 10 * MS_NS)
        l->over_10ms++;
    if (ns > 100 * MS_NS)
        l->over_100ms++;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

/* Digest of one stored record: provenance fields (explicit little-endian) then
 * the exact payload bytes. */
static uint64_t rec_digest(uint64_t h, const struct oc_prov *p, const void *payload, size_t len)
{
    uint8_t m[16] = {p->kind, p->origin, p->emitter, p->flags};
    for (unsigned i = 0; i < 4; i++)
        m[4 + i] = (uint8_t)(p->instance >> (8 * i));
    for (unsigned i = 0; i < 8; i++)
        m[8 + i] = (uint8_t)(p->session_id >> (8 * i));
    return fnv(fnv(h, m, sizeof m), payload, len);
}

static bool valid_source(uint8_t s)
{
    return s == OC_SOURCE_LIGHT || s == OC_SOURCE_DISTANCE;
}

/* First storage failure: remember its errno, mark the capture failed. */
static void latch_failure(struct oc_capture *c, uint32_t flag, int rc)
{
    if (!c->fail_latched) {
        c->fail_latched = true;
        c->res.store_failed = true;
        c->res.store_errno = rc < 0 ? -rc : rc;
    }
    c->flags |= flag | OC_CAPF_STORE_FAILED;
}

/* ---------- arming and the window ---------- */

static bool source_ready(const struct oc_capture *c, uint8_t s, uint64_t now)
{
    const struct live *l = &c->live[s];
    if (!l->active || !l->seen)
        return false;
    const uint64_t fresh = (uint64_t)c->cfg.fresh_ms * MS_NS;
    return now >= l->last_rx_ns ? now - l->last_rx_ns <= fresh : true;
}

static void try_start(struct oc_capture *c, uint64_t now)
{
    if (c->state != OC_CAP_ARMED)
        return;
    if (source_ready(c, OC_SOURCE_LIGHT, now) && source_ready(c, OC_SOURCE_DISTANCE, now)) {
        c->state = OC_CAP_RUNNING;
        c->started = true;
        c->start_ns = now;
        c->end_ns = now + (uint64_t)c->cfg.duration_ms * MS_NS;
        for (int s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++)
            c->res.src[s].last_rx_window_ns = now;       /* silence is measured from the window start */
    }
}

static void end_window(struct oc_capture *c, uint64_t at)
{
    if (c->state != OC_CAP_RUNNING)
        return;
    const uint64_t end = at > c->end_ns ? c->end_ns : at;
    c->res.window_ns = end >= c->start_ns ? end - c->start_ns : 0;
    const uint64_t fresh = (uint64_t)c->cfg.fresh_ms * MS_NS;
    for (int s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
        struct oc_capture_src *x = &c->res.src[s];
        const uint64_t tail = end >= x->last_rx_window_ns ? end - x->last_rx_window_ns : 0;
        if (tail > x->max_silence_ns)
            x->max_silence_ns = tail;
        if (x->max_silence_ns > fresh)
            c->flags |= OC_CAPF_SOURCE_STALLED;
    }
    c->state = OC_CAP_ENDED;
}

/* ---------- hub callbacks ---------- */

static void sample_impl(struct oc_capture *c, const struct oc_hub_sample_info *i)
{
    if (c->finished || i == nullptr || i->sample == nullptr || !valid_source(i->source))
        return;
    const uint8_t s = i->source;
    struct oc_capture_src *x = &c->res.src[s];
    const uint64_t now = i->hub_rx_ns;

    if (!c->clock_started) {
        c->clock_started = true;
        c->armed_ns = now;
    }
    c->last_now_ns = now;
    c->live[s].seen = true;
    c->live[s].last_rx_ns = now;
    try_start(c, now);
    if (c->state == OC_CAP_RUNNING && now >= c->end_ns)
        end_window(c, now);

    if (c->state == OC_CAP_ARMED) {
        x->pre_window++;
        return;
    }
    if (c->state != OC_CAP_RUNNING) {
        x->post_window++;
        return;
    }

    /* ---- inside the window ---- */
    x->accepted_window++;
    if (now >= x->last_rx_window_ns && now - x->last_rx_window_ns > x->max_silence_ns)
        x->max_silence_ns = now - x->last_rx_window_ns;
    x->last_rx_window_ns = now;
    if (!x->have_first) {
        x->have_first = true;
        x->instance = i->instance;
        x->session_id = i->session_id;
        x->first_seq = i->sample->seq;
    } else {
        x->gaps += i->gap;               /* the first window sample's gap is outside the window */
        if (i->instance != x->instance || i->session_id != x->session_id) {
            x->instance_changed = true;
            c->flags |= OC_CAPF_INSTANCE_CHANGED;
        }
    }
    x->last_seq = i->sample->seq;
    if (i->ts_backwards) {
        x->ts_backwards++;
        c->flags |= OC_CAPF_TS_BACKWARDS;
    }
    if (i->sample->status != OC_ST_OK)
        x->non_ok++;

    const bool origin_bad = i->origin != c->cfg.expected_origin ||
                            i->sample->origin != c->cfg.expected_origin;
    if (origin_bad)
        c->flags |= OC_CAPF_ORIGIN_MISMATCH;
    if (c->fail_latched || c->st == nullptr || origin_bad) {
        x->accepted_not_stored++;
        c->flags |= OC_CAPF_NOT_STORED;
        return;
    }

    struct oc_prov prov;
    uint8_t payload[OC_CAP_MAX_PAYLOAD];
    size_t n = 0;
    int rc = oc_cap_prov(s, i->origin, i->session_id, i->instance, &prov);
    if (rc == 0)
        rc = oc_cap_encode(i->sample, 1, payload, sizeof payload, &n);
    uint64_t store_seq = 0;
    if (rc == 0) {
        const uint64_t t0 = mono_ns();
        rc = oc_store_append(c->st, OC_STREAM_TRACE, OC_REC_SAMPLES, &prov, payload, n, 0, &store_seq);
        lat_add(&c->res.lat_append, mono_ns() - t0);
    }
    if (rc != 0) {
        x->append_fail++;
        c->flags |= OC_CAPF_APPEND_FAILED;
        latch_failure(c, 0, rc);
        return;
    }
    x->append_ok++;
    x->last_store_seq = store_seq;
    x->digest = rec_digest(x->digest, &prov, payload, n);
    if (!c->res.have_store_seq) {
        c->res.have_store_seq = true;
        c->res.first_store_seq = store_seq;
    }
    c->res.last_store_seq = store_seq;
    c->res.records_appended++;
}

void oc_capture_on_sample(void *ctx, const struct oc_hub_sample_info *info)
{
    struct oc_capture *c = ctx;
    if (c == nullptr)
        return;
    const uint64_t t0 = mono_ns();
    sample_impl(c, info);
    lat_add(&c->res.lat_cb, mono_ns() - t0);
}

void oc_capture_on_event(void *ctx, const struct oc_hub_event *ev)
{
    struct oc_capture *c = ctx;
    if (c == nullptr || ev == nullptr || c->finished || !valid_source(ev->source))
        return;
    /* The host advances the clock (oc_capture_clock) before dispatching, so by the time an event
     * arrives a window that has run out is already closed. */
    if (ev->kind == OC_HUB_EV_SOURCE_UP) {
        c->live[ev->source].active = true;
        c->live[ev->source].seen = false;      /* freshness must come from a sample of THIS connection */
    } else if (ev->kind == OC_HUB_EV_SOURCE_DOWN) {
        c->live[ev->source].active = false;
        c->live[ev->source].seen = false;
        if (c->state == OC_CAP_RUNNING) {
            c->res.src[ev->source].went_down = true;
            c->flags |= OC_CAPF_SOURCE_LOST;
        }
    }
}

void oc_capture_clock(struct oc_capture *c, uint64_t now_ns)
{
    if (c == nullptr || c->finished)
        return;
    if (!c->clock_started) {
        c->clock_started = true;
        c->armed_ns = now_ns;
    }
    if (now_ns > c->last_now_ns)
        c->last_now_ns = now_ns;
    if (c->state == OC_CAP_RUNNING && c->last_now_ns >= c->end_ns)
        end_window(c, c->last_now_ns);
}

int oc_capture_tick(struct oc_capture *c, uint64_t now_ns)
{
    if (c == nullptr || c->finished)
        return 0;
    oc_capture_clock(c, now_ns);
    if (c->state == OC_CAP_ARMED) {
        try_start(c, now_ns);
        if (c->state == OC_CAP_ARMED && now_ns >= c->armed_ns &&
            now_ns - c->armed_ns >= (uint64_t)c->cfg.arm_timeout_ms * MS_NS) {
            c->state = OC_CAP_ENDED;
            c->flags |= OC_CAPF_ARM_TIMEOUT;
        }
    } else if (c->state == OC_CAP_RUNNING && now_ns >= c->end_ns) {
        end_window(c, now_ns);
    }

    if (c->st == nullptr || c->fail_latched)
        return c->fail_latched ? -(c->res.store_errno ? c->res.store_errno : EIO) : 0;
    const uint64_t t0 = mono_ns();
    int rc = oc_store_tick(c->st, now_ns);
    lat_add(&c->res.lat_tick, mono_ns() - t0);
    if (rc != 0) {
        c->res.tick_fail++;
        latch_failure(c, OC_CAPF_TICK_FAILED, rc);
        return rc;
    }
    struct oc_store_status s;
    if (oc_store_status(c->st, &s) == 0 && s.state != OC_STORE_OK) {
        latch_failure(c, 0, s.last_errno != 0 ? -s.last_errno : -EIO);
        return -EIO;
    }
    return 0;
}

/* ---------- accessors ---------- */

enum oc_capture_state oc_capture_state(const struct oc_capture *c)
{
    return c->state;
}

uint32_t oc_capture_flags(const struct oc_capture *c)
{
    return c->flags;
}

const struct oc_capture_src *oc_capture_src(const struct oc_capture *c, uint8_t source)
{
    return valid_source(source) ? &c->res.src[source] : nullptr;
}

int oc_capture_store_status(const struct oc_capture *c, struct oc_store_status *out)
{
    if (c == nullptr || c->st == nullptr)
        return -ENODEV;
    return oc_store_status(c->st, out);
}

struct oc_store *oc_capture_store(const struct oc_capture *c)
{
    return c == nullptr ? nullptr : c->st;
}

const struct oc_capture_result *oc_capture_result(const struct oc_capture *c)
{
    return &c->res;
}

/* ---------- read-back verification ---------- */

struct vsrc {
    bool have_prev;
    uint32_t prev_seq;
    uint64_t prev_t;
};

struct vctx {
    const struct oc_capture *c;
    struct oc_cap_rb *rb;
    uint64_t first_store_seq, last_store_seq, next_store_seq;
    bool any;
    struct vsrc v[OC_SOURCE_DISTANCE + 1];
};

static int verify_visit(const struct oc_record_view *r, void *p)
{
    struct vctx *x = p;
    struct oc_cap_rb *rb = x->rb;
    rb->records++;
    if (!rb->first_store_seq_seen && !x->any)
        rb->first_store_seq_seen = r->seq;
    if (!x->any) {
        x->any = true;
        x->next_store_seq = x->first_store_seq;
    }
    rb->last_store_seq_seen = r->seq;
    if (r->seq != x->next_store_seq)
        rb->store_seq_contiguous = false;
    x->next_store_seq = r->seq + 1;
    if (r->seq < x->first_store_seq || r->seq > x->last_store_seq) {
        rb->foreign_records++;
        return 0;
    }
    if (r->type != OC_REC_SAMPLES || r->stream != OC_STREAM_TRACE) {
        rb->decode_errors++;
        return 0;
    }
    struct oc_cap_batch b;
    if (oc_cap_decode(r->payload, r->len, &r->prov, &b) != 0) {
        rb->decode_errors++;
        return 0;
    }
    const struct oc_capture_src *exp = &x->c->res.src[b.source];
    struct oc_cap_rb_src *o = &rb->src[b.source];
    struct vsrc *v = &x->v[b.source];
    if (!exp->have_first || r->prov.instance != exp->instance ||
        r->prov.session_id != exp->session_id || r->prov.origin != x->c->cfg.expected_origin)
        o->provenance_ok = false;
    o->digest = rec_digest(o->digest, &r->prov, r->payload, r->len);
    for (unsigned k = 0; k < b.count; k++) {
        const struct oc_sample *s = &b.sample[k];
        if (v->have_prev) {
            const uint32_t d = s->seq - v->prev_seq;      /* wrap-safe u32 distance */
            if (d == 0 || d >= 0x80000000u)
                rb->order_errors++;                       /* duplicate or regression */
            else
                o->gaps += d - 1;
            if (s->t_mono_ns < v->prev_t)
                o->ts_backwards++;
        }
        v->have_prev = true;
        v->prev_seq = s->seq;
        v->prev_t = s->t_mono_ns;
        if (s->status != OC_ST_OK)
            o->non_ok++;
        o->samples++;
    }
    return 0;
}

static void run_readback(struct oc_capture *c, struct oc_store *st, struct oc_cap_rb *rb,
                         uint64_t first_seq)
{
    memset(rb, 0, sizeof *rb);
    rb->done = true;
    rb->store_seq_contiguous = true;
    for (int s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
        rb->src[s].provenance_ok = true;
        rb->src[s].digest = FNV_INIT;
    }
    struct vctx x = {.c = c, .rb = rb, .first_store_seq = first_seq,
                     .last_store_seq = c->res.have_store_seq ? c->res.last_store_seq : first_seq - 1};
    if (!c->res.have_store_seq)
        x.last_store_seq = first_seq;       /* nothing was stored: anything read back is foreign */
    rb->rc = oc_store_read(st, OC_STREAM_TRACE, first_seq, verify_visit, &x);
    if (!c->res.have_store_seq)
        rb->foreign_records = rb->records;  /* every record found is foreign to this capture */
}

static void evaluate_rb(struct oc_capture *c, const struct oc_cap_rb *rb)
{
    if (!rb->done)
        return;
    if (rb->rc != 0 || rb->decode_errors != 0 || rb->foreign_records != 0 || rb->order_errors != 0 ||
        !rb->store_seq_contiguous || rb->records != c->res.records_appended)
        c->flags |= OC_CAPF_READBACK;
    if (c->res.have_store_seq && rb->records > 0 && rb->first_store_seq_seen > c->res.first_store_seq)
        c->flags |= OC_CAPF_RING_WRAP;
    for (int s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
        const struct oc_capture_src *x = &c->res.src[s];
        const struct oc_cap_rb_src *o = &rb->src[s];
        if (o->samples != x->append_ok || o->non_ok != x->non_ok ||
            !o->provenance_ok || o->digest != (x->append_ok ? x->digest : FNV_INIT))
            c->flags |= OC_CAPF_READBACK;
        if (o->samples == x->append_ok && o->gaps != x->gaps)
            c->flags |= OC_CAPF_GAP_MISMATCH;
        if (o->ts_backwards != 0)
            c->flags |= OC_CAPF_TS_BACKWARDS;
    }
}

/* ---------- finish ---------- */

static void check_status(struct oc_capture *c)
{
    struct oc_capture_result *r = &c->res;
    r->status_valid = oc_store_status(c->st, &r->status) == 0;
    if (!r->status_valid) {
        c->flags |= OC_CAPF_NOT_DURABLE;
        return;
    }
    const struct oc_store_stream_status *t = &r->status.stream[OC_STREAM_TRACE];
    if (r->status.state != OC_STORE_OK)
        latch_failure(c, 0, r->status.last_errno != 0 ? -r->status.last_errno : -EIO);
    if (r->records_appended != 0 &&
        (t->durable != r->records_appended || t->uncertain != 0 || !t->has_durable ||
         t->durable_seq < r->last_store_seq))
        c->flags |= OC_CAPF_NOT_DURABLE;
}

int oc_capture_finish(struct oc_capture *c, struct oc_capture_result *out)
{
    if (c == nullptr)
        return -EINVAL;
    if (c->finished) {
        if (out != nullptr)
            *out = c->res;
        return 0;
    }
    const uint64_t t_start = mono_ns();
    struct oc_capture_result *r = &c->res;

    if (c->state == OC_CAP_ARMED) {
        c->flags |= OC_CAPF_INTERRUPTED;
    } else if (c->state == OC_CAP_RUNNING) {
        c->flags |= OC_CAPF_INTERRUPTED;
        end_window(c, c->last_now_ns);
    }
    r->origin = c->cfg.expected_origin;
    if (c->started)
        for (int s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++)
            if (r->src[s].append_ok == 0)
                c->flags |= OC_CAPF_SOURCE_MISSING;

    if (c->st != nullptr) {
        const uint64_t first = r->have_store_seq ? r->first_store_seq
                                                 : r->rec_open.stream[OC_STREAM_TRACE].next_seq;
        if (!c->fail_latched && r->have_store_seq) {
            const uint64_t t0 = mono_ns();
            uint64_t dseq = 0;
            r->sync_rc = oc_store_sync(c->st, OC_STREAM_TRACE, &dseq);
            lat_add(&r->lat_sync, mono_ns() - t0);
            if (r->sync_rc != 0) {
                r->sync_fail++;
                latch_failure(c, OC_CAPF_SYNC_FAILED, r->sync_rc);
            }
        }
        check_status(c);
        run_readback(c, c->st, &r->rb[0], first);
        if (!c->fail_latched)
            evaluate_rb(c, &r->rb[0]);
        r->close_rc = oc_store_close(c->st);
        c->st = nullptr;
        if (r->close_rc != 0 && !c->fail_latched)
            c->flags |= OC_CAPF_CLOSE_FAILED;

        if (!c->fail_latched && r->close_rc == 0) {
            struct oc_store *st2 = nullptr;
            r->reopen_rc = oc_store_open(&c->scfg, &st2, &r->rec_reopen);
            if (r->reopen_rc != 0) {
                c->flags |= OC_CAPF_REOPEN_FAILED;
            } else {
                r->reopen_done = true;
                run_readback(c, st2, &r->rb[1], first);
                evaluate_rb(c, &r->rb[1]);
                if (oc_store_close(st2) != 0)
                    c->flags |= OC_CAPF_CLOSE_FAILED;
            }
        }
    }

    for (int s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
        const struct oc_capture_src *x = &r->src[s];
        if (x->accepted_window != x->append_ok + x->append_fail + x->accepted_not_stored)
            c->flags |= OC_CAPF_ACCOUNTING;
        if (x->append_fail != 0)
            c->flags |= OC_CAPF_APPEND_FAILED;
        if (x->accepted_not_stored != 0)
            c->flags |= OC_CAPF_NOT_STORED;
    }
    if (r->tick_fail != 0)
        c->flags |= OC_CAPF_TICK_FAILED;
    if (r->sync_fail != 0)
        c->flags |= OC_CAPF_SYNC_FAILED;

    r->flags = c->flags;
    r->verdict = c->flags == OC_CAPF_NONE ? OC_CAP_PASS : OC_CAP_FAIL;
    r->finish_ns = mono_ns() - t_start;
    c->state = OC_CAP_DONE;
    c->finished = true;
    if (out != nullptr)
        *out = *r;
    return 0;
}

void oc_capture_free(struct oc_capture *c)
{
    if (c == nullptr)
        return;
    if (!c->finished)
        (void)oc_capture_finish(c, nullptr);
    free(c);
}

/* ---------- construction ---------- */

int oc_capture_new(const struct oc_capture_cfg *cfg, struct oc_capture **out,
                   struct oc_store_report *rec)
{
    if (out != nullptr)
        *out = nullptr;
    if (cfg == nullptr || out == nullptr || cfg->dir == nullptr || cfg->dir[0] == '\0' ||
        cfg->duration_ms > OC_CAPTURE_MAX_MS)
        return -EINVAL;
    if (cfg->expected_origin != OC_ORIGIN_LIVE && cfg->expected_origin != OC_ORIGIN_SIMULATED &&
        cfg->expected_origin != OC_ORIGIN_REPLAYED)
        return -EINVAL;
    if (strlen(cfg->dir) >= DIR_MAX)
        return -ENAMETOOLONG;

    struct oc_capture *c = calloc(1, sizeof *c);
    if (c == nullptr)
        return -ENOMEM;
    c->cfg = *cfg;
    snprintf(c->dir_copy, sizeof c->dir_copy, "%s", cfg->dir);
    c->cfg.dir = c->dir_copy;
    if (c->cfg.duration_ms == 0)
        c->cfg.duration_ms = OC_CAPTURE_DEFAULT_MS;
    if (c->cfg.arm_timeout_ms == 0)
        c->cfg.arm_timeout_ms = OC_CAPTURE_DEFAULT_ARM_TIMEOUT_MS;
    if (c->cfg.fresh_ms == 0)
        c->cfg.fresh_ms = OC_CAPTURE_DEFAULT_FRESH_MS;
    c->scfg = (struct oc_store_cfg){
        .dir = c->dir_copy,
        .stream = {[OC_STREAM_EVIDENCE] = {c->cfg.evidence_segment_bytes ? c->cfg.evidence_segment_bytes : 4u << 20,
                                           c->cfg.evidence_segments ? c->cfg.evidence_segments : 16},
                   [OC_STREAM_TRACE] = {c->cfg.trace_segment_bytes ? c->cfg.trace_segment_bytes : 2u << 20,
                                        c->cfg.trace_segments ? c->cfg.trace_segments : 16}},
        .batch_records = 32,
        .batch_ms = 500};
    c->state = OC_CAP_ARMED;
    for (int s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++)
        c->res.src[s].digest = FNV_INIT;

    *out = c;
    struct oc_store_report local;
    int rc = oc_store_open(&c->scfg, &c->st, &local);
    if (rc != 0) {
        c->st = nullptr;
        c->flags |= OC_CAPF_OPEN_FAILED;
        c->state = OC_CAP_ENDED;
        return rc;
    }
    c->res.rec_open = local;
    if (rec != nullptr)
        *rec = local;
    return 0;
}

/* ---------- text ---------- */

void oc_capture_flags_str(uint32_t flags, char *out, size_t cap)
{
    static const struct { uint32_t bit; const char *name; } T[] = {
        {OC_CAPF_OPEN_FAILED, "OPEN_FAILED"}, {OC_CAPF_ARM_TIMEOUT, "ARM_TIMEOUT"},
        {OC_CAPF_INTERRUPTED, "INTERRUPTED"}, {OC_CAPF_SOURCE_LOST, "SOURCE_LOST"},
        {OC_CAPF_SOURCE_MISSING, "SOURCE_MISSING"}, {OC_CAPF_INSTANCE_CHANGED, "INSTANCE_CHANGED"},
        {OC_CAPF_TS_BACKWARDS, "TS_BACKWARDS"}, {OC_CAPF_ORIGIN_MISMATCH, "ORIGIN_MISMATCH"},
        {OC_CAPF_APPEND_FAILED, "APPEND_FAILED"}, {OC_CAPF_TICK_FAILED, "TICK_FAILED"},
        {OC_CAPF_SYNC_FAILED, "SYNC_FAILED"}, {OC_CAPF_STORE_FAILED, "STORE_FAILED"},
        {OC_CAPF_NOT_STORED, "NOT_STORED"}, {OC_CAPF_NOT_DURABLE, "NOT_DURABLE"},
        {OC_CAPF_READBACK, "READBACK"}, {OC_CAPF_GAP_MISMATCH, "GAP_MISMATCH"},
        {OC_CAPF_RING_WRAP, "RING_WRAP"}, {OC_CAPF_CLOSE_FAILED, "CLOSE_FAILED"},
        {OC_CAPF_REOPEN_FAILED, "REOPEN_FAILED"}, {OC_CAPF_ACCOUNTING, "ACCOUNTING"},
        {OC_CAPF_SOURCE_STALLED, "SOURCE_STALLED"}};
    if (out == nullptr || cap == 0)
        return;
    out[0] = '\0';
    if (flags == 0) {
        snprintf(out, cap, "-");
        return;
    }
    size_t len = 0;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        if ((flags & T[i].bit) == 0)
            continue;
        size_t nl = strlen(T[i].name);
        size_t need = (len ? 1 : 0) + nl;
        if (len + need + 1 > cap)
            break;                          /* truncate on a name boundary */
        if (len)
            out[len++] = ',';
        memcpy(out + len, T[i].name, nl);
        len += nl;
        out[len] = '\0';
    }
}
