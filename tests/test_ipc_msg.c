/* Tests for the IPC control-message codec and HELLO policy (include/oc_ipc_msg.h).
 *
 * Every input is a synthetic byte string or struct built here. Nothing is a
 * live sensor measurement and no socket, file or hardware is touched.
 *
 * Error contract under test: 0 or a negative errno; on any error the output
 * buffer / struct is left byte-for-byte unchanged. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oc_ipc_msg.h"

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

#define SESSION 0x1122334455667788ull

static const uint8_t golden_hello[24] = {
    0x01, 0x01, 0x14, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
    0x0D, 0x0C, 0x0B, 0x0A, 0x04, 0x03, 0x02, 0x01};
static const uint8_t golden_ack[20] = {
    0x01, 0x02, 0x10, 0x00,
    0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
    0x0D, 0x0C, 0x0B, 0x0A, 0x02, 0x00, 0x00, 0x00};
static const uint8_t golden_reject[12] = {
    0x01, 0x03, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04};
static const uint8_t golden_bye[8] = {
    0x01, 0x05, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00};

static struct oc_hello good_hello(void)
{
    return (struct oc_hello){.source = OC_SOURCE_LIGHT, .origin = OC_ORIGIN_SIMULATED,
                             .session_id = SESSION, .instance = 0x0A0B0C0D,
                             .pid = 0x01020304};
}
static struct oc_hello_ack good_ack(void)
{
    return (struct oc_hello_ack){.session_id = SESSION, .instance = 0x0A0B0C0D,
                                 .source = OC_SOURCE_DISTANCE};
}
static struct oc_hello_reject good_reject(void)
{
    return (struct oc_hello_reject){.reason = OC_REJECT_SESSION_MISMATCH,
                                    .detail = 0x04030201};
}
static struct oc_bye good_bye(void) { return (struct oc_bye){.reason = OC_BYE_SHUTDOWN}; }

static struct oc_sample good_sample(void)
{
    return (struct oc_sample){.t_mono_ns = 123456789, .seq = 7, .source = OC_SOURCE_LIGHT,
                              .origin = OC_ORIGIN_SIMULATED, .status = OC_ST_OK,
                              .value = -5, .aux = 0x4383};
}

/* ---- constants ---- */

static void test_constants(void)
{
    CHECK(OC_HELLO_SIZE == 20 && OC_HELLO_ACK_SIZE == 16 &&
          OC_HELLO_REJECT_SIZE == 8 && OC_BYE_SIZE == 4);
    CHECK(OC_MSG_HELLO_SIZE == 24 && OC_MSG_HELLO_ACK_SIZE == 20 &&
          OC_MSG_HELLO_REJECT_SIZE == 12 && OC_MSG_BYE_SIZE == 8);
    CHECK(OC_IPC_MAX_MSG == 28);   /* SAMPLE is the largest defined datagram */
    CHECK(OC_REJECT_MALFORMED == 1 && OC_REJECT_BAD_SOURCE == 2 &&
          OC_REJECT_BAD_ORIGIN == 3 && OC_REJECT_SESSION_MISMATCH == 4 &&
          OC_REJECT_PEER_CRED == 5 && OC_REJECT_DUPLICATE_SOURCE == 6 &&
          OC_REJECT_STALE_INSTANCE == 7 && OC_REJECT_NOT_READY == 8);
    CHECK(OC_BYE_NORMAL == 1 && OC_BYE_SHUTDOWN == 2 && OC_BYE_ERROR == 3);
}

/* ---- encode ---- */

static void test_golden_encode(void)
{
    uint8_t b[64];
    size_t n;
    struct oc_hello h = good_hello();
    struct oc_hello_ack a = good_ack();
    struct oc_hello_reject r = good_reject();
    struct oc_bye y = good_bye();

    n = 0; memset(b, 0xEE, sizeof b);
    CHECK(oc_ipc_encode_hello(&h, b, sizeof b, &n) == 0);
    CHECK(n == 24 && memcmp(b, golden_hello, 24) == 0 && b[24] == 0xEE);

    n = 0; memset(b, 0xEE, sizeof b);
    CHECK(oc_ipc_encode_hello_ack(&a, b, sizeof b, &n) == 0);
    CHECK(n == 20 && memcmp(b, golden_ack, 20) == 0 && b[20] == 0xEE);

    n = 0; memset(b, 0xEE, sizeof b);
    CHECK(oc_ipc_encode_hello_reject(&r, b, sizeof b, &n) == 0);
    CHECK(n == 12 && memcmp(b, golden_reject, 12) == 0 && b[12] == 0xEE);

    n = 0; memset(b, 0xEE, sizeof b);
    CHECK(oc_ipc_encode_bye(&y, b, sizeof b, &n) == 0);
    CHECK(n == 8 && memcmp(b, golden_bye, 8) == 0 && b[8] == 0xEE);
}

