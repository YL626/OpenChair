/* Unit tests for oc_backoff and oc_sim_source (pure, SIMULATED data only). */
#include <stdint.h>
#include <stdio.h>

#include "oc_backoff.h"
#include "oc_sample.h"
#include "oc_sim_source.h"

static int failures;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static void test_backoff_ladder(void)
{
    struct oc_backoff b;
    oc_backoff_init(&b, 100, 2000);
    const uint32_t want[] = {100, 200, 400, 800, 1600, 2000, 2000, 2000};
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++)
        CHECK(oc_backoff_next(&b) == want[i]);
    oc_backoff_reset(&b);
    CHECK(oc_backoff_next(&b) == 100);
    CHECK(oc_backoff_next(&b) == 200);
}

static void test_backoff_clamps_and_saturates(void)
{
    struct oc_backoff b;
    oc_backoff_init(&b, 0, 0);               /* initial >= 1, cap >= initial */
    CHECK(oc_backoff_next(&b) == 1);
    CHECK(oc_backoff_next(&b) == 1);
    oc_backoff_init(&b, 3000, 2000);         /* cap raised to initial */
    CHECK(oc_backoff_next(&b) == 3000);
    CHECK(oc_backoff_next(&b) == 3000);
    oc_backoff_init(&b, 0x80000000u, 0xFFFFFFFFu);   /* doubling must not wrap */
    CHECK(oc_backoff_next(&b) == 0x80000000u);
    CHECK(oc_backoff_next(&b) == 0xFFFFFFFFu);
    CHECK(oc_backoff_next(&b) == 0xFFFFFFFFu);
}

static void test_sim_valid_samples(void)
{
    for (uint8_t src = OC_SOURCE_LIGHT; src <= OC_SOURCE_DISTANCE; src++) {
        for (uint32_t seq = 0; seq < 1000; seq++) {
            struct oc_sim_reading r;
            CHECK(oc_sim_reading(src, seq, 0, &r) == 0);
            struct oc_sample s = {.t_mono_ns = 1, .seq = seq, .source = src,
                                  .origin = OC_ORIGIN_SIMULATED, .status = r.status,
                                  .value = r.value, .aux = r.aux};
            CHECK(oc_sample_validate(&s) == 0);
            CHECK(r.status == OC_ST_OK && r.value > 0);
        }
    }
}

static void test_sim_deterministic_and_ranges(void)
{
    struct oc_sim_reading a, b;
    for (uint32_t seq = 0; seq < 500; seq++) {
        CHECK(oc_sim_reading(OC_SOURCE_LIGHT, seq, 0, &a) == 0);
        CHECK(oc_sim_reading(OC_SOURCE_LIGHT, seq, 0, &b) == 0);
        CHECK(a.value == b.value && a.status == b.status);
        CHECK(a.value >= 1000 && a.value <= 7400);
        CHECK(oc_sim_reading(OC_SOURCE_DISTANCE, seq, 0, &a) == 0);
        CHECK(a.value >= 38 && a.value <= 42);
    }
}

static void test_sim_fault_every(void)
{
    unsigned timeouts = 0;
    for (uint32_t seq = 0; seq < 100; seq++) {
        struct oc_sim_reading r;
        CHECK(oc_sim_reading(OC_SOURCE_DISTANCE, seq, 10, &r) == 0);
        if (seq % 10 == 9) {
            CHECK(r.status == OC_ST_TIMEOUT && r.value == 0);
            timeouts++;
        } else {
            CHECK(r.status == OC_ST_OK && r.value > 0);
        }
        struct oc_sample s = {.t_mono_ns = 1, .seq = seq, .source = OC_SOURCE_DISTANCE,
                              .origin = OC_ORIGIN_SIMULATED, .status = r.status,
                              .value = r.value, .aux = r.aux};
        CHECK(oc_sample_validate(&s) == 0);
    }
    CHECK(timeouts == 10);
}

static void test_sim_bad_source_leaves_output(void)
{
    struct oc_sim_reading r = {.status = 99, .value = 77, .aux = 5};
    CHECK(oc_sim_reading(0, 0, 0, &r) == -22);
    CHECK(oc_sim_reading(3, 0, 0, &r) == -22);
    CHECK(oc_sim_reading(OC_SOURCE_LIGHT, 0, 0, nullptr) == -22);
    CHECK(r.status == 99 && r.value == 77 && r.aux == 5);
}

int main(void)
{
    test_backoff_ladder();
    test_backoff_clamps_and_saturates();
    test_sim_valid_samples();
    test_sim_deterministic_and_ranges();
    test_sim_fault_every();
    test_sim_bad_source_leaves_output();
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("test_backoff: all passed");
    return 0;
}
