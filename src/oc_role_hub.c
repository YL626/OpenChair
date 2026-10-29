#define _GNU_SOURCE

#include "oc_role_hub.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "oc_backoff.h"
#include "oc_capture.h"
#include "oc_clock.h"
#include "oc_hub.h"
#include "oc_runlog.h"

#define MS_NS 1000000ull

/* Per-source callback-side checks. Independent of the hub's own tracker: they
 * count what the sample callback actually saw. */
struct src_acc {
    uint64_t cb_samples;        /* on_sample calls */
    uint64_t check_fail;        /* origin / timestamp sanity failures */
    uint64_t origin_bad;        /* origin != the session's input mode */
};

struct hub_run {
    uint8_t origin;             /* enum oc_origin every sample must carry (from --input) */
    const char *origin_name;
    struct oc_runlog log;
    struct src_acc acc[OC_SOURCE_DISTANCE + 1];
    uint64_t backoffs;          /* accept-error backoffs entered */
    uint64_t iter;              /* main-loop iterations */
    struct oc_capture *cap;     /* M3-A capture, or NULL */
    bool cap_reported;          /* final CAPRESULT lines emitted */
    bool store_fail_reported;
    int cap_report_rc;          /* nonzero if a critical capture report was not delivered */
};

static const char *src_name(unsigned s)
{
    return s == OC_SOURCE_LIGHT ? "light" : s == OC_SOURCE_DISTANCE ? "distance" : "unknown";
}

static const char *state_name(enum oc_hub_src_state s)
{
    switch (s) {
    case OC_HUB_SRC_UNAUTHORIZED: return "UNAUTHORIZED";
    case OC_HUB_SRC_AWAITING: return "AWAITING";
    case OC_HUB_SRC_ACTIVE: return "ACTIVE";
    case OC_HUB_SRC_DISCONNECTED: return "DISCONNECTED";
    }
    return "?";
}

static const char *close_name(enum oc_hub_close_reason r)
{
    static const char *const N[] = {"?", "EOF", "RESET", "BYE", "ABUSE", "VERSION",
                                    "SUPERSEDED", "SHUTDOWN", "ERROR", "PRE_HELLO",
                                    "BAD_HELLO", "ACK_FAILED", "SHED"};
    return (unsigned)r < sizeof N / sizeof N[0] ? N[r] : "?";
}

static void on_sample(void *ctx, const struct oc_hub_sample_info *i)
{
    struct hub_run *h = ctx;
    if (h->cap != nullptr)
        oc_capture_on_sample(h->cap, i);       /* persists inside the window; times itself */
    if (i->source > OC_SOURCE_DISTANCE || i->source == 0)
        return;
    struct src_acc *a = &h->acc[i->source];
    a->cb_samples++;
    bool bad = false;
    if (i->origin != h->origin || i->sample->origin != h->origin) {
        a->origin_bad++;
        bad = true;
    }
    if (i->sample->t_mono_ns > i->hub_rx_ns ||
        i->hub_rx_ns - i->sample->t_mono_ns > OC_HUB_ROLE_MAX_SAMPLE_AGE_NS)
        bad = true;
    if (bad)
        a->check_fail++;
}

static void on_event(void *ctx, const struct oc_hub_event *e)
{
    struct hub_run *h = ctx;
    if (h->cap != nullptr)
        oc_capture_on_event(h->cap, e);
    switch (e->kind) {
    case OC_HUB_EV_SOURCE_UP:
        (void)oc_runlog_line(&h->log, "EVENT ev=SOURCE_UP src=%s inst=%u gen=%u reconnect=%d reset=%d",
                             src_name(e->source), e->instance, e->generation, e->reconnect, e->reset);
        break;
    case OC_HUB_EV_SOURCE_DOWN:
        (void)oc_runlog_line(&h->log, "EVENT ev=SOURCE_DOWN src=%s inst=%u gen=%u reason=%s",
                             src_name(e->source), e->instance, e->generation, close_name(e->reason));
        break;
    case OC_HUB_EV_CONN_REJECTED:
        (void)oc_runlog_line(&h->log, "EVENT ev=CONN_REJECTED src=%s reject=%u",
                             src_name(e->source), e->reject);
        break;
    case OC_HUB_EV_CONN_CLOSED:
        (void)oc_runlog_line(&h->log, "EVENT ev=CONN_CLOSED reason=%s", close_name(e->reason));
        break;
    case OC_HUB_EV_HELLO_TIMEOUT:
        (void)oc_runlog_line(&h->log, "EVENT ev=HELLO_TIMEOUT");
        break;
    case OC_HUB_EV_DEGRADED_CHANGED:
        (void)oc_runlog_line(&h->log, "EVENT ev=DEGRADED_CHANGED degraded=%d", e->degraded);
        break;
    }
}

