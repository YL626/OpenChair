#define _GNU_SOURCE

#include "oc_role_reader.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "oc_backoff.h"
#include "oc_clock.h"
#include "oc_ipc.h"
#include "oc_runlog.h"
#include "oc_sample.h"
#include "oc_reader_backend.h"

#define MS_NS 1000000ull

struct counters {
    uint64_t attempted, sent, unsent_disc, bp_drop, send_fail;
    uint64_t overruns;
    uint64_t non_ok;                /* sent-or-not attempts whose sample status was not OK */
    uint64_t connect_attempts, connects, disconnects, rejects;
};

struct reader {
    const struct oc_role_opts *o;
    const char *name;
    uint8_t source;
    char path[108];
    struct oc_clock clk;
    struct oc_runlog log;
    int sfd;
    struct oc_ipc_conn conn;
    struct oc_backoff bo;
    struct oc_reader_backend *be;   /* owns the sample sequence and the I2C fd; lives across reconnects */
    bool was_up;                    /* a connection was established since the last backoff reset */
    uint64_t up_since_ns;
    uint64_t next_connect_ns, next_tick_ns, next_stat_ns, end_ns;
    uint64_t period_ns;
    bool stop;
    bool ready_sent;
    bool fatal;                     /* a critical report failed, or a permanent reject */
    int exit_code;
    struct counters c;
};

static const char *const REJECT_NAMES[] = {"NONE", "MALFORMED", "BAD_SOURCE", "BAD_ORIGIN",
                                           "SESSION_MISMATCH", "PEER_CRED",
                                           "DUPLICATE_SOURCE", "STALE_INSTANCE", "NOT_READY"};

static uint64_t now_or_zero(struct reader *r)
{
    uint64_t t = 0;
    if (r->clk.now_ns(r->clk.ctx, &t) != 0) {
        /* CLOCK_MONOTONIC cannot realistically fail; if it does, nothing here
         * can be timestamped honestly. */
        r->fatal = true;
        r->stop = true;
        r->exit_code = OC_ROLE_EXIT_RUNTIME;
        fputs("openchair: reader: CLOCK_MONOTONIC unavailable\n", stderr);
        return 0;
    }
    return t;
}

static void fail_critical(struct reader *r, const char *what, int rc)
{
    fprintf(stderr, "openchair: %s: critical report not delivered (%d)\n", what, rc);
    r->fatal = true;
    r->stop = true;
    if (r->exit_code == OC_ROLE_EXIT_OK)
        r->exit_code = OC_ROLE_EXIT_RUNTIME;
}

/* The counters line must fit OC_RUNLOG_MAX together with its prefix/suffix; a line that does not
 * fit fails (-EMSGSIZE) rather than being truncated, which for SUMMARY means exit 4. */
static void format_counters(const struct reader *r, bool up, char *buf, size_t cap)
{
    snprintf(buf, cap,
             "role=%s origin=%s inst=%u up=%d attempted=%llu sent=%llu "
             "unsent_disc=%llu bp_drop=%llu send_fail=%llu overruns=%llu "
             "connect_attempts=%llu connects=%llu disconnects=%llu rejects=%llu "
             "non_ok=%llu next_seq=%u log_dropped=%llu",
             r->name, oc_role_origin_name(r->be->origin), r->o->instance, up ? 1 : 0,
             (unsigned long long)r->c.attempted,
             (unsigned long long)r->c.sent, (unsigned long long)r->c.unsent_disc,
             (unsigned long long)r->c.bp_drop, (unsigned long long)r->c.send_fail,
             (unsigned long long)r->c.overruns, (unsigned long long)r->c.connect_attempts,
             (unsigned long long)r->c.connects, (unsigned long long)r->c.disconnects,
             (unsigned long long)r->c.rejects, (unsigned long long)r->c.non_ok,
             oc_reader_backend_next_seq(r->be), (unsigned long long)r->log.dropped);
}

