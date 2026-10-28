/* Integration tests for the hub receiver (include/oc_hub.h) over REAL Linux
 * AF_UNIX SOCK_SEQPACKET sockets.
 *
 * Every measurement is SYNTHETIC and labelled SIMULATED (or REPLAYED in the
 * mixed-origin test): no hardware, no I2C, no storage. Passing these tests
 * shows hub logic on this Linux kernel only.
 *
 * Mostly single-threaded and deterministic: the test owns the hub and the
 * clients (S3A connections) and calls oc_hub_step itself. A client cannot use
 * the blocking oc_ipc_client_handshake there (nobody would step the hub), so
 * it sends HELLO by hand. Two tests fork a real client process.
 *
 * test_hub is linked with -Wl,--wrap=send so that one ACK send can be made to
 * fail with EAGAIN. */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "oc_hub.h"

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        long long a_ = (long long)(a), b_ = (long long)(b);                   \
        if (a_ != b_) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s == %lld, want %s == %lld\n",      \
                    __FILE__, __LINE__, #a, a_, #b, b_);                      \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define RUN(test)                          \
    do {                                   \
        fprintf(stderr, "== %s\n", #test); \
        test();                            \
    } while (0)

#define SESSION 0xA11CE5E551014ull
#define SIM OC_ORIGIN_SIMULATED
#define LIGHT OC_SOURCE_LIGHT
#define DIST OC_SOURCE_DISTANCE

/* ---------- fault seam: make one HELLO_ACK send fail ---------- */

static int fail_ack_once;
ssize_t __real_send(int fd, const void *b, size_t n, int flags);
ssize_t __wrap_send(int fd, const void *b, size_t n, int flags)
{
    if (fail_ack_once && n >= 2 && ((const uint8_t *)b)[1] == OC_MSG_HELLO_ACK) {
        fail_ack_once = 0;
        errno = EAGAIN;
        return -1;
    }
    return __real_send(fd, b, n, flags);
}

/* ---------- sink: everything the hub tells its consumer, in order ---------- */

enum log_kind { L_SAMPLE = 1, L_EVENT };
struct log_entry {
    enum log_kind k;
    struct oc_hub_sample_info si;
    struct oc_sample sample;
    struct oc_hub_event ev;
};

#define LOG_CAP 8192
static struct log_entry sink_log[LOG_CAP];
static struct {
    size_t n, overflow;
    size_t samples[3];
    struct oc_hub *hub;       /* for the re-entrancy probe */
    bool probe;
    bool probe_event;
    int probe_rc[4];
    int event_probe_rc[3];
    int event_probes;
    int probes;
} sink;

static void sink_reset(void)
{
    memset(&sink, 0, sizeof sink);
}

static struct log_entry *sink_next(void)
{
    if (sink.n >= LOG_CAP) {
        sink.overflow++;
        return nullptr;
    }
    return &sink_log[sink.n++];
}

static void on_sample(void *ctx, const struct oc_hub_sample_info *si)
{
    (void)ctx;
    struct log_entry *e = sink_next();
    if (si->source <= DIST)
        sink.samples[si->source]++;
    if (e != nullptr) {
        e->k = L_SAMPLE;
        e->si = *si;
        e->sample = *si->sample;
        e->si.sample = &e->sample;
    }
    if (sink.probe && sink.hub != nullptr) {     /* calls from inside a callback */
        sink.probe_rc[0] = oc_hub_authorize(sink.hub, LIGHT, 99);
        sink.probe_rc[1] = oc_hub_step(sink.hub, 0);
        sink.probe_rc[2] = oc_hub_dispatch(sink.hub, nullptr, 0);
        oc_hub_shutdown(sink.hub);               /* must be refused, not run */
        sink.probes++;
        sink.probe = false;
    }
}

static void on_event(void *ctx, const struct oc_hub_event *ev)
{
    (void)ctx;
    if (sink.probe_event && sink.hub != nullptr) {
        sink.probe_event = false;                    /* one probe; no recursion */
        sink.event_probe_rc[0] = oc_hub_authorize(sink.hub, LIGHT, 77);
        sink.event_probe_rc[1] = oc_hub_step(sink.hub, 0);
        sink.event_probe_rc[2] = oc_hub_dispatch(sink.hub, nullptr, 0);
        oc_hub_shutdown(sink.hub);                   /* refused: must not run */
        sink.event_probes++;
    }
    struct log_entry *e = sink_next();
    if (e != nullptr) {
        e->k = L_EVENT;
        e->ev = *ev;
    }
}

static size_t count_ev(enum oc_hub_event_kind k, uint8_t source)
{
    size_t n = 0;
    for (size_t i = 0; i < sink.n; i++)
        if (sink_log[i].k == L_EVENT && sink_log[i].ev.kind == k &&
            (source == 0 || sink_log[i].ev.source == source))
            n++;
    return n;
}

/* nth (0-based) matching event, or nullptr */
static const struct oc_hub_event *find_ev(enum oc_hub_event_kind k, uint8_t source, size_t nth)
{
    for (size_t i = 0; i < sink.n; i++)
        if (sink_log[i].k == L_EVENT && sink_log[i].ev.kind == k &&
            (source == 0 || sink_log[i].ev.source == source) && nth-- == 0)
            return &sink_log[i].ev;
    return nullptr;
}

/* find_ev, but a missing event reads as all-zero so a failing run reports
 * instead of crashing. */
static const struct oc_hub_event *ev_or0(enum oc_hub_event_kind k, uint8_t source, size_t nth)
{
    static const struct oc_hub_event none;
    const struct oc_hub_event *e = find_ev(k, source, nth);
    return e != nullptr ? e : &none;
}

static const struct log_entry *nth_sample(uint8_t source, size_t nth)
{
    for (size_t i = 0; i < sink.n; i++)
        if (sink_log[i].k == L_SAMPLE && sink_log[i].si.source == source && nth-- == 0)
            return &sink_log[i];
    return nullptr;
}

static ptrdiff_t last_sample_index(uint8_t source)
{
    ptrdiff_t r = -1;
    for (size_t i = 0; i < sink.n; i++)
        if (sink_log[i].k == L_SAMPLE && sink_log[i].si.source == source)
            r = (ptrdiff_t)i;
    return r;
}

static ptrdiff_t first_event_index(enum oc_hub_event_kind k, uint8_t source)
{
    for (size_t i = 0; i < sink.n; i++)
        if (sink_log[i].k == L_EVENT && sink_log[i].ev.kind == k &&
            (source == 0 || sink_log[i].ev.source == source))
            return (ptrdiff_t)i;
    return -1;
}

/* ---------- helpers ---------- */

