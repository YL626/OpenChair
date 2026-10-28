#ifndef OC_IPC_MSG_H
#define OC_IPC_MSG_H

/* oc_ipc_msg: payload codec for the IPC control messages HELLO, HELLO_ACK,
 * HELLO_REJECT and BYE, plus a generic decoder that also covers SAMPLE (which
 * is delegated unchanged to the oc_sample codec), and the stateless HELLO
 * policy check.
 *
 * STATUS: [REVIEW] byte layouts follow docs/IMPLEMENTATION_NOTES.md section 1.
 *
 * Pure functions: no I/O, no allocation, no globals. Wire bytes use explicit
 * little-endian shifts, never struct layout. Every message is the 4-byte
 * envelope of oc_sample.h (version u8, type u8, payload_len u16 LE) followed
 * by an exact-length payload:
 *
 *   HELLO         payload 20 B  datagram 24 B
 *     0 u8 source  1 u8 origin  2..3 reserved=0  4 u64 session_id (nonzero)
 *     12 u32 instance  16 u32 pid
 *   HELLO_ACK     payload 16 B  datagram 20 B
 *     0 u64 session_id  8 u32 instance  12 u8 source  13..15 reserved=0
 *   HELLO_REJECT  payload  8 B  datagram 12 B
 *     0 u16 reason  2..3 reserved=0  4 u32 detail
 *   BYE           payload  4 B  datagram  8 B
 *     0 u16 reason  2..3 reserved=0
 *   SAMPLE        payload 24 B  datagram 28 B   (oc_sample.h, unchanged)
 *
 * The largest currently defined datagram is 28 B (SAMPLE).
 *
 * Instance 0 is a legal value: the supervisor's per-session counter may start
 * at 0. session_id 0 is reserved and rejected (the supervisor draws a random
 * nonzero id). Whether an instance is acceptable for a source (duplicate,
 * stale, restart) is hub state and is NOT decided here.
 *
 * Errors: every function returns 0 or a negative errno.
 *   -EINVAL            NULL argument, or an invalid value passed to an encoder
 *                      (encoders refuse to emit bytes the decoder would reject)
 *   -EMSGSIZE          encode: buffer too small. decode: datagram shorter than
 *                      the envelope, or length != 4 + header.payload_len
 *                      (truncated or trailing bytes)
 *   -EPROTONOSUPPORT   first byte (version) is not OC_PROTO_VERSION. Checked
 *                      before anything else, for any datagram length. A
 *                      receiver must treat this as CONNECTION-FATAL: close
 *                      without a reply (oc_ipc_recv does so itself).
 *   -EBADMSG           unknown message type, header payload_len not the exact
 *                      length for the type, nonzero reserved byte, or an
 *                      illegal enum / zero session_id in the payload
 * On any error the output buffer or struct is left unchanged.
 *
 * Structure only: this layer says whether bytes are a well-formed message, not
 * whether a sender is acceptable. Acceptability (session, permitted sources,
 * expected origin, peer credentials) is oc_ipc_hello_check, which is separate
 * from decoding and from the transport.
 *
 * Ownership: all buffers and structs belong to the caller. Nothing is retained. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "oc_sample.h"

#define OC_HELLO_SIZE 20u
#define OC_HELLO_ACK_SIZE 16u
#define OC_HELLO_REJECT_SIZE 8u
#define OC_BYE_SIZE 4u

#define OC_MSG_HELLO_SIZE (OC_MSG_HEADER_SIZE + OC_HELLO_SIZE)               /* 24 */
#define OC_MSG_HELLO_ACK_SIZE (OC_MSG_HEADER_SIZE + OC_HELLO_ACK_SIZE)       /* 20 */
#define OC_MSG_HELLO_REJECT_SIZE (OC_MSG_HEADER_SIZE + OC_HELLO_REJECT_SIZE) /* 12 */
#define OC_MSG_BYE_SIZE (OC_MSG_HEADER_SIZE + OC_BYE_SIZE)                   /* 8 */

/* Largest currently defined datagram (SAMPLE). */
#define OC_IPC_MAX_MSG OC_MSG_SAMPLE_SIZE

