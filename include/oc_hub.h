#ifndef OC_HUB_H
#define OC_HUB_H

/* oc_hub: the hub's receiver and per-source state machine, built on the S3A
 * transport (oc_ipc). Single-threaded, poll-based, fixed-size, no allocation.
 *
 * STATUS: [REVIEW] host-tested with SIMULATED inputs over real AF_UNIX
 * SOCK_SEQPACKET sockets. It issues no diagnostic result and fabricates no
 * sample; storage and analysis attach through the callbacks below.
 *
 * ---- Address spaces (S4) ----
 * The supervisor runs in another process. Authorization therefore reaches the
 * hub as a COMMAND that the hub process turns into a local call of
 * oc_hub_authorize(). Nothing here assumes shared memory: authorization,
 * state, events and samples are plain values. The control protocol itself is
 * not defined in this slice.
 *
 * ---- Sources ----
 * LIGHT and DISTANCE are independent. Per-source state:
 *   UNAUTHORIZED  no instance authorized yet (HELLO -> NOT_READY)
 *   AWAITING      an instance is authorized, nobody connected for it
 *   ACTIVE        a connection sent a valid HELLO and received HELLO_ACK
 *   DISCONNECTED  was ACTIVE, connection lost; history kept; UNAVAILABLE
 * Transitions:
 *   UNAUTHORIZED --authorize--> AWAITING
 *   AWAITING --HELLO ok, ACK sent--> ACTIVE
 *   ACTIVE --EOF / reset / BYE / abuse / version / error--> DISCONNECTED
 *   DISCONNECTED --HELLO same instance, ACK sent--> ACTIVE   (history kept)
 *   ACTIVE|DISCONNECTED --authorize(higher instance)--> AWAITING; an ACTIVE
 *       connection is closed (SUPERSEDED). History is kept until the first
 *       HELLO of the new instance is ACKed, then the tracker is reset.
 * A source becomes ACTIVE only after its HELLO_ACK was sent successfully. If
 * the nonblocking ACK send fails the connection is closed and the source is
 * left exactly as it was (no state, tracker or generation change).
 *
 * ---- Authorization ----
 * The hub never trusts an instance number because it is larger. A HELLO is
 * accepted only if its instance equals the authorized one (otherwise
 * STALE_INSTANCE, higher or lower). oc_hub_authorize(source, instance):
 *   first call                       -> authorized (instance 0 is valid)
 *   same instance as authorized      -> 0, no-op
 *   lower than any authorized before -> -ESTALE
 *   higher                           -> new instance, as described above
 * It may call the event callback (SOURCE_DOWN SUPERSEDED, DEGRADED_CHANGED).
 *
 * ---- Connections ----
 * At most max_pending (<= OC_HUB_MAX_PENDING) connections wait for HELLO, each
 * for hello_timeout_ms; at most one active connection per source. When no slot
 * is free a new connection is accepted and closed at once ("shed") so the
 * kernel backlog stays drained. A pending connection may only send HELLO:
 *   SAMPLE/ACK/REJECT before HELLO   -> BYE(ERROR) best effort, close
 *   BYE before HELLO                 -> close
 *   malformed HELLO                  -> REJECT(MALFORMED), close
 *   unsupported version              -> close, no reply
 * A HELLO is checked in this order: oc_ipc_hello_check (peer, session, source,
 * origin), NOT_READY, STALE_INSTANCE, DUPLICATE_SOURCE, then ACK. HELLO_REJECT
 * detail is always 0. A rejected connection is closed after the REJECT.
 * Duplicate resolution is bounded: if the source is ACTIVE on another
 * connection, the hub services that incumbent within its remaining per-step
 * budget; if the incumbent is then conclusively closed the new connection is
 * admitted, otherwise it is rejected with DUPLICATE_SOURCE and may retry.
 *
 * ---- Fairness ----
 * Each oc_hub_dispatch consumes at most `batch` datagrams per connection and
 * starts at a rotating slot. Unread data stays queued (level-triggered poll),
 * so a noisy reader delays the other by at most one batch per step. POLLHUP is
 * not special: queued datagrams are still read within the budget, and the
 * connection is closed only when recv reports EOF.
 *
 * ---- Abuse policy ----
 * Per-connection score. +1 for: a malformed or oversized datagram, a sample
 * whose source or origin differs from the HELLO, a duplicate or regressed seq,
 * or a message type not allowed in the current state. -1 (floor 0) per 64
 * valid datagrams. At OC_HUB_STRIKE_LIMIT (8): BYE(ERROR) and close. A gap, a
 * non-OK status or a backward timestamp is a valid report, never a strike.
 * An unsupported version is immediately fatal.
 *
 * ---- Callbacks ----
 * Called synchronously from oc_hub_dispatch / oc_hub_authorize / init /
 * shutdown. They must not block and must not call back into the hub (hub calls
 * made from a callback return -EDEADLK). Pointers are valid only during the
 * call. The hub keeps no queue between receive and callback: a slow callback
 * slows the hub and readers then drop samples at their end (S3A semantics).
 * Every accepted sample is delivered exactly once. When a connection ends by
 * EOF or reset, every datagram the kernel had queued is read (within the
 * per-step budget) and delivered before its SOURCE_DOWN. That guarantee does
 * NOT cover endings the hub itself causes or that stop reading by design:
 * SUPERSEDED (the old instance's data is obsolete), SHUTDOWN, ABUSE, and
 * datagrams queued behind a BYE; whatever is still unread then is discarded.
 * ACTIVE means a valid connection exists, not that usable data is flowing: a
 * reader whose sequence has fallen behind the hub's history (for example
 * restarted without a new instance) is repeatedly struck and cut off; the
 * evidence is seq.regressions and ABUSE closes.
 *
 * Errors: 0 or a negative errno unless stated. Calls on a hub that is not open
 * (shut down, or never initialized successfully) return -ENOTCONN; calls made
 * from inside a callback return -EDEADLK (oc_hub_shutdown, which returns void,
 * does nothing in that case). Read-only accessors are allowed in callbacks.
 * If accept keeps failing (descriptor exhaustion) the pending connection stays
 * in the kernel backlog and poll keeps reporting the listening socket; that is
 * counted in stats.accept_errors and is for S4 to react to. */

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "oc_hub_seq.h"
#include "oc_ipc.h"

