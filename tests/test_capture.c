/* In-process tests for oc_capture over a REAL oc_store (SIMULATED samples,
 * fake clock, no sockets). See capture_rig.h. Storage failure injection is in
 * test_capture_faults.c (it needs link-time wrappers). */
#include "capture_rig.h"

static void both_up(struct rig *r)
{
    rig_event(r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_LIGHT);
    rig_event(r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_DISTANCE);
}

/* ---- two sources, exact readback, wrap, gaps, non-OK, pre/post window ---- */
static void test_two_sources_exact_readback(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    rig_event(&r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_LIGHT);
    r.seq[OC_SOURCE_LIGHT] = UINT32_MAX - 100;      /* sample seq wraps through 0 inside the window */
    r.first[OC_SOURCE_LIGHT] = true;

    /* iteration = 5 ms. Light every iteration; distance joins at iteration 20. */
    for (unsigned i = 0; i < 300; i++) {
        r.now = T0 + (uint64_t)i * 5 * MS;
        if (i == 20)
            rig_event(&r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_DISTANCE);
        if (i == 21)
            rig_skip(&r, OC_SOURCE_LIGHT, 2);       /* gap on light's FIRST window sample: outside the window, not counted */
        if (i == 100)
            rig_skip(&r, OC_SOURCE_LIGHT, 3);       /* the hub would report gap=3 */
        if (i == 200)
            rig_skip(&r, OC_SOURCE_DISTANCE, 1);
        struct feed_opts o = {0};
        if (i == 150) { o.status = OC_ST_SATURATED; }
        rig_feed(&r, OC_SOURCE_LIGHT, i == 150 ? &o : nullptr);
        if (i >= 20 && i % 2 == 0) {
            struct feed_opts d = {0};
            if (r.seq[OC_SOURCE_DISTANCE] % 7 == 6)
                d.status = OC_ST_TIMEOUT;           /* non-OK acquisition samples are records too */
            rig_feed(&r, OC_SOURCE_DISTANCE, &d);
        }
        if (i % 10 == 0)
            rig_tick(&r);
    }
    CHECK(oc_capture_state(r.c) == OC_CAP_ENDED);
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);

    const struct oc_capture_src *L = &res.src[OC_SOURCE_LIGHT], *D = &res.src[OC_SOURCE_DISTANCE];
    char fl[200];
    oc_capture_flags_str(res.flags, fl, sizeof fl);
    fprintf(stderr, "  verdict=%d flags=%s light ok=%llu pre=%llu post=%llu | dist ok=%llu non_ok=%llu\n",
            res.verdict, fl, (unsigned long long)L->append_ok, (unsigned long long)L->pre_window,
            (unsigned long long)L->post_window, (unsigned long long)D->append_ok,
            (unsigned long long)D->non_ok);
    CHECK(res.verdict == OC_CAP_PASS && res.flags == OC_CAPF_NONE);

    /* accounting identity, per source; pre/post kept apart */
    for (int s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
        const struct oc_capture_src *x = &res.src[s];
        CHECK(x->accepted_window == x->append_ok + x->append_fail + x->accepted_not_stored);
        CHECK(x->append_fail == 0 && x->accepted_not_stored == 0);
        CHECK(x->accepted_window > 0);
        CHECK(x->pre_window + x->accepted_window + x->post_window == r.fed[s]);
        CHECK(!x->went_down && !x->instance_changed);
    }
    CHECK(L->pre_window == 21 && D->pre_window == 0);           /* window opened on distance's first sample */
    CHECK(L->post_window > 0 && D->post_window > 0);
    CHECK(L->gaps == 3 && D->gaps == 1);                        /* only gaps between window samples */
    CHECK(L->non_ok == 1 && D->non_ok > 3);
    CHECK(L->instance == 7 && D->instance == 9 && L->session_id == SESSION);
    CHECK(res.records_appended == L->append_ok + D->append_ok);
    CHECK(res.records_appended == r.nexp);

    /* durability: everything stored is durable after the final sync */
    CHECK(res.status_valid && res.status.state == OC_STORE_OK);
    CHECK(res.status.stream[OC_STREAM_TRACE].durable == res.records_appended);
    CHECK(res.status.stream[OC_STREAM_TRACE].uncertain == 0);
    CHECK(res.status.stream[OC_STREAM_TRACE].has_durable &&
          res.status.stream[OC_STREAM_TRACE].durable_seq == res.last_store_seq);
    CHECK(res.sync_rc == 0 && res.close_rc == 0 && res.reopen_done && res.reopen_rc == 0);

    /* both read-back passes agree with the append-time counters */
    for (int p = 0; p < 2; p++) {
        const struct oc_cap_rb *b = &res.rb[p];
        CHECK(b->done && b->rc == 0 && b->decode_errors == 0 && b->foreign_records == 0);
        CHECK(b->store_seq_contiguous && b->records == res.records_appended);
        CHECK(b->first_store_seq_seen == res.first_store_seq && b->last_store_seq_seen == res.last_store_seq);
        for (int s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
            CHECK(b->src[s].samples == res.src[s].append_ok);
            CHECK(b->src[s].gaps == res.src[s].gaps);
            CHECK(b->src[s].ts_backwards == 0 && b->src[s].provenance_ok);
            CHECK(b->src[s].digest == res.src[s].digest);
            CHECK(b->src[s].non_ok == res.src[s].non_ok);
        }
    }
    /* recovery at the verification reopen: a clean close leaves nothing torn */
    const struct oc_store_stream_report *tr = &res.rec_reopen.stream[OC_STREAM_TRACE];
    CHECK(tr->torn_bytes == 0 && tr->damaged_segments == 0 && tr->seq_gaps == 0);
    CHECK(tr->next_seq == res.last_store_seq + 1);

    /* independent, direct comparison with what was fed (fields and bytes) */
    struct oc_store_cfg sc = rig_store_cfg(&r);
    CHECK(rig_compare_store(&r, res.first_store_seq, &sc) == 0);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

