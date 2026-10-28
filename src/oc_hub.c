#define _POSIX_C_SOURCE 200809L

#include "oc_hub.h"

#include <errno.h>
#include <string.h>
#include <time.h>

/* ---------- small helpers ---------- */

static bool source_valid(uint8_t source)
{
    return source == OC_SOURCE_LIGHT || source == OC_SOURCE_DISTANCE;
}

static int mono_now(uint64_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -errno;
    *out = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return 0;
}

/* ---------- consumer callbacks ---------- */

static void emit(struct oc_hub *h, const struct oc_hub_event *ev)
{
    if (h->cfg.on_event == nullptr)
        return;
    h->in_callback = true;
    h->cfg.on_event(h->cfg.ctx, ev);
    h->in_callback = false;
}

static bool compute_degraded(const struct oc_hub *h)
{
    for (uint8_t s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++)
        if (h->src[s].authorized && h->src[s].state != OC_HUB_SRC_ACTIVE)
            return true;
    return false;
}

static void update_degraded(struct oc_hub *h)
{
    bool d = compute_degraded(h);
    if (d == h->degraded)
        return;
    h->degraded = d;
    emit(h, &(struct oc_hub_event){.kind = OC_HUB_EV_DEGRADED_CHANGED, .degraded = d});
}

/* ---------- connection slots ---------- */

static void slot_clear(struct oc_hub_slot *s)
{
    oc_ipc_conn_close(&s->conn);
    s->state = OC_HUB_SLOT_FREE;
    s->deadline_ns = 0;
    s->source = 0;
    s->origin = 0;
    s->instance = 0;
    s->score = 0;
    s->good = 0;
    s->budget = 0;
    s->first_on_connection = false;
}

static void best_effort_bye(struct oc_hub_slot *s, uint16_t reason)
{
    uint8_t b[OC_IPC_MAX_MSG];
    size_t n = 0;
    struct oc_bye y = {.reason = reason};
    if (oc_ipc_encode_bye(&y, b, sizeof b, &n) == 0)
        (void)oc_ipc_send_msg(&s->conn, b, n);   /* courtesy only: failure changes nothing */
}

/* Ends a connection of either kind; ACTIVE ones take their source down. */
static void end_conn(struct oc_hub *h, unsigned i, enum oc_hub_close_reason why)
{
    struct oc_hub_slot *s = &h->slot[i];
    if (s->state == OC_HUB_SLOT_FREE)
        return;
    if (s->state == OC_HUB_SLOT_ACTIVE) {
        struct oc_hub_src *src = &h->src[s->source];
        struct oc_hub_event ev = {.kind = OC_HUB_EV_SOURCE_DOWN, .source = s->source,
                                  .instance = s->instance, .generation = src->generation,
                                  .reason = why};
        /* A connection of an instance that is no longer the authorized one leaves
         * the source waiting for the new instance, already visible to the
         * callbacks below (SUPERSEDED). */
        src->state = s->instance != src->authorized_instance ? OC_HUB_SRC_AWAITING
                                                             : OC_HUB_SRC_DISCONNECTED;
        src->slot = -1;
        slot_clear(s);
        emit(h, &ev);
        update_degraded(h);
    } else {
        slot_clear(s);
        emit(h, &(struct oc_hub_event){.kind = OC_HUB_EV_CONN_CLOSED, .reason = why});
    }
}

static void reject_and_close(struct oc_hub *h, unsigned i, enum oc_reject_reason why,
                             const struct oc_hello *hello)
{
    struct oc_hub_slot *s = &h->slot[i];
    uint8_t b[OC_IPC_MAX_MSG];
    size_t n = 0;
    struct oc_hello_reject r = {.reason = (uint16_t)why, .detail = 0};
    if (oc_ipc_encode_hello_reject(&r, b, sizeof b, &n) == 0)
        (void)oc_ipc_send_msg(&s->conn, b, n);   /* courtesy; the close tells the story */
    h->stats.rejects[why]++;
    struct oc_hub_event ev = {.kind = OC_HUB_EV_CONN_REJECTED, .reject = (uint16_t)why};
    if (hello != nullptr) {
        ev.source = hello->source;
        ev.instance = hello->instance;
    }
    slot_clear(s);
    emit(h, &ev);
}