#define OC_HUB_MAX_PENDING 4u
#define OC_HUB_MAX_ACTIVE 2u                      /* one per source */
#define OC_HUB_MAX_CONNS (OC_HUB_MAX_PENDING + OC_HUB_MAX_ACTIVE)
#define OC_HUB_MAX_POLLFDS (1u + OC_HUB_MAX_CONNS)
#define OC_HUB_STRIKE_LIMIT 8u
#define OC_HUB_FORGIVE_EVERY 64u
#define OC_HUB_DEFAULT_BATCH 16u
#define OC_HUB_DEFAULT_HELLO_TIMEOUT_MS 500

enum oc_hub_src_state {
    OC_HUB_SRC_UNAUTHORIZED = 0,
    OC_HUB_SRC_AWAITING,
    OC_HUB_SRC_ACTIVE,
    OC_HUB_SRC_DISCONNECTED
};

enum oc_hub_close_reason {
    OC_HUB_CLOSE_EOF = 1,
    OC_HUB_CLOSE_RESET,
    OC_HUB_CLOSE_BYE,
    OC_HUB_CLOSE_ABUSE,
    OC_HUB_CLOSE_VERSION,
    OC_HUB_CLOSE_SUPERSEDED,
    OC_HUB_CLOSE_SHUTDOWN,
    OC_HUB_CLOSE_ERROR,
    OC_HUB_CLOSE_PRE_HELLO,       /* non-HELLO before HELLO */
    OC_HUB_CLOSE_BAD_HELLO,       /* malformed HELLO */
    OC_HUB_CLOSE_ACK_FAILED,
    OC_HUB_CLOSE_SHED             /* no free connection slot */
};

