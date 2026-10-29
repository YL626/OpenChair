/* M3-A: three REAL processes (hub with capture + light reader + distance
 * reader, exec'd from `openchair --role ...`) persisting SIMULATED samples
 * through the real S3B hub into the real oc_store trace stream, then verified.
 *
 * SOFTWARE-ONLY. Every sample is SIMULATED; a PASS here is not M3 evidence
 * (M3 requires LIVE sensors on the Raspberry Pi) and says nothing about
 * hardware, SD-card durability or power loss.
 *
 * The persistent capture directory is separate from the runtime (socket)
 * directory; the harness deletes only directories it created itself, and
 * keeps the capture directory when a run fails.
 *
 * Independent verification: after the hub has exited, THIS process opens the
 * store itself and checks every record against oc_sim_reading() (the readers'
 * deterministic generator), so stored content is compared field by field with
 * what the readers must have produced, not with the hub's own opinion.
 *
 *   M1 normal capture            PASS, exact counts, exact content
 *   M2 reader lost mid-capture   FAIL(SOURCE_LOST); the other source still recorded
 *   M3 arming timeout            FAIL(ARM_TIMEOUT); nothing stored; hub keeps sensing
 *   M4 real EFBIG (RLIMIT_FSIZE) honest degraded storage; sensing continues
 *   M5 SIGKILL of the hub        confirmed-durable range survives; restart reports recovery */
#include "mp_harness.h"

#include "oc_capture_fmt.h"
#include "oc_sim_source.h"
#include "oc_store.h"

#define SNAP(dst, srcline) snprintf((dst), LINE_CAP, "%s", (srcline))
#define SESSION_ID 0x5333c0de00000001ull

static void show(const char *tag, const char *line)
{
    if (line[0] != '\0')
        printf("  %s %s\n", tag, line);
}

static bool has_flag(const char *line, const char *name)
{
    char v[200];
    return kv_str(line, "flags", v, sizeof v) && strstr(v, name) != nullptr;
}

/* ---------- independent verification straight from the store ---------- */

struct vstats {
    uint64_t records;
    uint64_t first_seq, last_seq;
    bool contiguous;
    uint64_t bad;               /* any mismatch against the generator or provenance */
    uint64_t samples[3], non_ok[3], gaps[3], ts_back[3], order_err[3];
};

struct vctx {
    struct vstats *s;
    uint64_t from, to, next;
    bool any;
    struct { bool have; uint32_t seq; uint64_t t; } prev[3];
};

static uint32_t fault_every_for(uint8_t src)
{
    return src == OC_SOURCE_DISTANCE ? 10u : 0u;
}

static int vvisit(const struct oc_record_view *r, void *p)
{
    struct vctx *x = p;
    struct vstats *s = x->s;
    if (r->seq < x->from || r->seq > x->to)
        return 0;
    if (!s->records)
        s->first_seq = r->seq;
    s->records++;
    s->last_seq = r->seq;
    if (x->any && r->seq != x->next)
        s->contiguous = false;
    x->any = true;
    x->next = r->seq + 1;
    struct oc_cap_batch b;
    if (r->type != OC_REC_SAMPLES || r->stream != OC_STREAM_TRACE ||
        oc_cap_decode(r->payload, r->len, &r->prov, &b) != 0) {
        s->bad++;
        return 0;
    }
    if (r->prov.kind != OC_KIND_SENSOR || r->prov.origin != OC_ORIGIN_SIMULATED ||
        r->prov.session_id != SESSION_ID ||
        r->prov.instance != (b.source == OC_SOURCE_LIGHT ? LIGHT_INST : DIST_INST) ||
        r->prov.emitter != (b.source == OC_SOURCE_LIGHT ? OC_EMIT_LIGHT_READER : OC_EMIT_DISTANCE_READER))
        s->bad++;
    for (unsigned k = 0; k < b.count; k++) {
        const struct oc_sample *m = &b.sample[k];
        struct oc_sim_reading want;
        if (oc_sim_reading(m->source, m->seq, fault_every_for(m->source), &want) != 0 ||
            m->status != want.status || m->value != want.value || m->aux != want.aux ||
            m->origin != OC_ORIGIN_SIMULATED)
            s->bad++;                                   /* content differs from what the reader generates */
        if (x->prev[m->source].have) {
            uint32_t d = m->seq - x->prev[m->source].seq;   /* wrap-safe */
            if (d == 0 || d >= 0x80000000u)
                s->order_err[m->source]++;
            else
                s->gaps[m->source] += d - 1;
            if (m->t_mono_ns < x->prev[m->source].t)
                s->ts_back[m->source]++;
        }
        x->prev[m->source].have = true;
        x->prev[m->source].seq = m->seq;
        x->prev[m->source].t = m->t_mono_ns;
        s->samples[m->source]++;
        if (m->status != OC_ST_OK)
            s->non_ok[m->source]++;
    }
    return 0;
}

