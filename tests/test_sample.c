/* Tests for the oc_sample / IPC-envelope codec (include/oc_sample.h).
 *
 * Every input is a synthetic byte string or struct built here. Nothing is a
 * live sensor measurement and no hardware, socket or file is touched.
 *
 * Error contract under test: 0 or a negative errno; on any error the output
 * buffer / struct is left byte-for-byte unchanged. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oc_sample.h"

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define RUN(test)                          \
    do {                                   \
        fprintf(stderr, "== %s\n", #test); \
        test();                            \
    } while (0)

#define SENTINEL 0xA5

static struct oc_sample good(void)
{
    struct oc_sample s = {
        .t_mono_ns = 0x0102030405060708ull,
        .seq = 0x090A0B0Cu,
        .source = OC_SOURCE_LIGHT,
        .origin = OC_ORIGIN_LIVE,
        .status = OC_ST_OK,
        .value = -2,
        .aux = 0x8583u,
    };
    return s;
}

static const uint8_t golden_payload[24] = {
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, /* t_mono_ns */
    0x0C, 0x0B, 0x0A, 0x09,                         /* seq */
    0x01, 0x01, 0x01, 0x00,                         /* source origin status reserved */
    0xFE, 0xFF, 0xFF, 0xFF,                         /* value = -2 */
    0x83, 0x85, 0x00, 0x00,                         /* aux */
};

static bool same_sample(const struct oc_sample *a, const struct oc_sample *b)
{
    return a->t_mono_ns == b->t_mono_ns && a->seq == b->seq &&
           a->source == b->source && a->origin == b->origin &&
           a->status == b->status && a->value == b->value && a->aux == b->aux;
}

static void test_constants(void)
{
    CHECK(OC_SAMPLE_SIZE == 24u);
    CHECK(OC_MSG_HEADER_SIZE == 4u);
    CHECK(OC_MSG_SAMPLE_SIZE == 28u);
    CHECK(OC_PROTO_VERSION == 1u);
    CHECK(OC_SOURCE_LIGHT == 1 && OC_SOURCE_DISTANCE == 2);
    CHECK(OC_ST_OK == 1 && OC_ST_SATURATED == 2 && OC_ST_NACK == 3 &&
          OC_ST_TIMEOUT == 4 && OC_ST_RANGE_ERR == 5 && OC_ST_STALE == 6);
    CHECK(OC_MSG_HELLO == 1 && OC_MSG_HELLO_ACK == 2 &&
          OC_MSG_HELLO_REJECT == 3 && OC_MSG_SAMPLE == 4 && OC_MSG_BYE == 5);
    /* Shared with oc_store.h: must not be redefined here. */
    CHECK(OC_ORIGIN_LIVE == 1 && OC_ORIGIN_SIMULATED == 2 &&
          OC_ORIGIN_REPLAYED == 3 && OC_ORIGIN_MIXED == 4 && OC_ORIGIN_NONE == 5);
}

static void test_sample_golden_encode(void)
{
    struct oc_sample s = good();
    uint8_t buf[OC_SAMPLE_SIZE];
    memset(buf, SENTINEL, sizeof buf);
    CHECK(oc_sample_encode(&s, buf, sizeof buf) == 0);
    CHECK(memcmp(buf, golden_payload, sizeof buf) == 0);
}

static void test_sample_golden_decode(void)
{
    struct oc_sample out;
    memset(&out, 0, sizeof out);
    CHECK(oc_sample_decode(golden_payload, sizeof golden_payload, &out) == 0);
    struct oc_sample want = good();
    CHECK(same_sample(&out, &want));
}

static void test_sample_roundtrip_all_valid_enums(void)
{
    static const uint8_t sources[] = {OC_SOURCE_LIGHT, OC_SOURCE_DISTANCE};
    static const uint8_t origins[] = {OC_ORIGIN_LIVE, OC_ORIGIN_SIMULATED,
                                      OC_ORIGIN_REPLAYED};
    for (size_t i = 0; i < sizeof sources; i++)
        for (size_t j = 0; j < sizeof origins; j++)
            for (uint8_t st = OC_ST_OK; st <= OC_ST_STALE; st++) {
                struct oc_sample s = good();
                s.source = sources[i];
                s.origin = origins[j];
                s.status = st;
                s.value = (st == OC_ST_OK) ? 1234 : 0;
                uint8_t buf[OC_SAMPLE_SIZE];
                struct oc_sample back;
                CHECK(oc_sample_encode(&s, buf, sizeof buf) == 0);
                CHECK(oc_sample_decode(buf, sizeof buf, &back) == 0);
                CHECK(same_sample(&s, &back));
            }
}

