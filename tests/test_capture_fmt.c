/* Unit tests for oc_capture_fmt: round trips, exact bytes, invalid input.
 * Pure; all samples are SIMULATED synthetic values. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "oc_capture_fmt.h"

static int failures;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static struct oc_sample mk(uint8_t src, uint32_t seq, uint8_t st, int32_t v)
{
    return (struct oc_sample){.t_mono_ns = 1000000000ull + seq, .seq = seq, .source = src,
                              .origin = OC_ORIGIN_SIMULATED, .status = st,
                              .value = st == OC_ST_OK ? v : 0, .aux = 0x4383};
}

static void test_emitter_and_prov(void)
{
    CHECK(OC_EMIT_LIGHT_READER == 1 && OC_EMIT_DISTANCE_READER == 2);
    CHECK(oc_cap_emitter_for(OC_SOURCE_LIGHT) == OC_EMIT_LIGHT_READER);
    CHECK(oc_cap_emitter_for(OC_SOURCE_DISTANCE) == OC_EMIT_DISTANCE_READER);
    CHECK(oc_cap_emitter_for(0) == 0 && oc_cap_emitter_for(3) == 0);
    struct oc_prov p;
    CHECK(oc_cap_prov(OC_SOURCE_DISTANCE, OC_ORIGIN_SIMULATED, 0xABCDEFull, 9, &p) == 0);
    CHECK(p.kind == OC_KIND_SENSOR && p.origin == OC_ORIGIN_SIMULATED &&
          p.emitter == OC_EMIT_DISTANCE_READER && p.flags == 0 && p.instance == 9 &&
          p.session_id == 0xABCDEFull);
    struct oc_prov q = p;
    CHECK(oc_cap_prov(7, OC_ORIGIN_SIMULATED, 1, 0, &q) == -EINVAL);
    CHECK(oc_cap_prov(OC_SOURCE_LIGHT, OC_ORIGIN_MIXED, 1, 0, &q) == -EINVAL);   /* not a sample origin */
    CHECK(oc_cap_prov(OC_SOURCE_LIGHT, OC_ORIGIN_NONE, 1, 0, &q) == -EINVAL);
    CHECK(oc_cap_prov(OC_SOURCE_LIGHT, OC_ORIGIN_LIVE, 0, 0, &q) == -EINVAL);    /* session 0 reserved */
    CHECK(oc_cap_prov(OC_SOURCE_LIGHT, OC_ORIGIN_LIVE, 1, 0, nullptr) == -EINVAL);
    CHECK(memcmp(&p, &q, sizeof p) == 0);                                        /* untouched */
}

static void test_exact_bytes_and_roundtrip(void)
{
    struct oc_sample s = mk(OC_SOURCE_LIGHT, 0x01020304u, OC_ST_OK, -5);
    uint8_t buf[OC_CAP_MAX_PAYLOAD];
    size_t n = 0;
    CHECK(oc_cap_encode(&s, 1, buf, sizeof buf, &n) == 0);
    CHECK(n == 4 + OC_SAMPLE_SIZE);
    CHECK(buf[0] == 1 && buf[1] == OC_SOURCE_LIGHT && buf[2] == 1 && buf[3] == 0);   /* fmt, source, count LE */
    uint8_t one[OC_SAMPLE_SIZE];
    CHECK(oc_sample_encode(&s, one, sizeof one) == 0);
    CHECK(memcmp(buf + 4, one, OC_SAMPLE_SIZE) == 0);                                /* the shared codec, verbatim */
    struct oc_prov p;
    CHECK(oc_cap_prov(OC_SOURCE_LIGHT, OC_ORIGIN_SIMULATED, 77, 3, &p) == 0);
    struct oc_cap_batch b;
    CHECK(oc_cap_decode(buf, n, &p, &b) == 0);
    CHECK(b.source == OC_SOURCE_LIGHT && b.count == 1);
    CHECK(b.sample[0].seq == 0x01020304u && b.sample[0].value == -5 &&
          b.sample[0].t_mono_ns == s.t_mono_ns && b.sample[0].aux == 0x4383 &&
          b.sample[0].origin == OC_ORIGIN_SIMULATED && b.sample[0].status == OC_ST_OK);
}