/* Opens the store (recovery runs), reads [from, to], closes it. The hub must not be running. */
static int verify_store(uint64_t from, uint64_t to, struct vstats *s, struct oc_store_report *rep)
{
    memset(s, 0, sizeof *s);
    s->contiguous = true;
    struct oc_store_cfg cfg = {.dir = g_capdir,
                               .stream = {[OC_STREAM_EVIDENCE] = {4u << 20, 16}, [OC_STREAM_TRACE] = {2u << 20, 16}},
                               .batch_records = 32,
                               .batch_ms = 500};
    struct oc_store *st = nullptr;
    struct oc_store_report local;
    int rc = oc_store_open(&cfg, &st, rep ? rep : &local);
    if (rc != 0)
        return rc;
    struct vctx x = {.s = s, .from = from, .to = to};
    rc = oc_store_read(st, OC_STREAM_TRACE, from, vvisit, &x);
    int crc = oc_store_close(st);
    return rc != 0 ? rc : crc;
}

/* ---------- spawning ---------- */

static struct child *spawn_cap_hub(const char *const *more)
{
    const char *extra[24];
    int n = 0;
    extra[n++] = "--capture-dir";
    extra[n++] = g_capdir;
    for (; more != nullptr && *more != nullptr && n < 22; more++)
        extra[n++] = *more;
    extra[n] = nullptr;
    return spawn_hub(extra);
}

static struct child *spawn_light(const char *period)
{
    static const char *const ex[] = {nullptr};
    return spawn_reader("light", "light", LIGHT_INST, period, ex);
}

static struct child *spawn_dist(const char *period)
{
    static const char *const ex[] = {"--fault-every", "10", nullptr};
    return spawn_reader("distance", "distance", DIST_INST, period, ex);
}

static void stop_all(struct child *a, struct child *b, struct child *c)
{
    if (a) CHECK(terminate(a, 5000) && exited_ok(a));
    if (b) CHECK(terminate(b, 5000) && exited_ok(b));
    if (c) CHECK(terminate(c, 5000) && exited_ok(c));
}

static void show_result(const char *m, const struct child *h)
{
    show(m, h->cap_result);
    show(m, h->cap_src[SRC_LIGHT]);
    show(m, h->cap_src[SRC_DIST]);
    show(m, h->cap_rb[0]);
    show(m, h->cap_rb[1]);
    show(m, h->cap_store);
    for (int i = 0; i < 4; i++)
        show(m, h->cap_lat[i]);
}

/* ---------- M1 ---------- */