static int fd_count(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (d == nullptr)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n - 1;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb; (void)flag; (void)ftw;
    (void)remove(path);
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void ms_sleep(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

struct fx {
    char dir[PATH_MAX];
    char path[PATH_MAX + 16];
    struct oc_hub hub;
    struct oc_hub_cfg cfg;
    bool dir_made, up;
    int fds_before;
};

/* Defaults: both sources authorized at instance 1, both SIMULATED, small
 * budget so fairness is visible, short HELLO timeout. */
static void fx_begin(struct fx *f)
{
    memset(f, 0, sizeof *f);
    sink_reset();
    f->fds_before = fd_count();
    const char *base = getenv("TMPDIR");
    snprintf(f->dir, sizeof f->dir, "%s/oc_hub_test.XXXXXX",
             base != nullptr && base[0] != '\0' ? base : "/tmp");
    f->dir_made = mkdtemp(f->dir) != nullptr;
    CHECK(f->dir_made);
    snprintf(f->path, sizeof f->path, "%s/hub.sock", f->dir);
    f->cfg = (struct oc_hub_cfg){
        .socket_path = f->path,
        .session_id = SESSION,
        .uid = (uint32_t)geteuid(),
        .check_pid = true,
        .origin = {[LIGHT] = SIM, [DIST] = SIM},
        .initial = {[LIGHT] = {true, 1}, [DIST] = {true, 1}},
        .hello_timeout_ms = 300,
        .max_pending = 4,
        .batch = 16,
        .on_sample = on_sample,
        .on_event = on_event,
    };
}

static bool fx_start(struct fx *f)
{
    int rc = oc_hub_init(&f->hub, &f->cfg);
    CHECK_EQ(rc, 0);
    f->up = rc == 0;
    sink.hub = &f->hub;
    return f->up;
}

static void fx_end(struct fx *f)
{
    if (f->up)
        oc_hub_shutdown(&f->hub);
    if (f->dir_made)
        (void)nftw(f->dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static int pump(struct fx *f, int steps)
{
    int total = 0;
    for (int i = 0; i < steps; i++) {
        int r = oc_hub_step(&f->hub, 0);
        CHECK(r >= 0);
        if (r > 0)
            total += r;
    }
    return total;
}

static bool cl_connect(struct fx *f, struct oc_ipc_conn *c)
{
    oc_ipc_conn_init(c);
    int rc = oc_ipc_client_connect(c, f->path);
    CHECK_EQ(rc, 0);
    return rc == 0;
}

static struct oc_hello hello_for(uint8_t source, uint32_t instance)
{
    return (struct oc_hello){.source = source, .origin = SIM, .session_id = SESSION,
                             .instance = instance, .pid = (uint32_t)getpid()};
}

static void send_hello(struct oc_ipc_conn *c, const struct oc_hello *h)
{
    uint8_t b[OC_IPC_MAX_MSG];
    size_t n;
    CHECK_EQ(oc_ipc_encode_hello(h, b, sizeof b, &n), 0);
    CHECK_EQ(oc_ipc_send_msg(c, b, n), 0);
}

/* connect-less: sends HELLO, lets the hub run, and returns the first reply type
 * (0 if none yet). *reject_reason is filled for a REJECT. */
static int hello_exchange(struct fx *f, struct oc_ipc_conn *c, const struct oc_hello *h,
                          uint16_t *reject_reason)
{
    send_hello(c, h);
    pump(f, 4);
    struct oc_ipc_msg m;
    if (oc_ipc_recv(c, &m) != 0)
        return 0;
    if (m.type == OC_MSG_HELLO_REJECT && reject_reason != nullptr)
        *reject_reason = m.u.reject.reason;
    if (m.type == OC_MSG_HELLO_ACK) {
        struct oc_hello_ack want = {h->session_id, h->instance, h->source};
        CHECK(m.u.ack.session_id == want.session_id && m.u.ack.instance == want.instance &&
              m.u.ack.source == want.source);
    }
    return m.type;
}

/* Connects, says HELLO, requires an ACK. */
static bool join_ok(struct fx *f, struct oc_ipc_conn *c, uint8_t source, uint32_t instance)
{
    if (!cl_connect(f, c))
        return false;
    struct oc_hello h = hello_for(source, instance);
    int t = hello_exchange(f, c, &h, nullptr);
    CHECK_EQ(t, OC_MSG_HELLO_ACK);
    return t == OC_MSG_HELLO_ACK;
}

struct tail {
    int first_type;               /* first message after the point of call, 0 if none */
    uint16_t bye_reason;
    bool eof;
};

static struct tail read_tail(struct oc_ipc_conn *c)
{
    struct tail t = {0};
    for (int i = 0; i < 1000; i++) {
        struct oc_ipc_msg m;
        int rc = oc_ipc_recv(c, &m);
        if (rc == 0) {
            if (t.first_type == 0)
                t.first_type = m.type;
            if (m.type == OC_MSG_BYE)
                t.bye_reason = m.u.bye.reason;
            continue;
        }
        if (rc == -EPIPE || rc == -ECONNRESET)
            t.eof = true;
        break;
    }
    return t;
}

static struct oc_sample mk_sample(uint8_t source, uint32_t seq, uint64_t t, uint8_t status)
{
    return (struct oc_sample){.t_mono_ns = t, .seq = seq, .source = source, .origin = SIM,
                              .status = status,
                              .value = status == OC_ST_OK ? (int32_t)(seq % 1000) : 0,
                              .aux = 0x4383};
}

/* Sends n samples seq = first..first+n-1 (t = 1000 + 10*seq), pumping the hub
 * whenever the kernel queue is full and retrying that same sample, so every
 * sample is eventually transmitted exactly once. */
static void send_n(struct fx *f, struct oc_ipc_conn *c, uint8_t source, uint32_t first,
                   uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        struct oc_sample s = mk_sample(source, first + i, 1000u + 10u * (first + i), OC_ST_OK);
        int rc;
        int tries = 0;
        while ((rc = oc_ipc_send_sample(c, &s)) == -EAGAIN && tries++ < 100)
            pump(f, 1);
        CHECK_EQ(rc, 0);
    }
}

static void raw(struct oc_ipc_conn *c, const void *b, size_t n)
{
    CHECK_EQ(send(c->fd, b, n, MSG_DONTWAIT | MSG_NOSIGNAL), (ssize_t)n);
}

static void send_bad(struct oc_ipc_conn *c, int kind)
{
    uint8_t big[200];
    memset(big, 0x5A, sizeof big);
    big[0] = OC_PROTO_VERSION;
    big[1] = OC_MSG_SAMPLE;
    switch (kind % 3) {
    case 0: raw(c, big, sizeof big); break;                      /* oversized */
    case 1: raw(c, big, 3); break;                               /* truncated */
    default: raw(c, "\x01\x63\x00\x00", 4); break;               /* unknown type */
    }
}

static struct oc_hub_source_status status_of(struct fx *f, uint8_t source)
{
    struct oc_hub_source_status st;
    memset(&st, 0, sizeof st);
    CHECK_EQ(oc_hub_source_status(&f->hub, source, &st), 0);
    return st;
}

static struct oc_hub_stats stats_of(struct fx *f)
{
    struct oc_hub_stats st;
    oc_hub_stats(&f->hub, &st);
    return st;
}

/* ============================ tests ============================ */

static void test_hello_both_sources(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    CHECK(oc_hub_degraded(&f.hub));                      /* nobody has connected yet */
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_AWAITING);

    struct oc_ipc_conn cl, cd;
    CHECK(join_ok(&f, &cl, LIGHT, 1));
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_ACTIVE);
    CHECK(oc_hub_degraded(&f.hub));                      /* DISTANCE still missing */
    CHECK(join_ok(&f, &cd, DIST, 1));
    CHECK(!oc_hub_degraded(&f.hub));
    CHECK(status_of(&f, LIGHT).available && status_of(&f, DIST).available);

    for (uint32_t i = 0; i < 10; i++) {
        struct oc_sample s = mk_sample(LIGHT, i, 1000 + 10 * i, OC_ST_OK);
        CHECK_EQ(oc_ipc_send_sample(&cl, &s), 0);
    }
    for (uint32_t i = 0; i < 5; i++) {
        struct oc_sample s = mk_sample(DIST, i, 2000 + 10 * i, OC_ST_OK);
        CHECK_EQ(oc_ipc_send_sample(&cd, &s), 0);
    }
    pump(&f, 4);
    CHECK_EQ(sink.samples[LIGHT], 10);
    CHECK_EQ(sink.samples[DIST], 5);

    const struct log_entry *e = nth_sample(LIGHT, 0);
    CHECK(e != nullptr);
    if (e != nullptr) {
        CHECK(e->si.first_of_instance && e->si.first_on_connection && !e->si.ts_backwards);
        CHECK_EQ(e->si.gap, 0);
        CHECK_EQ(e->si.session_id, SESSION);
        CHECK_EQ(e->si.instance, 1);
        CHECK_EQ(e->si.generation, 1);
        CHECK(e->si.source == LIGHT && e->si.origin == SIM && e->sample.origin == SIM);
        CHECK(e->si.hub_rx_ns != 0);
        CHECK(e->sample.t_mono_ns == 1000 && e->sample.seq == 0 && e->sample.value == 0);
    }
    e = nth_sample(LIGHT, 4);
    CHECK(e != nullptr && !e->si.first_of_instance && !e->si.first_on_connection);
    if (e != nullptr)
        CHECK(e->sample.value == 4 && e->sample.t_mono_ns == 1040);   /* untouched */
    for (size_t i = 1; i < 10; i++) {
        const struct log_entry *a = nth_sample(LIGHT, i - 1), *b = nth_sample(LIGHT, i);
        CHECK(a != nullptr && b != nullptr && a->si.hub_rx_ns <= b->si.hub_rx_ns);
    }

    struct oc_hub_source_status st = status_of(&f, LIGHT);
    CHECK_EQ(st.seq.accepted, 10);
    CHECK_EQ(st.seq.lost, 0);
    CHECK_EQ(st.status_count[OC_ST_OK], 10);
    CHECK_EQ(st.connections, 1);
    CHECK(oc_seq_consistent(&st.seq));
    CHECK_EQ(status_of(&f, DIST).seq.accepted, 5);

    /* events, in order */
    CHECK_EQ(count_ev(OC_HUB_EV_SOURCE_UP, LIGHT), 1);
    CHECK_EQ(count_ev(OC_HUB_EV_SOURCE_UP, DIST), 1);
    const struct oc_hub_event *up = find_ev(OC_HUB_EV_SOURCE_UP, LIGHT, 0);
    CHECK(up != nullptr && !up->reconnect && !up->reset && up->instance == 1);
    CHECK_EQ(count_ev(OC_HUB_EV_DEGRADED_CHANGED, 0), 2);        /* true, then false */
    CHECK(ev_or0(OC_HUB_EV_DEGRADED_CHANGED, 0, 0)->degraded);
    CHECK(!ev_or0(OC_HUB_EV_DEGRADED_CHANGED, 0, 1)->degraded);
    CHECK_EQ(sink.overflow, 0);

    oc_ipc_conn_close(&cl);
    oc_ipc_conn_close(&cd);
    fx_end(&f);
    CHECK_EQ(fd_count(), f.fds_before);
}

static void test_mixed_origin_session(void)
{
    struct fx f;
    fx_begin(&f);
    f.cfg.origin[LIGHT] = OC_ORIGIN_SIMULATED;
    f.cfg.origin[DIST] = OC_ORIGIN_REPLAYED;
    CHECK(fx_start(&f));
    struct oc_ipc_conn cl, cd;
    CHECK(join_ok(&f, &cl, LIGHT, 1));
    /* DISTANCE says SIMULATED but this session expects REPLAYED from it */
    CHECK(cl_connect(&f, &cd));
    struct oc_hello h = hello_for(DIST, 1);
    uint16_t why = 0;
    CHECK_EQ(hello_exchange(&f, &cd, &h, &why), OC_MSG_HELLO_REJECT);
    CHECK_EQ(why, OC_REJECT_BAD_ORIGIN);
    oc_ipc_conn_close(&cd);
    CHECK(cl_connect(&f, &cd));
    h.origin = OC_ORIGIN_REPLAYED;
    CHECK_EQ(hello_exchange(&f, &cd, &h, nullptr), OC_MSG_HELLO_ACK);
    struct oc_sample s = mk_sample(DIST, 0, 5, OC_ST_OK);
    s.origin = OC_ORIGIN_REPLAYED;
    CHECK_EQ(oc_ipc_send_sample(&cd, &s), 0);
    pump(&f, 3);
    const struct log_entry *e = nth_sample(DIST, 0);
    CHECK(e != nullptr && e->si.origin == OC_ORIGIN_REPLAYED);
    oc_ipc_conn_close(&cl);
    oc_ipc_conn_close(&cd);
    fx_end(&f);
}

static void test_sample_before_hello(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn c;
    CHECK(cl_connect(&f, &c));
    pump(&f, 2);
    struct oc_sample s = mk_sample(LIGHT, 0, 1, OC_ST_OK);
    CHECK_EQ(oc_ipc_send_sample(&c, &s), 0);
    pump(&f, 3);
    struct tail t = read_tail(&c);
    CHECK_EQ(t.first_type, OC_MSG_BYE);
    CHECK_EQ(t.bye_reason, OC_BYE_ERROR);
    CHECK(t.eof);
    CHECK_EQ(sink.samples[LIGHT], 0);                    /* never delivered */
    CHECK_EQ(stats_of(&f).violations, 1);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_AWAITING);
    const struct oc_hub_event *ev = find_ev(OC_HUB_EV_CONN_CLOSED, 0, 0);
    CHECK(ev != nullptr && ev->reason == OC_HUB_CLOSE_PRE_HELLO);
    oc_ipc_conn_close(&c);

    /* BYE before HELLO is just an orderly close; garbage is a malformed HELLO */
    CHECK(cl_connect(&f, &c));
    pump(&f, 2);
    uint8_t bye[8] = {1, 5, 4, 0, 1, 0, 0, 0};
    CHECK_EQ(oc_ipc_send_msg(&c, bye, 8), 0);
    pump(&f, 3);
    CHECK(read_tail(&c).eof);
    oc_ipc_conn_close(&c);
    CHECK(cl_connect(&f, &c));
    pump(&f, 2);
    raw(&c, "\x01\x01\x14\x00", 4);                      /* HELLO header, no payload */
    pump(&f, 3);
    t = read_tail(&c);
    CHECK_EQ(t.first_type, OC_MSG_HELLO_REJECT);
    CHECK(t.eof);
    CHECK_EQ(stats_of(&f).rejects[OC_REJECT_MALFORMED], 1);
    oc_ipc_conn_close(&c);
    fx_end(&f);
}