static void test_encode_rejects_invalid(void)
{
    uint8_t b[64], ref[64];
    size_t n = 99;
    memset(ref, 0xEE, sizeof ref);

#define EXPECT_EINVAL(call)                        \
    do {                                           \
        memset(b, 0xEE, sizeof b); n = 99;         \
        CHECK((call) == -EINVAL);                  \
        CHECK(memcmp(b, ref, sizeof b) == 0);      \
        CHECK(n == 99);                            \
    } while (0)

    struct oc_hello h;
    h = good_hello(); h.source = 0;                  EXPECT_EINVAL(oc_ipc_encode_hello(&h, b, sizeof b, &n));
    h = good_hello(); h.source = 3;                  EXPECT_EINVAL(oc_ipc_encode_hello(&h, b, sizeof b, &n));
    h = good_hello(); h.origin = 0;                  EXPECT_EINVAL(oc_ipc_encode_hello(&h, b, sizeof b, &n));
    h = good_hello(); h.origin = OC_ORIGIN_MIXED;    EXPECT_EINVAL(oc_ipc_encode_hello(&h, b, sizeof b, &n));
    h = good_hello(); h.origin = OC_ORIGIN_NONE;     EXPECT_EINVAL(oc_ipc_encode_hello(&h, b, sizeof b, &n));
    h = good_hello(); h.session_id = 0;              EXPECT_EINVAL(oc_ipc_encode_hello(&h, b, sizeof b, &n));
    h = good_hello();
    EXPECT_EINVAL(oc_ipc_encode_hello(nullptr, b, sizeof b, &n));
    EXPECT_EINVAL(oc_ipc_encode_hello(&h, nullptr, sizeof b, &n));
    EXPECT_EINVAL(oc_ipc_encode_hello(&h, b, sizeof b, nullptr));

    struct oc_hello_ack a;
    a = good_ack(); a.source = 0;                    EXPECT_EINVAL(oc_ipc_encode_hello_ack(&a, b, sizeof b, &n));
    a = good_ack(); a.source = 3;                    EXPECT_EINVAL(oc_ipc_encode_hello_ack(&a, b, sizeof b, &n));
    a = good_ack(); a.session_id = 0;                EXPECT_EINVAL(oc_ipc_encode_hello_ack(&a, b, sizeof b, &n));
    EXPECT_EINVAL(oc_ipc_encode_hello_ack(nullptr, b, sizeof b, &n));

    struct oc_hello_reject r;
    r = good_reject(); r.reason = 0;                 EXPECT_EINVAL(oc_ipc_encode_hello_reject(&r, b, sizeof b, &n));
    r = good_reject(); r.reason = 9;                 EXPECT_EINVAL(oc_ipc_encode_hello_reject(&r, b, sizeof b, &n));
    EXPECT_EINVAL(oc_ipc_encode_hello_reject(nullptr, b, sizeof b, &n));

    struct oc_bye y;
    y = good_bye(); y.reason = 0;                    EXPECT_EINVAL(oc_ipc_encode_bye(&y, b, sizeof b, &n));
    y = good_bye(); y.reason = 4;                    EXPECT_EINVAL(oc_ipc_encode_bye(&y, b, sizeof b, &n));
    EXPECT_EINVAL(oc_ipc_encode_bye(nullptr, b, sizeof b, &n));
#undef EXPECT_EINVAL
}