static void m1_normal_capture(void)
{
    t_begin("M1 three-process SIMULATED capture: PASS with exact counts and content");
    capdir_make();
    static const char *const hx[] = {"--capture-ms", "5000", "--capture-fresh-ms", "1000", nullptr};
    struct child *hub = spawn_cap_hub(hx);
    CHECK(hub != nullptr);
    if (hub == nullptr)
        return;
    bool ok;
    WAIT_UNTIL(ok, hub->ready[0] != '\0' && hub->store_recover[0] != '\0', 10000);
    CHECK(ok);
    CHECK(U(hub->store_recover, "trace_kept") == 0 && U(hub->store_recover, "trace_torn") == 0 &&
          U(hub->store_recover, "trace_next_seq") == 0);                 /* fresh directory */
    struct child *light = spawn_light("5");
    struct child *dist = spawn_dist("10");
    CHECK(light != nullptr && dist != nullptr);
    WAIT_UNTIL(ok, hub->cap_result[0] != '\0' && hub->cap_lat[3][0] != '\0', 60000);
    CHECK(ok);
    show_result("M1", hub);
    show("M1", hub->store_recover);

    const char *R = hub->cap_result;
    CHECK(kv_eq(R, "verdict", "PASS") && kv_eq(R, "flags", "-"));
    CHECK(kv_eq(R, "origin", "SIMULATED"));
    CHECK(U(R, "store_failed") == 0 && U(R, "tick_fail") == 0 && U(R, "sync_fail") == 0);
    CHECK(U(R, "reopen_done") == 1 && U(R, "sync_rc") == 0 && U(R, "close_rc") == 0);
    CHECK(kv_eq(R, "store", "OK") && U(R, "uncertain") == 0);
    CHECK(U(R, "durable") == U(R, "appended") && U(R, "appended") > 0);   /* all stored records durable */
    CHECK(U(R, "durable_seq") == U(R, "last_store_seq"));
    CHECK(U(R, "window_ms") >= 4900 && U(R, "window_ms") <= 5100);

    uint64_t total = 0;
    for (int s = SRC_LIGHT; s <= SRC_DIST; s++) {
        const char *L = hub->cap_src[s];
        CHECK(U(L, "accepted") > 0);
        CHECK(U(L, "accepted") == U(L, "ok") + U(L, "fail") + U(L, "not_stored"));   /* accounting identity */
        CHECK(U(L, "fail") == 0 && U(L, "not_stored") == 0);
        CHECK(U(L, "ok") == U(L, "rb0") && U(L, "ok") == U(L, "rb1"));               /* both read-back passes */
        CHECK(U(L, "went_down") == 0 && U(L, "inst_changed") == 0 && U(L, "ts_back") == 0);
        total += U(L, "ok");
    }
    CHECK(total == U(R, "appended"));
    CHECK(U(hub->cap_src[SRC_DIST], "non_ok") > 0);                    /* non-OK samples are records too */
    CHECK(U(hub->cap_src[SRC_LIGHT], "non_ok") == 0);
    for (int p = 0; p < 2; p++) {
        const char *B = hub->cap_rb[p];
        CHECK(U(B, "done") == 1 && U(B, "rc") == 0 && U(B, "decode_err") == 0 && U(B, "foreign") == 0 &&
              U(B, "order_err") == 0 && U(B, "contiguous") == 1 && U(B, "l_prov") == 1 && U(B, "d_prov") == 1);
        CHECK(U(B, "records") == U(R, "appended"));
    }
    CHECK(U(hub->cap_store, "reopen_torn") == 0 && U(hub->cap_store, "reopen_damaged") == 0 &&
          U(hub->cap_store, "reopen_next_seq") == U(R, "last_store_seq") + 1);
    /* the stored amount can never exceed what the readers actually transmitted */
    const unsigned ls = light->stat_n, ds = dist->stat_n;
    WAIT_UNTIL(ok, light->stat_n > ls + 1 && dist->stat_n > ds + 1, 5000);
    CHECK(U(hub->cap_src[SRC_LIGHT], "ok") <= U(light->stat, "sent"));
    CHECK(U(hub->cap_src[SRC_DIST], "ok") <= U(dist->stat, "sent"));
    /* the hub carried on sensing after the capture */
    CHECK(child_alive(hub) && kv_eq(hub->src[SRC_LIGHT], "state", "ACTIVE"));

    stop_all(light, dist, hub);
    CHECK(hub->summary[0] != '\0' && kv_eq(hub->summary, "exit", "0"));

    /* independent check straight from the store, against the readers' generator */
    struct vstats v;
    struct oc_store_report rep;
    CHECK(verify_store(U(R, "first_store_seq"), U(R, "last_store_seq"), &v, &rep) == 0);
    printf("  M1 independent: records=%llu light=%llu distance=%llu (non_ok=%llu) gaps l=%llu d=%llu bad=%llu\n",
           (unsigned long long)v.records, (unsigned long long)v.samples[SRC_LIGHT],
           (unsigned long long)v.samples[SRC_DIST], (unsigned long long)v.non_ok[SRC_DIST],
           (unsigned long long)v.gaps[SRC_LIGHT], (unsigned long long)v.gaps[SRC_DIST],
           (unsigned long long)v.bad);
    CHECK(v.records == U(R, "appended") && v.bad == 0 && v.contiguous);
    CHECK(v.samples[SRC_LIGHT] == U(hub->cap_src[SRC_LIGHT], "ok") &&
          v.samples[SRC_DIST] == U(hub->cap_src[SRC_DIST], "ok"));
    CHECK(v.non_ok[SRC_DIST] == U(hub->cap_src[SRC_DIST], "non_ok"));
    for (int s = SRC_LIGHT; s <= SRC_DIST; s++) {
        CHECK(v.gaps[s] == U(hub->cap_src[s], "gaps"));
        CHECK(v.ts_back[s] == 0 && v.order_err[s] == 0);
    }
    t_end();
}