/* ---- arming: both sources active AND fresh; timeout ---- */
static void test_arming_and_timeout(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    /* nothing active: samples are pre_window, not stored */
    rig_event(&r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_LIGHT);
    rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
    CHECK(oc_capture_state(r.c) == OC_CAP_ARMED);
    /* distance goes active but never sends: still ARMED */
    r.now += 100 * MS;
    rig_event(&r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_DISTANCE);
    rig_tick(&r);
    CHECK(oc_capture_state(r.c) == OC_CAP_ARMED);
    /* distance sends, but light is now STALE (last sample > fresh_ms ago) */
    r.now += 300 * MS;
    rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);
    CHECK(oc_capture_state(r.c) == OC_CAP_ARMED);
    /* light fresh again -> the window opens on this very sample */
    r.now += 50 * MS;
    rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
    CHECK(oc_capture_state(r.c) == OC_CAP_RUNNING);
    CHECK(oc_capture_src(r.c, OC_SOURCE_LIGHT)->accepted_window == 1);
    CHECK(oc_capture_src(r.c, OC_SOURCE_LIGHT)->pre_window == 1);
    CHECK(oc_capture_src(r.c, OC_SOURCE_DISTANCE)->pre_window == 1);
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);         /* interrupted mid-window */
    CHECK(res.verdict == OC_CAP_FAIL && (res.flags & OC_CAPF_INTERRUPTED));
    oc_capture_free(r.c);
    rig_rmdir(&r);

    /* timeout: only one source ever arrives */
    CHECK(rig_init(&r, nullptr) == 0);
    rig_event(&r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_LIGHT);
    for (unsigned i = 0; i < 120; i++) {
        r.now = T0 + (uint64_t)i * 50 * MS;
        rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
        rig_tick(&r);
    }
    CHECK(oc_capture_state(r.c) == OC_CAP_ENDED);
    CHECK(oc_capture_flags(r.c) & OC_CAPF_ARM_TIMEOUT);
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.verdict == OC_CAP_FAIL && (res.flags & OC_CAPF_ARM_TIMEOUT));
    CHECK(res.records_appended == 0 && !res.have_store_seq);
    CHECK(res.src[OC_SOURCE_LIGHT].accepted_window == 0 && res.src[OC_SOURCE_LIGHT].pre_window > 0);
    CHECK(res.close_rc == 0);                           /* an empty capture still closes cleanly */
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

