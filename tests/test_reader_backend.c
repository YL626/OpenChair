/* Tests for oc_reader_backend (M3-B): backend selection, sequence ownership,
 * non-OK status mapping, origin, clock failure, resource cleanup.
 *
 * Every I2C transaction here is SIMULATED by tests/fake_i2c.h (ADS1115) or
 * tests/fake_vl6180x.h (VL6180X). The one test that opens a real path uses
 * nonexistent or non-I2C files and proves only that the failure is reported
 * and nothing stays open. Nothing here is LIVE hardware evidence. A fake bus
 * whose origin is overwritten to LIVE exercises the "origin comes from the
 * bus" mechanics only; the samples are still fake. */

#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oc_reader_backend.h"
#include "oc_sample.h"
#include "oc_store.h"
#include "fake_i2c.h"
#include "fake_vl6180x.h"

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

static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    int n = 0;
    if (d == nullptr)
        return -1;
    while (readdir(d) != nullptr)
        n++;
    closedir(d);
    return n - 1;   /* the directory stream's own descriptor */
}

static struct oc_role_opts sim_opts(enum oc_role_kind role, uint32_t fault_every)
{
    return (struct oc_role_opts){.role = role, .input = OC_INPUT_SIM, .simulate = true,
                                 .fault_every = fault_every};
}

static void test_open_validation(void)
{
    struct oc_reader_backend b;
    struct oc_clock clk;
    oc_clock_system(&clk);
    struct oc_role_opts o = sim_opts(OC_ROLE_LIGHT, 0);
    CHECK(oc_reader_backend_open(nullptr, &o, &clk) == -EINVAL);
    CHECK(oc_reader_backend_open(&b, nullptr, &clk) == -EINVAL);
    CHECK(oc_reader_backend_open(&b, &o, nullptr) == -EINVAL);
    o.role = OC_ROLE_HUB;
    CHECK(oc_reader_backend_open(&b, &o, &clk) == -EINVAL);
    o = sim_opts(OC_ROLE_LIGHT, 0);
    o.input = OC_INPUT_NONE;
    CHECK(oc_reader_backend_open(&b, &o, &clk) == -EINVAL);
    o.input = OC_INPUT_LIVE;                 /* LIVE without a device path */
    o.i2c_dev = nullptr;
    CHECK(oc_reader_backend_open(&b, &o, &clk) == -EINVAL);
    CHECK(b.kind == OC_READER_CLOSED);

    struct oc_i2c_bus bus = {0};
    CHECK(oc_reader_backend_open_bus(&b, OC_SOURCE_LIGHT, &bus, &clk) == -EINVAL);   /* no xfer */
    struct fake f;
    struct oc_i2c_bus fb;
    struct oc_clock fc;
    fake_init(&f);
    fake_bus(&f, &fb, &fc);
    CHECK(oc_reader_backend_open_bus(&b, 99, &fb, &fc) == -EINVAL);
    CHECK(f.calls == 0);                     /* opening touches no hardware */
}

static void test_sim_backend(void)
{
    struct oc_clock clk;
    oc_clock_system(&clk);
    struct oc_reader_backend b;
    struct oc_role_opts o = sim_opts(OC_ROLE_DISTANCE, 4);
    CHECK(oc_reader_backend_open(&b, &o, &clk) == 0);
    CHECK(b.kind == OC_READER_SIM && b.origin == OC_ORIGIN_SIMULATED && b.i2c.fd == -1);
    CHECK(oc_reader_backend_next_seq(&b) == 0);
    for (uint32_t i = 0; i < 8; i++) {
        struct oc_sample s;
        int io = -1;
        CHECK(oc_reader_backend_acquire(&b, &s, &io) == 0);
        CHECK(io == 0);
        CHECK(s.seq == i && s.source == OC_SOURCE_DISTANCE && s.origin == OC_ORIGIN_SIMULATED);
        CHECK(oc_sample_validate(&s) == 0);
        if (i % 4 == 3)                      /* fault_every=4: seq 3 and 7 fail, still consume a seq */
            CHECK(s.status == OC_ST_TIMEOUT && s.value == 0);
        else
            CHECK(s.status == OC_ST_OK && s.value >= 38 && s.value <= 42);
        CHECK(oc_reader_backend_next_seq(&b) == i + 1);
    }
    oc_reader_backend_close(&b);
    oc_reader_backend_close(&b);             /* idempotent */
    struct oc_sample s;
    CHECK(oc_reader_backend_acquire(&b, &s, nullptr) == -EINVAL);   /* closed */
}