static int fmt_src(struct hub_run *h, const struct oc_hub *hub, unsigned src, bool critical)
{
    struct oc_hub_source_status st;
    int rc = oc_hub_source_status(hub, (uint8_t)src, &st);
    if (rc != 0)
        return rc;
    uint64_t other = 0;
    for (unsigned k = 0; k <= OC_ST_IO_ERR; k++)
        if (k != OC_ST_OK)
            other += st.status_count[k];
    const struct src_acc *a = &h->acc[src];
    const char *fmt =
        "SRC src=%s state=%s avail=%d authorized=%d inst=%u gen=%u accepted=%llu lost=%llu "
        "unobs=%llu dup=%llu regr=%llu ts_back=%llu last_seq=%u st_ok=%llu st_other=%llu "
        "ident_mismatch=%llu conns=%llu reconn=%llu resets=%llu cb_samples=%llu "
        "check_fail=%llu origin_bad=%llu";
#define SRC_ARGS                                                                              \
    src_name(src), state_name(st.state), st.available, st.authorized, st.instance,           \
        st.generation, (unsigned long long)st.seq.accepted, (unsigned long long)st.seq.lost, \
        (unsigned long long)st.seq.unobserved_prefix, (unsigned long long)st.seq.duplicates, \
        (unsigned long long)st.seq.regressions, (unsigned long long)st.seq.ts_backwards,     \
        st.seq.last, (unsigned long long)st.status_count[OC_ST_OK], (unsigned long long)other, \
        (unsigned long long)st.identity_mismatches, (unsigned long long)st.connections,      \
        (unsigned long long)st.reconnects, (unsigned long long)st.resets,                    \
        (unsigned long long)a->cb_samples, (unsigned long long)a->check_fail,                \
        (unsigned long long)a->origin_bad
    if (critical)
        return oc_runlog_critical(&h->log, OC_HUB_ROLE_CRITICAL_MS, fmt, SRC_ARGS);
    (void)oc_runlog_line(&h->log, fmt, SRC_ARGS);
#undef SRC_ARGS
    return 0;
}

static void fmt_hub_stat(struct hub_run *h, const struct oc_hub *hub, const char *tag)
{
    struct oc_hub_stats s;
    oc_hub_stats(hub, &s);
    (void)oc_runlog_line(
        &h->log,
        "%s role=hub origin=%s degraded=%d iter=%llu accept_errors=%llu backoffs=%llu "
        "conns_accepted=%llu shed=%llu hello_timeouts=%llu datagrams=%llu malformed=%llu "
        "abusive_closes=%llu log_dropped=%llu",
        tag, h->origin_name, oc_hub_degraded(hub), (unsigned long long)h->iter, (unsigned long long)s.accept_errors,
        (unsigned long long)h->backoffs, (unsigned long long)s.conns_accepted,
        (unsigned long long)s.shed, (unsigned long long)s.hello_timeouts,
        (unsigned long long)s.datagrams, (unsigned long long)s.malformed,
        (unsigned long long)s.abusive_closes, (unsigned long long)h->log.dropped);
}

static void emit_capture_progress(struct hub_run *h);

static void emit_periodic(struct hub_run *h, const struct oc_hub *hub)
{
    fmt_hub_stat(h, hub, "STAT");
    (void)fmt_src(h, hub, OC_SOURCE_LIGHT, false);
    (void)fmt_src(h, hub, OC_SOURCE_DISTANCE, false);
    emit_capture_progress(h);
}


/* ---------- capture reporting (M3-A) ---------- */

static const char *cap_state_name(enum oc_capture_state s)
{
    switch (s) {
    case OC_CAP_ARMED: return "ARMED";
    case OC_CAP_RUNNING: return "RUNNING";
    case OC_CAP_ENDED: return "ENDED";
    case OC_CAP_DONE: return "DONE";
    }
    return "?";
}

