/* Tests for the per-source sequence / timestamp tracker (include/oc_hub_seq.h).
 *
 * Pure: synthetic numbers only, no sockets, no hardware. The wraparound and
 * randomized tests compare the tracker with an independent 64-bit model. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oc_hub_seq.h"

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        unsigned long long a_ = (unsigned long long)(a), b_ = (unsigned long long)(b); \
        if (a_ != b_) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s == %llu, want %s == %llu\n",      \
                    __FILE__, __LINE__, #a, a_, #b, b_);                      \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define RUN(test)                          \
    do {                                   \
        fprintf(stderr, "== %s\n", #test); \
        test();                            \
    } while (0)

static struct oc_seq_result obs(struct oc_seq *s, uint32_t seq, uint64_t t)
{
    struct oc_seq_result r = oc_seq_observe(s, seq, t);
    CHECK(oc_seq_consistent(s));   /* the invariant holds after EVERY call */
    return r;
}

static void test_in_order_from_zero(void)
{
    struct oc_seq s;
    oc_seq_reset(&s);
    CHECK(oc_seq_consistent(&s));
    struct oc_seq_result r = obs(&s, 0, 100);
    CHECK_EQ(r.verdict, OC_SEQ_FIRST);
    CHECK_EQ(r.gap, 0);
    CHECK(!r.ts_backwards);
    for (uint32_t i = 1; i < 50; i++) {
        r = obs(&s, i, 100 + i);
        CHECK_EQ(r.verdict, OC_SEQ_IN_ORDER);
        CHECK_EQ(r.gap, 0);
    }
    CHECK_EQ(s.accepted, 50);
    CHECK_EQ(s.lost, 0);
    CHECK_EQ(s.unobserved_prefix, 0);
    CHECK_EQ(s.logical_last, 49);
}

static void test_first_nonzero_is_prefix_not_loss(void)
{
    struct oc_seq s;
    oc_seq_reset(&s);
    struct oc_seq_result r = obs(&s, 5000, 1);
    CHECK_EQ(r.verdict, OC_SEQ_FIRST);
    CHECK_EQ(r.gap, 5000);              /* reported to the consumer ... */
    CHECK_EQ(s.unobserved_prefix, 5000);
    CHECK_EQ(s.lost, 0);                /* ... but not counted as loss */
    CHECK_EQ(s.gap_events, 0);
    CHECK_EQ(s.logical_last, 5000);
    r = obs(&s, 5001, 2);
    CHECK_EQ(r.verdict, OC_SEQ_IN_ORDER);
    r = obs(&s, 5004, 3);               /* later skips ARE loss */
    CHECK_EQ(r.verdict, OC_SEQ_GAP);
    CHECK_EQ(r.gap, 2);
    CHECK_EQ(s.lost, 2);
    CHECK_EQ(s.gap_events, 1);
    CHECK_EQ(s.accepted, 3);
}

static void test_gaps(void)
{
    struct oc_seq s;
    oc_seq_reset(&s);
    obs(&s, 0, 1);
    struct oc_seq_result r = obs(&s, 1, 2);
    CHECK_EQ(r.verdict, OC_SEQ_IN_ORDER);
    r = obs(&s, 5, 3);
    CHECK_EQ(r.verdict, OC_SEQ_GAP);
    CHECK_EQ(r.gap, 3);
    CHECK_EQ(s.lost, 3);
    r = obs(&s, 6, 4);
    CHECK_EQ(r.verdict, OC_SEQ_IN_ORDER);
    r = obs(&s, 100, 5);
    CHECK_EQ(r.gap, 93);
    CHECK_EQ(s.lost, 96);
    CHECK_EQ(s.gap_events, 2);
    CHECK_EQ(s.accepted, 5);
    CHECK_EQ(s.logical_last, 100);
}

static void test_rejects_do_not_advance(void)
{
    struct oc_seq s;
    oc_seq_reset(&s);
    obs(&s, 10, 1000);
    obs(&s, 11, 1010);
    obs(&s, 12, 1020);
    struct oc_seq before = s;

    struct oc_seq_result r = obs(&s, 12, 5000);     /* duplicate */
    CHECK_EQ(r.verdict, OC_SEQ_DUPLICATE);
    CHECK_EQ(r.gap, 0);
    CHECK(!r.ts_backwards);
    r = obs(&s, 11, 5);                             /* stale, would also be "backward" in time */
    CHECK_EQ(r.verdict, OC_SEQ_REGRESSION);
    CHECK(!r.ts_backwards);                         /* rejected: no timestamp verdict */
    r = obs(&s, 0, 1);
    CHECK_EQ(r.verdict, OC_SEQ_REGRESSION);
    CHECK_EQ(s.duplicates, 1);
    CHECK_EQ(s.regressions, 2);

    /* nothing else moved */
    CHECK_EQ(s.last, before.last);
    CHECK_EQ(s.logical_last, before.logical_last);
    CHECK_EQ(s.accepted, before.accepted);
    CHECK_EQ(s.lost, before.lost);
    CHECK_EQ(s.last_t_ns, before.last_t_ns);
    CHECK_EQ(s.ts_backwards, before.ts_backwards);

    r = obs(&s, 13, 1030);                          /* the stream continues unharmed */
    CHECK_EQ(r.verdict, OC_SEQ_IN_ORDER);
    CHECK_EQ(s.lost, 0);
}