static void test_live_open_failures_leave_nothing_open(void)
{
    struct oc_clock clk;
    oc_clock_system(&clk);
    const char *const paths[] = {"/nonexistent/i2c-99", "/dev/null"};
    const int base = count_fds();
    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        struct oc_reader_backend b;
        struct oc_role_opts o = {.role = OC_ROLE_LIGHT, .input = OC_INPUT_LIVE,
                                 .i2c_dev = paths[i]};
        int rc = oc_reader_backend_open(&b, &o, &clk);
        CHECK(rc < 0);
        CHECK(b.kind == OC_READER_CLOSED && b.i2c.fd == -1);   /* never silently SIM */
        CHECK(count_fds() == base);
        CHECK(oc_reader_backend_next_seq(&b) == 0);
        oc_reader_backend_close(&b);
    }
    CHECK(count_fds() == base);
}

static struct oc_light_rig {
    struct fake f;
    struct oc_i2c_bus bus;
    struct oc_clock clk;
    struct oc_reader_backend b;
} g_lr;

static void light_rig(void)
{
    memset(&g_lr, 0, sizeof g_lr);
    fake_init(&g_lr.f);
    fake_bus(&g_lr.f, &g_lr.bus, &g_lr.clk);
    CHECK(oc_reader_backend_open_bus(&g_lr.b, OC_SOURCE_LIGHT, &g_lr.bus, &g_lr.clk) == 0);
}

static void test_light_backend_seq_and_failures(void)
{
    light_rig();
    struct oc_reader_backend *b = &g_lr.b;
    CHECK(b->kind == OC_READER_ADAPTER && b->source == OC_SOURCE_LIGHT);
    CHECK(b->origin == OC_ORIGIN_SIMULATED);          /* the fake bus says so; not LIVE */
    fake_push(&g_lr.f, 1111);
    fake_push(&g_lr.f, 2222);
    struct oc_sample s;
    int io = 0;
    CHECK(oc_reader_backend_acquire(b, &s, &io) == 0 && io == 0);
    CHECK(s.seq == 0 && s.status == OC_ST_OK && s.value == 1111 && s.source == OC_SOURCE_LIGHT);
    CHECK(oc_sample_validate(&s) == 0);

    /* The sensor disappears: honest NACK samples, value 0, one seq each, no switch to SIM. */
    g_lr.f.fail_from_call = g_lr.f.calls + 1;
    g_lr.f.fail_errno = ENXIO;
    for (uint32_t i = 1; i <= 2; i++) {
        CHECK(oc_reader_backend_acquire(b, &s, &io) == 0);
        CHECK(io == -ENXIO && s.status == OC_ST_NACK && s.value == 0 && s.seq == i);
        CHECK(s.origin == OC_ORIGIN_SIMULATED && s.source == OC_SOURCE_LIGHT);
    }
    g_lr.f.fail_errno = ETIMEDOUT;
    CHECK(oc_reader_backend_acquire(b, &s, &io) == 0);
    CHECK(io == -ETIMEDOUT && s.status == OC_ST_TIMEOUT && s.seq == 3);
    g_lr.f.fail_errno = EIO;
    CHECK(oc_reader_backend_acquire(b, &s, &io) == 0);
    CHECK(io == -EIO && s.status == OC_ST_IO_ERR && s.seq == 4);

    /* It comes back: the same adapter carries on with the next seq. */
    g_lr.f.fail_from_call = 0;
    CHECK(oc_reader_backend_acquire(b, &s, &io) == 0);
    CHECK(s.status == OC_ST_OK && s.value == 2222 && s.seq == 5);
    CHECK(oc_reader_backend_next_seq(b) == 6);
    oc_reader_backend_close(b);
}