static void emit_stat(struct reader *r)
{
    char buf[440];
    format_counters(r, r->conn.open, buf, sizeof buf);
    (void)oc_runlog_line(&r->log, "STAT %s", buf);
}

static void emit_event(struct reader *r, const char *ev, const char *extra)
{
    (void)oc_runlog_line(&r->log, "EVENT role=%s origin=%s inst=%u ev=%s%s%s", r->name,
                         oc_role_origin_name(r->be->origin), r->o->instance, ev, extra ? " " : "",
                         extra ? extra : "");
}

/* A failed attempt or lost connection: pick the next attempt time. */
static void schedule_reconnect(struct reader *r, uint64_t now)
{
    if (r->was_up && now - r->up_since_ns >= OC_READER_STABLE_NS)
        oc_backoff_reset(&r->bo);
    r->was_up = false;
    r->next_connect_ns = now + (uint64_t)oc_backoff_next(&r->bo) * MS_NS;
}

static void connection_lost(struct reader *r, uint64_t now, const char *why)
{
    if (r->conn.open)
        oc_ipc_conn_close(&r->conn);
    r->c.disconnects++;
    emit_event(r, "disconnected", why);
    schedule_reconnect(r, now);
}

static bool permanent_reject(uint16_t reason)
{
    return reason == OC_REJECT_MALFORMED || reason == OC_REJECT_BAD_SOURCE ||
           reason == OC_REJECT_BAD_ORIGIN || reason == OC_REJECT_SESSION_MISMATCH ||
           reason == OC_REJECT_PEER_CRED;
}

static void try_connect(struct reader *r)
{
    r->c.connect_attempts++;
    oc_ipc_conn_init(&r->conn);
    int rc = oc_ipc_client_connect(&r->conn, r->path);
    struct oc_hello_reject rej = {0};
    if (rc == 0 || rc == -EINPROGRESS) {
        const struct oc_hello hello = {.source = r->source,
                                       .origin = r->be->origin,
                                       .session_id = r->o->session_id,
                                       .instance = r->o->instance,
                                       .pid = (uint32_t)getpid()};
        rc = oc_ipc_client_handshake(&r->conn, &hello, OC_READER_HANDSHAKE_MS, &rej);
    }
    uint64_t now = now_or_zero(r);
    if (r->stop)
        return;
    if (rc == 0) {
        r->c.connects++;
        r->was_up = true;
        r->up_since_ns = now;
        emit_event(r, "connected", nullptr);
        if (!r->ready_sent) {
            r->ready_sent = true;
            int lrc = oc_runlog_critical(&r->log, OC_READER_CRITICAL_MS,
                                         "READY role=%s origin=%s pid=%d inst=%u "
                                         "session=%llx%s%s",
                                         r->name, oc_role_origin_name(r->be->origin), (int)getpid(),
                                         r->o->instance, (unsigned long long)r->o->session_id,
                                         r->o->input == OC_INPUT_LIVE ? " i2c_dev=" : "",
                                         r->o->input == OC_INPUT_LIVE ? r->o->i2c_dev : "");
            if (lrc != 0)
                fail_critical(r, "READY", lrc);
        }
        return;
    }
    if (rc == -EACCES) {
        r->c.rejects++;
        char extra[64];
        const char *nm = rej.reason < sizeof REJECT_NAMES / sizeof REJECT_NAMES[0]
                             ? REJECT_NAMES[rej.reason] : "?";
        snprintf(extra, sizeof extra, "reject=%s", nm);
        emit_event(r, "rejected", extra);
        if (permanent_reject(rej.reason)) {
            fprintf(stderr, "openchair: reader %s: HELLO rejected permanently (%s)\n", r->name, nm);
            r->fatal = true;
            r->stop = true;
            r->exit_code = OC_ROLE_EXIT_CONFIG;
            return;
        }
    } else if (rc == -EPERM) {
        fprintf(stderr, "openchair: reader %s: hub is not our uid\n", r->name);
        r->fatal = true;
        r->stop = true;
        r->exit_code = OC_ROLE_EXIT_CONFIG;
        return;
    }
    schedule_reconnect(r, now);       /* connect/handshake failure: conn is closed */
}

