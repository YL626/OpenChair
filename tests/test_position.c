/* Tests for oc_position (placement/stability verdict from distance samples).
 * Pure computation over synthetic, SIMULATED samples: proves the verdict
 * logic only, not that the default thresholds suit the real device. */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oc_position.h"
#include "oc_sample.h"
#include "oc_store.h"

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

#define MS 1000000ull

/* A small test configuration: 20..100 mm, spread 4, window 4, 300 ms age. */
static const struct oc_position_cfg cfg4 = {
    .min_mm = 20, .max_mm = 100, .max_spread_mm = 4, .window_n = 4, .max_age_ns = 300 * MS,
};

struct feed {
    struct oc_position p;
    uint32_t seq;
    uint64_t t;
};

static void feed_init(struct feed *f, const struct oc_position_cfg *c)
{
    CHECK(oc_position_init(&f->p, c) == 0);
    f->seq = 0;
    f->t = 1000 * MS;
}

static struct oc_sample mk(uint32_t seq, uint64_t t, uint8_t status, int32_t mm, uint32_t aux)
{
    return (struct oc_sample){
        .t_mono_ns = t, .seq = seq, .source = OC_SOURCE_DISTANCE, .origin = OC_ORIGIN_SIMULATED,
        .status = status, .value = status == OC_ST_OK ? mm : 0, .aux = aux,
    };
}

/* Pushes the next sample of an unbroken 100 ms stream. */
static void ok(struct feed *f, int32_t mm)
{
    struct oc_sample s = mk(f->seq++, f->t, OC_ST_OK, mm, 0x01);
    f->t += 100 * MS;
    CHECK(oc_position_push(&f->p, &s) == 0);
}

static void bad(struct feed *f, uint8_t status, uint32_t aux)
{
    struct oc_sample s = mk(f->seq++, f->t, status, 0, aux);
    f->t += 100 * MS;
    CHECK(oc_position_push(&f->p, &s) == 0);
}

/* Verdict right after the newest sample (age 0). */
static struct oc_position_verdict now(const struct feed *f)
{
    struct oc_position_verdict v;
    memset(&v, 0xAB, sizeof v);
    CHECK(oc_position_eval(&f->p, f->t - 100 * MS, &v) == 0);
    return v;
}

static void test_init_validation(void)
{
    struct oc_position p;
    struct oc_position_cfg c = cfg4;
    CHECK(oc_position_init(nullptr, &c) == -EINVAL);
    CHECK(oc_position_init(&p, nullptr) == -EINVAL);
    c = cfg4; c.min_mm = 101;
    CHECK(oc_position_init(&p, &c) == -EINVAL);
    c = cfg4; c.max_spread_mm = -1;
    CHECK(oc_position_init(&p, &c) == -EINVAL);
    c = cfg4; c.window_n = 0;
    CHECK(oc_position_init(&p, &c) == -EINVAL);
    c = cfg4; c.window_n = OC_POSITION_WINDOW_MAX + 1;
    CHECK(oc_position_init(&p, &c) == -EINVAL);
    c = cfg4; c.max_age_ns = 0;
    CHECK(oc_position_init(&p, &c) == -EINVAL);
    c = cfg4; c.min_mm = c.max_mm; c.window_n = OC_POSITION_WINDOW_MAX;
    CHECK(oc_position_init(&p, &c) == 0);
    const struct oc_position_cfg d = OC_POSITION_CFG_DEFAULT;
    CHECK(oc_position_init(&p, &d) == 0);
}