/* ---- source loss, missing source, instance change, backwards time ---- */
static void drive_window(struct rig *r, unsigned n, void (*hook)(struct rig *, unsigned))
{
    both_up(r);
    for (unsigned i = 0; i < n; i++) {
        r->now = T0 + (uint64_t)i * 5 * MS;
        if (hook)
            hook(r, i);
        rig_feed(r, OC_SOURCE_LIGHT, nullptr);
        if (i % 2 == 0)
            rig_feed(r, OC_SOURCE_DISTANCE, nullptr);
        if (i % 10 == 0)
            rig_tick(r);
    }
}

static void hook_distance_down(struct rig *r, unsigned i)
{
    if (i == 60)
        rig_event(r, OC_HUB_EV_SOURCE_DOWN, OC_SOURCE_DISTANCE);
}

static void test_source_disconnect_midcapture(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    drive_window(&r, 300, hook_distance_down);   /* distance goes down at 300 ms; light keeps going */
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.verdict == OC_CAP_FAIL && (res.flags & OC_CAPF_SOURCE_LOST));
    CHECK(res.src[OC_SOURCE_DISTANCE].went_down && !res.src[OC_SOURCE_LIGHT].went_down);
    /* the remaining source was still recorded, honestly and exactly */
    CHECK(res.src[OC_SOURCE_LIGHT].append_ok > res.src[OC_SOURCE_DISTANCE].append_ok);
    CHECK(res.rb[0].src[OC_SOURCE_LIGHT].samples == res.src[OC_SOURCE_LIGHT].append_ok);
    CHECK(res.rb[1].src[OC_SOURCE_DISTANCE].samples == res.src[OC_SOURCE_DISTANCE].append_ok);
    CHECK(!(res.flags & OC_CAPF_READBACK) && !(res.flags & OC_CAPF_APPEND_FAILED));
    struct oc_store_cfg sc = rig_store_cfg(&r);
    CHECK(rig_compare_store(&r, res.first_store_seq, &sc) == 0);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

static void test_source_missing(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    both_up(&r);
    rig_feed(&r, OC_SOURCE_LIGHT, nullptr);          /* light is fresh... */
    r.now += 10 * MS;
    rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);       /* ...window opens on distance */
    CHECK(oc_capture_state(&*r.c) == OC_CAP_RUNNING);
    for (unsigned i = 1; i < 260; i++) {             /* light falls silent; only distance flows */
        r.now += 5 * MS;
        rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);
        if (i % 10 == 0)
            rig_tick(&r);
    }
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.verdict == OC_CAP_FAIL && (res.flags & OC_CAPF_SOURCE_MISSING));
    CHECK(res.src[OC_SOURCE_LIGHT].append_ok == 0 && res.src[OC_SOURCE_DISTANCE].append_ok > 0);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

static void hook_instance_change(struct rig *r, unsigned i)
{
    if (i == 100)
        r->inst[OC_SOURCE_LIGHT] = 8;                /* a new instance appears inside the window */
}
static void test_instance_change_and_backwards_time(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    drive_window(&r, 300, hook_instance_change);
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.verdict == OC_CAP_FAIL && (res.flags & OC_CAPF_INSTANCE_CHANGED));
    CHECK(res.src[OC_SOURCE_LIGHT].instance_changed);
    oc_capture_free(r.c);
    rig_rmdir(&r);

    CHECK(rig_init(&r, nullptr) == 0);
    drive_window(&r, 100, nullptr);
    struct feed_opts o = {.ts_backwards = true};     /* the hub flagged a backwards timestamp */
    r.now += 5 * MS;
    rig_feed(&r, OC_SOURCE_LIGHT, &o);
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.verdict == OC_CAP_FAIL && (res.flags & OC_CAPF_TS_BACKWARDS));
    CHECK(res.src[OC_SOURCE_LIGHT].ts_backwards == 1);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}