static void test_encode_capacity(void)
{
    struct oc_hello h = good_hello();
    struct oc_hello_ack a = good_ack();
    struct oc_hello_reject r = good_reject();
    struct oc_bye y = good_bye();
    uint8_t b[64], ref[64];
    size_t n;
    memset(ref, 0xEE, sizeof ref);

    for (size_t cap = 0; cap < 24; cap++) {
        memset(b, 0xEE, sizeof b); n = 99;
        CHECK(oc_ipc_encode_hello(&h, b, cap, &n) == -EMSGSIZE);
        CHECK(memcmp(b, ref, sizeof b) == 0 && n == 99);
    }
    for (size_t cap = 0; cap < 20; cap++) {
        memset(b, 0xEE, sizeof b); n = 99;
        CHECK(oc_ipc_encode_hello_ack(&a, b, cap, &n) == -EMSGSIZE);
        CHECK(memcmp(b, ref, sizeof b) == 0 && n == 99);
    }
    for (size_t cap = 0; cap < 12; cap++) {
        memset(b, 0xEE, sizeof b); n = 99;
        CHECK(oc_ipc_encode_hello_reject(&r, b, cap, &n) == -EMSGSIZE);
        CHECK(memcmp(b, ref, sizeof b) == 0 && n == 99);
    }
    for (size_t cap = 0; cap < 8; cap++) {
        memset(b, 0xEE, sizeof b); n = 99;
        CHECK(oc_ipc_encode_bye(&y, b, cap, &n) == -EMSGSIZE);
        CHECK(memcmp(b, ref, sizeof b) == 0 && n == 99);
    }
    /* exact capacity works */
    CHECK(oc_ipc_encode_hello(&h, b, 24, &n) == 0 && n == 24);
    CHECK(oc_ipc_encode_bye(&y, b, 8, &n) == 0 && n == 8);
}

/* ---- roundtrip ---- */

static void test_roundtrip(void)
{
    uint8_t b[64];
    size_t n;
    struct oc_ipc_msg m;

    for (uint8_t src = OC_SOURCE_LIGHT; src <= OC_SOURCE_DISTANCE; src++) {
        for (uint8_t org = OC_ORIGIN_LIVE; org <= OC_ORIGIN_REPLAYED; org++) {
            static const uint32_t insts[] = {0, 1, 0x7FFFFFFF, 0xFFFFFFFF};
            static const uint64_t sess[] = {1, SESSION, UINT64_MAX};
            for (size_t i = 0; i < 4; i++) {
                for (size_t j = 0; j < 3; j++) {
                    struct oc_hello h = {src, org, sess[j], insts[i], 0xFFFFFFFFu};
                    CHECK(oc_ipc_encode_hello(&h, b, sizeof b, &n) == 0);
                    CHECK(oc_ipc_msg_decode(b, n, &m) == 0);
                    CHECK(m.type == OC_MSG_HELLO && m.u.hello.source == src &&
                          m.u.hello.origin == org && m.u.hello.session_id == sess[j] &&
                          m.u.hello.instance == insts[i] && m.u.hello.pid == 0xFFFFFFFFu);
                    struct oc_hello_ack a = {sess[j], insts[i], src};
                    CHECK(oc_ipc_encode_hello_ack(&a, b, sizeof b, &n) == 0);
                    CHECK(oc_ipc_msg_decode(b, n, &m) == 0);
                    CHECK(m.type == OC_MSG_HELLO_ACK && m.u.ack.session_id == sess[j] &&
                          m.u.ack.instance == insts[i] && m.u.ack.source == src);
                }
            }
        }
    }
    for (uint16_t rs = OC_REJECT_MALFORMED; rs <= OC_REJECT_NOT_READY; rs++) {
        struct oc_hello_reject r = {rs, 0xDEADBEEF};
        CHECK(oc_ipc_encode_hello_reject(&r, b, sizeof b, &n) == 0);
        CHECK(oc_ipc_msg_decode(b, n, &m) == 0);
        CHECK(m.type == OC_MSG_HELLO_REJECT && m.u.reject.reason == rs &&
              m.u.reject.detail == 0xDEADBEEF);
    }
    for (uint16_t bs = OC_BYE_NORMAL; bs <= OC_BYE_ERROR; bs++) {
        struct oc_bye y = {bs};
        CHECK(oc_ipc_encode_bye(&y, b, sizeof b, &n) == 0);
        CHECK(oc_ipc_msg_decode(b, n, &m) == 0);
        CHECK(m.type == OC_MSG_BYE && m.u.bye.reason == bs);
    }
}

