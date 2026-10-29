/* Tests for oc_vl6180x (driver) and oc_distance (oc_sample adapter).
 *
 * Every transaction is SIMULATED by tests/fake_vl6180x.h: a scripted VL6180X
 * register model and a fake clock. Passing proves driver and adapter logic
 * against that model only. It is not evidence about a real VL6180X, the I2C
 * bus or real timing; that comes from the live probe (docs/vl6180x.md). */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oc_vl6180x.h"
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

struct rig {
    struct fv f;
    struct oc_i2c_bus bus;
    struct oc_clock clk;
    struct oc_vl6180x_cfg cfg;
    struct oc_vl6180x d;
};

static void rig_init(struct rig *r)
{
    fv_init(&r->f);
    fv_bus(&r->f, &r->bus, &r->clk);
    r->cfg = (struct oc_vl6180x_cfg)OC_VL6180X_CFG_DEFAULT;
    CHECK(oc_vl6180x_init(&r->d, &r->bus, &r->clk, &r->cfg) == 0);
}

/* Index of the first logged transfer that wrote reg, or -1. */
static int find_write(const struct fv *f, uint16_t reg)
{
    for (unsigned i = 0; i < f->calls && i < FV_MAX_LOG; i++)
        if (!f->log[i].read && f->log[i].reg == reg)
            return (int)i;
    return -1;
}

/* ---- driver: init and setup ---- */

static void test_init_validates_before_hardware(void)
{
    struct rig r;
    rig_init(&r);
    struct oc_vl6180x d;
    struct oc_vl6180x_cfg cfg = OC_VL6180X_CFG_DEFAULT;

    CHECK(oc_vl6180x_init(nullptr, &r.bus, &r.clk, &cfg) == -EINVAL);
    CHECK(oc_vl6180x_init(&d, nullptr, &r.clk, &cfg) == -EINVAL);
    CHECK(oc_vl6180x_init(&d, &r.bus, nullptr, &cfg) == -EINVAL);
    CHECK(oc_vl6180x_init(&d, &r.bus, &r.clk, nullptr) == -EINVAL);

    struct oc_i2c_bus bus = r.bus;
    bus.xfer = nullptr;
    CHECK(oc_vl6180x_init(&d, &bus, &r.clk, &cfg) == -EINVAL);
    static const uint8_t bad_origin[] = {0, OC_ORIGIN_MIXED, OC_ORIGIN_NONE, 99};
    for (size_t i = 0; i < sizeof bad_origin; i++) {
        bus = r.bus;
        bus.origin = bad_origin[i];
        CHECK(oc_vl6180x_init(&d, &bus, &r.clk, &cfg) == -EINVAL);
    }
    struct oc_clock clk = r.clk;
    clk.now_ns = nullptr;
    CHECK(oc_vl6180x_init(&d, &r.bus, &clk, &cfg) == -EINVAL);
    clk = r.clk;
    clk.sleep_until_ns = nullptr;
    CHECK(oc_vl6180x_init(&d, &r.bus, &clk, &cfg) == -EINVAL);
    static const uint8_t bad_addr[] = {0x00, 0x07, 0x78, 0x80, 0xFF};
    for (size_t i = 0; i < sizeof bad_addr; i++) {
        cfg.addr = bad_addr[i];
        CHECK(oc_vl6180x_init(&d, &r.bus, &r.clk, &cfg) == -EINVAL);
    }
    CHECK(r.f.calls == 0);   /* nothing touched the bus */
}

/* Fresh out of reset: the mandatory settings are loaded, then the flag is
 * cleared, and the ranging configuration is written. */
static void test_setup_fresh_device(void)
{
    struct rig r;
    rig_init(&r);
    CHECK(oc_vl6180x_setup(&r.d) == 0);
    CHECK(r.f.private_writes == 30);
    CHECK(r.f.regs[0x0207] == 0x01 && r.f.regs[0x0097] == 0xFD && r.f.regs[0x0030] == 0x00);
    CHECK(r.f.regs[0x0016] == 0x00);          /* fresh-out-of-reset cleared */
    CHECK(r.f.regs[0x0014] == 0x24);          /* "new sample ready" interrupt */
    CHECK(r.f.regs[0x010A] == 0x30);
    /* The flag is cleared only after every private setting was written. */
    int clear = find_write(&r.f, 0x0016);
    CHECK(clear >= 0 && find_write(&r.f, 0x0030) < clear);
    /* The first transaction reads the model ID at 0x29, address high byte first. */
    CHECK(r.f.log[0].read && r.f.log[0].reg == 0x0000 && r.f.log[0].addr == 0x29);
}

/* Already set up (not fresh): the private settings are not reloaded, but
 * the ranging configuration is still written. */
