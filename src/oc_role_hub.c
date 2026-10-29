#define _GNU_SOURCE

#include "oc_role_hub.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#include "oc_backoff.h"
#include "oc_clock.h"
#include "oc_hub.h"
#include "oc_runlog.h"

#define MS_NS 1000000ull

/* Per-source callback-side checks. Independent of the hub's own tracker: they
 * count what the sample callback actually saw. */
struct src_acc {
    uint64_t cb_samples;        /* on_sample calls */
    uint64_t check_fail;        /* origin / timestamp sanity failures */
    uint64_t origin_bad;        /* origin != SIMULATED */
};

struct hub_run {
    struct oc_runlog log;
    struct src_acc acc[OC_SOURCE_DISTANCE + 1];
    uint64_t backoffs;          /* accept-error backoffs entered */
    uint64_t iter;              /* main-loop iterations */
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
    if (i->source > OC_SOURCE_DISTANCE || i->source == 0)
        return;
    struct src_acc *a = &h->acc[i->source];
    a->cb_samples++;
    bool bad = false;
    if (i->origin != OC_ORIGIN_SIMULATED || i->sample->origin != OC_ORIGIN_SIMULATED) {
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
        "%s role=hub origin=SIMULATED degraded=%d iter=%llu accept_errors=%llu backoffs=%llu "
        "conns_accepted=%llu shed=%llu hello_timeouts=%llu datagrams=%llu malformed=%llu "
        "abusive_closes=%llu log_dropped=%llu",
        tag, oc_hub_degraded(hub), (unsigned long long)h->iter, (unsigned long long)s.accept_errors,
        (unsigned long long)h->backoffs, (unsigned long long)s.conns_accepted,
        (unsigned long long)s.shed, (unsigned long long)s.hello_timeouts,
        (unsigned long long)s.datagrams, (unsigned long long)s.malformed,
        (unsigned long long)s.abusive_closes, (unsigned long long)h->log.dropped);
}

static void emit_periodic(struct hub_run *h, const struct oc_hub *hub)
{
    fmt_hub_stat(h, hub, "STAT");
    (void)fmt_src(h, hub, OC_SOURCE_LIGHT, false);
    (void)fmt_src(h, hub, OC_SOURCE_DISTANCE, false);
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
    cfg.origin[OC_SOURCE_LIGHT] = OC_ORIGIN_SIMULATED;
    cfg.origin[OC_SOURCE_DISTANCE] = OC_ORIGIN_SIMULATED;
    cfg.initial[OC_SOURCE_LIGHT].authorized = o->have_light_instance;
    cfg.initial[OC_SOURCE_LIGHT].instance = o->light_instance;
    cfg.initial[OC_SOURCE_DISTANCE].authorized = o->have_distance_instance;
    cfg.initial[OC_SOURCE_DISTANCE].instance = o->distance_instance;

    static struct oc_hub hub;            /* large; zero-initialized, one per process */
    rc = oc_hub_init(&hub, &cfg);
    if (rc != 0) {
        fprintf(stderr, "openchair: hub: oc_hub_init failed (%d)\n", rc);
        close(sfd);
        return rc == -EINVAL || rc == -ENAMETOOLONG ? OC_ROLE_EXIT_CONFIG : OC_ROLE_EXIT_RUNTIME;
    }

    int code = OC_ROLE_EXIT_OK;
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
                                "READY role=hub origin=SIMULATED pid=%d socket=%s "
                                "authz=TEST-CONFIG-NOT-SUPERVISOR session=%llx light_inst=%s "
                                "dist_inst=%s",
                                (int)getpid(), path, (unsigned long long)o->session_id, li, di);
        if (rc != 0) {
            fprintf(stderr, "openchair: hub: READY not delivered (%d)\n", rc);
            code = OC_ROLE_EXIT_RUNTIME;
        }
    }
    if (code == OC_ROLE_EXIT_OK)
        code = hub_loop(&h, &hub, o, sfd, &clk);

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
                            "SUMMARY role=hub origin=SIMULATED exit=%d log_dropped=%llu", code,
                            (unsigned long long)h.log.dropped);
    if (rc != 0) {
        fprintf(stderr, "openchair: hub: SUMMARY not delivered (%d)\n", rc);
        if (code == OC_ROLE_EXIT_OK)
            code = OC_ROLE_EXIT_RUNTIME;
    }
    close(sfd);
    oc_runlog_restore(&h.log);
    return code;
}