/* ---- a connected but silent source must not yield a PASS ---- */
static void test_stalled_source_is_not_a_pass(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    both_up(&r);
    rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
    r.now += 10 * MS;
    rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);        /* window opens; light has now sent 0 window samples */
    r.now += 5 * MS;
    rig_feed(&r, OC_SOURCE_LIGHT, nullptr);           /* ...and exactly ONE, then falls silent (no SOURCE_DOWN) */
    for (unsigned i = 0; i < 260; i++) {
        r.now += 5 * MS;
        rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);
        if (i % 10 == 0)
            rig_tick(&r);
    }
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.src[OC_SOURCE_LIGHT].append_ok == 1 && res.src[OC_SOURCE_DISTANCE].append_ok > 100);
    CHECK(res.verdict == OC_CAP_FAIL && (res.flags & OC_CAPF_SOURCE_STALLED));
    CHECK(res.src[OC_SOURCE_LIGHT].max_silence_ns > 200 * MS);
    CHECK(res.src[OC_SOURCE_DISTANCE].max_silence_ns <= 200 * MS);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

/* a short pause inside the tolerance (fresh_ms = 200 ms in the rig) is fine */
static void test_short_pause_is_tolerated(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    both_up(&r);
    for (unsigned i = 0; i < 300; i++) {
        r.now = T0 + (uint64_t)i * 5 * MS;
        if (!(i >= 100 && i < 130))                    /* light pauses for 150 ms */
            rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
        if (i % 2 == 0)
            rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);
        if (i % 10 == 0)
            rig_tick(&r);
    }
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.verdict == OC_CAP_PASS);
    CHECK(res.src[OC_SOURCE_LIGHT].max_silence_ns >= 150 * MS && res.src[OC_SOURCE_LIGHT].max_silence_ns <= 200 * MS);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

/* ---- freshness must come from THIS connection, not an earlier one ---- */
static void test_freshness_not_inherited_across_reconnect(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    both_up(&r);
    rig_feed(&r, OC_SOURCE_LIGHT, nullptr);            /* light is fresh... */
    CHECK(oc_capture_state(r.c) == OC_CAP_ARMED);
    r.now += 20 * MS;
    rig_event(&r, OC_HUB_EV_SOURCE_DOWN, OC_SOURCE_LIGHT);   /* ...then its connection dies */
    r.now += 20 * MS;
    rig_event(&r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_LIGHT);     /* and a new one is up, but silent */
    r.now += 20 * MS;
    rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);          /* old credit must not open the window */
    CHECK(oc_capture_state(r.c) == OC_CAP_ARMED);
    r.now += 20 * MS;
    rig_feed(&r, OC_SOURCE_LIGHT, nullptr);             /* a sample of the new connection */
    CHECK(oc_capture_state(r.c) == OC_CAP_RUNNING);
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    oc_capture_free(r.c);
    rig_rmdir(&r);

    /* A second SOURCE_UP with no DOWN in between (a re-announcement) also discards the credit. */
    CHECK(rig_init(&r, nullptr) == 0);
    both_up(&r);
    rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
    r.now += 20 * MS;
    rig_event(&r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_LIGHT);
    r.now += 20 * MS;
    rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);
    CHECK(oc_capture_state(r.c) == OC_CAP_ARMED);
    CHECK(oc_capture_finish(r.c, &res) == 0);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

/* ---- clock robustness: a tick time earlier than the first sample must not underflow ---- */
static void test_clock_order_does_not_underflow(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    r.now = T0 + 10000 * MS;
    both_up(&r);
    rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
    CHECK(oc_capture_tick(r.c, r.now - MS) == 0);       /* earlier than the first sample */
    CHECK(oc_capture_state(r.c) == OC_CAP_ARMED && !(oc_capture_flags(r.c) & OC_CAPF_ARM_TIMEOUT));
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.window_ns == 0);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