static void test_sample_boundaries(void)
{
    static const uint64_t ts[] = {0, 1, UINT64_MAX, 0x8000000000000000ull};
    static const uint32_t seqs[] = {0, 1, UINT32_MAX, 0x80000000u};
    static const int32_t vals[] = {INT32_MIN, -1, 0, 1, INT32_MAX};
    static const uint32_t auxs[] = {0, 1, UINT32_MAX, 0x80000000u};
    for (size_t a = 0; a < 4; a++)
        for (size_t b = 0; b < 4; b++)
            for (size_t c = 0; c < 5; c++)
                for (size_t d = 0; d < 4; d++) {
                    struct oc_sample s = good();
                    s.t_mono_ns = ts[a];
                    s.seq = seqs[b];
                    s.value = vals[c];
                    s.aux = auxs[d];
                    uint8_t buf[OC_SAMPLE_SIZE];
                    struct oc_sample back;
                    CHECK(oc_sample_encode(&s, buf, sizeof buf) == 0);
                    CHECK(oc_sample_decode(buf, sizeof buf, &back) == 0);
                    CHECK(same_sample(&s, &back));
                }
}

static void test_sample_signed_value_bytes(void)
{
    struct { int32_t v; uint8_t b[4]; } c[] = {
        {INT32_MIN, {0x00, 0x00, 0x00, 0x80}},
        {-1, {0xFF, 0xFF, 0xFF, 0xFF}},
        {0, {0, 0, 0, 0}},
        {INT32_MAX, {0xFF, 0xFF, 0xFF, 0x7F}},
    };
    for (size_t i = 0; i < 4; i++) {
        struct oc_sample s = good();
        s.value = c[i].v;
        uint8_t buf[OC_SAMPLE_SIZE];
        CHECK(oc_sample_encode(&s, buf, sizeof buf) == 0);
        CHECK(memcmp(buf + 16, c[i].b, 4) == 0);
    }
}

static void test_sample_encode_rejects_invalid(void)
{
    struct oc_sample bad[9];
    int n = 0;
    bad[n] = good(); bad[n++].source = 0;
    bad[n] = good(); bad[n++].source = 3;
    bad[n] = good(); bad[n++].origin = 0;
    bad[n] = good(); bad[n++].origin = OC_ORIGIN_MIXED;
    bad[n] = good(); bad[n++].origin = OC_ORIGIN_NONE;
    bad[n] = good(); bad[n].value = 0; bad[n++].status = 0;
    bad[n] = good(); bad[n].value = 0; bad[n++].status = OC_ST_STALE + 1;
    bad[n] = good(); bad[n].value = 0; bad[n++].status = 255;
    bad[n] = good(); bad[n].status = OC_ST_TIMEOUT; bad[n++].value = 7; /* non-OK needs value 0 */
    for (int i = 0; i < n; i++) {
        uint8_t buf[OC_SAMPLE_SIZE];
        memset(buf, SENTINEL, sizeof buf);
        CHECK(oc_sample_encode(&bad[i], buf, sizeof buf) == -EINVAL);
        for (size_t k = 0; k < sizeof buf; k++)
            CHECK(buf[k] == SENTINEL);
    }
}

static void test_sample_validate(void)
{
    struct oc_sample s = good();
    CHECK(oc_sample_validate(NULL) == -EINVAL);
    CHECK(oc_sample_validate(&s) == 0);
    s.status = OC_ST_STALE; s.value = 0;
    CHECK(oc_sample_validate(&s) == 0);
    s.status = OC_ST_STALE + 1;
    CHECK(oc_sample_validate(&s) == -EINVAL);
    s = good(); s.origin = OC_ORIGIN_MIXED;
    CHECK(oc_sample_validate(&s) == -EINVAL);
    s = good(); s.source = 3;
    CHECK(oc_sample_validate(&s) == -EINVAL);
    s = good(); s.status = OC_ST_NACK;   /* value -2 with non-OK status */
    CHECK(oc_sample_validate(&s) == -EINVAL);
}