static void test_push_rejects_invalid(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    struct oc_sample s = mk(0, 1, OC_ST_OK, 50, 1);
    CHECK(oc_position_push(nullptr, &s) == -EINVAL);
    CHECK(oc_position_push(&f.p, nullptr) == -EINVAL);
    s.source = OC_SOURCE_LIGHT;
    CHECK(oc_position_push(&f.p, &s) == -EINVAL);
    s = mk(0, 1, OC_ST_RANGE_ERR, 0, 0xB1);
    s.value = 7;   /* non-OK with a value: illegal sample */
    CHECK(oc_position_push(&f.p, &s) == -EINVAL);
    s = mk(0, 1, 99, 0, 0);
    CHECK(oc_position_push(&f.p, &s) == -EINVAL);
    struct oc_position_verdict v;
    CHECK(oc_position_eval(&f.p, 1, &v) == 0 && v.state == OC_POS_UNAVAILABLE);   /* unchanged */
    CHECK(v.origin == OC_ORIGIN_NONE && v.last_status == 0 && v.n == 0);
    CHECK(oc_position_eval(nullptr, 1, &v) == -EINVAL);
    CHECK(oc_position_eval(&f.p, 1, nullptr) == -EINVAL);
}

/* Steady readings inside the window: SETTLING until window_n, then STABLE. */
static void test_settling_then_stable(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    for (int i = 0; i < 3; i++) {
        ok(&f, 50 + i);
        struct oc_position_verdict v = now(&f);
        CHECK(v.state == OC_POS_SETTLING && v.n == (uint32_t)i + 1);
    }
    ok(&f, 51);
    struct oc_position_verdict v = now(&f);
    CHECK(v.state == OC_POS_STABLE);
    CHECK(v.n == 4 && v.median_mm == 51 && v.spread_mm == 2);   /* 50 51 51 52 */
    CHECK(v.origin == OC_ORIGIN_SIMULATED && v.last_status == OC_ST_OK && v.age_ns == 0);
}

/* The window slides: only the newest window_n values count. */
static void test_window_slides(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    ok(&f, 90);   /* would make the spread 40 if it stayed */
    for (int i = 0; i < 4; i++)
        ok(&f, 50);
    struct oc_position_verdict v = now(&f);
    CHECK(v.state == OC_POS_STABLE && v.spread_mm == 0 && v.median_mm == 50);
}

/* Spread above max_spread_mm is MOVING; exactly at it is still STABLE. */
static void test_moving(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    ok(&f, 50); ok(&f, 52); ok(&f, 54); ok(&f, 51);
    CHECK(now(&f).state == OC_POS_STABLE && now(&f).spread_mm == 4);
    ok(&f, 55);   /* window 52 54 51 55: spread 4 */
    CHECK(now(&f).state == OC_POS_STABLE);
    ok(&f, 56);   /* window 54 51 55 56: spread 5 */
    struct oc_position_verdict v = now(&f);
    CHECK(v.state == OC_POS_MOVING && v.spread_mm == 5);
}

/* Median outside [min, max] is OUT_OF_RANGE, also when steady; the bounds
 * are inclusive. Out of range wins over moving. */
static void test_out_of_range(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    for (int i = 0; i < 4; i++)
        ok(&f, 101);
    CHECK(now(&f).state == OC_POS_OUT_OF_RANGE && now(&f).median_mm == 101);
    for (int i = 0; i < 4; i++)
        ok(&f, 100);
    CHECK(now(&f).state == OC_POS_STABLE);
    for (int i = 0; i < 4; i++)
        ok(&f, 19);
    CHECK(now(&f).state == OC_POS_OUT_OF_RANGE);
    ok(&f, 10); ok(&f, 18); ok(&f, 5); ok(&f, 15);   /* moving AND too close */
    CHECK(now(&f).state == OC_POS_OUT_OF_RANGE);
}

/* Even window: mean of the two middle values, rounded down. One outlier does
 * not move the median far (but does count in the spread). */
static void test_median(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    ok(&f, 40); ok(&f, 43); ok(&f, 41); ok(&f, 44);   /* sorted 40 41 43 44 */
    CHECK(now(&f).median_mm == 42);
    struct oc_position_cfg c5 = cfg4;
    c5.window_n = 5;
    c5.max_spread_mm = 100;
    feed_init(&f, &c5);
    ok(&f, 50); ok(&f, 51); ok(&f, 99); ok(&f, 49); ok(&f, 50);   /* sorted 49 50 50 51 99 */
    struct oc_position_verdict v = now(&f);
    CHECK(v.median_mm == 50 && v.spread_mm == 50 && v.state == OC_POS_STABLE);
    feed_init(&f, &cfg4);
    ok(&f, 40); ok(&f, 41); ok(&f, 41); ok(&f, 42);   /* (41 + 41) / 2 */
    CHECK(now(&f).median_mm == 41);
    ok(&f, 42);   /* 41 41 42 42 -> 41.5 rounds down */
    CHECK(now(&f).median_mm == 41);
}