/* ---------- M2 ---------- */

static void m2_source_lost(void)
{
    t_begin("M2 reader SIGKILLed mid-capture: capture FAILS honestly, other source still recorded");
    capdir_make();
    static const char *const hx[] = {"--capture-ms", "7000", nullptr};
    struct child *hub = spawn_cap_hub(hx);
    CHECK(hub != nullptr);
    bool ok;
    WAIT_UNTIL(ok, hub->ready[0] != '\0', 10000);
    struct child *light = spawn_light("5");
    struct child *dist = spawn_dist("10");
    CHECK(light != nullptr && dist != nullptr);
    WAIT_UNTIL(ok, kv_eq(hub->cap_progress, "state", "RUNNING") && U(hub->cap_progress, "d_ok") >= 60 &&
                       U(hub->cap_progress, "l_ok") >= 120, 30000);
    CHECK(ok);
    const uint64_t light_at_kill = U(hub->cap_progress, "l_ok");
    CHECK(kill(dist->pid, SIGKILL) == 0);
    CHECK(reap_within(dist, 5000) && killed_by(dist, SIGKILL));
    WAIT_UNTIL(ok, hub->cap_result[0] != '\0' && hub->cap_lat[3][0] != '\0', 60000);
    CHECK(ok);
    show_result("M2", hub);
    const char *R = hub->cap_result;
    CHECK(kv_eq(R, "verdict", "FAIL") && has_flag(R, "SOURCE_LOST"));
    CHECK(!has_flag(R, "READBACK") && !has_flag(R, "APPEND_FAILED") && !has_flag(R, "NOT_STORED"));
    CHECK(U(hub->cap_src[SRC_DIST], "went_down") == 1 && U(hub->cap_src[SRC_LIGHT], "went_down") == 0);
    CHECK(U(hub->cap_src[SRC_LIGHT], "ok") > light_at_kill);              /* the survivor kept being recorded */
    for (int s = SRC_LIGHT; s <= SRC_DIST; s++) {
        const char *L = hub->cap_src[s];
        CHECK(U(L, "accepted") == U(L, "ok") + U(L, "fail") + U(L, "not_stored"));
        CHECK(U(L, "ok") == U(L, "rb0") && U(L, "ok") == U(L, "rb1"));
    }
    CHECK(child_alive(hub) && child_alive(light));
    stop_all(light, nullptr, hub);
    struct vstats v;
    CHECK(verify_store(U(R, "first_store_seq"), U(R, "last_store_seq"), &v, nullptr) == 0);
    CHECK(v.records == U(R, "appended") && v.bad == 0 && v.contiguous);
    t_end();
}

/* ---------- M3 ---------- */

static void m3_arm_timeout(void)
{
    t_begin("M3 only one reader: capture never arms, nothing stored, hub keeps sensing");
    capdir_make();
    static const char *const hx[] = {"--capture-ms", "2000", "--capture-arm-timeout-ms", "1500", nullptr};
    struct child *hub = spawn_cap_hub(hx);
    CHECK(hub != nullptr);
    bool ok;
    WAIT_UNTIL(ok, hub->ready[0] != '\0', 10000);
    struct child *light = spawn_light("10");
    CHECK(light != nullptr);
    WAIT_UNTIL(ok, hub->cap_result[0] != '\0' && hub->cap_lat[3][0] != '\0', 30000);
    CHECK(ok);
    show_result("M3", hub);
    const char *R = hub->cap_result;
    CHECK(kv_eq(R, "verdict", "FAIL") && has_flag(R, "ARM_TIMEOUT"));
    CHECK(U(R, "appended") == 0 && U(R, "have_seq") == 0);
    CHECK(U(hub->cap_src[SRC_LIGHT], "accepted") == 0 && U(hub->cap_src[SRC_LIGHT], "pre") > 0);
    CHECK(U(R, "close_rc") == 0);
    const uint64_t a0 = U(hub->src[SRC_LIGHT], "accepted");
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "accepted") >= a0 + 20, 10000);   /* sensing is unaffected */
    CHECK(ok && child_alive(hub));
    stop_all(light, nullptr, hub);
    struct vstats v;
    CHECK(verify_store(0, UINT64_MAX, &v, nullptr) == 0 && v.records == 0);
    t_end();
}

/* ---------- M4 ---------- */