static void test_golden_decode(void)
{
    struct oc_ipc_msg m;
    CHECK(oc_ipc_msg_decode(golden_hello, 24, &m) == 0 && m.type == OC_MSG_HELLO);
    CHECK(m.u.hello.source == OC_SOURCE_LIGHT && m.u.hello.origin == OC_ORIGIN_SIMULATED &&
          m.u.hello.session_id == SESSION && m.u.hello.instance == 0x0A0B0C0D &&
          m.u.hello.pid == 0x01020304);
    CHECK(oc_ipc_msg_decode(golden_ack, 20, &m) == 0 && m.type == OC_MSG_HELLO_ACK);
    CHECK(m.u.ack.session_id == SESSION && m.u.ack.instance == 0x0A0B0C0D &&
          m.u.ack.source == OC_SOURCE_DISTANCE);
    CHECK(oc_ipc_msg_decode(golden_reject, 12, &m) == 0 && m.type == OC_MSG_HELLO_REJECT);
    CHECK(m.u.reject.reason == OC_REJECT_SESSION_MISMATCH && m.u.reject.detail == 0x04030201);
    CHECK(oc_ipc_msg_decode(golden_bye, 8, &m) == 0 && m.type == OC_MSG_BYE);
    CHECK(m.u.bye.reason == OC_BYE_SHUTDOWN);
}

/* ---- decode rejection ---- */

struct vec {
    const uint8_t *bytes;
    size_t len;
};
#define VECS ((const struct vec[]){{golden_hello, 24}, {golden_ack, 20}, \
                                    {golden_reject, 12}, {golden_bye, 8}})

/* Decodes `buf` and checks `want`, and that the output was not touched. */
static void expect_decode(const uint8_t *buf, size_t len, int want, int line)
{
    struct oc_ipc_msg m, ref;
    memset(&m, 0xA5, sizeof m);
    memset(&ref, 0xA5, sizeof ref);
    int rc = oc_ipc_msg_decode(buf, len, &m);
    if (rc != want) {
        fprintf(stderr, "FAIL %s:%d: decode len %zu returned %d, want %d\n", __FILE__,
                line, len, rc, want);
        failures++;
    }
    if (want != 0 && memcmp(&m, &ref, sizeof m) != 0) {
        fprintf(stderr, "FAIL %s:%d: output modified on error\n", __FILE__, line);
        failures++;
    }
}
#define EXPECT_DECODE(buf, len, want) expect_decode((buf), (len), (want), __LINE__)

static void test_decode_truncated_and_trailing(void)
{
    uint8_t b[64];
    for (size_t v = 0; v < 4; v++) {
        const struct vec s = VECS[v];
        for (size_t len = 0; len < s.len; len++)
            EXPECT_DECODE(s.bytes, len, -EMSGSIZE);
        memcpy(b, s.bytes, s.len);
        b[s.len] = 0x00;
        EXPECT_DECODE(b, s.len + 1, -EMSGSIZE);
        memset(b + s.len, 0x7F, 40 - s.len);
        EXPECT_DECODE(b, 40, -EMSGSIZE);
    }
}

static void test_decode_exact_payload_length_per_type(void)
{
    uint8_t b[64];
    /* The header says a wrong payload length and the datagram is consistent
     * with that header: still rejected, because each type has ONE legal length. */
    for (size_t v = 0; v < 4; v++) {
        const struct vec s = VECS[v];
        const uint16_t good = (uint16_t)(s.len - 4);
        for (int delta = -4; delta <= 8; delta++) {
            if (delta == 0 || (int)good + delta < 0)
                continue;
            uint16_t plen = (uint16_t)((int)good + delta);
            memset(b, 0, sizeof b);
            memcpy(b, s.bytes, s.len);
            b[2] = (uint8_t)(plen & 0xFF);
            b[3] = (uint8_t)(plen >> 8);
            EXPECT_DECODE(b, 4u + plen, -EBADMSG);
        }
    }
    /* header length 0x1234 with a matching 4-byte datagram is not a HELLO */
    memcpy(b, golden_hello, 4);
    b[2] = 0x34; b[3] = 0x12;
    EXPECT_DECODE(b, 4, -EMSGSIZE);
    /* a bare header (payload_len 0) for each control type */
    for (uint8_t t = OC_MSG_HELLO; t <= OC_MSG_BYE; t++) {
        uint8_t hdr[4] = {OC_PROTO_VERSION, t, 0, 0};
        EXPECT_DECODE(hdr, 4, -EBADMSG);
    }
}