/* NO_TARGET for scene range errors, FAULT for system codes; both empty the
 * window, so stability must be rebuilt afterwards. */
static void test_range_errors(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    for (int i = 0; i < 4; i++)
        ok(&f, 50);
    bad(&f, OC_ST_RANGE_ERR, 15u << 4 | 1u);   /* overflow: nothing in range */
    struct oc_position_verdict v = now(&f);
    CHECK(v.state == OC_POS_NO_TARGET && v.range_error == 15 && v.n == 0);
    CHECK(v.last_status == OC_ST_RANGE_ERR);
    bad(&f, OC_ST_RANGE_ERR, 11u << 4 | 1u);
    CHECK(now(&f).state == OC_POS_NO_TARGET && now(&f).range_error == 11);
    bad(&f, OC_ST_RANGE_ERR, 3u << 4 | 1u);
    CHECK(now(&f).state == OC_POS_FAULT && now(&f).range_error == 3);
    ok(&f, 50);
    CHECK(now(&f).state == OC_POS_SETTLING && now(&f).n == 1);
    CHECK(f.p.discontinuities == 0);   /* non-OK samples are not discontinuities */
}

/* Bus-level failures say nothing about placement: UNAVAILABLE. */
static void test_bus_failures_unavailable(void)
{
    static const uint8_t st[] = {OC_ST_NACK, OC_ST_TIMEOUT, OC_ST_IO_ERR, OC_ST_STALE,
                                 OC_ST_SATURATED};
    for (size_t i = 0; i < sizeof st; i++) {
        struct feed f;
        feed_init(&f, &cfg4);
        for (int k = 0; k < 4; k++)
            ok(&f, 50);
        bad(&f, st[i], 0);
        struct oc_position_verdict v = now(&f);
        CHECK(v.state == OC_POS_UNAVAILABLE && v.n == 0 && v.range_error == 0);
        CHECK(v.last_status == st[i]);
    }
}

/* A stale newest sample is UNAVAILABLE even if the window was stable; the
 * limit is inclusive. A "now" before the newest timestamp counts as age 0. */
static void test_freshness(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    for (int i = 0; i < 4; i++)
        ok(&f, 50);
    const uint64_t last = f.t - 100 * MS;
    struct oc_position_verdict v;
    CHECK(oc_position_eval(&f.p, last + 300 * MS, &v) == 0 && v.state == OC_POS_STABLE);
    CHECK(v.age_ns == 300 * MS);
    CHECK(oc_position_eval(&f.p, last + 300 * MS + 1, &v) == 0 && v.state == OC_POS_UNAVAILABLE);
    CHECK(oc_position_eval(&f.p, last - 5, &v) == 0 && v.state == OC_POS_STABLE && v.age_ns == 0);
}

/* A seq gap, duplicate or regression, or a backwards timestamp, empties the
 * window and is counted. */
static void test_discontinuities_reset_window(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    for (int i = 0; i < 4; i++)
        ok(&f, 50);
    f.seq += 1;   /* one sample lost */
    ok(&f, 50);
    CHECK(now(&f).state == OC_POS_SETTLING && now(&f).n == 1 && f.p.discontinuities == 1);

    for (int i = 0; i < 3; i++)
        ok(&f, 50);
    CHECK(now(&f).state == OC_POS_STABLE);
    f.seq -= 1;   /* duplicate seq */
    ok(&f, 50);
    CHECK(now(&f).n == 1 && f.p.discontinuities == 2);
    f.seq -= 3;   /* regression: a restarted reader */
    ok(&f, 50);
    CHECK(now(&f).n == 1 && f.p.discontinuities == 3);

    ok(&f, 50);
    f.t -= 500 * MS;   /* time goes backwards */
    ok(&f, 50);
    CHECK(now(&f).n == 1 && f.p.discontinuities == 4);

    ok(&f, 50);       /* equal timestamps are not backwards */
    f.t -= 100 * MS;
    ok(&f, 50);
    CHECK(now(&f).n == 3 && f.p.discontinuities == 4);
}