static unsigned long long us(uint64_t ns)
{
    return (unsigned long long)(ns / 1000u);
}

/* Routine progress line while a capture exists and its store is open. */
static void emit_capture_progress(struct hub_run *h)
{
    if (h->cap == nullptr || oc_capture_state(h->cap) == OC_CAP_DONE)
        return;
    struct oc_store_status s;
    memset(&s, 0, sizeof s);
    const bool have = oc_capture_store_status(h->cap, &s) == 0;
    const struct oc_capture_src *L = oc_capture_src(h->cap, OC_SOURCE_LIGHT);
    const struct oc_capture_src *D = oc_capture_src(h->cap, OC_SOURCE_DISTANCE);
    const struct oc_capture_result *r = oc_capture_result(h->cap);
    char fl[160];
    oc_capture_flags_str(oc_capture_flags(h->cap), fl, sizeof fl);
    const struct oc_store_stream_status *t = &s.stream[OC_STREAM_TRACE];
    (void)oc_runlog_line(
        &h->log,
        "CAPTURE state=%s flags=%s origin=%s have_seq=%d first_store_seq=%llu last_store_seq=%llu "
        "appended=%llu store=%s buffered=%llu written=%llu durable=%llu uncertain=%llu has_durable=%d "
        "durable_seq=%llu l_acc=%llu l_ok=%llu l_fail=%llu l_ns=%llu d_acc=%llu d_ok=%llu d_fail=%llu "
        "d_ns=%llu tick_fail=%llu",
        cap_state_name(oc_capture_state(h->cap)), fl, h->origin_name, r->have_store_seq,
        (unsigned long long)r->first_store_seq, (unsigned long long)r->last_store_seq,
        (unsigned long long)r->records_appended, have ? (s.state == OC_STORE_OK ? "OK" : "FAILED") : "CLOSED",
        (unsigned long long)t->buffered, (unsigned long long)t->written, (unsigned long long)t->durable,
        (unsigned long long)t->uncertain, t->has_durable, (unsigned long long)t->durable_seq,
        (unsigned long long)L->accepted_window, (unsigned long long)L->append_ok,
        (unsigned long long)L->append_fail, (unsigned long long)L->accepted_not_stored,
        (unsigned long long)D->accepted_window, (unsigned long long)D->append_ok,
        (unsigned long long)D->append_fail, (unsigned long long)D->accepted_not_stored,
        (unsigned long long)r->tick_fail);
}

#define CRIT(h, ...)                                                                      \
    do {                                                                                  \
        int crc_ = oc_runlog_critical(&(h)->log, OC_HUB_ROLE_CRITICAL_MS, __VA_ARGS__);   \
        if (crc_ != 0 && (h)->cap_report_rc == 0)                                         \
            (h)->cap_report_rc = crc_;                                                    \
    } while (0)

static void report_rb(struct hub_run *h, const char *name, const struct oc_cap_rb *rb)
{
    CRIT(h,
         "CAPRB pass=%s done=%d rc=%d records=%llu decode_err=%llu foreign=%llu order_err=%llu "
         "contiguous=%d first_seen=%llu last_seen=%llu l_samples=%llu l_prov=%d d_samples=%llu d_prov=%d",
         name, rb->done, rb->rc, (unsigned long long)rb->records, (unsigned long long)rb->decode_errors,
         (unsigned long long)rb->foreign_records, (unsigned long long)rb->order_errors,
         rb->store_seq_contiguous, (unsigned long long)rb->first_store_seq_seen,
         (unsigned long long)rb->last_store_seq_seen,
         (unsigned long long)rb->src[OC_SOURCE_LIGHT].samples, rb->src[OC_SOURCE_LIGHT].provenance_ok,
         (unsigned long long)rb->src[OC_SOURCE_DISTANCE].samples, rb->src[OC_SOURCE_DISTANCE].provenance_ok);
}

static void lat_line(struct hub_run *h, const char *what, const struct oc_lat *l)
{
    CRIT(h, "CAPLAT what=%s n=%llu max_us=%llu total_us=%llu gt1ms=%llu gt10ms=%llu gt100ms=%llu", what,
         (unsigned long long)l->n, us(l->max_ns), us(l->total_ns), (unsigned long long)l->over_1ms,
         (unsigned long long)l->over_10ms, (unsigned long long)l->over_100ms);
}