static void m4_storage_failure(void)
{
    t_begin("M4 real EFBIG from the kernel: honest degraded storage, sensing continues");
    capdir_make();
    static const char *const hx[] = {"--capture-ms", "6000", "--test-fsize-limit", "100000", nullptr};
    struct child *hub = spawn_cap_hub(hx);
    CHECK(hub != nullptr);
    bool ok;
    WAIT_UNTIL(ok, hub->ready[0] != '\0', 10000);
    struct child *light = spawn_light("2");
    struct child *dist = spawn_dist("5");
    CHECK(light != nullptr && dist != nullptr);
    WAIT_UNTIL(ok, hub->store_fail[0] != '\0', 40000);
    CHECK(ok);
    show("M4", hub->store_fail);
    const uint64_t acc_at_fail = U(hub->src[SRC_LIGHT], "accepted") + U(hub->src[SRC_DIST], "accepted");
    WAIT_UNTIL(ok, hub->cap_result[0] != '\0' && hub->cap_lat[3][0] != '\0', 60000);
    CHECK(ok);
    show_result("M4", hub);
    const char *R = hub->cap_result;
    CHECK(kv_eq(R, "verdict", "FAIL") && has_flag(R, "STORE_FAILED") && has_flag(R, "NOT_STORED"));
    CHECK(U(R, "store_failed") == 1 && U(R, "store_errno") == EFBIG);
    /* exactly one failure, classified once (tick, append or sync -- never mixed) */
    CHECK(U(R, "tick_fail") + U(R, "sync_fail") + U(hub->cap_src[SRC_LIGHT], "fail") +
              U(hub->cap_src[SRC_DIST], "fail") == 1);
    uint64_t not_stored = 0, ok_total = 0;
    for (int s = SRC_LIGHT; s <= SRC_DIST; s++) {
        const char *L = hub->cap_src[s];
        CHECK(U(L, "accepted") == U(L, "ok") + U(L, "fail") + U(L, "not_stored"));
        not_stored += U(L, "not_stored");
        ok_total += U(L, "ok");
    }
    CHECK(not_stored > 0);                       /* receiving continued; every unstored sample is counted */
    CHECK(kv_eq(R, "store", "FAILED") && U(R, "uncertain") > 0);
    CHECK(U(R, "durable") <= ok_total);          /* uncertain records are never reported durable */
    CHECK(U(R, "reopen_done") == 0);             /* no verification reopen on a failed store */
    /* sensing stayed available */
    CHECK(child_alive(hub) && child_alive(light) && child_alive(dist));
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "accepted") + U(hub->src[SRC_DIST], "accepted") >= acc_at_fail + 100, 20000);
    CHECK(ok);
    CHECK(kv_eq(hub->src[SRC_LIGHT], "state", "ACTIVE") && kv_eq(hub->src[SRC_DIST], "state", "ACTIVE"));
    const uint64_t durable_count = U(R, "durable");
    const uint64_t first = U(R, "first_store_seq");
    stop_all(light, dist, hub);
    /* What the next recovery keeps must be whole, exact, and include everything that was durable. */
    struct vstats v;
    struct oc_store_report rep;
    CHECK(verify_store(first, UINT64_MAX, &v, &rep) == 0);
    printf("  M4 after failure, recovery kept %llu trace records (durable at failure: %llu), bad=%llu\n",
           (unsigned long long)v.records, (unsigned long long)durable_count, (unsigned long long)v.bad);
    CHECK(v.bad == 0 && v.contiguous);
    CHECK(v.records >= durable_count);
    CHECK(v.records <= ok_total);
    t_end();
}

/* ---------- M5 ---------- */

