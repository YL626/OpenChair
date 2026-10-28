#include "oc_ipc_msg.h"

#include <errno.h>
#include <string.h>

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++)
        v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* ---- field legality, shared by encoders and decoders ---- */

static bool source_ok(uint8_t v)
{
    return v == OC_SOURCE_LIGHT || v == OC_SOURCE_DISTANCE;
}

static bool origin_ok(uint8_t v)
{
    return v == OC_ORIGIN_LIVE || v == OC_ORIGIN_SIMULATED || v == OC_ORIGIN_REPLAYED;
}

static bool reject_reason_ok(uint16_t v)
{
    return v >= OC_REJECT_MALFORMED && v <= OC_REJECT_NOT_READY;
}

static bool bye_reason_ok(uint16_t v)
{
    return v >= OC_BYE_NORMAL && v <= OC_BYE_ERROR;
}

static bool hello_ok(const struct oc_hello *m)
{
    return source_ok(m->source) && origin_ok(m->origin) && m->session_id != 0;
}

static bool ack_ok(const struct oc_hello_ack *m)
{
    return source_ok(m->source) && m->session_id != 0;
}

/* ---- encode ---- */

/* Writes the envelope and returns the payload pointer. `out` has been
 * checked to hold at least header + payload_len bytes. */
static uint8_t *put_header(uint8_t *out, uint8_t type, uint16_t payload_len)
{
    out[0] = OC_PROTO_VERSION;
    out[1] = type;
    put_u16(out + 2, payload_len);
    return out + OC_MSG_HEADER_SIZE;
}

int oc_ipc_encode_hello(const struct oc_hello *m, uint8_t *out, size_t cap, size_t *n_out)
{
    if (m == nullptr || out == nullptr || n_out == nullptr || !hello_ok(m))
        return -EINVAL;
    if (cap < OC_MSG_HELLO_SIZE)
        return -EMSGSIZE;
    uint8_t *p = put_header(out, OC_MSG_HELLO, OC_HELLO_SIZE);
    p[0] = m->source;
    p[1] = m->origin;
    p[2] = 0;
    p[3] = 0;
    put_u64(p + 4, m->session_id);
    put_u32(p + 12, m->instance);
    put_u32(p + 16, m->pid);
    *n_out = OC_MSG_HELLO_SIZE;
    return 0;
}

int oc_ipc_encode_hello_ack(const struct oc_hello_ack *m, uint8_t *out, size_t cap,
                            size_t *n_out)
{
    if (m == nullptr || out == nullptr || n_out == nullptr || !ack_ok(m))
        return -EINVAL;
    if (cap < OC_MSG_HELLO_ACK_SIZE)
        return -EMSGSIZE;
    uint8_t *p = put_header(out, OC_MSG_HELLO_ACK, OC_HELLO_ACK_SIZE);
    put_u64(p + 0, m->session_id);
    put_u32(p + 8, m->instance);
    p[12] = m->source;
    p[13] = 0;
    p[14] = 0;
    p[15] = 0;
    *n_out = OC_MSG_HELLO_ACK_SIZE;
    return 0;
}

int oc_ipc_encode_hello_reject(const struct oc_hello_reject *m, uint8_t *out, size_t cap,
                               size_t *n_out)
{
    if (m == nullptr || out == nullptr || n_out == nullptr || !reject_reason_ok(m->reason))
        return -EINVAL;
    if (cap < OC_MSG_HELLO_REJECT_SIZE)
        return -EMSGSIZE;
    uint8_t *p = put_header(out, OC_MSG_HELLO_REJECT, OC_HELLO_REJECT_SIZE);
    put_u16(p + 0, m->reason);
    p[2] = 0;
    p[3] = 0;
    put_u32(p + 4, m->detail);
    *n_out = OC_MSG_HELLO_REJECT_SIZE;
    return 0;
}

int oc_ipc_encode_bye(const struct oc_bye *m, uint8_t *out, size_t cap, size_t *n_out)
{
    if (m == nullptr || out == nullptr || n_out == nullptr || !bye_reason_ok(m->reason))
        return -EINVAL;
    if (cap < OC_MSG_BYE_SIZE)
        return -EMSGSIZE;
    uint8_t *p = put_header(out, OC_MSG_BYE, OC_BYE_SIZE);
    put_u16(p + 0, m->reason);
    p[2] = 0;
    p[3] = 0;
    *n_out = OC_MSG_BYE_SIZE;
    return 0;
}

/* ---- decode ---- */

/* Exact payload length for each type, or 0 for an unknown type. */
static uint16_t payload_len_for(uint8_t type)
{
    switch (type) {
    case OC_MSG_HELLO: return OC_HELLO_SIZE;
    case OC_MSG_HELLO_ACK: return OC_HELLO_ACK_SIZE;
    case OC_MSG_HELLO_REJECT: return OC_HELLO_REJECT_SIZE;
    case OC_MSG_SAMPLE: return OC_SAMPLE_SIZE;
    case OC_MSG_BYE: return OC_BYE_SIZE;
    default: return 0;
    }
}