static void expect_reject(struct fx *f, struct oc_hello h, uint16_t want)
{
    struct oc_hub_stats before = stats_of(f);
    struct oc_ipc_conn c;
    CHECK(cl_connect(f, &c));
    uint16_t why = 0;
    int t = hello_exchange(f, &c, &h, &why);
    if (t != OC_MSG_HELLO_REJECT || why != want)
        fprintf(stderr, "  expected REJECT %u, got type %d reason %u\n", want, t, why);
    CHECK_EQ(t, OC_MSG_HELLO_REJECT);
    CHECK_EQ(why, want);
    CHECK(read_tail(&c).eof);                            /* then the hub hangs up */
    CHECK_EQ(stats_of(f).rejects[want], before.rejects[want] + 1);
    oc_ipc_conn_close(&c);
}

static void test_hello_rejections(void)
{
    struct fx f;
    fx_begin(&f);
    f.cfg.initial[DIST].authorized = false;              /* DISTANCE not yet authorized */
    CHECK(fx_start(&f));
    struct oc_hello h;

    h = hello_for(LIGHT, 1); h.session_id = SESSION + 1;
    expect_reject(&f, h, OC_REJECT_SESSION_MISMATCH);
    h = hello_for(LIGHT, 1); h.origin = OC_ORIGIN_LIVE;
    expect_reject(&f, h, OC_REJECT_BAD_ORIGIN);
    h = hello_for(LIGHT, 1); h.pid = (uint32_t)getpid() + 1;     /* forged pid */
    expect_reject(&f, h, OC_REJECT_PEER_CRED);
    expect_reject(&f, hello_for(LIGHT, 2), OC_REJECT_STALE_INSTANCE);   /* higher: not trusted */
    expect_reject(&f, hello_for(LIGHT, 0), OC_REJECT_STALE_INSTANCE);   /* lower */
    expect_reject(&f, hello_for(DIST, 1), OC_REJECT_NOT_READY);
    CHECK_EQ(status_of(&f, DIST).state, OC_HUB_SRC_UNAUTHORIZED);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_AWAITING);   /* nothing changed */
    CHECK_EQ(count_ev(OC_HUB_EV_CONN_REJECTED, 0), 6);
    CHECK_EQ(ev_or0(OC_HUB_EV_CONN_REJECTED, 0, 3)->reject, OC_REJECT_STALE_INSTANCE);
    fx_end(&f);

    /* a source the session does not permit */
    fx_begin(&f);
    f.cfg.origin[DIST] = 0;
    CHECK(fx_start(&f));
    expect_reject(&f, hello_for(DIST, 1), OC_REJECT_BAD_SOURCE);
    fx_end(&f);
    CHECK_EQ(fd_count(), f.fds_before);
}

static void test_duplicate_active_source(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn a, b;
    CHECK(join_ok(&f, &a, LIGHT, 1));
    send_n(&f, &a, LIGHT, 0, 3);
    pump(&f, 2);

    /* a second connection for the same source is refused, the incumbent lives on */
    CHECK(cl_connect(&f, &b));
    uint16_t why = 0;
    struct oc_hello h = hello_for(LIGHT, 1);
    CHECK_EQ(hello_exchange(&f, &b, &h, &why), OC_MSG_HELLO_REJECT);
    CHECK_EQ(why, OC_REJECT_DUPLICATE_SOURCE);
    oc_ipc_conn_close(&b);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_ACTIVE);
    send_n(&f, &a, LIGHT, 3, 2);
    pump(&f, 2);
    CHECK_EQ(sink.samples[LIGHT], 5);
    CHECK_EQ(status_of(&f, LIGHT).seq.lost, 0);

    /* race: the incumbent hangs up in the same round the newcomer says HELLO.
     * The hub's rotating start slot decides who is serviced first, so repeat the
     * scenario at every rotation phase: the newcomer must be admitted at all of
     * them (when it is served first this needs the bounded look at the incumbent). */
    oc_ipc_conn_close(&b);
    fx_end(&f);
    for (int phase = 0; phase < (int)OC_HUB_MAX_CONNS; phase++) {
        fx_begin(&f);
        CHECK(fx_start(&f));
        CHECK(join_ok(&f, &a, LIGHT, 1));
        CHECK(cl_connect(&f, &b));
        pump(&f, 3 + phase);                             /* b is pending, silent; rr advances */
        send_hello(&b, &h);
        oc_ipc_conn_close(&a);
        pump(&f, 4);
        struct oc_ipc_msg m2;
        int got = oc_ipc_recv(&b, &m2);
        if (got != 0 || m2.type != OC_MSG_HELLO_ACK)
            fprintf(stderr, "  race at rotation phase %d: not admitted\n", phase);
        CHECK_EQ(got, 0);
        CHECK_EQ(m2.type, OC_MSG_HELLO_ACK);
        CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_ACTIVE);
        const struct oc_hub_event *up2 = find_ev(OC_HUB_EV_SOURCE_UP, LIGHT, 1);
        CHECK(up2 != nullptr && up2->reconnect && !up2->reset);
        oc_ipc_conn_close(&b);
        fx_end(&f);
    }
    struct oc_ipc_msg m;

    /* bounded: an incumbent with a deep backlog is NOT drained to settle the race */
    fx_begin(&f);
    f.cfg.batch = 4;
    CHECK(fx_start(&f));
    CHECK(join_ok(&f, &a, LIGHT, 1));
    CHECK(cl_connect(&f, &b));
    pump(&f, 3);                                         /* b pending */
    uint32_t sent = 0;
    for (;; sent++) {                                    /* fill a's queue, hub not pumped */
        struct oc_sample s = mk_sample(LIGHT, sent, 1000 + sent, OC_ST_OK);
        if (oc_ipc_send_sample(&a, &s) != 0)
            break;
    }
    CHECK(sent >= 20);
    oc_ipc_conn_close(&a);                               /* queued samples + EOF */
    h = hello_for(LIGHT, 1);
    send_hello(&b, &h);
    int n = pump(&f, 1);
    CHECK(n <= 4 * 2);                                   /* per-connection budget respected */
    why = 0;
    CHECK_EQ(oc_ipc_recv(&b, &m), 0);
    CHECK_EQ(m.type, OC_MSG_HELLO_REJECT);
    CHECK_EQ(m.u.reject.reason, OC_REJECT_DUPLICATE_SOURCE);
    oc_ipc_conn_close(&b);
    for (int i = 0; i < 40 && status_of(&f, LIGHT).state == OC_HUB_SRC_ACTIVE; i++)
        pump(&f, 1);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_DISCONNECTED);
    CHECK_EQ(sink.samples[LIGHT], sent);                 /* nothing queued was lost */
    CHECK(join_ok(&f, &b, LIGHT, 1));                    /* a later retry succeeds */
    oc_ipc_conn_close(&b);
    fx_end(&f);
    CHECK_EQ(fd_count(), f.fds_before);
}