/* One strike. Returns true if the connection was closed for abuse. */
static bool strike(struct oc_hub *h, unsigned i)
{
    struct oc_hub_slot *s = &h->slot[i];
    if (++s->score < OC_HUB_STRIKE_LIMIT)
        return false;
    best_effort_bye(s, OC_BYE_ERROR);
    h->stats.abusive_closes++;
    end_conn(h, i, OC_HUB_CLOSE_ABUSE);
    return true;
}

static void forgive(struct oc_hub_slot *s)
{
    if (++s->good >= OC_HUB_FORGIVE_EVERY) {
        s->good = 0;
        if (s->score > 0)
            s->score--;
    }
}

/* ---------- servicing a connection ---------- */

static int service_slot(struct oc_hub *h, unsigned i);

static void handle_hello(struct oc_hub *h, unsigned i, const struct oc_hello *hello)
{
    struct oc_hub_slot *slot = &h->slot[i];

    enum oc_reject_reason why = oc_ipc_hello_check(&h->policy, hello, &(struct oc_ipc_peer){
        .pid = slot->conn.peer.pid, .uid = slot->conn.peer.uid, .gid = slot->conn.peer.gid});
    if (why != OC_REJECT_NONE) {
        reject_and_close(h, i, why, hello);
        return;
    }
    struct oc_hub_src *src = &h->src[hello->source];   /* source is 1 or 2 here */
    if (!src->authorized) {
        reject_and_close(h, i, OC_REJECT_NOT_READY, hello);
        return;
    }
    if (hello->instance != src->authorized_instance) {
        reject_and_close(h, i, OC_REJECT_STALE_INSTANCE, hello);
        return;
    }
    if (src->state == OC_HUB_SRC_ACTIVE && src->slot >= 0 && (unsigned)src->slot != i) {
        /* Is the incumbent really gone? Look, but only as far as ITS OWN budget
         * for this step allows; never drain it to settle the question. */
        service_slot(h, (unsigned)src->slot);
        if (src->state == OC_HUB_SRC_ACTIVE) {
            reject_and_close(h, i, OC_REJECT_DUPLICATE_SOURCE, hello);
            return;
        }
    }

    /* The source becomes ACTIVE only if the ACK really went out. */
    uint8_t b[OC_IPC_MAX_MSG];
    size_t n = 0;
    struct oc_hello_ack ack = {.session_id = hello->session_id, .instance = hello->instance,
                               .source = hello->source};
    int rc = oc_ipc_encode_hello_ack(&ack, b, sizeof b, &n);
    if (rc == 0)
        rc = oc_ipc_send_msg(&slot->conn, b, n);
    if (rc != 0) {
        h->stats.ack_failures++;
        end_conn(h, i, OC_HUB_CLOSE_ACK_FAILED);         /* source and history untouched */
        return;
    }

    bool had_data = src->has_data;
    bool new_instance = !had_data || src->data_instance != hello->instance;
    if (new_instance) {
        oc_seq_reset(&src->seq);
        memset(src->status_count, 0, sizeof src->status_count);
        src->identity_mismatches = 0;
        src->generation++;
        src->has_data = true;
        src->data_instance = hello->instance;
        if (had_data)
            src->resets++;
    } else {
        src->reconnects++;
    }
    src->connections++;
    src->state = OC_HUB_SRC_ACTIVE;
    src->slot = (int)i;

    slot->state = OC_HUB_SLOT_ACTIVE;
    slot->source = hello->source;
    slot->origin = hello->origin;
    slot->instance = hello->instance;
    slot->score = 0;
    slot->good = 0;
    slot->first_on_connection = true;

    emit(h, &(struct oc_hub_event){.kind = OC_HUB_EV_SOURCE_UP, .source = hello->source,
                                   .instance = hello->instance, .generation = src->generation,
                                   .reconnect = !new_instance, .reset = new_instance && had_data});
    update_degraded(h);
}