static void do_tick(struct reader *r, uint64_t now)
{
    /* Ticks that were due but could not be attempted (late loop) are skipped,
     * not faked: they consume no seq and are counted as overruns. */
    uint64_t late = now - r->next_tick_ns;
    if (late >= r->period_ns) {
        uint64_t missed = late / r->period_ns;
        r->c.overruns += missed;
        r->next_tick_ns += missed * r->period_ns;
    }
    r->next_tick_ns += r->period_ns;

    /* The backend owns seq and the timestamp (taken after the attempt). A failed sensor attempt
     * comes back as a non-OK sample and is sent like any other; only a failed clock (or an
     * internal error) gives no sample at all, and that ends the process. A LIVE backend never
     * falls back to simulation. */
    struct oc_sample s;
    int io_errno = 0;
    int arc = oc_reader_backend_acquire(r->be, &s, &io_errno);
    if (arc != 0) {
        fprintf(stderr, "openchair: reader %s: acquisition cannot continue (%d)\n", r->name, arc);
        r->fatal = true;
        r->stop = true;
        r->exit_code = OC_ROLE_EXIT_RUNTIME;
        return;
    }
    r->c.attempted++;
    if (s.status != OC_ST_OK)
        r->c.non_ok++;

    if (!r->conn.open) {
        r->c.unsent_disc++;
        return;
    }
    int rc = oc_ipc_send_sample(&r->conn, &s);
    if (rc == 0) {
        r->c.sent++;
    } else if (rc == -EAGAIN) {
        r->c.bp_drop++;
    } else {
        r->c.send_fail++;
        if (rc == -EPIPE || rc == -ECONNRESET || rc == -ENOTCONN || rc == -EIO)
            connection_lost(r, s.t_mono_ns, "send_failed");
    }
}

/* Drains what the hub sent (BYE, EOF). Bounded per wake-up. */
static void service_conn(struct reader *r, uint64_t now)
{
    for (int i = 0; i < 16 && r->conn.open; i++) {
        struct oc_ipc_msg m;
        int rc = oc_ipc_recv(&r->conn, &m);
        if (rc == -EAGAIN)
            return;
        if (rc == 0) {
            if (m.type == OC_MSG_BYE)
                connection_lost(r, now, "hub_bye");
            continue;                       /* anything else from the hub is ignored */
        }
        if (rc == -EMSGSIZE || rc == -EBADMSG)
            continue;                       /* that datagram was consumed; the stream is intact */
        /* EOF, reset, unsupported version, or any other error: the connection is unusable. Closing
         * it (instead of looping on a persistent POLLERR/POLLHUP) also keeps this from spinning. */
        connection_lost(r, now, rc == -EPIPE ? "hub_eof" : "hub_error");
        return;
    }
}

static int timeout_until(uint64_t now, uint64_t deadline)
{
    if (deadline <= now)
        return 0;
    uint64_t ms = (deadline - now + MS_NS - 1) / MS_NS;
    return ms > 1000 ? 1000 : (int)ms;
}

static void handle_signals(struct reader *r, uint64_t now)
{
    int sigs[8];
    int n = oc_signalfd_drain(r->sfd, sigs, 8);
    if (n < 0) {
        fprintf(stderr, "openchair: reader: signalfd read failed (%d)\n", n);
        r->fatal = true;
        r->stop = true;
        r->exit_code = OC_ROLE_EXIT_RUNTIME;
        return;
    }
    for (int i = 0; i < n; i++) {
        if (sigs[i] == SIGTERM || sigs[i] == SIGINT) {
            r->stop = true;
        } else if (sigs[i] == SIGUSR1 && r->o->drop_conn_on_sigusr1 && r->conn.open) {
            /* Testing control: drop the socket and reconnect in THIS process;
             * instance and seq are untouched. No BYE: a plain transport loss. */
            connection_lost(r, now, "forced_by_test");
        }
    }
}