static void test_same_instance_reconnect(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn a, b;
    CHECK(join_ok(&f, &a, LIGHT, 1));
    send_n(&f, &a, LIGHT, 0, 5);
    pump(&f, 2);
    oc_ipc_conn_close(&a);
    pump(&f, 3);

    struct oc_hub_source_status st = status_of(&f, LIGHT);
    CHECK_EQ(st.state, OC_HUB_SRC_DISCONNECTED);
    CHECK(!st.available);
    CHECK(oc_hub_degraded(&f.hub));
    const struct oc_hub_event *down = find_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT, 0);
    CHECK(down != nullptr && down->reason == OC_HUB_CLOSE_EOF);
    CHECK_EQ(st.seq.accepted, 5);                        /* history survives */

    CHECK(join_ok(&f, &b, LIGHT, 1));
    const struct oc_hub_event *up = find_ev(OC_HUB_EV_SOURCE_UP, LIGHT, 1);
    CHECK(up != nullptr && up->reconnect && !up->reset);
    send_n(&f, &b, LIGHT, 8, 2);                         /* 5,6,7 were consumed while away */
    pump(&f, 2);
    const struct log_entry *e = nth_sample(LIGHT, 5);
    CHECK(e != nullptr);
    if (e != nullptr) {
        CHECK_EQ(e->sample.seq, 8);
        CHECK_EQ(e->si.gap, 3);
        CHECK(e->si.first_on_connection && !e->si.first_of_instance);
        CHECK_EQ(e->si.generation, 1);
    }
    st = status_of(&f, LIGHT);
    CHECK_EQ(st.seq.lost, 3);
    CHECK_EQ(st.seq.gap_events, 1);
    CHECK_EQ(st.connections, 2);
    CHECK_EQ(st.reconnects, 1);
    CHECK_EQ(st.resets, 0);
    CHECK(oc_seq_consistent(&st.seq));
    oc_ipc_conn_close(&b);
    fx_end(&f);
}

static void test_authorized_new_instance(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn a, b;
    CHECK(join_ok(&f, &a, LIGHT, 1));
    send_n(&f, &a, LIGHT, 0, 4);
    pump(&f, 2);

    CHECK_EQ(oc_hub_authorize(&f.hub, LIGHT, 1), 0);     /* same instance: no-op */
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_ACTIVE);
    CHECK_EQ(oc_hub_authorize(&f.hub, 0, 5), -EINVAL);
    CHECK_EQ(oc_hub_authorize(&f.hub, 3, 5), -EINVAL);
    CHECK_EQ(oc_hub_authorize(nullptr, LIGHT, 5), -EINVAL);

    /* samples still queued from the old instance are discarded unread on SUPERSEDED */
    {
        struct oc_sample q = mk_sample(LIGHT, 4, 9000, OC_ST_OK);
        CHECK_EQ(oc_ipc_send_sample(&a, &q), 0);
    }
    CHECK_EQ(oc_hub_authorize(&f.hub, LIGHT, 2), 0);     /* supervisor restarted the reader */
    CHECK_EQ(sink.samples[LIGHT], 4);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_AWAITING);
    CHECK_EQ(status_of(&f, LIGHT).seq.accepted, 4);      /* kept until the new instance is ACKed */
    const struct oc_hub_event *down = find_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT, 0);
    CHECK(down != nullptr && down->reason == OC_HUB_CLOSE_SUPERSEDED);
    CHECK(read_tail(&a).eof);                            /* the old reader is cut off */
    oc_ipc_conn_close(&a);

    CHECK_EQ(oc_hub_authorize(&f.hub, LIGHT, 1), -ESTALE);   /* no going back */
    CHECK_EQ(oc_hub_authorize(&f.hub, LIGHT, 0), -ESTALE);
    expect_reject(&f, hello_for(LIGHT, 1), OC_REJECT_STALE_INSTANCE);   /* the old one is out */

    CHECK(join_ok(&f, &b, LIGHT, 2));
    const struct oc_hub_event *up = find_ev(OC_HUB_EV_SOURCE_UP, LIGHT, 1);
    CHECK(up != nullptr && up->reset && !up->reconnect && up->instance == 2);
    CHECK_EQ(ev_or0(OC_HUB_EV_SOURCE_UP, LIGHT, 1)->generation, 2);
    struct oc_hub_source_status st = status_of(&f, LIGHT);
    CHECK_EQ(st.seq.accepted, 0);                        /* per-source state reset */
    CHECK_EQ(st.generation, 2);
    CHECK_EQ(st.resets, 1);
    CHECK_EQ(st.instance, 2);

    /* the new instance starts over: seq 0 and an EARLIER timestamp are normal */
    struct oc_sample s = mk_sample(LIGHT, 0, 5, OC_ST_OK);
    CHECK_EQ(oc_ipc_send_sample(&b, &s), 0);
    pump(&f, 3);
    const struct log_entry *e = nth_sample(LIGHT, 4);
    CHECK(e != nullptr);
    if (e != nullptr) {
        CHECK(e->si.first_of_instance && !e->si.ts_backwards);
        CHECK_EQ(e->si.generation, 2);
        CHECK_EQ(e->si.instance, 2);
    }
    CHECK_EQ(status_of(&f, LIGHT).seq.ts_backwards, 0);

    /* the other source is independent and unaffected */
    CHECK_EQ(status_of(&f, DIST).state, OC_HUB_SRC_AWAITING);
    CHECK_EQ(oc_hub_authorize(&f.hub, DIST, 0), -ESTALE);    /* 0 < 1 already authorized */
    oc_ipc_conn_close(&b);
    fx_end(&f);

    /* instance 0 is a valid first authorization */
    fx_begin(&f);
    f.cfg.initial[DIST].authorized = false;
    CHECK(fx_start(&f));
    CHECK_EQ(oc_hub_authorize(&f.hub, DIST, 0), 0);
    CHECK_EQ(status_of(&f, DIST).state, OC_HUB_SRC_AWAITING);
    CHECK(join_ok(&f, &a, DIST, 0));
    oc_ipc_conn_close(&a);
    fx_end(&f);
}

static void test_sequence_via_hub(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn c;
    CHECK(join_ok(&f, &c, LIGHT, 1));
    static const uint32_t seqs[] = {0, 1, 2, 6, 6, 3, 7};   /* gap, duplicate, regression */
    for (size_t i = 0; i < sizeof seqs / sizeof seqs[0]; i++) {
        struct oc_sample s = mk_sample(LIGHT, seqs[i], 1000 + 10 * i, OC_ST_OK);
        CHECK_EQ(oc_ipc_send_sample(&c, &s), 0);
    }
    pump(&f, 3);
    CHECK_EQ(sink.samples[LIGHT], 5);                    /* 0,1,2,6,7 delivered */
    const struct log_entry *e = nth_sample(LIGHT, 3);
    CHECK(e != nullptr && e->sample.seq == 6 && e->si.gap == 3);
    e = nth_sample(LIGHT, 4);
    CHECK(e != nullptr && e->sample.seq == 7 && e->si.gap == 0);
    struct oc_hub_source_status st = status_of(&f, LIGHT);
    CHECK_EQ(st.seq.lost, 3);
    CHECK_EQ(st.seq.duplicates, 1);
    CHECK_EQ(st.seq.regressions, 1);
    CHECK_EQ(st.seq.logical_last, 7);                    /* rejects did not advance it */
    CHECK_EQ(st.state, OC_HUB_SRC_ACTIVE);               /* two strikes: still connected */
    oc_ipc_conn_close(&c);

    /* wraparound through the hub */
    struct oc_ipc_conn d;
    CHECK(join_ok(&f, &d, DIST, 1));
    static const uint32_t w[] = {0xFFFFFFFDu, 0xFFFFFFFEu, 0xFFFFFFFFu, 0, 1};
    for (size_t i = 0; i < 5; i++) {
        struct oc_sample s = mk_sample(DIST, w[i], 1000 + 10 * i, OC_ST_OK);
        CHECK_EQ(oc_ipc_send_sample(&d, &s), 0);
    }
    struct oc_sample dup = mk_sample(DIST, 1, 9999, OC_ST_OK);
    struct oc_sample old = mk_sample(DIST, 0xFFFFFFFFu, 9999, OC_ST_OK);
    CHECK_EQ(oc_ipc_send_sample(&d, &dup), 0);
    CHECK_EQ(oc_ipc_send_sample(&d, &old), 0);
    pump(&f, 3);
    st = status_of(&f, DIST);
    CHECK_EQ(st.seq.accepted, 5);
    CHECK_EQ(st.seq.unobserved_prefix, 0xFFFFFFFDull);
    CHECK_EQ(st.seq.lost, 0);
    CHECK_EQ(st.seq.logical_last, 0xFFFFFFFDull + 4);
    CHECK_EQ(st.seq.duplicates, 1);
    CHECK_EQ(st.seq.regressions, 1);
    CHECK(oc_seq_consistent(&st.seq));
    const struct log_entry *first = nth_sample(DIST, 0);
    CHECK(first != nullptr && first->si.first_of_instance && first->si.gap == 0xFFFFFFFDu);
    oc_ipc_conn_close(&d);
    fx_end(&f);
}