static void test_sample_encode_args_and_capacity(void)
{
    struct oc_sample s = good();
    uint8_t buf[OC_SAMPLE_SIZE + 4];
    memset(buf, SENTINEL, sizeof buf);
    CHECK(oc_sample_encode(NULL, buf, sizeof buf) == -EINVAL);
    CHECK(oc_sample_encode(&s, NULL, sizeof buf) == -EINVAL);
    CHECK(oc_sample_encode(&s, buf, 0) == -EMSGSIZE);
    CHECK(oc_sample_encode(&s, buf, OC_SAMPLE_SIZE - 1) == -EMSGSIZE);
    for (size_t k = 0; k < sizeof buf; k++)
        CHECK(buf[k] == SENTINEL);
    /* Larger capacity is fine; bytes past 24 are not touched. */
    CHECK(oc_sample_encode(&s, buf, sizeof buf) == 0);
    CHECK(memcmp(buf, golden_payload, OC_SAMPLE_SIZE) == 0);
    for (size_t k = OC_SAMPLE_SIZE; k < sizeof buf; k++)
        CHECK(buf[k] == SENTINEL);
}

/* Decode a mutated copy of the golden payload; expect err and out unchanged. */
static void expect_decode_err(const uint8_t *buf, size_t len, int err)
{
    struct oc_sample out;
    memset(&out, SENTINEL, sizeof out);
    struct oc_sample before = out;
    CHECK(oc_sample_decode(buf, len, &out) == err);
    CHECK(memcmp(&out, &before, sizeof out) == 0);
}

static void test_sample_decode_rejects(void)
{
    uint8_t m[OC_SAMPLE_SIZE];

    /* source */
    memcpy(m, golden_payload, sizeof m); m[12] = 0; expect_decode_err(m, sizeof m, -EBADMSG);
    memcpy(m, golden_payload, sizeof m); m[12] = 3; expect_decode_err(m, sizeof m, -EBADMSG);
    memcpy(m, golden_payload, sizeof m); m[12] = 255; expect_decode_err(m, sizeof m, -EBADMSG);
    /* origin: 0, MIXED, NONE and beyond are all invalid on a sample */
    uint8_t bad_origin[] = {0, 4, 5, 6, 255};
    for (size_t i = 0; i < sizeof bad_origin; i++) {
        memcpy(m, golden_payload, sizeof m); m[13] = bad_origin[i];
        expect_decode_err(m, sizeof m, -EBADMSG);
    }
    /* status */
    /* value zeroed so that only the status range check can reject these */
    uint8_t bad_status[] = {0, 7, 255};
    for (size_t i = 0; i < sizeof bad_status; i++) {
        memcpy(m, golden_payload, sizeof m);
        m[14] = bad_status[i];
        memset(m + 16, 0, 4);
        expect_decode_err(m, sizeof m, -EBADMSG);
    }
    /* reserved byte */
    for (unsigned v = 1; v <= 255; v += 127) {
        memcpy(m, golden_payload, sizeof m); m[15] = (uint8_t)v;
        expect_decode_err(m, sizeof m, -EBADMSG);
    }
    /* non-OK status with nonzero value (golden value is -2) */
    memcpy(m, golden_payload, sizeof m); m[14] = OC_ST_NACK;
    expect_decode_err(m, sizeof m, -EBADMSG);
    /* non-OK status with zero value is valid */
    memcpy(m, golden_payload, sizeof m); m[14] = OC_ST_NACK; memset(m + 16, 0, 4);
    struct oc_sample out;
    CHECK(oc_sample_decode(m, sizeof m, &out) == 0);
    CHECK(out.status == OC_ST_NACK && out.value == 0);
}