enum oc_hub_event_kind {
    OC_HUB_EV_SOURCE_UP = 1,      /* source became ACTIVE (after ACK) */
    OC_HUB_EV_SOURCE_DOWN,        /* an ACTIVE source lost its connection; `reason` */
    OC_HUB_EV_CONN_REJECTED,      /* HELLO_REJECT sent (`reject`), connection closed */
    OC_HUB_EV_CONN_CLOSED,        /* a not-yet-active connection closed; `reason` */
    OC_HUB_EV_HELLO_TIMEOUT,      /* pending connection closed for silence */
    OC_HUB_EV_DEGRADED_CHANGED    /* `degraded` is the new value */
};

struct oc_hub_event {
    enum oc_hub_event_kind kind;
    uint8_t source;               /* 0 if not yet known */
    uint32_t instance;
    uint32_t generation;          /* SOURCE_UP / SOURCE_DOWN */
    enum oc_hub_close_reason reason;
    uint16_t reject;              /* enum oc_reject_reason for CONN_REJECTED */
    bool reconnect;               /* SOURCE_UP: same instance as before, history kept */
    bool reset;                   /* SOURCE_UP: new instance, tracker reset */
    bool degraded;                /* DEGRADED_CHANGED */
};

struct oc_hub_sample_info {
    const struct oc_sample *sample;   /* validated; NOT modified, incl. its timestamp */
    uint64_t session_id;
    uint32_t instance;
    uint32_t generation;          /* 1 for the first instance; +1 on each reset */
    uint8_t source;
    uint8_t origin;               /* == HELLO origin == sample->origin */
    uint32_t gap;                 /* numbers skipped just before this sample; for the
                                   * first sample of an instance, the unobserved prefix
                                   * (not counted as lost) */
    bool first_of_instance;
    bool first_on_connection;
    bool ts_backwards;            /* timestamp earlier than the previous accepted one */
    uint64_t hub_rx_ns;           /* CLOCK_MONOTONIC when the hub read it */
};

typedef void (*oc_hub_sample_fn)(void *ctx, const struct oc_hub_sample_info *info);
typedef void (*oc_hub_event_fn)(void *ctx, const struct oc_hub_event *ev);

struct oc_hub_cfg {
    const char *socket_path;      /* absolute; see oc_ipc.h for directory rules */
    uint64_t session_id;          /* nonzero; supplied by the supervisor/test, never invented */
    uint32_t uid;                 /* required peer uid (caller passes geteuid()) */
    bool check_pid;
    uint8_t origin[OC_SOURCE_DISTANCE + 1];   /* expected origin per source, 0 = not permitted */
    struct {
        bool authorized;          /* standalone/test convenience; S4 uses oc_hub_authorize */
        uint32_t instance;
    } initial[OC_SOURCE_DISTANCE + 1];
    int hello_timeout_ms;         /* 0 -> default */
    uint8_t max_pending;          /* 0 -> OC_HUB_MAX_PENDING; > max is -EINVAL */
    uint16_t batch;               /* datagrams per connection per step; 0 -> default */
    oc_hub_sample_fn on_sample;   /* may be NULL */
    oc_hub_event_fn on_event;     /* may be NULL */
    void *ctx;
};

struct oc_hub_source_status {
    enum oc_hub_src_state state;
    bool available;               /* state == ACTIVE */
    bool authorized;
    uint32_t authorized_instance;
    uint32_t instance;            /* instance the counters below belong to */
    uint32_t generation;
    struct oc_seq seq;            /* per-instance: reset when a new instance is accepted */
    uint64_t status_count[OC_ST_IO_ERR + 1];   /* accepted samples by status, per instance */
    uint64_t identity_mismatches; /* per instance */
    uint64_t connections;         /* cumulative from here down */
    uint64_t reconnects;
    uint64_t resets;
    uint64_t last_rx_ns;
};

struct oc_hub_stats {
    uint64_t conns_accepted;
    uint64_t shed;
    uint64_t hello_timeouts;
    uint64_t rejects[OC_REJECT_NOT_READY + 1];
    uint64_t malformed;
    uint64_t oversized;
    uint64_t violations;          /* wrong message type for the state */
    uint64_t abusive_closes;
    uint64_t version_fatal;
    uint64_t ack_failures;
    uint64_t accept_errors;       /* accept failed (e.g. EMFILE); retried on the next step */
    uint64_t datagrams;           /* datagrams consumed from all connections */
};