static void test_decode_reserved_and_enums(void)
{
    uint8_t b[64];
    /* every reserved byte, each nonzero */
    static const struct { size_t v, off; } reserved[] = {
        {0, 6}, {0, 7}, {1, 17}, {1, 18}, {1, 19}, {2, 6}, {2, 7}, {3, 6}, {3, 7}};
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        const struct vec s = VECS[reserved[i].v];
        for (int val = 1; val <= 255; val += 127) {
            memcpy(b, s.bytes, s.len);
            b[reserved[i].off] = (uint8_t)val;
            EXPECT_DECODE(b, s.len, -EBADMSG);
        }
    }
    /* HELLO enums */
    memcpy(b, golden_hello, 24); b[4] = 0;                EXPECT_DECODE(b, 24, -EBADMSG);
    memcpy(b, golden_hello, 24); b[4] = 3;                EXPECT_DECODE(b, 24, -EBADMSG);
    memcpy(b, golden_hello, 24); b[5] = 0;                EXPECT_DECODE(b, 24, -EBADMSG);
    memcpy(b, golden_hello, 24); b[5] = OC_ORIGIN_MIXED;  EXPECT_DECODE(b, 24, -EBADMSG);
    memcpy(b, golden_hello, 24); b[5] = OC_ORIGIN_NONE;   EXPECT_DECODE(b, 24, -EBADMSG);
    memcpy(b, golden_hello, 24); memset(b + 8, 0, 8);     EXPECT_DECODE(b, 24, -EBADMSG);  /* session 0 */
    {   /* instance 0 is legal */
        struct oc_ipc_msg m;
        memcpy(b, golden_hello, 24); memset(b + 16, 0, 4);
        CHECK(oc_ipc_msg_decode(b, 24, &m) == 0 && m.u.hello.instance == 0);
    }
    /* ACK enums */
    memcpy(b, golden_ack, 20); b[16] = 0;                 EXPECT_DECODE(b, 20, -EBADMSG);
    memcpy(b, golden_ack, 20); b[16] = 3;                 EXPECT_DECODE(b, 20, -EBADMSG);
    memcpy(b, golden_ack, 20); memset(b + 4, 0, 8);       EXPECT_DECODE(b, 20, -EBADMSG);
    /* REJECT reason */
    memcpy(b, golden_reject, 12); b[4] = 0;               EXPECT_DECODE(b, 12, -EBADMSG);
    memcpy(b, golden_reject, 12); b[4] = 9;               EXPECT_DECODE(b, 12, -EBADMSG);
    memcpy(b, golden_reject, 12); b[5] = 1;               EXPECT_DECODE(b, 12, -EBADMSG);  /* 0x104 */
    /* BYE reason */
    memcpy(b, golden_bye, 8); b[4] = 0;                   EXPECT_DECODE(b, 8, -EBADMSG);
    memcpy(b, golden_bye, 8); b[4] = 4;                   EXPECT_DECODE(b, 8, -EBADMSG);
}

static void test_decode_type_and_version(void)
{
    uint8_t b[64];
    /* unknown types with an otherwise sane length */
    static const uint8_t bad_types[] = {0, 6, 7, 127, 255};
    for (size_t i = 0; i < sizeof bad_types; i++) {
        memcpy(b, golden_bye, 8);
        b[1] = bad_types[i];
        EXPECT_DECODE(b, 8, -EBADMSG);
    }
    /* Version is checked first, for every type and for ANY length (even 1
     * byte and datagrams far larger than defined): connection-fatal. */
    static const uint8_t bad_vers[] = {0, 2, 3, 127, 255};
    for (size_t i = 0; i < sizeof bad_vers; i++) {
        for (size_t v = 0; v < 4; v++) {
            const struct vec s = VECS[v];
            memcpy(b, s.bytes, s.len);
            b[0] = bad_vers[i];
            EXPECT_DECODE(b, s.len, -EPROTONOSUPPORT);
        }
        memset(b, 0x5A, sizeof b);
        b[0] = bad_vers[i];
        EXPECT_DECODE(b, 1, -EPROTONOSUPPORT);
        EXPECT_DECODE(b, 3, -EPROTONOSUPPORT);
        EXPECT_DECODE(b, 64, -EPROTONOSUPPORT);
        /* bad version with an unknown type is still a version error */
        b[1] = 99;
        EXPECT_DECODE(b, 4, -EPROTONOSUPPORT);
    }
    /* an empty datagram has no version to read */
    EXPECT_DECODE(b, 0, -EMSGSIZE);
}