static void test_sample_decode_lengths_and_args(void)
{
    uint8_t big[OC_SAMPLE_SIZE + 4];
    memcpy(big, golden_payload, OC_SAMPLE_SIZE);
    memset(big + OC_SAMPLE_SIZE, 0, 4);
    size_t bad_len[] = {0, 1, 23, 25, 28};
    for (size_t i = 0; i < sizeof bad_len / sizeof bad_len[0]; i++)
        expect_decode_err(big, bad_len[i], -EMSGSIZE);

    struct oc_sample out;
    CHECK(oc_sample_decode(NULL, OC_SAMPLE_SIZE, &out) == -EINVAL);
    CHECK(oc_sample_decode(golden_payload, OC_SAMPLE_SIZE, NULL) == -EINVAL);
}

static void test_header_golden(void)
{
    struct oc_msg_header h = {OC_PROTO_VERSION, OC_MSG_SAMPLE, OC_SAMPLE_SIZE};
    uint8_t buf[OC_MSG_HEADER_SIZE];
    const uint8_t want[4] = {0x01, 0x04, 0x18, 0x00};
    CHECK(oc_msg_header_encode(&h, buf, sizeof buf) == 0);
    CHECK(memcmp(buf, want, 4) == 0);

    struct oc_msg_header back = {0};
    CHECK(oc_msg_header_decode(want, sizeof want, &back) == 0);
    CHECK(back.version == 1 && back.type == OC_MSG_SAMPLE && back.payload_len == 24);
}

static void test_header_little_endian_length(void)
{
    /* Payload layouts for these types are not defined yet (#16), so any
     * u16 length is accepted by the envelope. */
    struct oc_msg_header h = {OC_PROTO_VERSION, OC_MSG_HELLO, 0x1234};
    uint8_t buf[4];
    CHECK(oc_msg_header_encode(&h, buf, sizeof buf) == 0);
    CHECK(buf[2] == 0x34 && buf[3] == 0x12);
    struct oc_msg_header back;
    CHECK(oc_msg_header_decode(buf, sizeof buf, &back) == 0);
    CHECK(back.payload_len == 0x1234 && back.type == OC_MSG_HELLO);

    for (uint8_t t = OC_MSG_HELLO; t <= OC_MSG_BYE; t++) {
        if (t == OC_MSG_SAMPLE)
            continue;
        h.type = t;
        h.payload_len = UINT16_MAX;
        CHECK(oc_msg_header_encode(&h, buf, sizeof buf) == 0);
        CHECK(oc_msg_header_decode(buf, sizeof buf, &back) == 0);
        CHECK(back.type == t && back.payload_len == UINT16_MAX);
    }
}

static void test_header_encode_rejects(void)
{
    uint8_t buf[8];
    struct oc_msg_header ok = {OC_PROTO_VERSION, OC_MSG_BYE, 0};
    struct oc_msg_header h;

    memset(buf, SENTINEL, sizeof buf);
    CHECK(oc_msg_header_encode(NULL, buf, sizeof buf) == -EINVAL);
    CHECK(oc_msg_header_encode(&ok, NULL, sizeof buf) == -EINVAL);
    CHECK(oc_msg_header_encode(&ok, buf, 3) == -EMSGSIZE);
    h = ok; h.version = 0;
    CHECK(oc_msg_header_encode(&h, buf, sizeof buf) == -EPROTONOSUPPORT);
    h = ok; h.version = 2;
    CHECK(oc_msg_header_encode(&h, buf, sizeof buf) == -EPROTONOSUPPORT);
    h = ok; h.type = 0;
    CHECK(oc_msg_header_encode(&h, buf, sizeof buf) == -EINVAL);
    h = ok; h.type = OC_MSG_BYE + 1;
    CHECK(oc_msg_header_encode(&h, buf, sizeof buf) == -EINVAL);
    h = ok; h.type = OC_MSG_SAMPLE; h.payload_len = 23;
    CHECK(oc_msg_header_encode(&h, buf, sizeof buf) == -EINVAL);
    h = ok; h.type = OC_MSG_SAMPLE; h.payload_len = 28;
    CHECK(oc_msg_header_encode(&h, buf, sizeof buf) == -EINVAL);
    for (size_t k = 0; k < sizeof buf; k++)
        CHECK(buf[k] == SENTINEL);
}