static void test_setup_not_fresh(void)
{
    struct rig r;
    rig_init(&r);
    r.f.regs[0x0016] = 0x00;
    CHECK(oc_vl6180x_setup(&r.d) == 0);
    CHECK(r.f.private_writes == 0);
    CHECK(r.f.regs[0x0014] == 0x24);
}

/* Something answers at the address but is not a VL6180X: nothing is written. */
static void test_setup_wrong_model_id(void)
{
    struct rig r;
    rig_init(&r);
    r.f.regs[0x0000] = 0xAA;
    CHECK(oc_vl6180x_setup(&r.d) == -EPROTO);
    CHECK(r.f.calls == 1);
    CHECK(r.f.regs[0x0016] == 0x01);
}

static void test_setup_bus_errors_pass_through(void)
{
    for (unsigned k = 1; k <= 3; k++) {   /* model ID read, fresh read, first write */
        struct rig r;
        rig_init(&r);
        r.f.fail_call = k;
        r.f.fail_errno = EREMOTEIO;
        CHECK(oc_vl6180x_setup(&r.d) == -EREMOTEIO);
    }
    struct rig r;
    rig_init(&r);
    r.f.unplugged = true;
    CHECK(oc_vl6180x_setup(&r.d) == -ENXIO);
}

/* ---- driver: ranging ---- */

static void test_range_transactions(void)
{
    struct rig r;
    rig_init(&r);
    r.f.regs[0x0016] = 0x00;
    fv_push(&r.f, 123, 0);
    CHECK(oc_vl6180x_setup(&r.d) == 0);
    unsigned base = r.f.calls;

    uint8_t mm = 0, st = 0xEE;
    CHECK(oc_vl6180x_range(&r.d, &mm, &st) == 0);
    CHECK(mm == 123);
    CHECK(st == 0x01);   /* error code 0, device ready */
    CHECK(oc_vl6180x_range_error(st) == OC_VL6180X_RANGE_OK);

    /* ready check, clear, start, poll, value, status, clear */
    const struct fv_rec *l = &r.f.log[base];
    CHECK(r.f.calls - base == 7);
    CHECK(l[0].read && l[0].reg == 0x004D);
    CHECK(!l[1].read && l[1].reg == 0x0015 && l[1].value == 0x07);
    CHECK(!l[2].read && l[2].reg == 0x0018 && l[2].value == 0x01);
    CHECK(l[3].read && l[3].reg == 0x004F);
    CHECK(l[4].read && l[4].reg == 0x0062);
    CHECK(l[5].read && l[5].reg == 0x004D);
    CHECK(!l[6].read && l[6].reg == 0x0015 && l[6].value == 0x07);
    for (unsigned i = 0; i < 7; i++)
        CHECK(l[i].addr == 0x29);
    CHECK(r.f.starts == 1);
}

static void test_range_uses_configured_address(void)
{
    struct rig r;
    fv_init(&r.f);
    fv_bus(&r.f, &r.bus, &r.clk);
    r.cfg = (struct oc_vl6180x_cfg){.addr = 0x30};
    CHECK(oc_vl6180x_init(&r.d, &r.bus, &r.clk, &r.cfg) == 0);
    uint8_t mm, st;
    CHECK(oc_vl6180x_range(&r.d, &mm, &st) == 0);
    for (unsigned i = 0; i < r.f.calls; i++)
        CHECK(r.f.log[i].addr == 0x30);
}

/* The value is never read before "new sample ready", however long the range
 * takes within the deadline. */
static void test_range_waits_for_new_sample(void)
{
    struct rig r;
    rig_init(&r);
    r.f.meas_polls = 20;
    r.f.not_ready_polls = 3;
    fv_push(&r.f, 77, 0);
    uint8_t mm = 0, st = 0;
    CHECK(oc_vl6180x_range(&r.d, &mm, &st) == 0);
    CHECK(mm == 77);
    CHECK(r.f.early_value_reads == 0);
}

/* Every call starts a new range and returns that range's result. */
static void test_no_duplicate_samples(void)
{
    struct rig r;
    rig_init(&r);
    fv_push(&r.f, 10, 0);
    fv_push(&r.f, 20, 0);
    fv_push(&r.f, 30, 0);
    uint8_t mm, st;
    for (uint8_t want = 10; want <= 30; want += 10) {
        CHECK(oc_vl6180x_range(&r.d, &mm, &st) == 0);
        CHECK(mm == want);
    }
    CHECK(r.f.starts == 3);
}

/* An attempt that fails after its range completed leaves "new sample ready"
 * set. The next attempt must clear it before starting, so it waits for its
 * own range instead of returning the stale one. */
