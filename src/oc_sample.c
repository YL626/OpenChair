#include "oc_sample.h"

#include <assert.h>
#include <errno.h>

static_assert(OC_SAMPLE_SIZE == 24, "wire sample is 24 bytes");
static_assert(OC_MSG_SAMPLE_SIZE == 28, "SAMPLE datagram is 28 bytes");

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
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

static bool source_ok(uint8_t v)
{
    return v == OC_SOURCE_LIGHT || v == OC_SOURCE_DISTANCE;
}

static bool origin_ok(uint8_t v)
{
    return v == OC_ORIGIN_LIVE || v == OC_ORIGIN_SIMULATED ||
           v == OC_ORIGIN_REPLAYED;
}

static bool status_ok(uint8_t v)
{
    return v >= OC_ST_OK && v <= OC_ST_STALE;
}

static bool type_ok(uint8_t v)
{
    return v >= OC_MSG_HELLO && v <= OC_MSG_BYE;
}

int oc_sample_validate(const struct oc_sample *s)
{
    if (s == NULL || !source_ok(s->source) || !origin_ok(s->origin) ||
        !status_ok(s->status))
        return -EINVAL;
    if (s->status != OC_ST_OK && s->value != 0)
        return -EINVAL;
    return 0;
}

int oc_sample_encode(const struct oc_sample *s, uint8_t *out, size_t cap)
{
    if (s == NULL || out == NULL)
        return -EINVAL;
    if (oc_sample_validate(s) != 0)
        return -EINVAL;
    if (cap < OC_SAMPLE_SIZE)
        return -EMSGSIZE;

    put_u64(out + 0, s->t_mono_ns);
    put_u32(out + 8, s->seq);
    out[12] = s->source;
    out[13] = s->origin;
    out[14] = s->status;
    out[15] = 0;
    put_u32(out + 16, (uint32_t)s->value);
    put_u32(out + 20, s->aux);
    return 0;
}

int oc_sample_decode(const uint8_t *buf, size_t len, struct oc_sample *out)
{
    if (buf == NULL || out == NULL)
        return -EINVAL;
    if (len != OC_SAMPLE_SIZE)
        return -EMSGSIZE;
    if (buf[15] != 0)
        return -EBADMSG;

    struct oc_sample s = {
        .t_mono_ns = get_u64(buf + 0),
        .seq = get_u32(buf + 8),
        .source = buf[12],
        .origin = buf[13],
        .status = buf[14],
        .value = (int32_t)get_u32(buf + 16),
        .aux = get_u32(buf + 20),
    };
    if (oc_sample_validate(&s) != 0)
        return -EBADMSG;

    *out = s;
    return 0;
}

/* Header rules shared by encode and decode. Version is reported separately
 * (-EPROTONOSUPPORT) from other problems (-EBADMSG). */
static int header_check(const struct oc_msg_header *h)
{
    if (h->version != OC_PROTO_VERSION)
        return -EPROTONOSUPPORT;
    if (!type_ok(h->type))
        return -EBADMSG;
    if (h->type == OC_MSG_SAMPLE && h->payload_len != OC_SAMPLE_SIZE)
        return -EBADMSG;
    return 0;
}

int oc_msg_header_encode(const struct oc_msg_header *h, uint8_t *out, size_t cap)
{
    if (h == NULL || out == NULL)
        return -EINVAL;
    int rc = header_check(h);
    if (rc == -EBADMSG)
        return -EINVAL;   /* encoder: caller's value is invalid */
    if (rc != 0)
        return rc;
    if (cap < OC_MSG_HEADER_SIZE)
        return -EMSGSIZE;

    out[0] = h->version;
    out[1] = h->type;
    put_u16(out + 2, h->payload_len);
    return 0;
}

int oc_msg_header_decode(const uint8_t *buf, size_t len, struct oc_msg_header *out)
{
    if (buf == NULL || out == NULL)
        return -EINVAL;
    if (len != OC_MSG_HEADER_SIZE)
        return -EMSGSIZE;

    struct oc_msg_header h = {
        .version = buf[0],
        .type = buf[1],
        .payload_len = get_u16(buf + 2),
    };
    int rc = header_check(&h);
    if (rc != 0)
        return rc;

    *out = h;
    return 0;
}

int oc_msg_sample_encode(const struct oc_sample *s, uint8_t *out, size_t cap,
                         size_t *n_out)
{
    if (s == NULL || out == NULL || n_out == NULL)
        return -EINVAL;
    if (oc_sample_validate(s) != 0)
        return -EINVAL;
    if (cap < OC_MSG_SAMPLE_SIZE)
        return -EMSGSIZE;

    const struct oc_msg_header h = {OC_PROTO_VERSION, OC_MSG_SAMPLE, OC_SAMPLE_SIZE};
    int rc = oc_msg_header_encode(&h, out, OC_MSG_HEADER_SIZE);
    if (rc == 0)
        rc = oc_sample_encode(s, out + OC_MSG_HEADER_SIZE, OC_SAMPLE_SIZE);
    if (rc != 0)
        return rc;
    *n_out = OC_MSG_SAMPLE_SIZE;
    return 0;
}

int oc_msg_sample_decode(const uint8_t *buf, size_t len, struct oc_sample *out)
{
    if (buf == NULL || out == NULL)
        return -EINVAL;
    if (len < OC_MSG_HEADER_SIZE)
        return -EMSGSIZE;

    struct oc_msg_header h;
    int rc = oc_msg_header_decode(buf, OC_MSG_HEADER_SIZE, &h);
    if (rc != 0)
        return rc;
    if (h.type != OC_MSG_SAMPLE)
        return -EBADMSG;
    if (len != (size_t)OC_MSG_HEADER_SIZE + h.payload_len)
        return -EMSGSIZE;
    return oc_sample_decode(buf + OC_MSG_HEADER_SIZE, h.payload_len, out);
}
