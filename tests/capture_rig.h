/* Shared rig for the in-process capture tests (test_capture.c,
 * test_capture_faults.c): a REAL oc_store in a private temp directory, the
 * REAL oc_capture module, and SYNTHETIC hub callbacks driven with a fake
 * monotonic clock. Everything is SIMULATED. No sockets, no processes.
 *
 * The rig remembers every sample the capture reports as stored, in append
 * order, so tests can compare read-back DIRECTLY (fields and encoded bytes)
 * with what was fed, independently of the capture's own verification. */
#ifndef CAPTURE_RIG_H
#define CAPTURE_RIG_H

#define _GNU_SOURCE

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "oc_capture.h"

static int g_failures;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)
#define RUN(t)                          \
    do {                                \
        fprintf(stderr, "== %s\n", #t); \
        t();                            \
    } while (0)

#define SESSION 0x5333C0DE00000001ull
#define MS 1000000ull
#define T0 5000000000ull
#define RIG_MAX_EXP 8192

struct stored_rec {
    struct oc_sample s;
    uint8_t src;
};

struct rig {
    char dir[64];
    struct oc_capture *c;
    uint64_t now;                       /* fake CLOCK_MONOTONIC, ns */
    uint32_t seq[3];                    /* next sample seq per source (wraps) */
    uint32_t inst[3];
    uint32_t pending_gap[3];
    bool first[3];
    struct stored_rec exp[RIG_MAX_EXP]; /* samples reported stored, in append order */
    size_t nexp;
    uint64_t fed[3];                    /* samples fed per source */
};

static int rm_cb(const char *p, const struct stat *sb, int f, struct FTW *w)
{
    (void)sb; (void)f; (void)w;
    return remove(p);
}

[[maybe_unused]] static void rig_rmdir(struct rig *r)
{
    /* Only ever removes the directory this rig created with mkdtemp. */
    if (strncmp(r->dir, "/tmp/oc-cap-", 12) == 0)
        nftw(r->dir, rm_cb, 8, FTW_DEPTH | FTW_PHYS);
}

static struct oc_capture_cfg rig_cfg(struct rig *r)
{
    return (struct oc_capture_cfg){.dir = r->dir, .duration_ms = 1000, .arm_timeout_ms = 5000,
                                   .fresh_ms = 200, .expected_origin = OC_ORIGIN_SIMULATED};
}

static int rig_init(struct rig *r, const struct oc_capture_cfg *over)
{
    memset(r, 0, sizeof *r);
    snprintf(r->dir, sizeof r->dir, "/tmp/oc-cap-XXXXXX");
    if (mkdtemp(r->dir) == nullptr) {
        perror("mkdtemp");
        exit(2);
    }
    struct oc_capture_cfg cfg = over ? *over : rig_cfg(r);
    cfg.dir = r->dir;
    r->now = T0;
    r->inst[OC_SOURCE_LIGHT] = 7;
    r->inst[OC_SOURCE_DISTANCE] = 9;
    r->first[OC_SOURCE_LIGHT] = r->first[OC_SOURCE_DISTANCE] = true;
    return oc_capture_new(&cfg, &r->c, nullptr);
}

static void rig_event(struct rig *r, enum oc_hub_event_kind k, uint8_t src)
{
    struct oc_hub_event e = {.kind = k, .source = src, .instance = r->inst[src], .generation = 1};
    oc_capture_on_event(r->c, &e);
}

[[maybe_unused]] static void rig_skip(struct rig *r, uint8_t src, uint32_t n)
{
    r->seq[src] += n;
    r->pending_gap[src] += n;
}

struct feed_opts {
    uint8_t status;             /* 0 -> OK */
    int32_t value;
    bool ts_backwards;
    uint32_t instance;          /* 0 -> the rig's */
};

/* Feeds one accepted sample for `src` at rig->now; records it as stored if the
 * capture says it appended it. */
static void rig_feed(struct rig *r, uint8_t src, const struct feed_opts *o)
{
    uint8_t st = (o && o->status) ? o->status : OC_ST_OK;
    struct oc_sample s = {.t_mono_ns = r->now - 100000ull, .seq = r->seq[src], .source = src,
                          .origin = OC_ORIGIN_SIMULATED, .status = st,
                          .value = st == OC_ST_OK ? (o && o->value ? o->value : 100 + (int32_t)(r->seq[src] % 50)) : 0,
                          .aux = 0x4383};
    uint32_t inst = (o && o->instance) ? o->instance : r->inst[src];
    struct oc_hub_sample_info info = {.sample = &s, .session_id = SESSION, .instance = inst,
                                      .generation = 1, .source = src, .origin = OC_ORIGIN_SIMULATED,
                                      .gap = r->pending_gap[src], .first_of_instance = r->first[src],
                                      .first_on_connection = r->first[src],
                                      .ts_backwards = o && o->ts_backwards, .hub_rx_ns = r->now};
    uint64_t before = oc_capture_src(r->c, src)->append_ok;
    oc_capture_on_sample(r->c, &info);
    if (oc_capture_src(r->c, src)->append_ok > before && r->nexp < RIG_MAX_EXP)
        r->exp[r->nexp++] = (struct stored_rec){.s = s, .src = src};
    r->seq[src]++;
    r->pending_gap[src] = 0;
    r->first[src] = false;
    r->fed[src]++;
}

[[maybe_unused]] static void rig_tick(struct rig *r)
{
    (void)oc_capture_tick(r->c, r->now);
}

/* Reopens the store with a private handle and checks the trace records from
 * first_store_seq on against r->exp, field by field and byte by byte. */
struct cmp_ctx {
    const struct rig *r;
    size_t i;
    uint64_t next_seq;
    int bad;
};

static int cmp_visit(const struct oc_record_view *v, void *p)
{
    struct cmp_ctx *x = p;
    if (x->i >= x->r->nexp || v->seq != x->next_seq || v->type != OC_REC_SAMPLES ||
        v->stream != OC_STREAM_TRACE) {
        x->bad++;
        return 0;
    }
    const struct stored_rec *e = &x->r->exp[x->i];
    struct oc_prov want;
    if (oc_cap_prov(e->src, OC_ORIGIN_SIMULATED, SESSION, x->r->inst[e->src], &want) != 0 ||
        memcmp(&want, &v->prov, sizeof want) != 0)
        x->bad++;
    uint8_t enc[OC_CAP_MAX_PAYLOAD];
    size_t n = 0;
    if (oc_cap_encode(&e->s, 1, enc, sizeof enc, &n) != 0 || n != v->len ||
        memcmp(enc, v->payload, n) != 0)
        x->bad++;                                           /* byte-for-byte */
    struct oc_cap_batch b;
    if (oc_cap_decode(v->payload, v->len, &v->prov, &b) != 0 || b.count != 1 ||
        b.sample[0].t_mono_ns != e->s.t_mono_ns || b.sample[0].seq != e->s.seq ||
        b.sample[0].source != e->s.source || b.sample[0].origin != e->s.origin ||
        b.sample[0].status != e->s.status || b.sample[0].value != e->s.value ||
        b.sample[0].aux != e->s.aux)
        x->bad++;                                           /* field-for-field */
    x->i++;
    x->next_seq++;
    return 0;
}

[[maybe_unused]] static int rig_compare_store(const struct rig *r, uint64_t first_store_seq,
                             const struct oc_store_cfg *scfg)
{
    struct oc_store *st = nullptr;
    struct oc_store_report rep;
    if (oc_store_open(scfg, &st, &rep) != 0)
        return -1;
    struct cmp_ctx x = {.r = r, .next_seq = first_store_seq};
    int rc = oc_store_read(st, OC_STREAM_TRACE, first_store_seq, cmp_visit, &x);
    (void)oc_store_close(st);
    if (rc != 0)
        return -2;
    if (x.i != r->nexp)
        return -3;
    return x.bad;
}

[[maybe_unused]] static struct oc_store_cfg rig_store_cfg(const struct rig *r)
{
    return (struct oc_store_cfg){.dir = r->dir,
                                 .stream = {[OC_STREAM_EVIDENCE] = {4u << 20, 16},
                                            [OC_STREAM_TRACE] = {2u << 20, 16}},
                                 .batch_records = 32,
                                 .batch_ms = 500};
}

#endif