static void test_failed_acquisition_statuses(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn c;
    CHECK(join_ok(&f, &c, LIGHT, 1));
    static const uint8_t cycle[] = {OC_ST_NACK, OC_ST_TIMEOUT, OC_ST_IO_ERR,
                                    OC_ST_SATURATED, OC_ST_RANGE_ERR, OC_ST_STALE};
    for (uint32_t i = 0; i < 120; i++) {
        struct oc_sample s = mk_sample(LIGHT, i, 1000 + 10 * i, cycle[i % 6]);
        int tries = 0, rc;
        while ((rc = oc_ipc_send_sample(&c, &s)) == -EAGAIN && tries++ < 100)
            pump(&f, 1);
        CHECK_EQ(rc, 0);
    }
    pump(&f, 8);
    CHECK_EQ(sink.samples[LIGHT], 120);                  /* each is a valid report */
    struct oc_hub_source_status st = status_of(&f, LIGHT);
    for (size_t k = 0; k < 6; k++)
        CHECK_EQ(st.status_count[cycle[k]], 20);
    CHECK_EQ(st.status_count[OC_ST_OK], 0);
    CHECK_EQ(st.seq.lost, 0);                            /* failures consumed their seq */
    CHECK_EQ(st.seq.accepted, 120);
    CHECK_EQ(st.state, OC_HUB_SRC_ACTIVE);
    CHECK_EQ(stats_of(&f).abusive_closes, 0);
    CHECK_EQ(stats_of(&f).malformed, 0);
    const struct log_entry *e = nth_sample(LIGHT, 7);
    CHECK(e != nullptr && e->sample.status == cycle[1] && e->sample.value == 0);
    /* enough non-OK reports would exceed the strike limit if they were strikes */
    send_bad(&c, 0);
    pump(&f, 3);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_ACTIVE);
    oc_ipc_conn_close(&c);
    fx_end(&f);
}

static void test_identity_mismatch(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn c;
    CHECK(join_ok(&f, &c, LIGHT, 1));
    struct oc_sample wrong_src = mk_sample(DIST, 0, 1, OC_ST_OK);
    struct oc_sample wrong_org = mk_sample(LIGHT, 0, 1, OC_ST_OK);
    wrong_org.origin = OC_ORIGIN_REPLAYED;
    CHECK_EQ(oc_ipc_send_sample(&c, &wrong_src), 0);
    CHECK_EQ(oc_ipc_send_sample(&c, &wrong_org), 0);
    pump(&f, 3);
    CHECK_EQ(sink.samples[LIGHT] + sink.samples[DIST], 0);
    CHECK_EQ(status_of(&f, LIGHT).identity_mismatches, 2);
    CHECK_EQ(status_of(&f, DIST).seq.accepted, 0);
    CHECK_EQ(status_of(&f, LIGHT).seq.accepted, 0);      /* tracker untouched */
    /* the connection is still good for honest samples */
    send_n(&f, &c, LIGHT, 0, 2);
    pump(&f, 2);
    CHECK_EQ(sink.samples[LIGHT], 2);
    oc_ipc_conn_close(&c);
    fx_end(&f);
}

static void test_abuse_policy(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn l, d;
    CHECK(join_ok(&f, &l, LIGHT, 1));
    CHECK(join_ok(&f, &d, DIST, 1));

    for (int i = 0; i < 7; i++)
        send_bad(&l, i);
    pump(&f, 3);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_ACTIVE);   /* 7 strikes: still tolerated */
    struct oc_hub_stats st = stats_of(&f);
    CHECK_EQ(st.malformed + st.oversized, 7);
    CHECK_EQ(st.oversized, 3);

    send_bad(&l, 0);                                           /* the 8th */
    pump(&f, 3);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_DISCONNECTED);
    struct tail t = read_tail(&l);
    CHECK_EQ(t.first_type, OC_MSG_BYE);
    CHECK_EQ(t.bye_reason, OC_BYE_ERROR);
    CHECK(t.eof);
    CHECK_EQ(stats_of(&f).abusive_closes, 1);
    const struct oc_hub_event *down = find_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT, 0);
    CHECK(down != nullptr && down->reason == OC_HUB_CLOSE_ABUSE);
    oc_ipc_conn_close(&l);

    /* the other source never noticed */
    CHECK_EQ(status_of(&f, DIST).state, OC_HUB_SRC_ACTIVE);
    send_n(&f, &d, DIST, 0, 3);
    pump(&f, 2);
    CHECK_EQ(sink.samples[DIST], 3);

    /* forgiveness: one strike is forgiven per 64 valid datagrams */
    CHECK(join_ok(&f, &l, LIGHT, 1));
    for (int i = 0; i < 7; i++)
        send_bad(&l, i);
    pump(&f, 3);
    send_n(&f, &l, LIGHT, 0, 64);                              /* 7 -> 6 */
    pump(&f, 6);
    send_bad(&l, 1);                                           /* 6 -> 7 */
    pump(&f, 3);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_ACTIVE);   /* without forgiveness: 8, closed */
    send_bad(&l, 2);                                           /* 7 -> 8 */
    pump(&f, 3);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_DISCONNECTED);
    CHECK_EQ(stats_of(&f).abusive_closes, 2);
    oc_ipc_conn_close(&l);
    oc_ipc_conn_close(&d);
    fx_end(&f);
}

static void test_unsupported_version(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn c;

    /* before HELLO: closed, no reply of any kind */
    CHECK(cl_connect(&f, &c));
    pump(&f, 2);
    uint8_t v2[24] = {2, 1, 20, 0};
    raw(&c, v2, sizeof v2);
    pump(&f, 3);
    struct tail t = read_tail(&c);
    CHECK_EQ(t.first_type, 0);
    CHECK(t.eof);
    CHECK_EQ(stats_of(&f).version_fatal, 1);
    const struct oc_hub_event *ev = find_ev(OC_HUB_EV_CONN_CLOSED, 0, 0);
    CHECK(ev != nullptr && ev->reason == OC_HUB_CLOSE_VERSION);
    oc_ipc_conn_close(&c);

    /* after HELLO: the source goes down, again with no reply */
    CHECK(join_ok(&f, &c, LIGHT, 1));
    raw(&c, "\x07", 1);
    pump(&f, 3);
    t = read_tail(&c);
    CHECK_EQ(t.first_type, 0);
    CHECK(t.eof);
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_DISCONNECTED);
    const struct oc_hub_event *down = find_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT, 0);
    CHECK(down != nullptr && down->reason == OC_HUB_CLOSE_VERSION);
    CHECK_EQ(stats_of(&f).version_fatal, 2);
    CHECK_EQ(stats_of(&f).abusive_closes, 0);            /* not scored: fatal at once */
    oc_ipc_conn_close(&c);
    CHECK(join_ok(&f, &c, LIGHT, 1));                    /* a corrected reader may return */
    oc_ipc_conn_close(&c);
    fx_end(&f);
}