static void test_wraparound(void)
{
    struct oc_seq s;
    oc_seq_reset(&s);
    struct oc_seq_result r = obs(&s, 0xFFFFFFFDu, 1);
    CHECK_EQ(r.verdict, OC_SEQ_FIRST);
    CHECK_EQ(s.logical_last, 0xFFFFFFFDull);
    const uint32_t seqs[] = {0xFFFFFFFEu, 0xFFFFFFFFu, 0, 1, 2};
    for (size_t i = 0; i < 5; i++) {
        r = obs(&s, seqs[i], 2 + i);
        CHECK_EQ(r.verdict, OC_SEQ_IN_ORDER);
    }
    /* the logical position kept counting past 2^32 */
    CHECK_EQ(s.logical_last, 0xFFFFFFFDull + 5);
    CHECK(s.logical_last > 0xFFFFFFFFull);
    CHECK_EQ(s.accepted + s.lost + s.unobserved_prefix, s.logical_last + 1);

    /* duplicate and regression right after the wrap */
    r = obs(&s, 2, 100);
    CHECK_EQ(r.verdict, OC_SEQ_DUPLICATE);
    r = obs(&s, 0xFFFFFFFFu, 100);
    CHECK_EQ(r.verdict, OC_SEQ_REGRESSION);
    r = obs(&s, 1, 100);
    CHECK_EQ(r.verdict, OC_SEQ_REGRESSION);
    CHECK_EQ(s.logical_last, 0xFFFFFFFDull + 5);

    /* a gap that straddles the wrap: last = 2, next seq 7 -> 4 lost, no wrap yet;
     * then jump from 7 across to... build a fresh tracker for the straddle */
    oc_seq_reset(&s);
    obs(&s, 0xFFFFFFFEu, 1);
    r = obs(&s, 2, 2);                 /* skipped FFFFFFFF, 0, 1 */
    CHECK_EQ(r.verdict, OC_SEQ_GAP);
    CHECK_EQ(r.gap, 3);
    CHECK_EQ(s.lost, 3);
    CHECK_EQ(s.logical_last, 0xFFFFFFFEull + 4);
    CHECK_EQ(s.accepted + s.lost + s.unobserved_prefix, s.logical_last + 1);
}

static void test_half_range_boundary(void)
{
    struct oc_seq s;
    oc_seq_reset(&s);
    obs(&s, 10, 1);
    /* distance 0x7FFFFFFF ahead of `next` is the largest accepted jump */
    uint32_t next = 11;
    struct oc_seq_result r = obs(&s, next + 0x7FFFFFFFu, 2);
    CHECK_EQ(r.verdict, OC_SEQ_GAP);
    CHECK_EQ(r.gap, 0x7FFFFFFFu);
    CHECK_EQ(s.lost, 0x7FFFFFFFull);

    oc_seq_reset(&s);
    obs(&s, 10, 1);
    r = obs(&s, next + 0x80000000u, 2);  /* exactly half a range ahead is "behind" */
    CHECK_EQ(r.verdict, OC_SEQ_REGRESSION);
    CHECK_EQ(s.accepted, 1);
}

static void test_timestamps(void)
{
    struct oc_seq s;
    oc_seq_reset(&s);
    struct oc_seq_result r = obs(&s, 0, 1000);
    CHECK(!r.ts_backwards);
    r = obs(&s, 1, 1000);                 /* equal is not backward */
    CHECK(!r.ts_backwards);
    r = obs(&s, 2, 2000);
    CHECK(!r.ts_backwards);
    r = obs(&s, 3, 1500);                 /* step back */
    CHECK_EQ(r.verdict, OC_SEQ_IN_ORDER); /* the sample itself is still accepted */
    CHECK(r.ts_backwards);
    CHECK_EQ(s.ts_backwards, 1);
    r = obs(&s, 4, 1600);                 /* continues from the new value: flagged once */
    CHECK(!r.ts_backwards);
    CHECK_EQ(s.ts_backwards, 1);
    r = obs(&s, 5, 1);                    /* another step back */
    CHECK(r.ts_backwards);
    CHECK_EQ(s.ts_backwards, 2);
}

/* A backward timestamp on a GAP sample is flagged too. */
static void test_timestamp_on_gap_sample(void)
{
    struct oc_seq s;
    oc_seq_reset(&s);
    obs(&s, 0, 100);
    struct oc_seq_result r = obs(&s, 5, 50);
    CHECK_EQ(r.verdict, OC_SEQ_GAP);
    CHECK(r.ts_backwards);
    CHECK_EQ(s.ts_backwards, 1);
    CHECK_EQ(s.lost, 4);
}