/* Finishes the capture (sync, verify, close, reopen, verify) and emits the
 * CRITICAL result lines. A line that cannot be delivered is surfaced through
 * h->cap_report_rc (the process then exits 4). */
static void finish_and_report(struct hub_run *h)
{
    if (h->cap == nullptr || h->cap_reported)
        return;
    struct oc_capture_result r;
    (void)oc_capture_finish(h->cap, &r);
    h->cap_reported = true;
    char fl[160];
    oc_capture_flags_str(r.flags, fl, sizeof fl);
    const struct oc_store_stream_status *t = &r.status.stream[OC_STREAM_TRACE];
    CRIT(h,
         "CAPRESULT verdict=%s flags=%s origin=%s window_ms=%llu have_seq=%d first_store_seq=%llu "
         "last_store_seq=%llu appended=%llu store_failed=%d store_errno=%d tick_fail=%llu sync_fail=%llu "
         "sync_rc=%d close_rc=%d reopen_rc=%d reopen_done=%d status_valid=%d store=%s buffered=%llu "
         "written=%llu durable=%llu uncertain=%llu durable_seq=%llu finish_ms=%llu",
         r.verdict == OC_CAP_PASS ? "PASS" : "FAIL", fl, h->origin_name, (unsigned long long)(r.window_ns / 1000000u),
         r.have_store_seq, (unsigned long long)r.first_store_seq, (unsigned long long)r.last_store_seq,
         (unsigned long long)r.records_appended, r.store_failed, r.store_errno,
         (unsigned long long)r.tick_fail, (unsigned long long)r.sync_fail, r.sync_rc, r.close_rc,
         r.reopen_rc, r.reopen_done, r.status_valid, r.status.state == OC_STORE_OK ? "OK" : "FAILED",
         (unsigned long long)t->buffered, (unsigned long long)t->written, (unsigned long long)t->durable,
         (unsigned long long)t->uncertain, (unsigned long long)t->durable_seq,
         (unsigned long long)(r.finish_ns / 1000000u));
    for (unsigned s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
        const struct oc_capture_src *x = &r.src[s];
        CRIT(h,
             "CAPSRC src=%s pre=%llu accepted=%llu ok=%llu fail=%llu not_stored=%llu post=%llu "
             "non_ok=%llu gaps=%llu ts_back=%llu went_down=%d inst_changed=%d inst=%u first_seq=%u "
             "last_seq=%u last_store_seq=%llu rb0=%llu rb1=%llu",
             src_name(s), (unsigned long long)x->pre_window, (unsigned long long)x->accepted_window,
             (unsigned long long)x->append_ok, (unsigned long long)x->append_fail,
             (unsigned long long)x->accepted_not_stored, (unsigned long long)x->post_window,
             (unsigned long long)x->non_ok, (unsigned long long)x->gaps,
             (unsigned long long)x->ts_backwards, x->went_down, x->instance_changed, x->instance,
             x->first_seq, x->last_seq, (unsigned long long)x->last_store_seq,
             (unsigned long long)r.rb[0].src[s].samples, (unsigned long long)r.rb[1].src[s].samples);
    }
    report_rb(h, "sync", &r.rb[0]);
    report_rb(h, "reopen", &r.rb[1]);
    const struct oc_store_stream_report *o = &r.rec_open.stream[OC_STREAM_TRACE];
    const struct oc_store_stream_report *p = &r.rec_reopen.stream[OC_STREAM_TRACE];
    CRIT(h,
         "CAPSTORE open_kept=%llu open_torn=%llu open_damaged=%u open_removed=%u open_gaps=%llu "
         "open_next_seq=%llu reopen_kept=%llu reopen_torn=%llu reopen_damaged=%u reopen_removed=%u "
         "reopen_gaps=%llu reopen_next_seq=%llu",
         (unsigned long long)o->records_kept, (unsigned long long)o->torn_bytes, o->damaged_segments,
         o->removed_segments, (unsigned long long)o->seq_gaps, (unsigned long long)o->next_seq,
         (unsigned long long)p->records_kept, (unsigned long long)p->torn_bytes, p->damaged_segments,
         p->removed_segments, (unsigned long long)p->seq_gaps, (unsigned long long)p->next_seq);
    lat_line(h, "callback", &r.lat_cb);
    lat_line(h, "append", &r.lat_append);
    lat_line(h, "tick", &r.lat_tick);
    lat_line(h, "sync", &r.lat_sync);
    if (r.verdict != OC_CAP_PASS)
        fprintf(stderr, "openchair: hub: capture FAILED (%s); %s data\n", fl,
                h->origin == OC_ORIGIN_LIVE ? "LIVE" : "SIMULATED, not M3 evidence either way");
}