static void wait_writable(const struct oc_ipc_conn *c, int ms)
{
    struct pollfd pf = {.fd = c->fd, .events = POLLOUT};
    (void)poll(&pf, 1, ms);
}

static void run_loop(struct reader *r)
{
    while (!r->stop) {
        uint64_t now = now_or_zero(r);
        if (r->stop || (r->end_ns != 0 && now >= r->end_ns))
            break;
        if (!r->conn.open && now >= r->next_connect_ns) {
            try_connect(r);
            if (r->stop)
                break;
            now = now_or_zero(r);
            if (r->stop)
                break;
        }
        if (now >= r->next_tick_ns) {
            do_tick(r, now);
            if (r->stop)
                break;
        }
        if (r->o->stat_ms != 0 && now >= r->next_stat_ns) {
            emit_stat(r);
            r->next_stat_ns = now + (uint64_t)r->o->stat_ms * MS_NS;
        }

        uint64_t dl = r->next_tick_ns;
        if (!r->conn.open && r->next_connect_ns < dl)
            dl = r->next_connect_ns;
        if (r->o->stat_ms != 0 && r->next_stat_ns < dl)
            dl = r->next_stat_ns;
        if (r->end_ns != 0 && r->end_ns < dl)
            dl = r->end_ns;

        struct pollfd pf[2];
        nfds_t n = 0;
        pf[n++] = (struct pollfd){.fd = r->sfd, .events = POLLIN};
        if (r->conn.open)
            pf[n++] = (struct pollfd){.fd = r->conn.fd, .events = POLLIN};
        int prc = poll(pf, n, timeout_until(now, dl));
        if (prc < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "openchair: reader: poll failed: %s\n", strerror(errno));
            r->fatal = true;
            r->exit_code = OC_ROLE_EXIT_RUNTIME;
            return;
        }
        if (prc == 0)
            continue;
        now = now_or_zero(r);
        if (r->stop)
            break;
        if (pf[0].revents != 0)
            handle_signals(r, now);
        if (!r->stop && n == 2 && pf[1].revents != 0 && r->conn.open)
            service_conn(r, now);
    }
}

static int finish(struct reader *r)
{
    if (r->conn.open) {
        /* Orderly goodbye: BYE goes after every sample already queued, so the
         * hub reads all of them first. EAGAIN is retried for a bounded time;
         * if it never fits, a plain close (EOF) is just as safe: the hub reads
         * everything queued before it reports the source down. */
        const struct oc_bye bye = {.reason = OC_BYE_NORMAL};
        uint8_t buf[OC_MSG_BYE_SIZE];
        size_t len = 0;
        if (oc_ipc_encode_bye(&bye, buf, sizeof buf, &len) == 0) {
            for (int i = 0; i < 5; i++) {
                int rc = oc_ipc_send_msg(&r->conn, buf, len);
                if (rc != -EAGAIN)
                    break;
                wait_writable(&r->conn, OC_READER_BYE_WAIT_MS / 5);
            }
        }
        oc_ipc_conn_close(&r->conn);
    }
    char buf[440];
    format_counters(r, false, buf, sizeof buf);
    int rc = oc_runlog_critical(&r->log, OC_READER_CRITICAL_MS, "SUMMARY %s exit=%d", buf,
                                r->exit_code);
    if (rc != 0) {
        fprintf(stderr, "openchair: reader %s: SUMMARY not delivered (%d)\n", r->name, rc);
        if (r->exit_code == OC_ROLE_EXIT_OK)
            r->exit_code = OC_ROLE_EXIT_RUNTIME;
    }
    if (r->sfd >= 0)
        close(r->sfd);
    oc_reader_backend_close(r->be);           /* idempotent; releases the I2C descriptor */
    oc_runlog_restore(&r->log);
    return r->exit_code;
}