/* Returns true if the connection was closed (so the caller stops reading). */
static bool handle_sample(struct oc_hub *h, unsigned i, const struct oc_sample *s)
{
    struct oc_hub_slot *slot = &h->slot[i];
    struct oc_hub_src *src = &h->src[slot->source];

    if (s->source != slot->source || s->origin != slot->origin) {
        src->identity_mismatches++;
        return strike(h, i);
    }
    struct oc_seq_result r = oc_seq_observe(&src->seq, s->seq, s->t_mono_ns);
    if (r.verdict == OC_SEQ_DUPLICATE || r.verdict == OC_SEQ_REGRESSION)
        return strike(h, i);        /* rejected: not delivered, nothing advanced */

    forgive(slot);
    src->status_count[s->status]++;
    uint64_t now = 0;
    if (mono_now(&now) == 0)
        src->last_rx_ns = now;
    else
        now = src->last_rx_ns;      /* clock unreadable: reuse the last good stamp */

    if (h->cfg.on_sample != nullptr) {
        struct oc_hub_sample_info info = {
            .sample = s,
            .session_id = h->cfg.session_id,
            .instance = slot->instance,
            .generation = src->generation,
            .source = slot->source,
            .origin = slot->origin,
            .gap = r.gap,
            .first_of_instance = r.verdict == OC_SEQ_FIRST,
            .first_on_connection = slot->first_on_connection,
            .ts_backwards = r.ts_backwards,
            .hub_rx_ns = now,
        };
        h->in_callback = true;
        h->cfg.on_sample(h->cfg.ctx, &info);
        h->in_callback = false;
    }
    slot->first_on_connection = false;
    return false;
}

/* Reads at most the connection's remaining budget. Queued datagrams are read
 * before EOF is acted on, whatever poll reported (POLLIN, POLLHUP or both).
 * Returns the number of datagrams consumed. */
static int service_slot(struct oc_hub *h, unsigned i)
{
    struct oc_hub_slot *slot = &h->slot[i];
    int consumed = 0;

    while (slot->state != OC_HUB_SLOT_FREE && slot->budget > 0) {
        struct oc_ipc_msg m;
        uint64_t oversized_before = slot->conn.st.rx_oversized;
        int rc = oc_ipc_recv(&slot->conn, &m);

        if (rc == -EAGAIN)
            break;
        if (rc == -EPIPE) {
            end_conn(h, i, OC_HUB_CLOSE_EOF);
            break;
        }
        if (rc == -ECONNRESET) {
            end_conn(h, i, OC_HUB_CLOSE_RESET);
            break;
        }
        if (rc != 0 && rc != -EMSGSIZE && rc != -EBADMSG && rc != -EPROTONOSUPPORT) {
            end_conn(h, i, OC_HUB_CLOSE_ERROR);
            break;
        }

        slot->budget--;
        consumed++;
        h->stats.datagrams++;

        if (rc == -EPROTONOSUPPORT) {                    /* already closed by the transport */
            h->stats.version_fatal++;
            end_conn(h, i, OC_HUB_CLOSE_VERSION);
            break;
        }
        if (rc != 0) {                                   /* consumed, counted, in sync */
            if (slot->conn.st.rx_oversized != oversized_before)
                h->stats.oversized++;
            else
                h->stats.malformed++;
            if (slot->state == OC_HUB_SLOT_PENDING) {
                reject_and_close(h, i, OC_REJECT_MALFORMED, nullptr);
                break;
            }
            if (strike(h, i))
                break;
            continue;
        }

        if (slot->state == OC_HUB_SLOT_PENDING) {
            if (m.type == OC_MSG_HELLO) {
                handle_hello(h, i, &m.u.hello);
            } else if (m.type == OC_MSG_BYE) {
                end_conn(h, i, OC_HUB_CLOSE_BYE);
            } else {
                h->stats.violations++;
                best_effort_bye(slot, OC_BYE_ERROR);
                end_conn(h, i, OC_HUB_CLOSE_PRE_HELLO);
            }
            continue;                                    /* ACTIVE now? keep reading samples */
        }

        /* ACTIVE */
        if (m.type == OC_MSG_SAMPLE) {
            if (handle_sample(h, i, &m.u.sample))
                break;
        } else if (m.type == OC_MSG_BYE) {
            end_conn(h, i, OC_HUB_CLOSE_BYE);
            break;
        } else {
            h->stats.violations++;                       /* HELLO/ACK/REJECT mid-stream */
            if (strike(h, i))
                break;
        }
    }
    return consumed;
}