/* Once per failure: surface a storage failure the moment it is latched. */
static void note_store_failure(struct hub_run *h)
{
    if (h->cap == nullptr || h->store_fail_reported ||
        (oc_capture_flags(h->cap) & OC_CAPF_STORE_FAILED) == 0)
        return;
    h->store_fail_reported = true;
    char fl[160];
    oc_capture_flags_str(oc_capture_flags(h->cap), fl, sizeof fl);
    CRIT(h, "STORE_FAIL origin=%s flags=%s errno=%d (sensing continues; nothing is retried or reported durable)",
         h->origin_name, fl, oc_capture_result(h->cap)->store_errno);
    fprintf(stderr, "openchair: hub: storage failed (%s); samples are no longer being stored\n", fl);
}

static void emit_store_recover(struct hub_run *h, const char *dir, const struct oc_store_report *r, int rc)
{
    if (rc != 0) {
        CRIT(h, "STORE_OPEN_FAILED dir=%s rc=%d (capture cannot run; sensing continues)", dir, rc);
        fprintf(stderr, "openchair: hub: oc_store_open(%s) failed (%d); capture disabled\n", dir, rc);
        return;
    }
    const struct oc_store_stream_report *t = &r->stream[OC_STREAM_TRACE];
    const struct oc_store_stream_report *e = &r->stream[OC_STREAM_EVIDENCE];
    CRIT(h,
         "STORE_RECOVER dir=%s trace_kept=%llu trace_torn=%llu trace_damaged=%u trace_removed=%u "
         "trace_gaps=%llu trace_next_seq=%llu ev_kept=%llu ev_torn=%llu ev_next_seq=%llu",
         dir, (unsigned long long)t->records_kept, (unsigned long long)t->torn_bytes, t->damaged_segments,
         t->removed_segments, (unsigned long long)t->seq_gaps, (unsigned long long)t->next_seq,
         (unsigned long long)e->records_kept, (unsigned long long)e->torn_bytes,
         (unsigned long long)e->next_seq);
}

static int timeout_until(uint64_t now, uint64_t deadline)
{
    if (deadline <= now)
        return 0;
    uint64_t ms = (deadline - now + MS_NS - 1) / MS_NS;
    return ms > 1000 ? 1000 : (int)ms;
}

/* Testing control: after every descriptor the hub needs (listener, signalfd,
 * stdout) exists, lower RLIMIT_NOFILE so that only `headroom` more can be
 * opened. Exhaustion then hits accept() only, never start-up or logging. */
static int apply_fd_headroom(int headroom, struct rlimit *saved)
{
    int probe = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 0);     /* lowest free descriptor */
    if (probe < 0)
        return -errno;
    close(probe);
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return -errno;
    rlim_t want = (rlim_t)probe + (rlim_t)headroom;
    if (want > rl.rlim_max)
        want = rl.rlim_max;
    *saved = rl;
    rl.rlim_cur = want;
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
        return -errno;
    return 0;
}

static int hub_loop(struct hub_run *h, struct oc_hub *hub, const struct oc_role_opts *o, int sfd,
                    struct oc_clock *clk)
{
    struct oc_backoff abo;
    oc_backoff_init(&abo, OC_HUB_ROLE_ACCEPT_BACKOFF_INITIAL_MS, OC_HUB_ROLE_ACCEPT_BACKOFF_CAP_MS);
    uint64_t now = 0, backoff_until = 0;
    bool in_backoff = false;
    uint64_t seen_errors = 0, seen_accepted = 0;
    if (clk->now_ns(clk->ctx, &now) != 0)
        return OC_ROLE_EXIT_RUNTIME;
    uint64_t next_stat = now + (uint64_t)o->stat_ms * MS_NS;
    uint64_t next_cap_tick = now;
    uint64_t end_ns = o->lifetime_ms != 0 ? now + o->lifetime_ms * MS_NS : 0;
    bool stop = false;