enum oc_reject_reason {
    OC_REJECT_NONE = 0,              /* only returned by oc_ipc_hello_check */
    OC_REJECT_MALFORMED = 1,
    OC_REJECT_BAD_SOURCE = 2,        /* unknown, or not permitted by policy */
    OC_REJECT_BAD_ORIGIN = 3,        /* illegal, or not the expected origin */
    OC_REJECT_SESSION_MISMATCH = 4,
    OC_REJECT_PEER_CRED = 5,         /* uid, or declared pid != SO_PEERCRED pid */
    OC_REJECT_DUPLICATE_SOURCE = 6,  /* wire value reserved for hub state (S3B) */
    OC_REJECT_STALE_INSTANCE = 7,    /* wire value reserved for hub state (S3B) */
    OC_REJECT_NOT_READY = 8
};

enum oc_bye_reason {
    OC_BYE_NORMAL = 1,
    OC_BYE_SHUTDOWN = 2,
    OC_BYE_ERROR = 3
};

struct oc_hello {
    uint8_t source;        /* enum oc_source */
    uint8_t origin;        /* enum oc_origin: LIVE, SIMULATED or REPLAYED */
    uint64_t session_id;   /* nonzero */
    uint32_t instance;     /* 0 is legal */
    uint32_t pid;          /* declared; compared with SO_PEERCRED by the check */
};

struct oc_hello_ack {
    uint64_t session_id;   /* echo of the HELLO */
    uint32_t instance;     /* echo */
    uint8_t source;        /* echo */
};

struct oc_hello_reject {
    uint16_t reason;       /* enum oc_reject_reason, 1..8 */
    uint32_t detail;       /* 0 unless a later slice defines it */
};

struct oc_bye {
    uint16_t reason;       /* enum oc_bye_reason */
};

/* Tagged union of everything decodable. `type` is enum oc_msg_type and selects
 * the live member. */
struct oc_ipc_msg {
    uint8_t type;
    union {
        struct oc_hello hello;
        struct oc_hello_ack ack;
        struct oc_hello_reject reject;
        struct oc_bye bye;
        struct oc_sample sample;
    } u;
};

/* Each encoder writes one complete datagram and sets *n_out to its size.
 * (SAMPLE is encoded by oc_msg_sample_encode.) */
int oc_ipc_encode_hello(const struct oc_hello *m, uint8_t *out, size_t cap, size_t *n_out);
int oc_ipc_encode_hello_ack(const struct oc_hello_ack *m, uint8_t *out, size_t cap, size_t *n_out);
int oc_ipc_encode_hello_reject(const struct oc_hello_reject *m, uint8_t *out, size_t cap, size_t *n_out);
int oc_ipc_encode_bye(const struct oc_bye *m, uint8_t *out, size_t cap, size_t *n_out);

/* Decodes one complete datagram (len must be the whole datagram). */
int oc_ipc_msg_decode(const uint8_t *buf, size_t len, struct oc_ipc_msg *out);

/* ---- HELLO policy (stateless) ---- */

/* Local peer identity as reported by the kernel (SO_PEERCRED). It proves which
 * local process connected, NOT that the sensor data it sends is genuine. */
struct oc_ipc_peer {
    uint32_t pid;
    uint32_t uid;
    uint32_t gid;
};

struct oc_ipc_policy {
    uint64_t session_id;    /* the hub's session */
    uint32_t uid;           /* required peer uid (normally geteuid()) */
    bool check_pid;         /* require hello.pid == peer.pid */
    /* Indexed by enum oc_source (index 0 unused). 0 = that source is not
     * permitted; otherwise the single origin (LIVE, SIMULATED or REPLAYED)
     * the hub expects from it. Sources may differ, so a development session
     * can mix origins; a mixed session is never M3 two-LIVE evidence. */
    uint8_t origin[OC_SOURCE_DISTANCE + 1];
};

/* Returns OC_REJECT_NONE if the HELLO is acceptable to the stateless policy,
 * else the first failing reason, checked in this order: PEER_CRED (including a
 * NULL peer: fail closed), SESSION_MISMATCH (a zero session on either side
 * never matches), BAD_SOURCE (unknown source, or one the policy does not
 * permit), BAD_ORIGIN (illegal origin, or not the one expected for that
 * source; an illegal policy origin also fails closed). NULL p or h gives
 * MALFORMED. Duplicate/stale instance checks need hub state and are not made
 * here. */
enum oc_reject_reason oc_ipc_hello_check(const struct oc_ipc_policy *p,
                                         const struct oc_hello *h,
                                         const struct oc_ipc_peer *peer);

#endif