static void test_hello_timeout_and_pending_cap(void)
{
    struct fx f;
    fx_begin(&f);
    f.cfg.hello_timeout_ms = 100;
    CHECK(fx_start(&f));
    struct oc_ipc_conn c;
    CHECK(cl_connect(&f, &c));
    uint64_t t0 = now_ns();
    while (count_ev(OC_HUB_EV_HELLO_TIMEOUT, 0) == 0 && now_ns() - t0 < 2000000000ull)
        CHECK(oc_hub_step(&f.hub, 20) >= 0);
    uint64_t ms = (now_ns() - t0) / 1000000ull;
    fprintf(stderr, "  silent client closed after %llu ms (HELLO timeout 100 ms)\n",
            (unsigned long long)ms);
    CHECK_EQ(count_ev(OC_HUB_EV_HELLO_TIMEOUT, 0), 1);
    CHECK(ms >= 95 && ms < 1800);   /* upper bound only guards against a hang */
    CHECK(read_tail(&c).eof);
    CHECK_EQ(stats_of(&f).hello_timeouts, 1);
    oc_ipc_conn_close(&c);

    /* a HELLO sent after the deadline is too late: nobody is listening on it */
    fx_end(&f);

    /* pending cap: extra connections are shed, not queued without bound */
    fx_begin(&f);
    f.cfg.hello_timeout_ms = 5000;
    f.cfg.max_pending = 2;
    CHECK(fx_start(&f));
    struct oc_ipc_conn many[5];
    for (int i = 0; i < 5; i++)
        CHECK(cl_connect(&f, &many[i]));
    pump(&f, 4);
    int eof = 0;
    for (int i = 0; i < 5; i++)
        if (read_tail(&many[i]).eof) {
            eof++;
            oc_ipc_conn_close(&many[i]);                 /* the hub shed it */
        }
    CHECK_EQ(eof, 3);
    CHECK_EQ(stats_of(&f).shed, 3);
    CHECK_EQ(stats_of(&f).conns_accepted, 5);
    CHECK_EQ(count_ev(OC_HUB_EV_CONN_CLOSED, 0), 3);
    CHECK(ev_or0(OC_HUB_EV_CONN_CLOSED, 0, 0)->reason == OC_HUB_CLOSE_SHED);
    /* the two that got slots can still complete a handshake */
    int acks = 0;
    for (int i = 0; i < 5; i++) {
        if (!many[i].open)
            continue;
        struct oc_hello h = hello_for(i % 2 == 0 ? LIGHT : DIST, 1);
        uint16_t why;
        if (hello_exchange(&f, &many[i], &h, &why) == OC_MSG_HELLO_ACK)
            acks++;
    }
    CHECK(acks >= 1);
    for (int i = 0; i < 5; i++)
        oc_ipc_conn_close(&many[i]);
    fx_end(&f);
    CHECK_EQ(fd_count(), f.fds_before);
}

static void test_disconnect_after_queued_samples(void)
{
    struct fx f;
    fx_begin(&f);
    f.cfg.batch = 4;
    CHECK(fx_start(&f));
    struct oc_ipc_conn c;
    CHECK(join_ok(&f, &c, LIGHT, 1));

    uint32_t sent = 0;                                   /* hub is NOT pumped meanwhile */
    for (;; sent++) {
        struct oc_sample s = mk_sample(LIGHT, sent, 1000 + sent, OC_ST_OK);
        if (oc_ipc_send_sample(&c, &s) != 0)
            break;
    }
    fprintf(stderr, "  %u samples queued before the client hung up\n", sent);
    CHECK(sent >= 20);
    oc_ipc_conn_close(&c);                               /* immediate close: EOF behind the queue */

    int steps = 0;
    while (status_of(&f, LIGHT).state == OC_HUB_SRC_ACTIVE && steps < 200) {
        int n = oc_hub_step(&f.hub, 0);
        CHECK(n >= 0 && n <= 4);                         /* POLLHUP does not lift the budget */
        steps++;
    }
    CHECK(steps >= (int)(sent / 4));                     /* it really took several steps */
    CHECK_EQ(sink.samples[LIGHT], sent);                 /* every transmitted sample delivered */
    CHECK_EQ(status_of(&f, LIGHT).seq.lost, 0);
    ptrdiff_t last = last_sample_index(LIGHT);
    ptrdiff_t down = first_event_index(OC_HUB_EV_SOURCE_DOWN, LIGHT);
    CHECK(last >= 0 && down >= 0 && last < down);        /* all samples precede SOURCE_DOWN */
    CHECK_EQ(ev_or0(OC_HUB_EV_SOURCE_DOWN, LIGHT, 0)->reason, OC_HUB_CLOSE_EOF);

    /* BYE after samples: the same, but an orderly reason */
    struct oc_ipc_conn b;
    CHECK(join_ok(&f, &b, LIGHT, 1));
    uint8_t bye[8] = {1, 5, 4, 0, 1, 0, 0, 0};
    for (uint32_t i = 0; i < 3; i++) {
        struct oc_sample s = mk_sample(LIGHT, sent + i, 5000 + i, OC_ST_OK);
        CHECK_EQ(oc_ipc_send_sample(&b, &s), 0);
    }
    CHECK_EQ(oc_ipc_send_msg(&b, bye, 8), 0);
    pump(&f, 4);
    CHECK_EQ(sink.samples[LIGHT], sent + 3);
    const struct oc_hub_event *down2 = find_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT, 1);
    CHECK(down2 != nullptr && down2->reason == OC_HUB_CLOSE_BYE);
    CHECK_EQ(stats_of(&f).abusive_closes, 0);
    oc_ipc_conn_close(&b);
    fx_end(&f);
}

static void test_fair_processing(void)
{
    struct fx f;
    fx_begin(&f);
    f.cfg.batch = 8;
    CHECK(fx_start(&f));
    struct oc_ipc_conn l, d;
    CHECK(join_ok(&f, &l, LIGHT, 1));
    CHECK(join_ok(&f, &d, DIST, 1));

    uint32_t sent = 0;                                   /* LIGHT floods */
    for (;; sent++) {
        struct oc_sample s = mk_sample(LIGHT, sent, 1000 + sent, OC_ST_OK);
        if (oc_ipc_send_sample(&l, &s) != 0)
            break;
    }
    CHECK(sent > 3 * 8);
    struct oc_sample one = mk_sample(DIST, 0, 77, OC_ST_OK);   /* DISTANCE sends one */
    CHECK_EQ(oc_ipc_send_sample(&d, &one), 0);

    int n = oc_hub_step(&f.hub, 0);                      /* ONE step */
    CHECK(n <= 16);
    CHECK_EQ(sink.samples[DIST], 1);                     /* not starved behind the flood */
    CHECK(sink.samples[LIGHT] >= 1 && sink.samples[LIGHT] <= 8);
    int steps = 1;
    while (sink.samples[LIGHT] < sent && steps < 100) {
        int k = oc_hub_step(&f.hub, 0);
        CHECK(k <= 8);                                   /* one connection left: its budget only */
        steps++;
    }
    CHECK_EQ(sink.samples[LIGHT], sent);
    CHECK(steps >= (int)(sent / 8));
    CHECK_EQ(status_of(&f, LIGHT).seq.lost, 0);
    oc_ipc_conn_close(&l);
    oc_ipc_conn_close(&d);
    fx_end(&f);
}

static void test_ack_send_failure(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn a, b;
    CHECK(join_ok(&f, &a, LIGHT, 1));
    send_n(&f, &a, LIGHT, 0, 3);
    pump(&f, 2);
    oc_ipc_conn_close(&a);
    pump(&f, 3);
    CHECK_EQ(oc_hub_authorize(&f.hub, LIGHT, 2), 0);     /* a new instance is expected */
    int fds = fd_count();
    size_t ups = count_ev(OC_HUB_EV_SOURCE_UP, LIGHT);

    /* (1) nonblocking ACK send fails with EAGAIN */
    CHECK(cl_connect(&f, &b));
    pump(&f, 2);
    fail_ack_once = 1;
    struct oc_hello h = hello_for(LIGHT, 2);
    send_hello(&b, &h);
    pump(&f, 3);
    CHECK_EQ(fail_ack_once, 0);                          /* the fault really fired */
    struct tail t = read_tail(&b);
    CHECK_EQ(t.first_type, 0);                           /* no ACK ... */
    CHECK(t.eof);                                        /* ... the connection was closed */
    oc_ipc_conn_close(&b);
    struct oc_hub_source_status st = status_of(&f, LIGHT);
    CHECK_EQ(st.state, OC_HUB_SRC_AWAITING);             /* NOT active */
    CHECK(!st.available);
    CHECK_EQ(st.generation, 1);                          /* nothing was reset */
    CHECK_EQ(st.seq.accepted, 3);
    CHECK_EQ(st.resets, 0);
    CHECK_EQ(count_ev(OC_HUB_EV_SOURCE_UP, LIGHT), ups);
    CHECK_EQ(stats_of(&f).ack_failures, 1);
    CHECK_EQ(fd_count(), fds);

    /* (2) the peer vanished between HELLO and ACK (EPIPE) */
    CHECK(cl_connect(&f, &b));
    pump(&f, 2);
    send_hello(&b, &h);
    oc_ipc_conn_close(&b);
    pump(&f, 4);
    st = status_of(&f, LIGHT);
    CHECK_EQ(st.state, OC_HUB_SRC_AWAITING);
    CHECK_EQ(st.seq.accepted, 3);
    CHECK_EQ(count_ev(OC_HUB_EV_SOURCE_UP, LIGHT), ups);
    CHECK_EQ(stats_of(&f).ack_failures, 2);
    CHECK_EQ(fd_count(), fds);

    /* (3) the retry works and only now is the tracker reset */
    CHECK(join_ok(&f, &b, LIGHT, 2));
    st = status_of(&f, LIGHT);
    CHECK_EQ(st.state, OC_HUB_SRC_ACTIVE);
    CHECK_EQ(st.generation, 2);
    CHECK_EQ(st.seq.accepted, 0);
    oc_ipc_conn_close(&b);
    fx_end(&f);
}