static int reader_run(const struct oc_role_opts *o, struct oc_reader_backend *injected)
{
    struct oc_reader_backend own = {.i2c = {.fd = -1}};
    struct reader r = {.o = o, .sfd = -1, .be = injected != nullptr ? injected : &own};
    r.name = o->role == OC_ROLE_LIGHT ? "light" : "distance";
    r.source = o->role == OC_ROLE_LIGHT ? OC_SOURCE_LIGHT : OC_SOURCE_DISTANCE;
    r.period_ns = (uint64_t)o->period_ms * MS_NS;
    oc_ipc_conn_init(&r.conn);
    oc_backoff_init(&r.bo, OC_READER_BACKOFF_INITIAL_MS, OC_READER_BACKOFF_CAP_MS);
    oc_clock_system(&r.clk);

    int rc = oc_role_socket_path(o, r.path, sizeof r.path);
    if (rc != 0) {
        fprintf(stderr, "openchair: reader %s: bad runtime dir (%d)\n", r.name, rc);
        return OC_ROLE_EXIT_CONFIG;
    }
    rc = oc_runlog_init(&r.log, STDOUT_FILENO);
    if (rc != 0) {
        fprintf(stderr, "openchair: reader %s: stdout unusable (%d)\n", r.name, rc);
        return OC_ROLE_EXIT_RUNTIME;
    }
    int sigs[3] = {SIGTERM, SIGINT, SIGUSR1};
    r.sfd = oc_signalfd_open(sigs, o->drop_conn_on_sigusr1 ? 3 : 2);
    if (r.sfd < 0) {
        fprintf(stderr, "openchair: reader %s: signalfd failed (%d)\n", r.name, r.sfd);
        r.sfd = -1;
        return OC_ROLE_EXIT_RUNTIME;
    }

    uint64_t now = now_or_zero(&r);
    if (r.stop) {
        close(r.sfd);
        return OC_ROLE_EXIT_RUNTIME;
    }
    /* Opened once; a bad input configuration (e.g. no such I2C device) is a configuration error
     * and the process never connects or sends anything. Closed by finish(). */
    rc = injected != nullptr ? 0 : oc_reader_backend_open(r.be, o, &r.clk);
    if (rc != 0) {
        fprintf(stderr, "openchair: reader %s: cannot open the %s input%s%s: %s\n", r.name,
                o->input == OC_INPUT_LIVE ? "LIVE" : "SIMULATED",
                o->input == OC_INPUT_LIVE ? " " : "", o->input == OC_INPUT_LIVE ? o->i2c_dev : "",
                strerror(-rc));
        close(r.sfd);
        oc_runlog_restore(&r.log);
        return OC_ROLE_EXIT_CONFIG;
    }
    r.next_connect_ns = now;
    r.next_tick_ns = now + r.period_ns;
    r.next_stat_ns = now + (uint64_t)o->stat_ms * MS_NS;
    r.end_ns = o->lifetime_ms != 0 ? now + o->lifetime_ms * MS_NS : 0;

    run_loop(&r);
    if (r.fatal && r.exit_code == OC_ROLE_EXIT_OK)
        r.exit_code = OC_ROLE_EXIT_RUNTIME;
    return finish(&r);
}

int oc_role_reader_run(const struct oc_role_opts *o)
{
    return reader_run(o, nullptr);
}

int oc_role_reader_run_backend(const struct oc_role_opts *o, struct oc_reader_backend *be)
{
    if (o == nullptr || be == nullptr || be->kind == OC_READER_CLOSED)
        return OC_ROLE_EXIT_CONFIG;
    int rc = reader_run(o, be);
    oc_reader_backend_close(be);              /* also covers the early returns before the loop */
    return rc;
}