/* ---------- accepting ---------- */

static unsigned pending_count(const struct oc_hub *h)
{
    unsigned n = 0;
    for (unsigned i = 0; i < OC_HUB_MAX_CONNS; i++)
        if (h->slot[i].state == OC_HUB_SLOT_PENDING)
            n++;
    return n;
}

static int free_slot(const struct oc_hub *h)
{
    for (unsigned i = 0; i < OC_HUB_MAX_CONNS; i++)
        if (h->slot[i].state == OC_HUB_SLOT_FREE)
            return (int)i;
    return -1;
}

static void accept_connections(struct oc_hub *h, uint64_t now)
{
    /* bounded per step: at most every slot plus a few shed connections */
    for (unsigned k = 0; k < OC_HUB_MAX_CONNS + 4; k++) {
        int f = free_slot(h);
        bool room = f >= 0 && pending_count(h) < h->cfg.max_pending;
        struct oc_ipc_conn shed;
        oc_ipc_conn_init(&shed);
        struct oc_ipc_conn *dst = room ? &h->slot[f].conn : &shed;

        int rc = oc_ipc_server_accept(&h->server, dst);
        if (rc == -EAGAIN)
            break;
        if (rc == -ECONNABORTED)
            continue;
        if (rc != 0) {
            h->stats.accept_errors++;                    /* e.g. EMFILE; retried next step */
            break;
        }
        h->stats.conns_accepted++;
        if (!room) {
            h->stats.shed++;
            oc_ipc_conn_close(&shed);
            emit(h, &(struct oc_hub_event){.kind = OC_HUB_EV_CONN_CLOSED,
                                           .reason = OC_HUB_CLOSE_SHED});
            continue;
        }
        struct oc_hub_slot *s = &h->slot[f];
        s->state = OC_HUB_SLOT_PENDING;
        s->deadline_ns = now + (uint64_t)h->cfg.hello_timeout_ms * 1000000ull;
    }
}

/* ---------- public API ---------- */

int oc_hub_init(struct oc_hub *h, const struct oc_hub_cfg *cfg)
{
    if (h == nullptr || cfg == nullptr || cfg->socket_path == nullptr || cfg->session_id == 0)
        return -EINVAL;
    if (cfg->max_pending > OC_HUB_MAX_PENDING || cfg->hello_timeout_ms < 0)
        return -EINVAL;
    for (uint8_t s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
        uint8_t o = cfg->origin[s];
        if (o != 0 && o != OC_ORIGIN_LIVE && o != OC_ORIGIN_SIMULATED && o != OC_ORIGIN_REPLAYED)
            return -EINVAL;
    }

    *h = (struct oc_hub){0};
    h->cfg = *cfg;
    if (h->cfg.hello_timeout_ms == 0)
        h->cfg.hello_timeout_ms = OC_HUB_DEFAULT_HELLO_TIMEOUT_MS;
    if (h->cfg.max_pending == 0)
        h->cfg.max_pending = OC_HUB_MAX_PENDING;
    if (h->cfg.batch == 0)
        h->cfg.batch = OC_HUB_DEFAULT_BATCH;
    h->cfg.socket_path = nullptr;                        /* not kept: the server owns a copy */

    h->policy = (struct oc_ipc_policy){.session_id = cfg->session_id, .uid = cfg->uid,
                                       .check_pid = cfg->check_pid};
    for (uint8_t s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++)
        h->policy.origin[s] = cfg->origin[s];

    oc_ipc_server_init(&h->server);
    for (unsigned i = 0; i < OC_HUB_MAX_CONNS; i++)
        oc_ipc_conn_init(&h->slot[i].conn);
    for (uint8_t s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++)
        h->src[s].slot = -1;

    int rc = oc_ipc_server_open(&h->server, cfg->socket_path);
    if (rc != 0)
        return rc;
    h->open = true;

    for (uint8_t s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
        if (!cfg->initial[s].authorized)
            continue;
        rc = oc_hub_authorize(h, s, cfg->initial[s].instance);
        if (rc != 0) {
            oc_hub_shutdown(h);
            return rc;
        }
    }
    return 0;
}