    while (!stop) {
        if (clk->now_ns(clk->ctx, &now) != 0)
            return OC_ROLE_EXIT_RUNTIME;
        if (end_ns != 0 && now >= end_ns)
            break;
        if (in_backoff && now >= backoff_until)
            in_backoff = false;
        h->iter++;

        uint64_t dl = UINT64_MAX;
        if (o->stat_ms != 0)
            dl = next_stat;
        if (end_ns != 0 && end_ns < dl)
            dl = end_ns;
        if (in_backoff && backoff_until < dl)
            dl = backoff_until;
        if (h->cap != nullptr && !h->cap_reported && next_cap_tick < dl)
            dl = next_cap_tick;             /* oc_store_tick is due even if no sample arrives */
        int tmo = dl == UINT64_MAX ? 1000 : timeout_until(now, dl);

        struct pollfd fds[1 + OC_HUB_MAX_POLLFDS];
        fds[0] = (struct pollfd){.fd = sfd, .events = POLLIN};
        size_t n = oc_hub_poll_setup(hub, fds + 1, OC_HUB_MAX_POLLFDS, &tmo);
        if (n == 0)
            return OC_ROLE_EXIT_RUNTIME;
        if (in_backoff)
            fds[1].fd = -1;              /* listener entry: poll ignores it until the backoff ends */

        int prc = poll(fds, (nfds_t)(n + 1), tmo);
        if (prc < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "openchair: hub: poll failed: %s\n", strerror(errno));
            return OC_ROLE_EXIT_RUNTIME;
        }
        if (fds[0].revents != 0) {
            int sigs[8];
            int ns = oc_signalfd_drain(sfd, sigs, 8);
            if (ns < 0) {
                fprintf(stderr, "openchair: hub: signalfd read failed (%d)\n", ns);
                return OC_ROLE_EXIT_RUNTIME;
            }
            for (int i = 0; i < ns; i++)
                if (sigs[i] == SIGTERM || sigs[i] == SIGINT)
                    stop = true;
        }
        if (h->cap != nullptr && !h->cap_reported) {
            uint64_t t_poll = 0;
            if (clk->now_ns(clk->ctx, &t_poll) == 0)
                oc_capture_clock(h->cap, t_poll);      /* close an expired window BEFORE this dispatch's events/samples */
        }
        int drc = oc_hub_dispatch(hub, fds + 1, n);
        if (drc < 0 && drc != -EINTR) {
            fprintf(stderr, "openchair: hub: dispatch failed (%d)\n", drc);
            return OC_ROLE_EXIT_RUNTIME;
        }

        struct oc_hub_stats st;
        oc_hub_stats(hub, &st);
        if (st.accept_errors != seen_errors) {
            seen_errors = st.accept_errors;
            h->backoffs++;
            uint32_t d = oc_backoff_next(&abo);
            in_backoff = true;
            backoff_until = now + (uint64_t)d * MS_NS;
            (void)oc_runlog_line(&h->log, "EVENT ev=ACCEPT_BACKOFF delay_ms=%u accept_errors=%llu",
                                 d, (unsigned long long)st.accept_errors);
        } else if (st.conns_accepted != seen_accepted) {
            oc_backoff_reset(&abo);
        }
        seen_accepted = st.conns_accepted;

        if (h->cap != nullptr && !h->cap_reported) {
            uint64_t tnow = now;
            if (clk->now_ns(clk->ctx, &tnow) == 0 && tnow >= next_cap_tick) {
                (void)oc_capture_tick(h->cap, tnow);
                next_cap_tick = tnow + (uint64_t)OC_CAPTURE_TICK_MS * MS_NS;
            }
            note_store_failure(h);
            if (oc_capture_state(h->cap) == OC_CAP_ENDED) {
                finish_and_report(h);       /* sync + read-back may take a while; reported as finish_ms */
                if (h->cap_report_rc != 0)
                    return OC_ROLE_EXIT_RUNTIME;
            }
        }
        if (o->stat_ms != 0 && now >= next_stat) {
            emit_periodic(h, hub);
            next_stat = now + (uint64_t)o->stat_ms * MS_NS;
        }
    }
    return OC_ROLE_EXIT_OK;
}