static void test_batch_and_non_ok(void)
{
    struct oc_sample s[OC_CAP_MAX_COUNT];
    for (uint32_t i = 0; i < OC_CAP_MAX_COUNT; i++)
        s[i] = mk(OC_SOURCE_DISTANCE, UINT32_MAX - 3 + i, i % 4 == 3 ? OC_ST_TIMEOUT : OC_ST_OK, 40);
    uint8_t buf[OC_CAP_MAX_PAYLOAD];
    size_t n = 0;
    CHECK(oc_cap_encode(s, OC_CAP_MAX_COUNT, buf, sizeof buf, &n) == 0);
    CHECK(n == OC_CAP_MAX_PAYLOAD);
    struct oc_prov p;
    CHECK(oc_cap_prov(OC_SOURCE_DISTANCE, OC_ORIGIN_SIMULATED, 5, 9, &p) == 0);
    struct oc_cap_batch b;
    CHECK(oc_cap_decode(buf, n, &p, &b) == 0 && b.count == OC_CAP_MAX_COUNT);
    for (uint32_t i = 0; i < OC_CAP_MAX_COUNT; i++) {
        CHECK(b.sample[i].seq == s[i].seq && b.sample[i].status == s[i].status);   /* wraps through 0 */
        CHECK(b.sample[i].value == s[i].value);
    }
    CHECK(b.sample[3].status == OC_ST_TIMEOUT && b.sample[3].value == 0);          /* non-OK preserved */
}

static void test_encode_rejects(void)
{
    struct oc_sample s[3] = {mk(OC_SOURCE_LIGHT, 1, OC_ST_OK, 1), mk(OC_SOURCE_LIGHT, 2, OC_ST_OK, 1),
                             mk(OC_SOURCE_LIGHT, 3, OC_ST_OK, 1)};
    uint8_t buf[OC_CAP_MAX_PAYLOAD];
    uint8_t keep[sizeof buf];
    memset(buf, 0xEE, sizeof buf);
    memcpy(keep, buf, sizeof buf);
    size_t n = 99;
    CHECK(oc_cap_encode(s, 0, buf, sizeof buf, &n) == -EINVAL);
    CHECK(oc_cap_encode(s, OC_CAP_MAX_COUNT + 1, buf, sizeof buf, &n) == -EINVAL);
    CHECK(oc_cap_encode(nullptr, 1, buf, sizeof buf, &n) == -EINVAL);
    CHECK(oc_cap_encode(s, 1, nullptr, sizeof buf, &n) == -EINVAL);
    CHECK(oc_cap_encode(s, 1, buf, sizeof buf, nullptr) == -EINVAL);
    CHECK(oc_cap_encode(s, 3, buf, 4 + 3 * 24 - 1, &n) == -EMSGSIZE);
    struct oc_sample mixed[2] = {s[0], mk(OC_SOURCE_DISTANCE, 2, OC_ST_OK, 1)};
    CHECK(oc_cap_encode(mixed, 2, buf, sizeof buf, &n) == -EINVAL);          /* two sources */
    mixed[1] = s[1];
    mixed[1].origin = OC_ORIGIN_LIVE;
    CHECK(oc_cap_encode(mixed, 2, buf, sizeof buf, &n) == -EINVAL);          /* two origins */
    struct oc_sample bad = s[0];
    bad.status = OC_ST_TIMEOUT;
    bad.value = 5;                                                           /* non-OK with a value */
    CHECK(oc_cap_encode(&bad, 1, buf, sizeof buf, &n) == -EINVAL);
    CHECK(n == 99 && memcmp(buf, keep, sizeof buf) == 0);                    /* nothing written */
}