/* ---- a reader leaving just after the window ended is not blamed ---- */
static void test_departure_after_window_end_is_not_source_lost(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    drive_window(&r, 200, nullptr);                     /* window is 1000 ms; last sample at 995 ms */
    CHECK(oc_capture_state(r.c) == OC_CAP_RUNNING);
    oc_capture_clock(r.c, T0 + 1004 * MS);              /* the hub loop's clock tick, before it dispatches */
    CHECK(oc_capture_state(r.c) == OC_CAP_ENDED);
    rig_event(&r, OC_HUB_EV_SOURCE_DOWN, OC_SOURCE_DISTANCE);   /* the reader leaves 4 ms after the window ended */
    oc_capture_clock(r.c, T0 + 500 * MS);               /* time never goes backwards: ignored */
    CHECK(oc_capture_state(r.c) == OC_CAP_ENDED);
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(!(res.flags & OC_CAPF_SOURCE_LOST) && !(res.flags & OC_CAPF_INTERRUPTED));
    CHECK(res.verdict == OC_CAP_PASS);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

/* ---- the capture's own verifier must catch foreign or damaged data ---- */
static void test_readback_detects_foreign_records(void)
{
    for (int variant = 0; variant < 2; variant++) {
        struct rig r;
        CHECK(rig_init(&r, nullptr) == 0);
        both_up(&r);
        for (unsigned i = 0; i < 40; i++) {
            r.now = T0 + (uint64_t)i * 5 * MS;
            rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
            rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);
            if (i == 20) {
                struct oc_store *st = oc_capture_store(r.c);
                CHECK(st != nullptr);
                struct oc_prov p;
                CHECK(oc_cap_prov(OC_SOURCE_LIGHT, OC_ORIGIN_SIMULATED, SESSION, 7, &p) == 0);
                struct oc_sample s = {.t_mono_ns = 1, .seq = 99999, .source = OC_SOURCE_LIGHT,
                                      .origin = OC_ORIGIN_SIMULATED, .status = OC_ST_OK, .value = 5};
                uint8_t pl[OC_CAP_MAX_PAYLOAD];
                size_t n = 0;
                CHECK(oc_cap_encode(&s, 1, pl, sizeof pl, &n) == 0);
                if (variant == 1)
                    pl[0] = 77;                      /* undecodable garbage */
                CHECK(oc_store_append(st, OC_STREAM_TRACE, OC_REC_SAMPLES, &p, pl, n, 0, nullptr) == 0);
            }
        }
        struct oc_capture_result res;
        CHECK(oc_capture_finish(r.c, &res) == 0);
        CHECK(res.verdict == OC_CAP_FAIL && (res.flags & OC_CAPF_READBACK));
        oc_capture_free(r.c);
        rig_rmdir(&r);
    }
}