static void test_decode_args(void)
{
    struct oc_ipc_msg m;
    CHECK(oc_ipc_msg_decode(nullptr, 24, &m) == -EINVAL);
    CHECK(oc_ipc_msg_decode(golden_hello, 24, nullptr) == -EINVAL);
}

static void test_decode_sample_passthrough(void)
{
    struct oc_sample s = good_sample();
    uint8_t b[64];
    size_t n = 0;
    struct oc_ipc_msg m;
    CHECK(oc_msg_sample_encode(&s, b, sizeof b, &n) == 0 && n == 28);
    CHECK(oc_ipc_msg_decode(b, n, &m) == 0);
    CHECK(m.type == OC_MSG_SAMPLE && memcmp(&m.u.sample, &s, sizeof s) == 0);

    EXPECT_DECODE(b, 27, -EMSGSIZE);
    uint8_t c[64];
    memcpy(c, b, 28); c[28] = 0;
    EXPECT_DECODE(c, 29, -EMSGSIZE);
    memcpy(c, b, 28); c[2] = 23;                 /* header says 23, datagram is 28 */
    EXPECT_DECODE(c, 28, -EMSGSIZE);
    memcpy(c, b, 27); c[2] = 23;                 /* consistent, but not 24 */
    EXPECT_DECODE(c, 27, -EBADMSG);
    memcpy(c, b, 28); c[4 + 14] = 99;            /* status out of range */
    EXPECT_DECODE(c, 28, -EBADMSG);
    memcpy(c, b, 28); c[4 + 15] = 1;             /* reserved */
    EXPECT_DECODE(c, 28, -EBADMSG);
    memcpy(c, b, 28); c[0] = 2;
    EXPECT_DECODE(c, 28, -EPROTONOSUPPORT);
}

/* ---- policy ---- */

static struct oc_ipc_policy good_policy(void)
{
    struct oc_ipc_policy p = {.session_id = SESSION, .uid = 1000, .check_pid = true};
    p.origin[OC_SOURCE_LIGHT] = OC_ORIGIN_LIVE;
    p.origin[OC_SOURCE_DISTANCE] = OC_ORIGIN_SIMULATED;   /* deliberately mixed */
    return p;
}
static struct oc_hello policy_hello(void)
{
    return (struct oc_hello){.source = OC_SOURCE_LIGHT, .origin = OC_ORIGIN_LIVE,
                             .session_id = SESSION, .instance = 0, .pid = 4242};
}
static struct oc_ipc_peer good_peer(void)
{
    return (struct oc_ipc_peer){.pid = 4242, .uid = 1000, .gid = 1000};
}