static void test_decode_rejects(void)
{
    struct oc_sample s = mk(OC_SOURCE_LIGHT, 10, OC_ST_OK, 100);
    uint8_t good[OC_CAP_MAX_PAYLOAD];
    size_t n = 0;
    CHECK(oc_cap_encode(&s, 1, good, sizeof good, &n) == 0);
    struct oc_prov p;
    CHECK(oc_cap_prov(OC_SOURCE_LIGHT, OC_ORIGIN_SIMULATED, 5, 1, &p) == 0);
    struct oc_cap_batch b, keep;
    memset(&b, 0x5A, sizeof b);
    keep = b;
    uint8_t m[OC_CAP_MAX_PAYLOAD];

    CHECK(oc_cap_decode(nullptr, n, &p, &b) == -EINVAL);
    CHECK(oc_cap_decode(good, n, nullptr, &b) == -EINVAL);
    CHECK(oc_cap_decode(good, n, &p, nullptr) == -EINVAL);
    CHECK(oc_cap_decode(good, 3, &p, &b) == -EMSGSIZE);                       /* shorter than the header */
    CHECK(oc_cap_decode(good, n - 1, &p, &b) == -EMSGSIZE);                   /* truncated */
    memcpy(m, good, n);
    m[n] = 0;
    CHECK(oc_cap_decode(m, n + 1, &p, &b) == -EMSGSIZE);                      /* trailing byte */
    memcpy(m, good, n); m[0] = 2;
    CHECK(oc_cap_decode(m, n, &p, &b) == -EBADMSG);                           /* fmt */
    memcpy(m, good, n); m[2] = 0;
    CHECK(oc_cap_decode(m, n, &p, &b) == -EBADMSG || oc_cap_decode(m, n, &p, &b) == -EMSGSIZE);   /* count 0 */
    memcpy(m, good, n); m[2] = 2;
    CHECK(oc_cap_decode(m, n, &p, &b) == -EMSGSIZE);                          /* count says 2, one present */
    memcpy(m, good, n); m[2] = 33;
    CHECK(oc_cap_decode(m, n, &p, &b) == -EBADMSG || oc_cap_decode(m, n, &p, &b) == -EMSGSIZE);   /* > max */
    memcpy(m, good, n); m[1] = OC_SOURCE_DISTANCE;
    CHECK(oc_cap_decode(m, n, &p, &b) == -EBADMSG);                           /* header source != sample source */
    memcpy(m, good, n); m[1] = 9;
    CHECK(oc_cap_decode(m, n, &p, &b) == -EBADMSG);                           /* unknown source */
    memcpy(m, good, n); m[4 + 15] = 1;                                        /* sample reserved byte */
    CHECK(oc_cap_decode(m, n, &p, &b) == -EBADMSG);
    memcpy(m, good, n); m[4 + 14] = 0;                                        /* sample status 0 */
    CHECK(oc_cap_decode(m, n, &p, &b) == -EBADMSG);

    struct oc_prov q = p;
    q.origin = OC_ORIGIN_LIVE;                                                /* origin disagrees with sample */
    CHECK(oc_cap_decode(good, n, &q, &b) == -EBADMSG);
    q = p; q.kind = OC_KIND_SYSTEM;
    CHECK(oc_cap_decode(good, n, &q, &b) == -EBADMSG);
    q = p; q.emitter = OC_EMIT_DISTANCE_READER;
    CHECK(oc_cap_decode(good, n, &q, &b) == -EBADMSG);
    q = p; q.flags = OC_PROV_INJECTED;
    CHECK(oc_cap_decode(good, n, &q, &b) == -EBADMSG);
    CHECK(memcmp(&b, &keep, sizeof b) == 0);                                  /* output untouched on every error */
    CHECK(oc_cap_decode(good, n, &p, &b) == 0);
}

int main(void)
{
    test_emitter_and_prov();
    test_exact_bytes_and_roundtrip();
    test_batch_and_non_ok();
    test_encode_rejects();
    test_decode_rejects();
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("test_capture_fmt: all passed");
    return 0;
}