int oc_role_hub_run(const struct oc_role_opts *o)
{
    struct hub_run h = {0};
    h.origin = oc_role_input_origin(o->input);
    h.origin_name = oc_role_origin_name(h.origin);
    if (h.origin == 0) {
        fputs("openchair: hub: no input mode\n", stderr);
        return OC_ROLE_EXIT_CONFIG;
    }
    struct oc_clock clk;
    oc_clock_system(&clk);
    if (oc_runlog_init(&h.log, STDOUT_FILENO) != 0) {
        fputs("openchair: hub: stdout unusable\n", stderr);
        return OC_ROLE_EXIT_RUNTIME;
    }
    char path[108];
    int rc = oc_role_socket_path(o, path, sizeof path);
    if (rc != 0) {
        fprintf(stderr, "openchair: hub: bad runtime dir (%d)\n", rc);
        return OC_ROLE_EXIT_CONFIG;
    }
    int sigs[2] = {SIGTERM, SIGINT};
    int sfd = oc_signalfd_open(sigs, 2);
    if (sfd < 0) {
        fprintf(stderr, "openchair: hub: signalfd failed (%d)\n", sfd);
        return OC_ROLE_EXIT_RUNTIME;
    }

    struct oc_hub_cfg cfg = {.socket_path = path,
                             .session_id = o->session_id,
                             .uid = (uint32_t)geteuid(),
                             .check_pid = true,
                             .hello_timeout_ms = (int)o->hello_timeout_ms,
                             .on_sample = on_sample,
                             .on_event = on_event,
                             .ctx = &h};
    cfg.origin[OC_SOURCE_LIGHT] = h.origin;
    cfg.origin[OC_SOURCE_DISTANCE] = h.origin;
    cfg.initial[OC_SOURCE_LIGHT].authorized = o->have_light_instance;
    cfg.initial[OC_SOURCE_LIGHT].instance = o->light_instance;
    cfg.initial[OC_SOURCE_DISTANCE].authorized = o->have_distance_instance;
    cfg.initial[OC_SOURCE_DISTANCE].instance = o->distance_instance;

    int code = OC_ROLE_EXIT_OK;
    if (o->capture_dir != nullptr) {
        /* Persistent evidence must never share a tree with the runtime (socket) directory. */
        char rc_dir[PATH_MAX], rd_dir[PATH_MAX];
        if (realpath(o->capture_dir, rc_dir) == nullptr || realpath(o->runtime_dir, rd_dir) == nullptr) {
            fprintf(stderr, "openchair: hub: --capture-dir and --runtime-dir must both exist\n");
            close(sfd);
            return OC_ROLE_EXIT_CONFIG;
        }
        if (oc_role_paths_overlap(rc_dir, rd_dir)) {
            fprintf(stderr, "openchair: hub: capture dir %s overlaps runtime dir %s\n", rc_dir, rd_dir);
            close(sfd);
            return OC_ROLE_EXIT_CONFIG;
        }
        const struct oc_capture_cfg ccfg = {.dir = rc_dir,
                                            .duration_ms = o->capture_ms,
                                            .arm_timeout_ms = o->capture_arm_timeout_ms,
                                            .fresh_ms = o->capture_fresh_ms,
                                            .expected_origin = h.origin,
                                            .trace_segment_bytes = o->capture_segment_bytes,
                                            .trace_segments = o->capture_trace_segments};
        struct oc_store_report srep;
        memset(&srep, 0, sizeof srep);
        rc = oc_capture_new(&ccfg, &h.cap, &srep);
        if (h.cap == nullptr) {
            fprintf(stderr, "openchair: hub: capture setup failed (%d)\n", rc);
            close(sfd);
            return rc == -EINVAL || rc == -ENAMETOOLONG ? OC_ROLE_EXIT_CONFIG : OC_ROLE_EXIT_RUNTIME;
        }
        emit_store_recover(&h, rc_dir, &srep, rc);      /* STORE_RECOVER or STORE_OPEN_FAILED */
        if (o->test_fsize_limit != 0 && rc == 0) {
            /* Testing control: real EFBIG from the kernel. SIGXFSZ must not kill the hub. */
            struct sigaction ign = {.sa_handler = SIG_IGN};
            sigemptyset(&ign.sa_mask);
            struct rlimit rl;
            if (sigaction(SIGXFSZ, &ign, nullptr) != 0 || getrlimit(RLIMIT_FSIZE, &rl) != 0) {
                fprintf(stderr, "openchair: hub: cannot prepare the fsize test limit\n");
                code = OC_ROLE_EXIT_RUNTIME;
            } else {
                rl.rlim_cur = (rlim_t)o->test_fsize_limit;
                if (setrlimit(RLIMIT_FSIZE, &rl) != 0)
                    code = OC_ROLE_EXIT_RUNTIME;
            }
        }
    }

    static struct oc_hub hub;            /* large; zero-initialized, one per process */
    rc = oc_hub_init(&hub, &cfg);
    if (rc != 0) {
        fprintf(stderr, "openchair: hub: oc_hub_init failed (%d)\n", rc);
        oc_capture_free(h.cap);
        close(sfd);
        return rc == -EINVAL || rc == -ENAMETOOLONG ? OC_ROLE_EXIT_CONFIG : OC_ROLE_EXIT_RUNTIME;
    }

    struct rlimit saved_nofile;
    bool limit_lowered = false;
    if (o->fd_headroom >= 0) {
        rc = apply_fd_headroom(o->fd_headroom, &saved_nofile);
        limit_lowered = rc == 0;
        if (rc != 0) {
            fprintf(stderr, "openchair: hub: fd headroom failed (%d)\n", rc);
            code = OC_ROLE_EXIT_RUNTIME;
        }
    }
    if (code == OC_ROLE_EXIT_OK) {
        char li[16] = "none", di[16] = "none";
        if (o->have_light_instance)
            snprintf(li, sizeof li, "%u", o->light_instance);
        if (o->have_distance_instance)
            snprintf(di, sizeof di, "%u", o->distance_instance);
        rc = oc_runlog_critical(&h.log, OC_HUB_ROLE_CRITICAL_MS,
                                "READY role=hub origin=%s pid=%d socket=%s "
                                "authz=TEST-CONFIG-NOT-SUPERVISOR session=%llx light_inst=%s "
                                "dist_inst=%s",
                                h.origin_name, (int)getpid(), path, (unsigned long long)o->session_id, li, di);
        if (rc != 0) {
            fprintf(stderr, "openchair: hub: READY not delivered (%d)\n", rc);
            code = OC_ROLE_EXIT_RUNTIME;
        }
    }
    if (code == OC_ROLE_EXIT_OK)
        code = hub_loop(&h, &hub, o, sfd, &clk);

    finish_and_report(&h);                /* no-op unless a capture exists and was not yet reported */
    if (h.cap_report_rc != 0) {
        fprintf(stderr, "openchair: hub: capture report not delivered (%d)\n", h.cap_report_rc);
        if (code == OC_ROLE_EXIT_OK)
            code = OC_ROLE_EXIT_RUNTIME;
    }
    if (limit_lowered)                   /* give back the descriptors needed for orderly exit work */
        (void)setrlimit(RLIMIT_NOFILE, &saved_nofile);

    /* Final snapshot BEFORE shutdown (shutdown marks every source down). */
    for (unsigned s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
        rc = fmt_src(&h, &hub, s, true);
        if (rc != 0) {
            fprintf(stderr, "openchair: hub: final SRC not delivered (%d)\n", rc);
            if (code == OC_ROLE_EXIT_OK)
                code = OC_ROLE_EXIT_RUNTIME;
        }
    }
    fmt_hub_stat(&h, &hub, "FINAL");
    oc_hub_shutdown(&hub);
    rc = oc_runlog_critical(&h.log, OC_HUB_ROLE_CRITICAL_MS,
                            "SUMMARY role=hub origin=%s exit=%d log_dropped=%llu", h.origin_name, code,
                            (unsigned long long)h.log.dropped);
    if (rc != 0) {
        fprintf(stderr, "openchair: hub: SUMMARY not delivered (%d)\n", rc);
        if (code == OC_ROLE_EXIT_OK)
            code = OC_ROLE_EXIT_RUNTIME;
    }
    close(sfd);
    oc_capture_free(h.cap);
    oc_runlog_restore(&h.log);
    return code;
}