/* ---- The hub object ----
 * Caller-owned, no heap parts. Members below `cfg` are INTERNAL: use the
 * functions, not the fields. cfg.socket_path is only used during oc_hub_init
 * (the server keeps its own copy). */
enum oc_hub_slot_state { OC_HUB_SLOT_FREE = 0, OC_HUB_SLOT_PENDING, OC_HUB_SLOT_ACTIVE };

struct oc_hub_slot {
    enum oc_hub_slot_state state;
    struct oc_ipc_conn conn;
    uint64_t deadline_ns;         /* PENDING: HELLO deadline (CLOCK_MONOTONIC) */
    uint8_t source;               /* ACTIVE */
    uint8_t origin;               /* declared in HELLO */
    uint32_t instance;
    uint32_t score;               /* abuse score */
    uint32_t good;                /* valid datagrams since the last forgiveness */
    uint32_t budget;              /* datagrams left in this dispatch */
    bool first_on_connection;
};

struct oc_hub_src {
    enum oc_hub_src_state state;
    bool authorized;
    uint32_t authorized_instance;
    bool have_max;
    uint32_t max_authorized;      /* highest instance ever authorized (-ESTALE floor) */
    int slot;                     /* ACTIVE connection's slot, else -1 */
    bool has_data;                /* tracker belongs to data_instance */
    uint32_t data_instance;
    uint32_t generation;
    struct oc_seq seq;
    uint64_t status_count[OC_ST_IO_ERR + 1];
    uint64_t identity_mismatches;
    uint64_t connections;
    uint64_t reconnects;
    uint64_t resets;
    uint64_t last_rx_ns;
};

struct oc_hub {
    bool open;
    bool in_callback;
    bool degraded;
    struct oc_hub_cfg cfg;
    struct oc_ipc_server server;
    struct oc_ipc_policy policy;
    struct oc_hub_slot slot[OC_HUB_MAX_CONNS];
    struct oc_hub_src src[OC_SOURCE_DISTANCE + 1];   /* indexed by enum oc_source */
    unsigned rr;                  /* rotating start slot for fairness */
    struct oc_hub_stats stats;
};

/* Opens the listening socket (oc_ipc_server_open rules) and applies cfg.initial.
 * On failure nothing stays open. */
int oc_hub_init(struct oc_hub *h, const struct oc_hub_cfg *cfg);

/* See "Authorization". */
int oc_hub_authorize(struct oc_hub *h, uint8_t source, uint32_t instance);

/* Two-phase loop for a hub process that also watches other descriptors:
 *   poll_setup  fills fds[0..n) (n >= 1 on success; 0 if cap is too small:
 *               cap must be >= OC_HUB_MAX_POLLFDS) and lowers *timeout_ms so that
 *               poll returns by the nearest HELLO deadline. *timeout_ms in: the
 *               caller's wish (-1 = infinite); out: the value to pass to poll.
 *   dispatch    consumes the poll result (stale entries for connections closed
 *               since poll_setup are ignored), services connections within the
 *               budget, expires HELLO deadlines and accepts new connections.
 *               Returns the number of datagrams consumed, or -errno.
 * oc_hub_step does poll_setup + poll + dispatch. EINTR returns 0. */
size_t oc_hub_poll_setup(const struct oc_hub *h, struct pollfd *fds, size_t cap,
                         int *timeout_ms);
int oc_hub_dispatch(struct oc_hub *h, const struct pollfd *fds, size_t n);
int oc_hub_step(struct oc_hub *h, int timeout_ms);

int oc_hub_source_status(const struct oc_hub *h, uint8_t source, struct oc_hub_source_status *out);
void oc_hub_stats(const struct oc_hub *h, struct oc_hub_stats *out);

/* True if any authorized source is not ACTIVE. Before the readers have
 * connected this is true; a startup grace period is policy for S4/UI. NOTE: a
 * hub with NO source authorized reports false (nothing is expected), so S4/UI
 * must not read that as "healthy". */
bool oc_hub_degraded(const struct oc_hub *h);

/* BYE(SHUTDOWN) to every connection, close everything, unlink the socket path.
 * Active sources emit SOURCE_DOWN(SHUTDOWN). Idempotent. */
void oc_hub_shutdown(struct oc_hub *h);

#endif