static void test_reset_clears_everything(void)
{
    struct oc_seq s;
    oc_seq_reset(&s);
    obs(&s, 5, 900);
    obs(&s, 9, 800);   /* gap + backward */
    obs(&s, 9, 800);   /* duplicate */
    CHECK(s.lost > 0 && s.ts_backwards > 0 && s.duplicates > 0);
    oc_seq_reset(&s);
    struct oc_seq zero;
    memset(&zero, 0, sizeof zero);
    CHECK(memcmp(&s, &zero, sizeof s) == 0);
    /* timestamps restart: a small value after reset is NOT backward */
    struct oc_seq_result r = obs(&s, 0, 1);
    CHECK_EQ(r.verdict, OC_SEQ_FIRST);
    CHECK(!r.ts_backwards);
}

static void test_null_and_consistency(void)
{
    struct oc_seq_result r = oc_seq_observe(nullptr, 1, 1);
    CHECK_EQ(r.verdict, 0);
    oc_seq_reset(nullptr);
    CHECK(!oc_seq_consistent(nullptr));
    struct oc_seq s;
    oc_seq_reset(&s);
    obs(&s, 3, 1);
    s.accepted += 1;                      /* corrupt it: the invariant must notice */
    CHECK(!oc_seq_consistent(&s));
}

/* ---- randomized comparison with an independent 64-bit model ---- */

static uint64_t rng_state;
static uint32_t rnd(void)
{
    rng_state = rng_state * 6364136223846793005ull + 1442695040888963407ull;
    return (uint32_t)(rng_state >> 33);
}

static void run_random(uint64_t seed, uint64_t start_index)
{
    rng_state = seed;
    struct oc_seq s;
    oc_seq_reset(&s);

    uint64_t next_true = start_index;      /* true index of the next fresh sample */
    uint64_t last_acc = 0;                 /* true index of the last accepted */
    bool any = false;
    uint64_t model_acc = 0, model_dup = 0, model_reg = 0;
    uint64_t t = 1000;
    uint64_t prev_true = 0;                /* last emitted, for duplicates */
    bool have_prev = false;

    for (int i = 0; i < 20000; i++) {
        uint32_t dice = rnd() % 100;
        uint64_t idx;
        enum { FRESH, DUP, OLD } kind;
        if (dice < 70 || !have_prev) {
            next_true += rnd() % 4 == 0 ? 1 + rnd() % 7 : 0;   /* some drops */
            idx = next_true++;
            kind = FRESH;
        } else if (dice < 85) {
            idx = last_acc;
            kind = DUP;
        } else {
            uint64_t back = 1 + rnd() % 50;
            idx = last_acc >= start_index + back ? last_acc - back : last_acc;
            kind = idx == last_acc ? DUP : OLD;
        }
        prev_true = idx;
        have_prev = true;
        t += 5;
        struct oc_seq_result r = oc_seq_observe(&s, (uint32_t)idx, t);
        CHECK(oc_seq_consistent(&s));
        if (kind == FRESH) {
            CHECK(r.verdict == OC_SEQ_FIRST || r.verdict == OC_SEQ_IN_ORDER ||
                  r.verdict == OC_SEQ_GAP);
            any = true;
            last_acc = idx;
            model_acc++;
        } else if (kind == DUP) {
            if (!any) {
                CHECK(r.verdict == OC_SEQ_FIRST);
                any = true; last_acc = idx; model_acc++;
            } else {
                CHECK_EQ(r.verdict, OC_SEQ_DUPLICATE);
                model_dup++;
            }
        } else {
            CHECK_EQ(r.verdict, OC_SEQ_REGRESSION);
            model_reg++;
        }
        /* the 64-bit logical position equals the model's true index, across wraps */
        CHECK_EQ(s.logical_last, last_acc);
    }
    (void)prev_true;
    CHECK_EQ(s.accepted, model_acc);
    CHECK_EQ(s.duplicates, model_dup);
    CHECK_EQ(s.regressions, model_reg);
    CHECK_EQ(s.accepted + s.lost + s.unobserved_prefix, s.logical_last + 1);
    if (start_index >= 0xFFFFF000ull)
        CHECK(s.logical_last > 0xFFFFFFFFull);    /* the run really crossed the wrap */
}

static void test_randomized_against_model(void)
{
    run_random(1, 0);
    run_random(2, 0);
    run_random(3, 0xFFFFFF00ull);       /* starts just below the wrap */
    run_random(4, 0xFFFFFFC0ull);
    run_random(5, 0xFFFFF000ull);
}

int main(void)
{
    RUN(test_in_order_from_zero);
    RUN(test_first_nonzero_is_prefix_not_loss);
    RUN(test_gaps);
    RUN(test_rejects_do_not_advance);
    RUN(test_wraparound);
    RUN(test_half_range_boundary);
    RUN(test_timestamps);
    RUN(test_timestamp_on_gap_sample);
    RUN(test_reset_clears_everything);
    RUN(test_null_and_consistency);
    RUN(test_randomized_against_model);

    if (failures != 0) {
        fprintf(stderr, "test_hub_seq: %d FAILURE(S)\n", failures);
        return 1;
    }
    fprintf(stderr, "test_hub_seq: all passed\n");
    return 0;
}