static void test_timestamp_anomalies(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn c;
    CHECK(join_ok(&f, &c, LIGHT, 1));
    static const uint64_t ts[] = {1000, 2000, 1500, 1600, 1600};
    for (uint32_t i = 0; i < 5; i++) {
        struct oc_sample s = mk_sample(LIGHT, i, ts[i], OC_ST_OK);
        CHECK_EQ(oc_ipc_send_sample(&c, &s), 0);
    }
    pump(&f, 3);
    CHECK_EQ(sink.samples[LIGHT], 5);
    for (size_t i = 0; i < 5; i++) {
        const struct log_entry *e = nth_sample(LIGHT, i);
        CHECK(e != nullptr);
        if (e != nullptr) {
            CHECK_EQ(e->sample.t_mono_ns, ts[i]);        /* never rewritten */
            CHECK_EQ(e->si.ts_backwards, i == 2);        /* flagged once, at the step back */
        }
    }
    CHECK_EQ(status_of(&f, LIGHT).seq.ts_backwards, 1);
    CHECK_EQ(status_of(&f, LIGHT).seq.lost, 0);          /* it is not a sequence problem */
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_ACTIVE);   /* and not a strike */

    /* a reconnect to the same instance keeps the timestamp state */
    oc_ipc_conn_close(&c);
    pump(&f, 3);
    CHECK(join_ok(&f, &c, LIGHT, 1));
    struct oc_sample s = mk_sample(LIGHT, 5, 100, OC_ST_OK);
    CHECK_EQ(oc_ipc_send_sample(&c, &s), 0);
    pump(&f, 3);
    const struct log_entry *e = nth_sample(LIGHT, 5);
    CHECK(e != nullptr && e->si.ts_backwards);
    CHECK_EQ(status_of(&f, LIGHT).seq.ts_backwards, 2);

    /* a rejected duplicate carries no timestamp verdict */
    struct oc_sample dup = mk_sample(LIGHT, 5, 1, OC_ST_OK);
    CHECK_EQ(oc_ipc_send_sample(&c, &dup), 0);
    pump(&f, 3);
    CHECK_EQ(status_of(&f, LIGHT).seq.ts_backwards, 2);

    /* a supervisor-authorized new instance starts a fresh timestamp history */
    CHECK_EQ(oc_hub_authorize(&f.hub, LIGHT, 2), 0);
    oc_ipc_conn_close(&c);
    struct oc_ipc_conn n;
    CHECK(join_ok(&f, &n, LIGHT, 2));
    struct oc_sample first = mk_sample(LIGHT, 0, 3, OC_ST_OK);
    CHECK_EQ(oc_ipc_send_sample(&n, &first), 0);
    pump(&f, 3);
    e = nth_sample(LIGHT, 6);
    CHECK(e != nullptr && !e->si.ts_backwards);
    CHECK_EQ(status_of(&f, LIGHT).seq.ts_backwards, 0);
    oc_ipc_conn_close(&n);
    fx_end(&f);
}

static void test_gaps_and_backward_time_are_not_strikes(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn c;
    CHECK(join_ok(&f, &c, LIGHT, 1));
    /* far more events than OC_HUB_STRIKE_LIMIT on ONE connection */
    for (uint32_t i = 0; i < 3 * OC_HUB_STRIKE_LIMIT; i++) {
        struct oc_sample s = mk_sample(LIGHT, 2 * i, 100000 - 10 * i, OC_ST_OK);   /* gap AND time going back */
        int tries = 0, rc;
        while ((rc = oc_ipc_send_sample(&c, &s)) == -EAGAIN && tries++ < 100)
            pump(&f, 1);
        CHECK_EQ(rc, 0);
    }
    pump(&f, 6);
    struct oc_hub_source_status st = status_of(&f, LIGHT);
    CHECK_EQ(st.state, OC_HUB_SRC_ACTIVE);
    CHECK_EQ(stats_of(&f).abusive_closes, 0);
    CHECK_EQ(st.seq.accepted, 3 * OC_HUB_STRIKE_LIMIT);
    CHECK_EQ(st.seq.gap_events, 3 * OC_HUB_STRIKE_LIMIT - 1);
    CHECK_EQ(st.seq.lost, 3 * OC_HUB_STRIKE_LIMIT - 1);
    CHECK_EQ(st.seq.ts_backwards, 3 * OC_HUB_STRIKE_LIMIT - 1);
    CHECK_EQ(sink.samples[LIGHT], 3 * OC_HUB_STRIKE_LIMIT);
    CHECK(oc_seq_consistent(&st.seq));
    oc_ipc_conn_close(&c);
    fx_end(&f);
}

static void test_callback_cannot_reenter(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    struct oc_ipc_conn c;
    CHECK(join_ok(&f, &c, LIGHT, 1));
    sink.probe = true;
    send_n(&f, &c, LIGHT, 0, 2);
    pump(&f, 3);
    CHECK_EQ(sink.probes, 1);
    CHECK_EQ(sink.probe_rc[0], -EDEADLK);
    CHECK_EQ(sink.probe_rc[1], -EDEADLK);
    CHECK_EQ(sink.probe_rc[2], -EDEADLK);
    CHECK_EQ(sink.samples[LIGHT], 2);                    /* the hub carried on normally */
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_ACTIVE);

    /* the same from an EVENT callback (here SOURCE_DOWN of a closing connection) */
    sink.probe_event = true;
    oc_ipc_conn_close(&c);
    pump(&f, 4);
    CHECK_EQ(sink.event_probes, 1);
    CHECK_EQ(sink.event_probe_rc[0], -EDEADLK);
    CHECK_EQ(sink.event_probe_rc[1], -EDEADLK);
    CHECK_EQ(sink.event_probe_rc[2], -EDEADLK);
    CHECK(f.hub.open);                                   /* shutdown did not run */
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_DISCONNECTED);
    CHECK_EQ(status_of(&f, LIGHT).authorized_instance, 1);   /* authorize(77) did not apply */
    oc_ipc_conn_close(&c);
    fx_end(&f);
}

static void test_poll_setup_and_dispatch(void)
{
    struct fx f;
    fx_begin(&f);
    f.cfg.hello_timeout_ms = 400;
    CHECK(fx_start(&f));
    struct pollfd fds[OC_HUB_MAX_POLLFDS];
    int timeout = -1;
    CHECK_EQ(oc_hub_poll_setup(&f.hub, fds, OC_HUB_MAX_POLLFDS - 1, &timeout), 0);   /* cap too small */
    timeout = -1;
    size_t n = oc_hub_poll_setup(&f.hub, fds, OC_HUB_MAX_POLLFDS, &timeout);
    CHECK_EQ(n, 1);
    CHECK_EQ(fds[0].fd, f.hub.server.fd);
    CHECK_EQ(timeout, -1);                               /* nothing pending: no deadline */

    struct oc_ipc_conn silent, l;
    CHECK(cl_connect(&f, &silent));
    pump(&f, 2);
    timeout = -1;
    n = oc_hub_poll_setup(&f.hub, fds, OC_HUB_MAX_POLLFDS, &timeout);
    CHECK_EQ(n, 2);
    CHECK(timeout >= 0 && timeout <= 400);               /* clamped to the HELLO deadline */
    timeout = 5;
    oc_hub_poll_setup(&f.hub, fds, OC_HUB_MAX_POLLFDS, &timeout);
    CHECK(timeout <= 5);                                 /* a shorter wish is kept */
    oc_ipc_conn_close(&silent);
    pump(&f, 3);

    /* a poll result that goes stale (the connection is closed meanwhile) is ignored */
    CHECK(join_ok(&f, &l, LIGHT, 1));
    send_n(&f, &l, LIGHT, 0, 2);
    timeout = 0;
    n = oc_hub_poll_setup(&f.hub, fds, OC_HUB_MAX_POLLFDS, &timeout);
    CHECK(n >= 2);
    int pr = poll(fds, (nfds_t)n, 0);
    CHECK(pr >= 1);
    CHECK_EQ(oc_hub_authorize(&f.hub, LIGHT, 3), 0);     /* supersedes: closes that connection */
    CHECK(oc_hub_dispatch(&f.hub, fds, n) >= 0);
    CHECK_EQ(sink.samples[LIGHT], 0);                    /* nothing read from a closed connection */
    CHECK(oc_hub_dispatch(&f.hub, fds, n) >= 0);
    CHECK_EQ(oc_hub_dispatch(&f.hub, nullptr, 3), -EINVAL);
    oc_ipc_conn_close(&l);
    fx_end(&f);
}