/* ---- periodic tick/sync policy, with and without data ---- */
static void test_tick_schedule(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    both_up(&r);
    for (unsigned i = 0; i < 10; i++) {              /* ticks with no data are fine */
        r.now = T0 + (uint64_t)i * 50 * MS;
        CHECK(oc_capture_tick(r.c, r.now) == 0);
    }
    r.now = T0 + 600 * MS;
    rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
    r.now += 5 * MS;
    rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);      /* window opens; 1 record stored */
    struct oc_store_status s;
    CHECK(oc_capture_store_status(r.c, &s) == 0);
    CHECK(s.stream[OC_STREAM_TRACE].durable == 0);   /* buffered only */
    const uint64_t t1 = r.now + 10 * MS;
    CHECK(oc_capture_tick(r.c, t1) == 0);            /* the batch timer starts at this tick */
    CHECK(oc_capture_tick(r.c, t1 + 499 * MS) == 0);
    CHECK(oc_capture_store_status(r.c, &s) == 0);
    CHECK(s.stream[OC_STREAM_TRACE].durable == 0);   /* 499 ms: not yet */
    CHECK(oc_capture_tick(r.c, t1 + 500 * MS) == 0);
    CHECK(oc_capture_store_status(r.c, &s) == 0);
    CHECK(s.stream[OC_STREAM_TRACE].durable >= 1 && s.stream[OC_STREAM_TRACE].has_durable);
    CHECK(s.stream[OC_STREAM_TRACE].uncertain == 0);

    /* batch_records: 32 pending records sync without waiting for the timer */
    const uint64_t d0 = s.stream[OC_STREAM_TRACE].durable;
    r.now = t1 + 501 * MS;
    for (unsigned i = 0; i < 32; i++) {
        r.now += MS;
        rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
    }
    CHECK(oc_capture_tick(r.c, r.now) == 0);         /* starts the timer; count >= 32 syncs at once */
    CHECK(oc_capture_tick(r.c, r.now + MS) == 0);
    CHECK(oc_capture_store_status(r.c, &s) == 0);
    CHECK(s.stream[OC_STREAM_TRACE].durable >= d0 + 32);
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.lat_tick.n >= 13);                     /* ticks were really issued, data or not */
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

/* ---- configuration limits, reused directory, ring-sized capture ---- */
static void test_config_limits(void)
{
    char dir[] = "/tmp/oc-cap-XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    struct oc_capture *c = nullptr;
    struct oc_capture_cfg cfg = {.dir = dir, .duration_ms = OC_CAPTURE_MAX_MS + 1,
                                 .expected_origin = OC_ORIGIN_SIMULATED};
    CHECK(oc_capture_new(&cfg, &c, nullptr) == -EINVAL && c == nullptr);   /* > 10 min */
    cfg.duration_ms = OC_CAPTURE_MAX_MS;
    CHECK(oc_capture_new(&cfg, &c, nullptr) == 0 && c != nullptr);          /* exactly 10 min is allowed */
    oc_capture_free(c);
    c = nullptr;
    cfg.duration_ms = 0;                                                    /* default: 60 s */
    CHECK(oc_capture_new(&cfg, &c, nullptr) == 0);
    oc_capture_free(c);
    c = nullptr;
    cfg.expected_origin = OC_ORIGIN_MIXED;
    CHECK(oc_capture_new(&cfg, &c, nullptr) == -EINVAL && c == nullptr);
    cfg.expected_origin = OC_ORIGIN_SIMULATED;
    cfg.dir = nullptr;
    CHECK(oc_capture_new(&cfg, &c, nullptr) == -EINVAL && c == nullptr);
    CHECK(oc_capture_new(nullptr, &c, nullptr) == -EINVAL);
    nftw(dir, rm_cb, 8, FTW_DEPTH | FTW_PHYS);
}