/* seq wraps from UINT32_MAX to 0 without a discontinuity. */
static void test_seq_wrap(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    f.seq = UINT32_MAX - 1;
    for (int i = 0; i < 4; i++)
        ok(&f, 50);
    CHECK(f.seq == 2);
    CHECK(now(&f).state == OC_POS_STABLE && f.p.discontinuities == 0);
}

/* The verdict's origin: the window's common origin, MIXED when it differs.
 * A window with one SIMULATED sample can never come out LIVE. */
static void test_origin(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    for (int i = 0; i < 4; i++) {
        struct oc_sample s = mk(f.seq++, f.t, OC_ST_OK, 50, 1);
        s.origin = i == 2 ? OC_ORIGIN_SIMULATED : OC_ORIGIN_LIVE;
        f.t += 100 * MS;
        CHECK(oc_position_push(&f.p, &s) == 0);
    }
    CHECK(now(&f).state == OC_POS_STABLE && now(&f).origin == OC_ORIGIN_MIXED);
    for (int i = 0; i < 4; i++) {
        struct oc_sample s = mk(f.seq++, f.t, OC_ST_OK, 50, 1);
        s.origin = OC_ORIGIN_LIVE;
        f.t += 100 * MS;
        CHECK(oc_position_push(&f.p, &s) == 0);
    }
    CHECK(now(&f).origin == OC_ORIGIN_LIVE);   /* the SIMULATED one slid out */
    struct oc_sample s = mk(f.seq++, f.t, OC_ST_NACK, 0, 0);
    s.origin = OC_ORIGIN_REPLAYED;
    f.t += 100 * MS;
    CHECK(oc_position_push(&f.p, &s) == 0);
    CHECK(now(&f).origin == OC_ORIGIN_REPLAYED);   /* decided by the newest sample */
}

static void test_reset(void)
{
    struct feed f;
    feed_init(&f, &cfg4);
    f.seq += 3;
    ok(&f, 50);
    for (int i = 0; i < 4; i++)
        ok(&f, 50);
    oc_position_reset(&f.p);
    oc_position_reset(nullptr);
    struct oc_position_verdict v;
    CHECK(oc_position_eval(&f.p, f.t, &v) == 0);
    CHECK(v.state == OC_POS_UNAVAILABLE && v.origin == OC_ORIGIN_NONE && v.n == 0);
    f.seq = 0;   /* a new instance may start again at 0 */
    ok(&f, 50);
    CHECK(now(&f).state == OC_POS_SETTLING && f.p.discontinuities == 0);
}

static void test_state_names(void)
{
    static const char *const want[] = {"?", "UNAVAILABLE", "FAULT", "NO_TARGET", "SETTLING",
                                       "OUT_OF_RANGE", "MOVING", "STABLE", "?"};
    for (uint8_t i = 0; i < sizeof want / sizeof want[0]; i++)
        CHECK(strcmp(oc_position_state_name(i), want[i]) == 0);
    CHECK(strcmp(oc_position_state_name(255), "?") == 0);
}

int main(void)
{
    RUN(test_init_validation);
    RUN(test_push_rejects_invalid);
    RUN(test_settling_then_stable);
    RUN(test_window_slides);
    RUN(test_moving);
    RUN(test_out_of_range);
    RUN(test_median);
    RUN(test_range_errors);
    RUN(test_bus_failures_unavailable);
    RUN(test_freshness);
    RUN(test_discontinuities_reset_window);
    RUN(test_seq_wrap);
    RUN(test_origin);
    RUN(test_reset);
    RUN(test_state_names);

    if (failures != 0) {
        fprintf(stderr, "test_position: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_position: all passed [SIMULATED]\n");
    return 0;
}