static void test_policy(void)
{
    struct oc_ipc_policy p = good_policy();
    struct oc_hello h = policy_hello();
    struct oc_ipc_peer peer = good_peer();

    CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_NONE);   /* instance 0 accepted */

    /* per-source expected origin: the mixed session */
    h = policy_hello(); h.source = OC_SOURCE_DISTANCE; h.origin = OC_ORIGIN_SIMULATED;
    CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_NONE);
    h.origin = OC_ORIGIN_LIVE;
    CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_BAD_ORIGIN);
    h = policy_hello(); h.origin = OC_ORIGIN_SIMULATED;     /* LIGHT expects LIVE */
    CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_BAD_ORIGIN);
    h.origin = OC_ORIGIN_REPLAYED;
    CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_BAD_ORIGIN);

    /* illegal origins never pass, even if the policy says the same illegal value */
    for (uint8_t o = 0; o < 10; o++) {
        if (o >= OC_ORIGIN_LIVE && o <= OC_ORIGIN_REPLAYED)
            continue;
        struct oc_ipc_policy q = good_policy();
        q.origin[OC_SOURCE_LIGHT] = o;   /* 0 would mean "source not permitted" */
        h = policy_hello(); h.origin = o;
        if (o != 0)
            CHECK(oc_ipc_hello_check(&q, &h, &peer) == OC_REJECT_BAD_ORIGIN);
        CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_BAD_ORIGIN);
    }

    /* sources: unknown and not-permitted */
    for (uint8_t s = 0; s < 6; s++) {
        if (s == OC_SOURCE_LIGHT || s == OC_SOURCE_DISTANCE)
            continue;
        h = policy_hello(); h.source = s;
        CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_BAD_SOURCE);
    }
    {
        struct oc_ipc_policy q = good_policy();
        q.origin[OC_SOURCE_DISTANCE] = 0;                  /* DISTANCE not permitted */
        h = policy_hello(); h.source = OC_SOURCE_DISTANCE; h.origin = OC_ORIGIN_SIMULATED;
        CHECK(oc_ipc_hello_check(&q, &h, &peer) == OC_REJECT_BAD_SOURCE);
    }

    /* session */
    h = policy_hello(); h.session_id = SESSION + 1;
    CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_SESSION_MISMATCH);
    h = policy_hello(); h.session_id = 0;
    CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_SESSION_MISMATCH);
    {
        struct oc_ipc_policy q = good_policy();
        q.session_id = 0;                                  /* zero never matches zero */
        h = policy_hello(); h.session_id = 0;
        CHECK(oc_ipc_hello_check(&q, &h, &peer) == OC_REJECT_SESSION_MISMATCH);
    }

    /* peer credentials */
    h = policy_hello();
    CHECK(oc_ipc_hello_check(&p, &h, nullptr) == OC_REJECT_PEER_CRED);   /* fail closed */
    struct oc_ipc_peer bad = good_peer(); bad.uid = 0;
    CHECK(oc_ipc_hello_check(&p, &h, &bad) == OC_REJECT_PEER_CRED);
    bad = good_peer(); bad.pid = 1;
    CHECK(oc_ipc_hello_check(&p, &h, &bad) == OC_REJECT_PEER_CRED);
    {
        struct oc_ipc_policy q = good_policy();
        q.check_pid = false;                               /* pid ignored when off */
        CHECK(oc_ipc_hello_check(&q, &h, &bad) == OC_REJECT_NONE);
        bad.uid = 0;                                       /* but uid is always checked */
        CHECK(oc_ipc_hello_check(&q, &h, &bad) == OC_REJECT_PEER_CRED);
    }

    /* precedence: PEER_CRED, SESSION, SOURCE, ORIGIN */
    h = policy_hello(); h.session_id = 5; h.source = 9; h.origin = 9;
    bad = good_peer(); bad.uid = 7;
    CHECK(oc_ipc_hello_check(&p, &h, &bad) == OC_REJECT_PEER_CRED);
    CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_SESSION_MISMATCH);
    h.session_id = SESSION;
    CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_BAD_SOURCE);
    h.source = OC_SOURCE_LIGHT;
    CHECK(oc_ipc_hello_check(&p, &h, &peer) == OC_REJECT_BAD_ORIGIN);

    /* NULL arguments */
    h = policy_hello();
    CHECK(oc_ipc_hello_check(nullptr, &h, &peer) == OC_REJECT_MALFORMED);
    CHECK(oc_ipc_hello_check(&p, nullptr, &peer) == OC_REJECT_MALFORMED);
}

int main(void)
{
    RUN(test_constants);
    RUN(test_golden_encode);
    RUN(test_encode_rejects_invalid);
    RUN(test_encode_capacity);
    RUN(test_roundtrip);
    RUN(test_golden_decode);
    RUN(test_decode_truncated_and_trailing);
    RUN(test_decode_exact_payload_length_per_type);
    RUN(test_decode_reserved_and_enums);
    RUN(test_decode_type_and_version);
    RUN(test_decode_args);
    RUN(test_decode_sample_passthrough);
    RUN(test_policy);

    if (failures != 0) {
        fprintf(stderr, "test_ipc_msg: %d FAILURE(S)\n", failures);
        return 1;
    }
    fprintf(stderr, "test_ipc_msg: all passed\n");
    return 0;
}