static void test_reused_directory_excludes_earlier_records(void)
{
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    drive_window(&r, 300, nullptr);
    struct oc_capture_result res1;
    CHECK(oc_capture_finish(r.c, &res1) == 0);
    CHECK(res1.verdict == OC_CAP_PASS);
    oc_capture_free(r.c);

    /* second capture in the SAME storage directory */
    struct oc_capture_cfg cfg = rig_cfg(&r);
    struct oc_store_report rec;
    r.c = nullptr;
    CHECK(oc_capture_new(&cfg, &r.c, &rec) == 0);
    CHECK(rec.stream[OC_STREAM_TRACE].records_kept == res1.records_appended);   /* recovery saw capture 1 */
    CHECK(rec.stream[OC_STREAM_TRACE].next_seq == res1.last_store_seq + 1);
    r.nexp = 0;
    memset(r.fed, 0, sizeof r.fed);
    r.now = T0 + 10000 * MS;
    both_up(&r);
    for (unsigned i = 0; i < 300; i++) {
        r.now += 5 * MS;
        rig_feed(&r, OC_SOURCE_LIGHT, nullptr);
        if (i % 2 == 0)
            rig_feed(&r, OC_SOURCE_DISTANCE, nullptr);
        if (i % 10 == 0)
            rig_tick(&r);
    }
    struct oc_capture_result res2;
    CHECK(oc_capture_finish(r.c, &res2) == 0);
    CHECK(res2.verdict == OC_CAP_PASS);
    CHECK(res2.first_store_seq == res1.last_store_seq + 1);          /* store seq continues */
    for (int p = 0; p < 2; p++)                                      /* only THIS capture's records counted */
        CHECK(res2.rb[p].records == res2.records_appended && res2.rb[p].decode_errors == 0);
    struct oc_store_cfg sc = rig_store_cfg(&r);
    CHECK(rig_compare_store(&r, res2.first_store_seq, &sc) == 0);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

static void test_open_failure_is_honest(void)
{
    struct oc_capture *c = nullptr;
    struct oc_capture_cfg cfg = {.dir = "/tmp/oc-cap-does-not-exist-xyz", .duration_ms = 1000,
                                 .expected_origin = OC_ORIGIN_SIMULATED};
    int rc = oc_capture_new(&cfg, &c, nullptr);
    CHECK(rc < 0 && rc != -EINVAL && c != nullptr);                  /* usable object, no store */
    struct oc_hub_event e = {.kind = OC_HUB_EV_SOURCE_UP, .source = OC_SOURCE_LIGHT};
    oc_capture_on_event(c, &e);
    e.source = OC_SOURCE_DISTANCE;
    oc_capture_on_event(c, &e);
    struct oc_sample s = {.t_mono_ns = 1, .seq = 0, .source = OC_SOURCE_LIGHT,
                          .origin = OC_ORIGIN_SIMULATED, .status = OC_ST_OK, .value = 1};
    struct oc_hub_sample_info i = {.sample = &s, .session_id = SESSION, .instance = 1, .generation = 1,
                                   .source = OC_SOURCE_LIGHT, .origin = OC_ORIGIN_SIMULATED,
                                   .hub_rx_ns = T0};
    oc_capture_on_sample(c, &i);                                     /* must not crash */
    CHECK(oc_capture_tick(c, T0 + MS) == 0);                         /* no store: nothing to tick, nothing to fail */
    struct oc_capture_result res;
    CHECK(oc_capture_finish(c, &res) == 0);
    CHECK(res.verdict == OC_CAP_FAIL && (res.flags & OC_CAPF_OPEN_FAILED));
    CHECK(res.records_appended == 0);
    oc_capture_free(c);
}

static void test_flags_str(void)
{
    char b[64];
    oc_capture_flags_str(0, b, sizeof b);
    CHECK(strcmp(b, "-") == 0);
    oc_capture_flags_str(OC_CAPF_SOURCE_LOST | OC_CAPF_TICK_FAILED, b, sizeof b);
    CHECK(strstr(b, "SOURCE_LOST") && strstr(b, "TICK_FAILED"));
    oc_capture_flags_str(0xFFFFFFFFu, b, 12);                        /* truncates safely */
    CHECK(strlen(b) < 12);
    oc_capture_flags_str(1, nullptr, 0);                             /* no crash */
}

int main(void)
{
    RUN(test_two_sources_exact_readback);
    RUN(test_arming_and_timeout);
    RUN(test_source_disconnect_midcapture);
    RUN(test_source_missing);
    RUN(test_stalled_source_is_not_a_pass);
    RUN(test_short_pause_is_tolerated);
    RUN(test_freshness_not_inherited_across_reconnect);
    RUN(test_clock_order_does_not_underflow);
    RUN(test_departure_after_window_end_is_not_source_lost);
    RUN(test_instance_change_and_backwards_time);
    RUN(test_readback_detects_foreign_records);
    RUN(test_tick_schedule);
    RUN(test_config_limits);
    RUN(test_reused_directory_excludes_earlier_records);
    RUN(test_open_failure_is_honest);
    RUN(test_flags_str);
    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    puts("test_capture: all passed");
    return 0;
}