static void test_origin_comes_from_the_bus(void)
{
    light_rig();
    oc_reader_backend_close(&g_lr.b);
    g_lr.bus.origin = OC_ORIGIN_LIVE;     /* MECHANICS ONLY: a fake bus claiming LIVE */
    CHECK(oc_reader_backend_open_bus(&g_lr.b, OC_SOURCE_LIGHT, &g_lr.bus, &g_lr.clk) == 0);
    CHECK(g_lr.b.origin == OC_ORIGIN_LIVE);
    fake_push(&g_lr.f, 5);
    struct oc_sample s;
    CHECK(oc_reader_backend_acquire(&g_lr.b, &s, nullptr) == 0);
    CHECK(s.origin == OC_ORIGIN_LIVE);
    g_lr.bus.origin = OC_ORIGIN_SIMULATED;     /* later edits of the caller's struct do not reach samples */
    fake_push(&g_lr.f, 6);
    CHECK(oc_reader_backend_acquire(&g_lr.b, &s, nullptr) == 0);
    CHECK(s.origin == OC_ORIGIN_LIVE);
    oc_reader_backend_close(&g_lr.b);
}

static void test_clock_failure_is_not_fabricated(void)
{
    light_rig();
    fake_push(&g_lr.f, 7);
    g_lr.f.now_fail_from = g_lr.f.now_calls + 1;
    g_lr.f.now_errno = EIO;
    struct oc_sample s;
    memset(&s, 0xA5, sizeof s);
    struct oc_sample before = s;
    int rc = oc_reader_backend_acquire(&g_lr.b, &s, nullptr);
    CHECK(rc < 0);
    CHECK(memcmp(&s, &before, sizeof s) == 0);          /* out untouched */
    CHECK(oc_reader_backend_next_seq(&g_lr.b) == 1);    /* the attempt still consumed its seq */
    oc_reader_backend_close(&g_lr.b);
}

static void test_distance_backend(void)
{
    struct fv f;
    struct oc_i2c_bus bus;
    struct oc_clock clk;
    fv_init(&f);
    fv_bus(&f, &bus, &clk);
    struct oc_reader_backend b;
    CHECK(oc_reader_backend_open_bus(&b, OC_SOURCE_DISTANCE, &bus, &clk) == 0);
    CHECK(b.source == OC_SOURCE_DISTANCE && b.origin == OC_ORIGIN_SIMULATED);
    CHECK(f.calls == 0);

    fv_push(&f, 41, 0);
    struct oc_sample s;
    int io = 0;
    CHECK(oc_reader_backend_acquire(&b, &s, &io) == 0);
    CHECK(s.seq == 0 && s.status == OC_ST_OK && s.value == 41);

    fv_push(&f, 0, 11);                       /* device-flagged range error */
    CHECK(oc_reader_backend_acquire(&b, &s, &io) == 0);
    CHECK(s.seq == 1 && s.status == OC_ST_RANGE_ERR && s.value == 0);

    f.unplugged = true;                       /* sensor lost: NACK samples, seq keeps counting */
    for (uint32_t i = 2; i <= 3; i++) {
        CHECK(oc_reader_backend_acquire(&b, &s, &io) == 0);
        CHECK(s.status == OC_ST_NACK && s.value == 0 && s.seq == i && io == -ENXIO);
    }
    fv_power_on(&f);                          /* re-plugged and power-cycled: settings are gone */
    f.unplugged = false;
    fv_push(&f, 77, 0);
    CHECK(oc_reader_backend_acquire(&b, &s, &io) == 0);
    CHECK(s.status == OC_ST_OK && s.value == 77 && s.seq == 4);
    CHECK(oc_reader_backend_next_seq(&b) == 5);
    oc_reader_backend_close(&b);
}

int main(void)
{
    RUN(test_open_validation);
    RUN(test_sim_backend);
    RUN(test_live_open_failures_leave_nothing_open);
    RUN(test_light_backend_seq_and_failures);
    RUN(test_origin_comes_from_the_bus);
    RUN(test_clock_failure_is_not_fabricated);
    RUN(test_distance_backend);
    if (failures != 0) {
        fprintf(stderr, "test_reader_backend: %d check(s) FAILED\n", failures);
        return 1;
    }
    fprintf(stderr, "test_reader_backend: all passed [SIMULATED]\n");
    return 0;
}