int oc_hub_authorize(struct oc_hub *h, uint8_t source, uint32_t instance)
{
    if (h == nullptr || !source_valid(source))
        return -EINVAL;
    if (h->in_callback)
        return -EDEADLK;
    if (!h->open)
        return -ENOTCONN;

    struct oc_hub_src *src = &h->src[source];
    if (src->have_max && instance < src->max_authorized)
        return -ESTALE;                                  /* no going back to an old reader */
    if (src->authorized && instance == src->authorized_instance)
        return 0;                                        /* idempotent */

    src->authorized = true;
    src->authorized_instance = instance;
    src->have_max = true;
    src->max_authorized = instance;

    if (src->state == OC_HUB_SRC_ACTIVE && src->slot >= 0)
        end_conn(h, (unsigned)src->slot, OC_HUB_CLOSE_SUPERSEDED);
    /* History (tracker, counters) stays until the new instance's HELLO is ACKed. */
    src->state = OC_HUB_SRC_AWAITING;
    update_degraded(h);
    return 0;
}

size_t oc_hub_poll_setup(const struct oc_hub *h, struct pollfd *fds, size_t cap, int *timeout_ms)
{
    if (h == nullptr || fds == nullptr || timeout_ms == nullptr || !h->open ||
        cap < OC_HUB_MAX_POLLFDS)
        return 0;
    size_t n = 0;
    fds[n++] = (struct pollfd){.fd = h->server.fd, .events = POLLIN};

    bool have_deadline = false;
    uint64_t nearest = 0;
    for (unsigned i = 0; i < OC_HUB_MAX_CONNS; i++) {
        const struct oc_hub_slot *s = &h->slot[i];
        if (s->state == OC_HUB_SLOT_FREE)
            continue;
        fds[n++] = (struct pollfd){.fd = s->conn.fd, .events = POLLIN};
        if (s->state == OC_HUB_SLOT_PENDING && (!have_deadline || s->deadline_ns < nearest)) {
            have_deadline = true;
            nearest = s->deadline_ns;
        }
    }
    if (have_deadline) {
        uint64_t now = 0;
        int rem = 0;
        if (mono_now(&now) == 0 && nearest > now) {
            uint64_t ms = (nearest - now + 999999ull) / 1000000ull;
            rem = ms > 0x7FFFFFFFull ? 0x7FFFFFFF : (int)ms;
        }                                                /* clock error or expired: poll now */
        if (*timeout_ms < 0 || rem < *timeout_ms)
            *timeout_ms = rem;
    }
    return n;
}