static void m5_hub_sigkill_recovery(void)
{
    t_begin("M5 hub SIGKILL after a confirmed-durable watermark: retained range verified, restart reports recovery");
    capdir_make();
    static const char *const hx1[] = {"--capture-ms", "60000", nullptr};
    struct child *hub1 = spawn_cap_hub(hx1);
    CHECK(hub1 != nullptr);
    bool ok;
    WAIT_UNTIL(ok, hub1->ready[0] != '\0', 10000);
    struct child *light = spawn_light("5");
    struct child *dist = spawn_dist("10");
    CHECK(light != nullptr && dist != nullptr);
    WAIT_UNTIL(ok, kv_eq(hub1->cap_progress, "state", "RUNNING") && U(hub1->cap_progress, "has_durable") == 1 &&
                       U(hub1->cap_progress, "durable_seq") >= 300, 40000);
    CHECK(ok);
    /* D = the highest durable watermark this harness has SEEN before the kill. */
    const uint64_t D = U(hub1->cap_progress, "durable_seq");
    const uint64_t F = U(hub1->cap_progress, "first_store_seq");
    show("M5 last progress before kill:", hub1->cap_progress);
    CHECK(kill(hub1->pid, SIGKILL) == 0);
    CHECK(reap_within(hub1, 5000) && killed_by(hub1, SIGKILL));
    CHECK(hub1->cap_result[0] == '\0');                         /* a killed hub reports nothing: no invented result */
    CHECK(access(g_sock, F_OK) == 0);                           /* stale socket left behind */
    CHECK(child_alive(light) && child_alive(dist));             /* sensors survive the hub's death */

    struct vstats v1;
    struct oc_store_report rep1;
    CHECK(verify_store(0, UINT64_MAX, &v1, &rep1) == 0);        /* our own recovery of the retained range */
    const struct oc_store_stream_report *t1 = &rep1.stream[OC_STREAM_TRACE];
    printf("  M5 D=%llu F=%llu; recovery kept %llu records [%llu..%llu] torn_bytes=%llu damaged=%u bad=%llu\n",
           (unsigned long long)D, (unsigned long long)F, (unsigned long long)v1.records,
           (unsigned long long)v1.first_seq, (unsigned long long)v1.last_seq,
           (unsigned long long)t1->torn_bytes, t1->damaged_segments, (unsigned long long)v1.bad);
    CHECK(v1.contiguous && v1.bad == 0);                        /* nothing corrupt was accepted */
    CHECK(v1.records > 0 && v1.first_seq <= F);                 /* the ring has not rotated the range away */
    CHECK(v1.last_seq >= D);                                    /* the confirmed-durable range is retained */
    CHECK(t1->next_seq == v1.last_seq + 1 && t1->damaged_segments == 0);
    /* records beyond D are not promised; whatever survived is whole and valid (checked above) */

    static const char *const hx2[] = {"--capture-ms", "4000", nullptr};
    struct child *hub2 = spawn_cap_hub(hx2);                    /* restart on the same directory */
    CHECK(hub2 != nullptr);
    WAIT_UNTIL(ok, hub2->ready[0] != '\0' && hub2->store_recover[0] != '\0', 15000);
    CHECK(ok);
    show("M5 restart", hub2->store_recover);
    const char *SR = hub2->store_recover;
    /* The second recovery is its own event (opens mutate): assert only what the guarantee implies. */
    CHECK(U(SR, "trace_kept") >= D - v1.first_seq + 1);
    CHECK(U(SR, "trace_next_seq") >= D + 1);
    WAIT_UNTIL(ok, hub2->cap_result[0] != '\0' && hub2->cap_lat[3][0] != '\0', 90000);
    CHECK(ok);
    show_result("M5 restart", hub2);
    const char *R = hub2->cap_result;
    CHECK(kv_eq(R, "verdict", "PASS"));
    CHECK(U(R, "first_store_seq") == U(SR, "trace_next_seq"));  /* continues after the recovered tail */
    CHECK(U(R, "first_store_seq") > D);
    CHECK(U(hub2->cap_rb[0], "records") == U(R, "appended") &&   /* read-back sees ONLY this capture */
          U(hub2->cap_rb[1], "records") == U(R, "appended"));
    CHECK(child_alive(light) && child_alive(dist));
    CHECK(U(light->stat, "connects") >= 2 && U(dist->stat, "connects") >= 2);   /* readers reconnected */
    stop_all(light, dist, hub2);

    struct vstats v2;
    CHECK(verify_store(0, UINT64_MAX, &v2, nullptr) == 0);
    CHECK(v2.contiguous && v2.bad == 0);
    CHECK(v2.first_seq <= F && v2.last_seq == U(R, "last_store_seq"));
    CHECK(v2.records == v2.last_seq - v2.first_seq + 1);
    for (int s = SRC_LIGHT; s <= SRC_DIST; s++)
        CHECK(v2.order_err[s] == 0 && v2.ts_back[s] == 0);
    CHECK(access(g_sock, F_OK) != 0);                           /* hub2 shut down cleanly */
    t_end();
}

int main(void)
{
    harness_init(240);
    m1_normal_capture();
    m2_source_lost();
    m3_arm_timeout();
    m4_storage_failure();
    m5_hub_sigkill_recovery();
    harness_teardown();
    return harness_finish("test_multiproc_capture");
}