static void test_stale_result_is_not_returned(void)
{
    struct rig r;
    rig_init(&r);
    fv_push(&r.f, 11, 0);
    fv_push(&r.f, 22, 0);
    r.f.meas_polls = 2;
    r.f.fail_call = 7;   /* ready, clear, start, 3 polls (done on the 3rd), value read fails */
    r.f.fail_errno = EIO;
    uint8_t mm = 0, st = 0;
    CHECK(oc_vl6180x_range(&r.d, &mm, &st) == -EIO);
    CHECK(r.f.int_status == 0x04);   /* the stale result is still flagged */
    CHECK(oc_vl6180x_range(&r.d, &mm, &st) == 0);
    CHECK(mm == 22);
    CHECK(r.f.early_value_reads == 0);
}

/* A range flagged invalid by the device is returned, not hidden: 0 with the
 * error code in bits 7:4 of the status byte. */
static void test_range_error_code_is_reported(void)
{
    struct rig r;
    rig_init(&r);
    fv_push(&r.f, 255, 11);
    uint8_t mm = 0, st = 0;
    CHECK(oc_vl6180x_range(&r.d, &mm, &st) == 0);
    CHECK(oc_vl6180x_range_error(st) == 11);
    CHECK(st == (11u << 4 | 0x01u));
}

static void test_range_timeout_never_completes(void)
{
    struct rig r;
    rig_init(&r);
    r.f.stuck = true;
    uint64_t t0 = r.f.now;
    uint8_t mm = 0xAB, st = 0xCD;
    CHECK(oc_vl6180x_range(&r.d, &mm, &st) == -ETIMEDOUT);
    CHECK(mm == 0xAB && st == 0xCD);   /* outputs untouched */
    CHECK(r.f.now - t0 >= OC_VL6180X_TIMEOUT_NS);
    CHECK(r.f.now - t0 <= OC_VL6180X_TIMEOUT_NS + OC_VL6180X_POLL_NS);
    CHECK(r.f.max_sleep_ns <= OC_VL6180X_POLL_NS);
    CHECK(r.f.early_value_reads == 0);
}

static void test_range_timeout_device_never_ready(void)
{
    struct rig r;
    rig_init(&r);
    r.f.not_ready_polls = 1000000;
    uint8_t mm, st;
    CHECK(oc_vl6180x_range(&r.d, &mm, &st) == -ETIMEDOUT);
    CHECK(r.f.starts == 0);   /* never started while not ready */
}

/* A clock that never advances must not hang the reader. */
static void test_timeout_terminates_with_frozen_clock(void)
{
    struct rig r;
    rig_init(&r);
    r.f.stuck = true;
    r.f.frozen = true;
    uint8_t mm, st;
    CHECK(oc_vl6180x_range(&r.d, &mm, &st) == -ETIMEDOUT);
    CHECK(r.f.calls < 2 * (OC_VL6180X_TIMEOUT_NS / OC_VL6180X_POLL_NS) + 10);
}

static void test_range_bus_errors_pass_through_each_step(void)
{
    for (unsigned k = 1; k <= 7; k++) {
        struct rig r;
        rig_init(&r);
        fv_push(&r.f, 50, 0);
        r.f.fail_call = k;
        r.f.fail_errno = EREMOTEIO;
        uint8_t mm = 0xAB, st = 0xCD;
        CHECK(oc_vl6180x_range(&r.d, &mm, &st) == -EREMOTEIO);
        CHECK(mm == 0xAB && st == 0xCD);
    }
}

static void test_range_clock_failure_stops_before_bus(void)
{
    struct rig r;
    rig_init(&r);
    r.f.now_fail_from = 1;
    r.f.now_errno = EINVAL;
    uint8_t mm, st;
    CHECK(oc_vl6180x_range(&r.d, &mm, &st) == -EINVAL);
    CHECK(r.f.calls == 0);
    CHECK(oc_vl6180x_range(nullptr, &mm, &st) == -EINVAL);
    CHECK(oc_vl6180x_range(&r.d, nullptr, &st) == -EINVAL);
    CHECK(oc_vl6180x_range(&r.d, &mm, nullptr) == -EINVAL);
    CHECK(oc_vl6180x_setup(nullptr) == -EINVAL);
}

int main(void)
{
    RUN(test_init_validates_before_hardware);
    RUN(test_setup_fresh_device);
    RUN(test_setup_not_fresh);
    RUN(test_setup_wrong_model_id);
    RUN(test_setup_bus_errors_pass_through);
    RUN(test_range_transactions);
    RUN(test_range_uses_configured_address);
    RUN(test_range_waits_for_new_sample);
    RUN(test_no_duplicate_samples);
    RUN(test_stale_result_is_not_returned);
    RUN(test_range_error_code_is_reported);
    RUN(test_range_timeout_never_completes);
    RUN(test_range_timeout_device_never_ready);
    RUN(test_timeout_terminates_with_frozen_clock);
    RUN(test_range_bus_errors_pass_through_each_step);
    RUN(test_range_clock_failure_stops_before_bus);

    if (failures != 0) {
        fprintf(stderr, "test_distance: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_distance: all passed\n");
    return 0;
}