static void expect_header_err(const uint8_t *b, size_t len, int err)
{
    struct oc_msg_header out;
    memset(&out, SENTINEL, sizeof out);
    struct oc_msg_header before = out;
    CHECK(oc_msg_header_decode(b, len, &out) == err);
    CHECK(memcmp(&out, &before, sizeof out) == 0);
}

static void test_header_decode_rejects(void)
{
    const uint8_t v0[4] = {0x00, 0x04, 0x18, 0x00};
    const uint8_t v2[4] = {0x02, 0x04, 0x18, 0x00};
    const uint8_t t0[4] = {0x01, 0x00, 0x18, 0x00};
    const uint8_t t6[4] = {0x01, 0x06, 0x00, 0x00};
    const uint8_t l23[4] = {0x01, 0x04, 0x17, 0x00};
    const uint8_t l280[4] = {0x01, 0x04, 0x18, 0x01};
    uint8_t ok[8] = {0x01, 0x04, 0x18, 0x00, 0, 0, 0, 0};

    expect_header_err(v0, 4, -EPROTONOSUPPORT);
    expect_header_err(v2, 4, -EPROTONOSUPPORT);
    expect_header_err(t0, 4, -EBADMSG);
    expect_header_err(t6, 4, -EBADMSG);
    expect_header_err(l23, 4, -EBADMSG);
    expect_header_err(l280, 4, -EBADMSG);
    expect_header_err(ok, 0, -EMSGSIZE);
    expect_header_err(ok, 3, -EMSGSIZE);
    expect_header_err(ok, 5, -EMSGSIZE); /* header decoder wants exactly 4 */
    struct oc_msg_header out;
    CHECK(oc_msg_header_decode(NULL, 4, &out) == -EINVAL);
    CHECK(oc_msg_header_decode(ok, 4, NULL) == -EINVAL);
}

static void test_msg_sample_golden(void)
{
    struct oc_sample s = good();
    uint8_t buf[OC_MSG_SAMPLE_SIZE];
    size_t n = 999;
    uint8_t want[28] = {0x01, 0x04, 0x18, 0x00};
    memcpy(want + 4, golden_payload, 24);

    CHECK(oc_msg_sample_encode(&s, buf, sizeof buf, &n) == 0);
    CHECK(n == 28);
    CHECK(memcmp(buf, want, 28) == 0);

    struct oc_sample back;
    CHECK(oc_msg_sample_decode(want, sizeof want, &back) == 0);
    CHECK(same_sample(&s, &back));
}

static void test_msg_sample_encode_errors(void)
{
    struct oc_sample s = good();
    uint8_t buf[40];
    size_t n = 777;
    memset(buf, SENTINEL, sizeof buf);

    CHECK(oc_msg_sample_encode(NULL, buf, sizeof buf, &n) == -EINVAL);
    CHECK(oc_msg_sample_encode(&s, NULL, sizeof buf, &n) == -EINVAL);
    CHECK(oc_msg_sample_encode(&s, buf, sizeof buf, NULL) == -EINVAL);
    CHECK(oc_msg_sample_encode(&s, buf, 27, &n) == -EMSGSIZE);
    CHECK(oc_msg_sample_encode(&s, buf, 0, &n) == -EMSGSIZE);
    s.origin = OC_ORIGIN_MIXED;
    CHECK(oc_msg_sample_encode(&s, buf, sizeof buf, &n) == -EINVAL);
    CHECK(n == 777);
    for (size_t k = 0; k < sizeof buf; k++)
        CHECK(buf[k] == SENTINEL);

    /* Extra capacity: only the first 28 bytes are written. */
    s = good();
    CHECK(oc_msg_sample_encode(&s, buf, sizeof buf, &n) == 0);
    CHECK(n == 28);
    for (size_t k = 28; k < sizeof buf; k++)
        CHECK(buf[k] == SENTINEL);
}

