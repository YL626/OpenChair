#include "oc_capture_fmt.h"

#include <errno.h>
#include <string.h>

static bool known_source(uint8_t s)
{
    return s == OC_SOURCE_LIGHT || s == OC_SOURCE_DISTANCE;
}

static bool sample_origin(uint8_t o)
{
    return o == OC_ORIGIN_LIVE || o == OC_ORIGIN_SIMULATED || o == OC_ORIGIN_REPLAYED;
}

uint8_t oc_cap_emitter_for(uint8_t source)
{
    switch (source) {
    case OC_SOURCE_LIGHT: return OC_EMIT_LIGHT_READER;
    case OC_SOURCE_DISTANCE: return OC_EMIT_DISTANCE_READER;
    default: return 0;
    }
}

int oc_cap_prov(uint8_t source, uint8_t origin, uint64_t session_id, uint32_t instance,
                struct oc_prov *out)
{
    if (out == nullptr || !known_source(source) || !sample_origin(origin) || session_id == 0)
        return -EINVAL;
    *out = (struct oc_prov){.kind = OC_KIND_SENSOR,
                            .origin = origin,
                            .emitter = oc_cap_emitter_for(source),
                            .flags = 0,
                            .instance = instance,
                            .session_id = session_id};
    return 0;
}

int oc_cap_encode(const struct oc_sample *s, size_t count, uint8_t *out, size_t cap,
                  size_t *n_out)
{
    if (s == nullptr || out == nullptr || n_out == nullptr || count == 0 || count > OC_CAP_MAX_COUNT)
        return -EINVAL;
    for (size_t i = 0; i < count; i++) {
        if (oc_sample_validate(&s[i]) != 0 || s[i].source != s[0].source ||
            s[i].origin != s[0].origin || !known_source(s[i].source))
            return -EINVAL;
    }
    const size_t need = OC_CAP_HEADER_SIZE + count * OC_SAMPLE_SIZE;
    if (cap < need)
        return -EMSGSIZE;
    /* Encode into a local buffer first so a failure leaves `out` untouched. */
    uint8_t tmp[OC_CAP_MAX_PAYLOAD];
    tmp[0] = (uint8_t)OC_CAP_FMT_VERSION;
    tmp[1] = s[0].source;
    tmp[2] = (uint8_t)(count & 0xFFu);
    tmp[3] = (uint8_t)(count >> 8);
    for (size_t i = 0; i < count; i++) {
        int rc = oc_sample_encode(&s[i], tmp + OC_CAP_HEADER_SIZE + i * OC_SAMPLE_SIZE,
                                  OC_SAMPLE_SIZE);
        if (rc != 0)
            return -EINVAL;
    }
    memcpy(out, tmp, need);
    *n_out = need;
    return 0;
}

int oc_cap_decode(const void *payload, size_t len, const struct oc_prov *prov,
                  struct oc_cap_batch *out)
{
    if (payload == nullptr || prov == nullptr || out == nullptr)
        return -EINVAL;
    if (len < OC_CAP_HEADER_SIZE)
        return -EMSGSIZE;
    const uint8_t *p = payload;
    const size_t count = (size_t)p[2] | ((size_t)p[3] << 8);
    if (p[0] != OC_CAP_FMT_VERSION || count == 0 || count > OC_CAP_MAX_COUNT ||
        !known_source(p[1]))
        return -EBADMSG;
    if (len != OC_CAP_HEADER_SIZE + count * OC_SAMPLE_SIZE)
        return -EMSGSIZE;
    if (prov->kind != OC_KIND_SENSOR || prov->flags != 0 || !sample_origin(prov->origin) ||
        prov->emitter != oc_cap_emitter_for(p[1]))
        return -EBADMSG;

    struct oc_cap_batch tmp;
    tmp.source = p[1];
    tmp.count = (uint16_t)count;
    for (size_t i = 0; i < count; i++) {
        if (oc_sample_decode(p + OC_CAP_HEADER_SIZE + i * OC_SAMPLE_SIZE, OC_SAMPLE_SIZE,
                             &tmp.sample[i]) != 0)
            return -EBADMSG;
        if (tmp.sample[i].source != tmp.source || tmp.sample[i].origin != prov->origin)
            return -EBADMSG;
    }
    *out = tmp;
    return 0;
}