static void test_connection_cycles_leak_nothing(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));
    int baseline = fd_count();                           /* includes the listening socket */
    for (int i = 0; i < 200; i++) {
        struct oc_ipc_conn c;
        if (!join_ok(&f, &c, i % 2 ? DIST : LIGHT, 1))
            break;
        send_n(&f, &c, i % 2 ? DIST : LIGHT, (uint32_t)(i / 2) * 3, 3);
        oc_ipc_conn_close(&c);
        for (int k = 0; k < 6; k++)
            pump(&f, 1);
    }
    pump(&f, 4);
    CHECK_EQ(fd_count(), baseline);
    CHECK_EQ(sink.samples[LIGHT], 300);
    CHECK_EQ(sink.samples[DIST], 300);
    CHECK_EQ(status_of(&f, LIGHT).connections, 100);
    CHECK_EQ(status_of(&f, LIGHT).seq.lost, 0);
    CHECK_EQ(sink.overflow, 0);

    for (int i = 0; i < 50; i++) {                       /* rejected connections too */
        struct oc_hello h = hello_for(LIGHT, 1);
        h.session_id = SESSION ^ 1;
        struct oc_ipc_conn c;
        CHECK(cl_connect(&f, &c));
        uint16_t why = 0;
        CHECK_EQ(hello_exchange(&f, &c, &h, &why), OC_MSG_HELLO_REJECT);
        oc_ipc_conn_close(&c);
        pump(&f, 2);
    }
    CHECK_EQ(fd_count(), baseline);

    /* clean shutdown */
    struct oc_ipc_conn live;
    CHECK(join_ok(&f, &live, LIGHT, 1));
    oc_hub_shutdown(&f.hub);
    struct tail t = read_tail(&live);
    CHECK_EQ(t.first_type, OC_MSG_BYE);
    CHECK_EQ(t.bye_reason, OC_BYE_SHUTDOWN);
    CHECK(t.eof);
    oc_ipc_conn_close(&live);
    struct stat sb;
    CHECK_EQ(lstat(f.path, &sb), -1);                    /* socket path removed */
    CHECK_EQ(fd_count(), f.fds_before);
    CHECK(count_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT) >= 1);
    CHECK_EQ(find_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT, count_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT) - 1)->reason, OC_HUB_CLOSE_SHUTDOWN);
    CHECK_EQ(oc_hub_step(&f.hub, 0), -ENOTCONN);         /* a stopped hub refuses work */
    CHECK_EQ(oc_hub_authorize(&f.hub, LIGHT, 9), -ENOTCONN);
    oc_hub_shutdown(&f.hub);                             /* idempotent */
    f.up = false;
    if (f.dir_made)
        (void)nftw(f.dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

static void test_init_validation(void)
{
    struct fx f;
    fx_begin(&f);
    struct oc_hub h;
    struct oc_hub_cfg c = f.cfg;
    CHECK_EQ(oc_hub_init(nullptr, &c), -EINVAL);
    CHECK_EQ(oc_hub_init(&h, nullptr), -EINVAL);
    c = f.cfg; c.socket_path = nullptr;
    CHECK_EQ(oc_hub_init(&h, &c), -EINVAL);
    c = f.cfg; c.session_id = 0;                         /* never invented, never zero */
    CHECK_EQ(oc_hub_init(&h, &c), -EINVAL);
    c = f.cfg; c.max_pending = OC_HUB_MAX_PENDING + 1;
    CHECK_EQ(oc_hub_init(&h, &c), -EINVAL);
    c = f.cfg; c.origin[LIGHT] = OC_ORIGIN_MIXED;
    CHECK_EQ(oc_hub_init(&h, &c), -EINVAL);
    CHECK_EQ(fd_count(), f.fds_before);                  /* nothing left open by failures */
    struct stat sb;
    CHECK_EQ(lstat(f.path, &sb), -1);

    CHECK(fx_start(&f));
    struct oc_hub h2;
    CHECK_EQ(oc_hub_init(&h2, &f.cfg), -EADDRINUSE);     /* one hub per socket */
    struct oc_hub_source_status st;
    CHECK_EQ(oc_hub_source_status(&f.hub, 0, &st), -EINVAL);
    CHECK_EQ(oc_hub_source_status(&f.hub, 3, &st), -EINVAL);
    fx_end(&f);
}

/* ---------- real client processes ---------- */

[[noreturn]] static void client_process(const char *path, uint32_t first, uint32_t samples,
                                         bool forever)
{
    struct oc_ipc_conn c;
    oc_ipc_conn_init(&c);
    int rc = -EAGAIN;
    for (int i = 0; i < 200 && rc != 0; i++) {
        rc = oc_ipc_client_connect(&c, path);
        if (rc != 0)
            ms_sleep(10);
    }
    if (rc != 0)
        _exit(10);
    struct oc_hello h = {.source = LIGHT, .origin = SIM, .session_id = SESSION,
                         .instance = 1, .pid = (uint32_t)getpid()};
    if (oc_ipc_client_handshake(&c, &h, 3000, nullptr) != 0)
        _exit(11);
    for (uint32_t i = first; forever || i < first + samples; i++) {
        struct oc_sample s = mk_sample(LIGHT, i, 1000u + i, OC_ST_OK);
        int tries = 0;
        while ((rc = oc_ipc_send_sample(&c, &s)) == -EAGAIN && tries++ < 3000)
            ms_sleep(1);
        if (rc != 0)
            _exit(12);
    }
    oc_ipc_conn_close(&c);
    _exit(0);
}

static void test_real_client_processes(void)
{
    struct fx f;
    fx_begin(&f);
    CHECK(fx_start(&f));

    /* (1) a client process sends 50 samples and exits */
    pid_t pid = fork();
    if (pid == 0)
        client_process(f.path, 0, 50, false);
    CHECK(pid > 0);
    uint64_t t0 = now_ns();
    while (count_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT) == 0 && now_ns() - t0 < 5000000000ull)
        CHECK(oc_hub_step(&f.hub, 20) >= 0);
    int st = 0;
    CHECK_EQ(waitpid(pid, &st, 0), pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK_EQ(sink.samples[LIGHT], 50);
    CHECK_EQ(status_of(&f, LIGHT).seq.lost, 0);
    CHECK_EQ(ev_or0(OC_HUB_EV_SOURCE_DOWN, LIGHT, 0)->reason, OC_HUB_CLOSE_EOF);
    CHECK_EQ(stats_of(&f).rejects[OC_REJECT_PEER_CRED], 0);   /* real SO_PEERCRED matched */

    /* (2) a client process is killed with SIGKILL while sending */
    pid = fork();
    if (pid == 0)
        client_process(f.path, 50, 0, true);   /* same instance: seq continues */
    CHECK(pid > 0);
    t0 = now_ns();
    while (sink.samples[LIGHT] < 60 && now_ns() - t0 < 5000000000ull)
        CHECK(oc_hub_step(&f.hub, 20) >= 0);
    CHECK(sink.samples[LIGHT] >= 60);               /* 50 from the first client + more */
    CHECK_EQ(kill(pid, SIGKILL), 0);
    CHECK_EQ(waitpid(pid, &st, 0), pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    t0 = now_ns();
    while (count_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT) < 2 && now_ns() - t0 < 5000000000ull)
        CHECK(oc_hub_step(&f.hub, 20) >= 0);
    const struct oc_hub_event *down = find_ev(OC_HUB_EV_SOURCE_DOWN, LIGHT, 1);
    CHECK(down != nullptr &&
          (down->reason == OC_HUB_CLOSE_EOF || down->reason == OC_HUB_CLOSE_RESET));
    CHECK_EQ(status_of(&f, LIGHT).state, OC_HUB_SRC_DISCONNECTED);
    CHECK(oc_hub_degraded(&f.hub));
    struct oc_hub_source_status ss = status_of(&f, LIGHT);
    CHECK_EQ(ss.seq.lost, 0);       /* the child retried each seq; nothing skipped */
    CHECK_EQ(ss.seq.duplicates + ss.seq.regressions, 0);
    CHECK_EQ(ss.connections, 2);
    fx_end(&f);
    CHECK_EQ(fd_count(), f.fds_before);
}

int main(void)
{
    RUN(test_init_validation);
    RUN(test_hello_both_sources);
    RUN(test_mixed_origin_session);
    RUN(test_sample_before_hello);
    RUN(test_hello_rejections);
    RUN(test_duplicate_active_source);
    RUN(test_same_instance_reconnect);
    RUN(test_authorized_new_instance);
    RUN(test_sequence_via_hub);
    RUN(test_failed_acquisition_statuses);
    RUN(test_identity_mismatch);
    RUN(test_abuse_policy);
    RUN(test_unsupported_version);
    RUN(test_hello_timeout_and_pending_cap);
    RUN(test_disconnect_after_queued_samples);
    RUN(test_fair_processing);
    RUN(test_ack_send_failure);
    RUN(test_timestamp_anomalies);
    RUN(test_gaps_and_backward_time_are_not_strikes);
    RUN(test_callback_cannot_reenter);
    RUN(test_poll_setup_and_dispatch);
    RUN(test_connection_cycles_leak_nothing);
    RUN(test_real_client_processes);

    if (failures != 0) {
        fprintf(stderr, "test_hub: %d FAILURE(S)\n", failures);
        return 1;
    }
    fprintf(stderr, "test_hub: all passed\n");
    return 0;
}