static void test_msg_sample_decode_rejects(void)
{
    uint8_t m[28] = {0x01, 0x04, 0x18, 0x00};
    memcpy(m + 4, golden_payload, 24);
    uint8_t x[32];
    struct oc_sample out, before;
    memset(&out, SENTINEL, sizeof out);
    before = out;

    /* datagram length must be exactly 4 + payload_len */
    memcpy(x, m, 28); memset(x + 28, 0, 4);
    size_t bad_len[] = {0, 3, 4, 27, 29, 32};
    for (size_t i = 0; i < sizeof bad_len / sizeof bad_len[0]; i++) {
        CHECK(oc_msg_sample_decode(x, bad_len[i], &out) == -EMSGSIZE);
        CHECK(memcmp(&out, &before, sizeof out) == 0);
    }
    /* version */
    memcpy(x, m, 28); x[0] = 2;
    CHECK(oc_msg_sample_decode(x, 28, &out) == -EPROTONOSUPPORT);
    memcpy(x, m, 28); x[0] = 0;
    CHECK(oc_msg_sample_decode(x, 28, &out) == -EPROTONOSUPPORT);
    /* wrong / unknown type */
    memcpy(x, m, 28); x[1] = OC_MSG_BYE;
    CHECK(oc_msg_sample_decode(x, 28, &out) == -EBADMSG);
    memcpy(x, m, 28); x[1] = 0;
    CHECK(oc_msg_sample_decode(x, 28, &out) == -EBADMSG);
    /* payload_len says 28 (the datagram size) — the classic off-by-4 mistake */
    memcpy(x, m, 28); x[2] = 0x1C;
    CHECK(oc_msg_sample_decode(x, 28, &out) == -EBADMSG);
    /* bad payload fields */
    memcpy(x, m, 28); x[4 + 15] = 1;
    CHECK(oc_msg_sample_decode(x, 28, &out) == -EBADMSG);
    memcpy(x, m, 28); x[4 + 13] = OC_ORIGIN_NONE;
    CHECK(oc_msg_sample_decode(x, 28, &out) == -EBADMSG);
    memcpy(x, m, 28); x[4 + 12] = 0;
    CHECK(oc_msg_sample_decode(x, 28, &out) == -EBADMSG);
    memcpy(x, m, 28); x[4 + 14] = 0;
    CHECK(oc_msg_sample_decode(x, 28, &out) == -EBADMSG);
    CHECK(memcmp(&out, &before, sizeof out) == 0);

    CHECK(oc_msg_sample_decode(NULL, 28, &out) == -EINVAL);
    CHECK(oc_msg_sample_decode(m, 28, NULL) == -EINVAL);
}

static void test_no_fabricated_reading_on_error(void)
{
    /* A failed decode must not leave anything that looks like a valid sample:
     * the caller's struct keeps whatever it held, including an all-zero one. */
    struct oc_sample out;
    memset(&out, 0, sizeof out);
    uint8_t m[24];
    memcpy(m, golden_payload, sizeof m);
    m[15] = 9;
    CHECK(oc_sample_decode(m, sizeof m, &out) == -EBADMSG);
    struct oc_sample zero;
    memset(&zero, 0, sizeof zero);
    CHECK(memcmp(&out, &zero, sizeof out) == 0);
}

int main(void)
{
    RUN(test_constants);
    RUN(test_sample_golden_encode);
    RUN(test_sample_golden_decode);
    RUN(test_sample_roundtrip_all_valid_enums);
    RUN(test_sample_boundaries);
    RUN(test_sample_signed_value_bytes);
    RUN(test_sample_encode_rejects_invalid);
    RUN(test_sample_validate);
    RUN(test_sample_encode_args_and_capacity);
    RUN(test_sample_decode_rejects);
    RUN(test_sample_decode_lengths_and_args);
    RUN(test_header_golden);
    RUN(test_header_little_endian_length);
    RUN(test_header_encode_rejects);
    RUN(test_header_decode_rejects);
    RUN(test_msg_sample_golden);
    RUN(test_msg_sample_encode_errors);
    RUN(test_msg_sample_decode_rejects);
    RUN(test_no_fabricated_reading_on_error);

    if (failures != 0) {
        fprintf(stderr, "test_sample: %d FAILURE(S)\n", failures);
        return 1;
    }
    fprintf(stderr, "test_sample: all passed\n");
    return 0;
}