int oc_hub_dispatch(struct oc_hub *h, const struct pollfd *fds, size_t n)
{
    if (h == nullptr)
        return -EINVAL;
    if (h->in_callback)
        return -EDEADLK;
    if (!h->open)
        return -ENOTCONN;
    if (fds == nullptr && n != 0)
        return -EINVAL;

    uint64_t now = 0;
    int rc = mono_now(&now);
    if (rc != 0)
        return rc;

    bool listen_ready = false;
    bool ready[OC_HUB_MAX_CONNS] = {false};
    for (unsigned i = 0; i < OC_HUB_MAX_CONNS; i++)
        h->slot[i].budget = h->cfg.batch;

    /* Match poll results to connections by descriptor and current ownership, so
     * an entry that went stale since poll_setup is simply ignored. */
    for (size_t k = 0; k < n; k++) {
        if (fds[k].revents == 0)
            continue;
        if (fds[k].fd == h->server.fd) {
            listen_ready = true;
            continue;
        }
        for (unsigned i = 0; i < OC_HUB_MAX_CONNS; i++)
            if (h->slot[i].state != OC_HUB_SLOT_FREE && h->slot[i].conn.open &&
                h->slot[i].conn.fd == fds[k].fd)
                ready[i] = true;
    }

    const uint64_t datagrams_before = h->stats.datagrams;
    for (unsigned j = 0; j < OC_HUB_MAX_CONNS; j++) {    /* rotating start: no fixed favourite */
        unsigned i = (h->rr + j) % OC_HUB_MAX_CONNS;
        if (ready[i] && h->slot[i].state != OC_HUB_SLOT_FREE)
            (void)service_slot(h, i);
    }
    h->rr = (h->rr + 1) % OC_HUB_MAX_CONNS;

    for (unsigned i = 0; i < OC_HUB_MAX_CONNS; i++) {
        struct oc_hub_slot *s = &h->slot[i];
        if (s->state == OC_HUB_SLOT_PENDING && now >= s->deadline_ns) {
            h->stats.hello_timeouts++;
            slot_clear(s);
            emit(h, &(struct oc_hub_event){.kind = OC_HUB_EV_HELLO_TIMEOUT});
        }
    }

    if (listen_ready)
        accept_connections(h, now);
    return (int)(h->stats.datagrams - datagrams_before);
}

int oc_hub_step(struct oc_hub *h, int timeout_ms)
{
    if (h == nullptr)
        return -EINVAL;
    if (h->in_callback)
        return -EDEADLK;
    if (!h->open)
        return -ENOTCONN;
    struct pollfd fds[OC_HUB_MAX_POLLFDS];
    int t = timeout_ms;
    size_t n = oc_hub_poll_setup(h, fds, OC_HUB_MAX_POLLFDS, &t);
    if (n == 0)
        return -EINVAL;
    int r = poll(fds, (nfds_t)n, t);
    if (r < 0)
        return errno == EINTR ? 0 : -errno;
    return oc_hub_dispatch(h, fds, n);                   /* also when r == 0: deadlines */
}

int oc_hub_source_status(const struct oc_hub *h, uint8_t source, struct oc_hub_source_status *out)
{
    if (h == nullptr || out == nullptr || !source_valid(source))
        return -EINVAL;
    const struct oc_hub_src *s = &h->src[source];
    *out = (struct oc_hub_source_status){
        .state = s->state,
        .available = s->state == OC_HUB_SRC_ACTIVE,
        .authorized = s->authorized,
        .authorized_instance = s->authorized_instance,
        .instance = s->data_instance,
        .generation = s->generation,
        .seq = s->seq,
        .identity_mismatches = s->identity_mismatches,
        .connections = s->connections,
        .reconnects = s->reconnects,
        .resets = s->resets,
        .last_rx_ns = s->last_rx_ns,
    };
    memcpy(out->status_count, s->status_count, sizeof out->status_count);
    return 0;
}

void oc_hub_stats(const struct oc_hub *h, struct oc_hub_stats *out)
{
    if (out == nullptr)
        return;
    if (h == nullptr)
        memset(out, 0, sizeof *out);
    else
        *out = h->stats;
}

bool oc_hub_degraded(const struct oc_hub *h)
{
    return h != nullptr && compute_degraded(h);
}

void oc_hub_shutdown(struct oc_hub *h)
{
    if (h == nullptr || h->in_callback || !h->open)
        return;
    for (unsigned i = 0; i < OC_HUB_MAX_CONNS; i++) {
        if (h->slot[i].state == OC_HUB_SLOT_FREE)
            continue;
        best_effort_bye(&h->slot[i], OC_BYE_SHUTDOWN);
        end_conn(h, i, OC_HUB_CLOSE_SHUTDOWN);
    }
    oc_ipc_server_close(&h->server);
    h->open = false;
}