static int decode_hello(const uint8_t *p, struct oc_ipc_msg *out)
{
    if (p[2] != 0 || p[3] != 0)
        return -EBADMSG;
    struct oc_hello h = {
        .source = p[0],
        .origin = p[1],
        .session_id = get_u64(p + 4),
        .instance = get_u32(p + 12),
        .pid = get_u32(p + 16),
    };
    if (!hello_ok(&h))
        return -EBADMSG;
    out->type = OC_MSG_HELLO;
    out->u.hello = h;
    return 0;
}

static int decode_ack(const uint8_t *p, struct oc_ipc_msg *out)
{
    if (p[13] != 0 || p[14] != 0 || p[15] != 0)
        return -EBADMSG;
    struct oc_hello_ack a = {
        .session_id = get_u64(p + 0),
        .instance = get_u32(p + 8),
        .source = p[12],
    };
    if (!ack_ok(&a))
        return -EBADMSG;
    out->type = OC_MSG_HELLO_ACK;
    out->u.ack = a;
    return 0;
}

static int decode_reject(const uint8_t *p, struct oc_ipc_msg *out)
{
    if (p[2] != 0 || p[3] != 0)
        return -EBADMSG;
    struct oc_hello_reject r = {.reason = get_u16(p + 0), .detail = get_u32(p + 4)};
    if (!reject_reason_ok(r.reason))
        return -EBADMSG;
    out->type = OC_MSG_HELLO_REJECT;
    out->u.reject = r;
    return 0;
}

static int decode_bye(const uint8_t *p, struct oc_ipc_msg *out)
{
    if (p[2] != 0 || p[3] != 0)
        return -EBADMSG;
    struct oc_bye y = {.reason = get_u16(p + 0)};
    if (!bye_reason_ok(y.reason))
        return -EBADMSG;
    out->type = OC_MSG_BYE;
    out->u.bye = y;
    return 0;
}

int oc_ipc_msg_decode(const uint8_t *buf, size_t len, struct oc_ipc_msg *out)
{
    if (buf == nullptr || out == nullptr)
        return -EINVAL;
    /* Version first, for any length: an unknown version means the rest of the
     * bytes cannot be interpreted at all, and the connection is fatal. */
    if (len >= 1 && buf[0] != OC_PROTO_VERSION)
        return -EPROTONOSUPPORT;
    if (len < OC_MSG_HEADER_SIZE)
        return -EMSGSIZE;

    uint8_t type = buf[1];
    uint16_t want = payload_len_for(type);
    if (want == 0)
        return -EBADMSG;
    if (len != (size_t)OC_MSG_HEADER_SIZE + get_u16(buf + 2))
        return -EMSGSIZE;             /* truncated, or bytes after the payload */
    if (get_u16(buf + 2) != want)
        return -EBADMSG;              /* each type has exactly one legal length */

    const uint8_t *p = buf + OC_MSG_HEADER_SIZE;
    struct oc_ipc_msg m;
    int rc;
    switch (type) {
    case OC_MSG_HELLO: rc = decode_hello(p, &m); break;
    case OC_MSG_HELLO_ACK: rc = decode_ack(p, &m); break;
    case OC_MSG_HELLO_REJECT: rc = decode_reject(p, &m); break;
    case OC_MSG_BYE: rc = decode_bye(p, &m); break;
    default: /* OC_MSG_SAMPLE: existing codec, unchanged */
        rc = oc_msg_sample_decode(buf, len, &m.u.sample);
        m.type = OC_MSG_SAMPLE;
        break;
    }
    if (rc != 0)
        return rc;
    *out = m;
    return 0;
}

/* ---- HELLO policy ---- */

enum oc_reject_reason oc_ipc_hello_check(const struct oc_ipc_policy *p,
                                         const struct oc_hello *h,
                                         const struct oc_ipc_peer *peer)
{
    if (p == nullptr || h == nullptr)
        return OC_REJECT_MALFORMED;

    /* Who connected: fail closed without credentials. */
    if (peer == nullptr || peer->uid != p->uid || (p->check_pid && peer->pid != h->pid))
        return OC_REJECT_PEER_CRED;

    if (p->session_id == 0 || h->session_id != p->session_id)
        return OC_REJECT_SESSION_MISMATCH;

    if (!source_ok(h->source) || p->origin[h->source] == 0)
        return OC_REJECT_BAD_SOURCE;

    uint8_t expected = p->origin[h->source];
    if (!origin_ok(h->origin) || !origin_ok(expected) || h->origin != expected)
        return OC_REJECT_BAD_ORIGIN;

    return OC_REJECT_NONE;
}
