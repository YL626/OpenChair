/* Tests for oc_ads1115 (driver) and oc_light (oc_sample adapter).
 *
 * Every transaction is SIMULATED by tests/fake_i2c.h: a scripted ADS1115 model
 * and a fake clock. Passing proves driver and adapter logic against that
 * model only. It is not evidence about a real ADS1115, ALS-PT19, I2C bus or
 * timing. The Linux transport is tested in test_i2c_linux.c. */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "oc_ads1115.h"
#include "oc_light.h"
#include "oc_sample.h"
#include "fake_i2c.h"

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
    struct fake f;
    struct oc_i2c_bus bus;
    struct oc_clock clk;
    struct oc_ads1115_cfg cfg;
    struct oc_ads1115 d;
};

static void rig_init(struct rig *r)
{
    fake_init(&r->f);
    fake_bus(&r->f, &r->bus, &r->clk);
    r->cfg = (struct oc_ads1115_cfg)OC_ADS1115_CFG_DEFAULT;
    CHECK(oc_ads1115_init(&r->d, &r->bus, &r->clk, &r->cfg) == 0);
}

/* ---- configuration ---- */

static void test_config_words(void)
{
    struct oc_ads1115_cfg c = OC_ADS1115_CFG_DEFAULT;
    uint16_t w = 0;
    CHECK(oc_ads1115_config_word(&c, true, &w) == 0 && w == 0xC383);
    CHECK(oc_ads1115_config_word(&c, false, &w) == 0 && w == 0x4383);

    /* PGA field is bits 11:9, rate field bits 7:5; MUX 100, MODE 1, COMP 11. */
    for (uint8_t p = 0; p <= 5; p++)
        for (uint8_t r = 0; r <= 7; r++) {
            c.pga = p; c.rate = r;
            CHECK(oc_ads1115_config_word(&c, false, &w) == 0);
            CHECK(w == (uint16_t)(0x4000 | (p << 9) | 0x0100 | (r << 5) | 0x0003));
            CHECK(oc_ads1115_config_word(&c, true, &w) == 0 && (w & 0x8000));
        }

    static const uint32_t sps[] = {8, 16, 32, 64, 128, 250, 475, 860};
    for (uint8_t r = 0; r <= 7; r++) {
        CHECK(oc_ads1115_rate_sps(r) == sps[r]);
        CHECK(oc_ads1115_timeout_ns(r) > 2 * (1000000000ull / sps[r]));
    }
    CHECK(oc_ads1115_rate_sps(8) == 0 && oc_ads1115_timeout_ns(8) == 0);
}

static void test_config_rejects_invalid(void)
{
    uint16_t w = 0xBEEF;
    struct oc_ads1115_cfg ok = OC_ADS1115_CFG_DEFAULT, c;
    CHECK(oc_ads1115_config_word(nullptr, true, &w) == -EINVAL);
    CHECK(oc_ads1115_config_word(&ok, true, nullptr) == -EINVAL);
    c = ok; c.pga = 6;  CHECK(oc_ads1115_config_word(&c, true, &w) == -EINVAL);
    c = ok; c.pga = 7;  CHECK(oc_ads1115_config_word(&c, true, &w) == -EINVAL);
    c = ok; c.rate = 8; CHECK(oc_ads1115_config_word(&c, true, &w) == -EINVAL);
    c = ok; c.addr = 0x47; CHECK(oc_ads1115_config_word(&c, true, &w) == -EINVAL);
    c = ok; c.addr = 0x4C; CHECK(oc_ads1115_config_word(&c, true, &w) == -EINVAL);
    c = ok; c.addr = 0x29; CHECK(oc_ads1115_config_word(&c, true, &w) == -EINVAL);
    CHECK(w == 0xBEEF);   /* untouched on error */
    c = ok; c.addr = 0x4B; CHECK(oc_ads1115_config_word(&c, true, &w) == 0);
}

static void test_init_validates_before_hardware(void)
{
    struct rig r;
    fake_init(&r.f);
    fake_bus(&r.f, &r.bus, &r.clk);
    struct oc_ads1115_cfg ok = OC_ADS1115_CFG_DEFAULT, bad = ok;
    struct oc_ads1115 d;
    bad.pga = 9;
    CHECK(oc_ads1115_init(&d, &r.bus, &r.clk, &bad) == -EINVAL);
    CHECK(oc_ads1115_init(nullptr, &r.bus, &r.clk, &ok) == -EINVAL);
    CHECK(oc_ads1115_init(&d, nullptr, &r.clk, &ok) == -EINVAL);
    CHECK(oc_ads1115_init(&d, &r.bus, nullptr, &ok) == -EINVAL);
    CHECK(oc_ads1115_init(&d, &r.bus, &r.clk, nullptr) == -EINVAL);

    struct oc_i2c_bus b = r.bus;
    b.xfer = nullptr;
    CHECK(oc_ads1115_init(&d, &b, &r.clk, &ok) == -EINVAL);
    b = r.bus; b.origin = 0;
    CHECK(oc_ads1115_init(&d, &b, &r.clk, &ok) == -EINVAL);
    b.origin = OC_ORIGIN_MIXED;
    CHECK(oc_ads1115_init(&d, &b, &r.clk, &ok) == -EINVAL);

    struct oc_clock c = r.clk;
    c.now_ns = nullptr;
    CHECK(oc_ads1115_init(&d, &r.bus, &c, &ok) == -EINVAL);
    c = r.clk; c.sleep_until_ns = nullptr;
    CHECK(oc_ads1115_init(&d, &r.bus, &c, &ok) == -EINVAL);

    CHECK(r.f.calls == 0 && r.f.now_calls == 0);   /* nothing touched */
    CHECK(oc_ads1115_init(&d, &r.bus, &r.clk, &ok) == 0);
    CHECK(d.start_word == 0xC383 && d.idle_word == 0x4383);
    CHECK(r.f.calls == 0);                           /* init is still silent */
}

/* ---- register transactions and readiness ---- */

static void test_read_transactions(void)
{
    struct rig r; rig_init(&r);
    r.f.busy_polls = 2;
    fake_push(&r.f, 0x1234);
    int16_t raw = 0;
    CHECK(oc_ads1115_read(&r.d, &raw) == 0);
    CHECK(raw == 0x1234);                      /* big-endian assembled correctly */

    /* call 1: start write [01 C3 83] to 0x48, one message */
    CHECK(r.f.log[0].n == 1 && r.f.log[0].addr[0] == 0x48 && r.f.log[0].flags[0] == 0);
    CHECK(r.f.log[0].len[0] == 3);
    CHECK(r.f.log[0].data[0][0] == 0x01 && r.f.log[0].data[0][1] == 0xC3 &&
          r.f.log[0].data[0][2] == 0x83);
    /* config polls: ONE transaction of two messages (pointer write, read) */
    CHECK(r.f.cfg_reads == 3 && r.f.conv_reads == 1 && r.f.starts == 1);
    CHECK(r.f.calls == 1 + 3 + 1);
    for (unsigned i = 1; i < r.f.calls; i++) {
        CHECK(r.f.log[i].n == 2);
        CHECK(r.f.log[i].addr[0] == 0x48 && r.f.log[i].addr[1] == 0x48);
        CHECK(r.f.log[i].flags[0] == 0 && r.f.log[i].flags[1] == OC_I2C_M_RD);
        CHECK(r.f.log[i].len[0] == 1 && r.f.log[i].len[1] == 2);
    }
    CHECK(r.f.log[1].data[0][0] == 1 && r.f.log[3].data[0][0] == 1);
    CHECK(r.f.log[4].data[0][0] == 0);          /* last: conversion register */
    /* final config readback: OS set, rest as written */
    CHECK(r.f.log[3].data[1][0] == 0xC3 && r.f.log[3].data[1][1] == 0x83);
}

static void test_read_uses_configured_address(void)
{
    struct rig r; fake_init(&r.f); fake_bus(&r.f, &r.bus, &r.clk);
    r.cfg = (struct oc_ads1115_cfg)OC_ADS1115_CFG_DEFAULT;
    r.cfg.addr = 0x4A;
    r.cfg.pga = OC_ADS1115_PGA_2V048;
    r.cfg.rate = OC_ADS1115_RATE_860SPS;
    CHECK(oc_ads1115_init(&r.d, &r.bus, &r.clk, &r.cfg) == 0);
    fake_push(&r.f, 5);
    int16_t raw = 0;
    CHECK(oc_ads1115_read(&r.d, &raw) == 0 && raw == 5);
    CHECK(r.f.log[0].addr[0] == 0x4A);
    CHECK(r.f.log[0].data[0][1] == (uint8_t)((0xC000 | (2 << 9) | 0x0100 | 0) >> 8));
    CHECK(r.f.log[0].data[0][2] == (uint8_t)((7 << 5) | 3));
}

static void test_signed_values(void)
{
    static const int16_t vals[] = {0, 1, -1, 32766, -32767, 0x1234, -0x1234, 255, -256};
    struct rig r; rig_init(&r);
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++)
        fake_push(&r.f, vals[i]);
    for (size_t i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        int16_t raw = 0x5555;
        CHECK(oc_ads1115_read(&r.d, &raw) == 0);
        CHECK(raw == vals[i]);
    }
}

static void test_no_duplicate_samples(void)
{
    struct rig r; rig_init(&r);
    fake_push(&r.f, 100);
    fake_push(&r.f, 200);
    fake_push(&r.f, 300);
    int16_t a, b, c;
    CHECK(oc_ads1115_read(&r.d, &a) == 0);
    CHECK(oc_ads1115_read(&r.d, &b) == 0);
    CHECK(oc_ads1115_read(&r.d, &c) == 0);
    CHECK(a == 100 && b == 200 && c == 300);
    CHECK(r.f.starts == 3 && r.f.conv_reads == 3);   /* one conversion per read */
}

static void test_conversion_is_never_read_before_ready(void)
{
    struct rig r; rig_init(&r);
    r.f.busy_polls = 5;
    fake_push(&r.f, 42);
    int16_t raw = 0;
    CHECK(oc_ads1115_read(&r.d, &raw) == 0 && raw == 42);
    /* the conversion read is the last transaction and follows 6 config reads */
    CHECK(r.f.cfg_reads == 6 && r.f.conv_reads == 1);
    CHECK(r.f.log[r.f.calls - 1].data[0][0] == 0);
}

/* ---- bounded waiting ---- */

static void test_time_based_conversion(void)
{
    /* Device is busy for the nominal conversion time of fake time. */
    for (uint8_t rate = 0; rate <= 7; rate++) {
        struct rig r; fake_init(&r.f); fake_bus(&r.f, &r.bus, &r.clk);
        r.cfg = (struct oc_ads1115_cfg)OC_ADS1115_CFG_DEFAULT;
        r.cfg.rate = rate;
        CHECK(oc_ads1115_init(&r.d, &r.bus, &r.clk, &r.cfg) == 0);
        r.f.conv_ns = 1000000000ull / oc_ads1115_rate_sps(rate);
        r.f.xfer_cost_ns = 50000;
        fake_push(&r.f, 321);
        uint64_t t0 = r.f.now;
        int16_t raw = 0;
        CHECK(oc_ads1115_read(&r.d, &raw) == 0 && raw == 321);
        CHECK(r.f.early_conv_reads == 0);
        CHECK(r.f.now - t0 >= r.f.conv_ns);            /* waited for the device */
        CHECK(r.f.cfg_reads <= 12);                    /* without hammering the bus */
    }
}

static void test_timeout_when_never_ready(void)
{
    struct rig r; rig_init(&r);
    r.f.stuck_busy = true;
    fake_push(&r.f, 7);
    uint64_t t0 = r.f.now;
    int16_t raw = 0x2222;
    CHECK(oc_ads1115_read(&r.d, &raw) == -ETIMEDOUT);
    CHECK(raw == 0x2222);                      /* untouched on error */
    CHECK(r.f.conv_reads == 0);
    uint64_t budget = oc_ads1115_timeout_ns(r.cfg.rate);
    CHECK(r.f.now - t0 <= budget);             /* no sleep overshoots the deadline */
    CHECK(r.f.now - t0 >= budget / 2);         /* and it did actually wait */
    CHECK(r.f.sleeps < 200 && r.f.calls < 200);
    CHECK(r.f.max_sleep_ns <= budget);
}

static void test_timeout_terminates_with_frozen_clock(void)
{
    struct rig r; rig_init(&r);
    r.f.stuck_busy = true;
    r.f.frozen = true;                         /* time never advances */
    int16_t raw = 0;
    CHECK(oc_ads1115_read(&r.d, &raw) == -ETIMEDOUT);
    CHECK(r.f.calls < 10000);                  /* poll-count cap, not time, ended it */
}

static void test_deadline_holds_when_transfers_are_slow(void)
{
    /* Slow transfers must be stopped by the deadline, not only by the
     * poll-count cap: each xfer costs 5 ms of fake time. */
    struct rig r; rig_init(&r);
    r.f.stuck_busy = true;
    r.f.xfer_cost_ns = 5000000;
    uint64_t t0 = r.f.now;
    int16_t raw;
    CHECK(oc_ads1115_read(&r.d, &raw) == -ETIMEDOUT);
    /* start write + at most one transfer that began before the deadline */
    CHECK(r.f.now - t0 <= oc_ads1115_timeout_ns(r.cfg.rate) + 2 * r.f.xfer_cost_ns);
    CHECK(r.f.cfg_reads <= 6);
}

static void test_timeout_each_rate(void)
{
    for (uint8_t rate = 0; rate <= 7; rate++) {
        struct rig r; fake_init(&r.f); fake_bus(&r.f, &r.bus, &r.clk);
        r.cfg = (struct oc_ads1115_cfg)OC_ADS1115_CFG_DEFAULT;
        r.cfg.rate = rate;
        CHECK(oc_ads1115_init(&r.d, &r.bus, &r.clk, &r.cfg) == 0);
        r.f.stuck_busy = true;
        uint64_t t0 = r.f.now;
        int16_t raw;
        CHECK(oc_ads1115_read(&r.d, &raw) == -ETIMEDOUT);
        CHECK(r.f.now - t0 <= oc_ads1115_timeout_ns(rate));
    }
}

static void test_sleep_failure_propagates(void)
{
    struct rig r; rig_init(&r);
    r.f.sleep_errno = EINVAL;
    fake_push(&r.f, 1);
    int16_t raw = 0;
    CHECK(oc_ads1115_read(&r.d, &raw) == -EINVAL);
    CHECK(r.f.conv_reads == 0);
}

static void test_clock_failure_propagates(void)
{
    struct rig r; rig_init(&r);
    r.f.now_fail_from = 1;
    r.f.now_errno = EFAULT;
    int16_t raw = 0;
    CHECK(oc_ads1115_read(&r.d, &raw) == -EFAULT);
    CHECK(r.f.calls == 0);                     /* nothing started without a clock */
}

/* ---- bus errors ---- */

static void test_bus_errors_pass_through_each_step(void)
{
    static const int errs[] = {ENXIO, EREMOTEIO, ETIMEDOUT, EIO, EBUSY, EAGAIN};
    for (size_t e = 0; e < sizeof errs / sizeof errs[0]; e++)
        for (unsigned call = 1; call <= 3; call++) {   /* start, poll, conv read */
            struct rig r; rig_init(&r);
            fake_push(&r.f, 9);
            r.f.fail_call = call;
            r.f.fail_errno = errs[e];
            int16_t raw = 0x3333;
            CHECK(oc_ads1115_read(&r.d, &raw) == -errs[e]);
            CHECK(raw == 0x3333);
            /* a failed attempt leaves the driver usable */
            r.f.fail_call = 0;
            fake_push(&r.f, 10);
            CHECK(oc_ads1115_read(&r.d, &raw) == 0);
        }
}

static void test_config_readback_mismatch(void)
{
    struct rig r; rig_init(&r);
    r.f.readback_xor = 0x0200;                 /* device reports a different PGA */
    fake_push(&r.f, 5);
    int16_t raw = 0x4444;
    CHECK(oc_ads1115_read(&r.d, &raw) == -EPROTO);
    CHECK(raw == 0x4444 && r.f.conv_reads == 0);
}

/* ---- oc_light ---- */

static void on_alarm(int sig) { (void)sig; }

static void test_system_clock(void)
{
    struct oc_clock c;
    oc_clock_system(&c);
    CHECK(c.now_ns != nullptr && c.sleep_until_ns != nullptr);
    uint64_t a = 0, b = 0;
    CHECK(c.now_ns(c.ctx, &a) == 0 && a > 0);
    CHECK(c.sleep_until_ns(c.ctx, a + 3000000) == 0);
    CHECK(c.now_ns(c.ctx, &b) == 0 && b >= a + 3000000);
    CHECK(c.sleep_until_ns(c.ctx, a) == 0);       /* target in the past returns */

    /* A signal mid-sleep must not shorten the wait. */
    struct sigaction sa = {.sa_handler = on_alarm}, old;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGALRM, &sa, &old) == 0);
    struct itimerval it = {.it_interval = {0, 2000}, .it_value = {0, 2000}};
    CHECK(setitimer(ITIMER_REAL, &it, nullptr) == 0);
    CHECK(c.now_ns(c.ctx, &a) == 0);
    CHECK(c.sleep_until_ns(c.ctx, a + 20000000) == 0);
    CHECK(c.now_ns(c.ctx, &b) == 0 && b >= a + 20000000);
    struct itimerval off = {0};
    setitimer(ITIMER_REAL, &off, nullptr);
    sigaction(SIGALRM, &old, nullptr);
}

static void light_rig(struct rig *r, struct oc_light *l)
{
    fake_init(&r->f);
    fake_bus(&r->f, &r->bus, &r->clk);
    r->cfg = (struct oc_ads1115_cfg)OC_ADS1115_CFG_DEFAULT;
    CHECK(oc_light_init(l, &r->bus, &r->clk, &r->cfg) == 0);
}

static void test_status_mapping(void)
{
    CHECK(oc_light_status_for_errno(-ENXIO) == OC_ST_NACK);
    CHECK(oc_light_status_for_errno(-EREMOTEIO) == OC_ST_NACK);
    CHECK(oc_light_status_for_errno(-ETIMEDOUT) == OC_ST_TIMEOUT);
    static const int others[] = {EIO, EBUSY, EAGAIN, ENODEV, EINVAL, EINTR,
                                 EBADF, EPROTO, ENOMEM, EOPNOTSUPP};
    for (size_t i = 0; i < sizeof others / sizeof others[0]; i++)
        CHECK(oc_light_status_for_errno(-others[i]) == OC_ST_IO_ERR);
}

static void test_light_init_validation(void)
{
    struct rig r; struct oc_light l;
    fake_init(&r.f); fake_bus(&r.f, &r.bus, &r.clk);
    struct oc_ads1115_cfg ok = OC_ADS1115_CFG_DEFAULT, bad = ok;
    bad.rate = 99;
    CHECK(oc_light_init(&l, &r.bus, &r.clk, &bad) == -EINVAL);
    CHECK(oc_light_init(nullptr, &r.bus, &r.clk, &ok) == -EINVAL);
    struct oc_i2c_bus b = r.bus; b.origin = 0;
    CHECK(oc_light_init(&l, &b, &r.clk, &ok) == -EINVAL);
    CHECK(r.f.calls == 0);
    struct oc_sample s;
    CHECK(oc_light_init(&l, &r.bus, &r.clk, &ok) == 0);
    CHECK(oc_light_sample(nullptr, &s, nullptr) == -EINVAL);
    CHECK(oc_light_sample(&l, nullptr, nullptr) == -EINVAL);
    CHECK(r.f.calls == 0);
}

static void test_light_ok_sample(void)
{
    struct rig r; struct oc_light l; light_rig(&r, &l);
    r.f.xfer_cost_ns = 50000;
    fake_push(&r.f, 12345);
    struct oc_sample s; memset(&s, 0xAA, sizeof s);
    int ioe = 99;
    uint64_t t0 = r.f.now;
    CHECK(oc_light_sample(&l, &s, &ioe) == 0);
    CHECK(ioe == 0);
    CHECK(s.source == OC_SOURCE_LIGHT && s.status == OC_ST_OK);
    CHECK(s.value == 12345 && s.seq == 0);
    CHECK(s.origin == OC_ORIGIN_SIMULATED);
    CHECK(s.aux == 0x4383);
    CHECK(s.t_mono_ns == r.f.now && s.t_mono_ns > t0);   /* stamped after the read */
    CHECK(oc_sample_validate(&s) == 0);
}

static void test_light_timestamp_taken_after_attempt_even_on_failure(void)
{
    struct rig r; struct oc_light l; light_rig(&r, &l);
    r.f.stuck_busy = true;
    struct oc_sample s;
    uint64_t t0 = r.f.now;
    CHECK(oc_light_sample(&l, &s, nullptr) == 0);
    CHECK(s.status == OC_ST_TIMEOUT);
    CHECK(s.t_mono_ns == r.f.now && s.t_mono_ns >= t0 + oc_ads1115_timeout_ns(l.adc.cfg.rate) / 2);
}

static void test_light_failures_map_and_zero_value(void)
{
    static const struct { int err; uint8_t st; } t[] = {
        {ENXIO, OC_ST_NACK}, {EREMOTEIO, OC_ST_NACK}, {ETIMEDOUT, OC_ST_TIMEOUT},
        {EIO, OC_ST_IO_ERR}, {EBUSY, OC_ST_IO_ERR},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        struct rig r; struct oc_light l; light_rig(&r, &l);
        fake_push(&r.f, 777);
        r.f.fail_call = 2;                    /* the poll */
        r.f.fail_errno = t[i].err;
        struct oc_sample s; int ioe = 0;
        CHECK(oc_light_sample(&l, &s, &ioe) == 0);
        CHECK(s.status == t[i].st);
        CHECK(s.value == 0);                  /* never a fabricated reading */
        CHECK(ioe == -t[i].err);              /* original errno preserved */
        CHECK(s.aux == 0x4383 && s.source == OC_SOURCE_LIGHT);
        CHECK(oc_sample_validate(&s) == 0);
    }
}

static void test_light_readback_mismatch_is_io_err(void)
{
    struct rig r; struct oc_light l; light_rig(&r, &l);
    r.f.readback_xor = 0x0040;
    fake_push(&r.f, 5);
    struct oc_sample s; int ioe = 0;
    CHECK(oc_light_sample(&l, &s, &ioe) == 0);
    CHECK(s.status == OC_ST_IO_ERR && s.value == 0 && ioe == -EPROTO);
}

static void test_light_seq_counts_attempts(void)
{
    struct rig r; struct oc_light l; light_rig(&r, &l);
    struct oc_sample s;
    fake_push(&r.f, 1);
    CHECK(oc_light_sample(&l, &s, nullptr) == 0 && s.seq == 0 && s.status == OC_ST_OK);
    r.f.fail_call = r.f.calls + 1; r.f.fail_errno = ENXIO;
    CHECK(oc_light_sample(&l, &s, nullptr) == 0 && s.seq == 1 && s.status == OC_ST_NACK);
    fake_push(&r.f, 2);
    CHECK(oc_light_sample(&l, &s, nullptr) == 0 && s.seq == 2 && s.status == OC_ST_OK);
    CHECK(s.value == 2);
}

static void test_light_seq_wraps(void)
{
    struct rig r; struct oc_light l; light_rig(&r, &l);
    l.seq = UINT32_MAX;
    struct oc_sample s;
    fake_push(&r.f, 1); fake_push(&r.f, 1);
    CHECK(oc_light_sample(&l, &s, nullptr) == 0 && s.seq == UINT32_MAX);
    CHECK(oc_light_sample(&l, &s, nullptr) == 0 && s.seq == 0);
}

static void test_light_digital_clipping(void)
{
    static const struct { int16_t raw; uint8_t st; } t[] = {
        {INT16_MAX, OC_ST_SATURATED}, {INT16_MIN, OC_ST_SATURATED},
        {INT16_MAX - 1, OC_ST_OK},    {INT16_MIN + 1, OC_ST_OK},
        {0, OC_ST_OK},                {-1, OC_ST_OK},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        struct rig r; struct oc_light l; light_rig(&r, &l);
        fake_push(&r.f, t[i].raw);
        struct oc_sample s; int ioe = 5;
        CHECK(oc_light_sample(&l, &s, &ioe) == 0);
        CHECK(s.status == t[i].st);
        CHECK(ioe == 0);                      /* clipping is not an I/O error */
        CHECK(s.value == (t[i].st == OC_ST_OK ? t[i].raw : 0));
    }
}

static void test_light_origin_comes_from_bus(void)
{
    static const uint8_t origins[] = {OC_ORIGIN_SIMULATED, OC_ORIGIN_LIVE,
                                      OC_ORIGIN_REPLAYED};
    for (size_t i = 0; i < sizeof origins; i++) {
        struct rig r; struct oc_light l;
        fake_init(&r.f); fake_bus(&r.f, &r.bus, &r.clk);
        r.bus.origin = origins[i];
        r.cfg = (struct oc_ads1115_cfg)OC_ADS1115_CFG_DEFAULT;
        CHECK(oc_light_init(&l, &r.bus, &r.clk, &r.cfg) == 0);
        fake_push(&r.f, 3);
        struct oc_sample s;
        CHECK(oc_light_sample(&l, &s, nullptr) == 0);
        CHECK(s.origin == origins[i]);
    }
}

static void test_light_origin_fixed_at_init(void)
{
    /* Routine mutation of the caller's bus after init cannot relabel samples. */
    struct rig r; struct oc_light l; light_rig(&r, &l);
    CHECK(r.bus.origin == OC_ORIGIN_SIMULATED);
    r.bus.origin = OC_ORIGIN_LIVE;
    fake_push(&r.f, 3);
    struct oc_sample s;
    CHECK(oc_light_sample(&l, &s, nullptr) == 0);
    CHECK(s.origin == OC_ORIGIN_SIMULATED);
}

static void test_light_clock_failure_keeps_attempt_errno(void)
{
    struct rig r; struct oc_light l; light_rig(&r, &l);
    r.f.fail_call = 1;                  /* the attempt fails (NACK on the start write) ... */
    r.f.fail_errno = ENXIO;
    r.f.now_fail_from = 2;              /* ... and the post-attempt clock read fails too */
    r.f.now_errno = EFAULT;
    int ioe = 77;
    struct oc_sample s;
    memset(&s, 0xAA, sizeof s);
    struct oc_sample before = s;
    CHECK(oc_light_sample(&l, &s, &ioe) == -EFAULT);
    CHECK(ioe == -ENXIO);               /* the attempt's errno is still observable */
    CHECK(memcmp(&s, &before, sizeof s) == 0);
    CHECK(l.seq == 1);                  /* and the attempt consumed its seq */
}

static void test_light_clock_failure_is_not_fabricated(void)
{
    /* Learn how many clock reads one healthy sample makes; the last one is
     * the post-attempt timestamp. */
    struct rig r0; struct oc_light l0; light_rig(&r0, &l0);
    fake_push(&r0.f, 1);
    struct oc_sample s;
    CHECK(oc_light_sample(&l0, &s, nullptr) == 0);
    const unsigned per_sample = r0.f.now_calls;
    CHECK(per_sample >= 2);

    struct rig r; struct oc_light l; light_rig(&r, &l);
    fake_push(&r.f, 1); fake_push(&r.f, 2);
    r.f.now_fail_from = per_sample;
    r.f.now_errno = EFAULT;
    memset(&s, 0xAA, sizeof s);
    struct oc_sample before = s;
    CHECK(oc_light_sample(&l, &s, nullptr) == -EFAULT);
    CHECK(memcmp(&s, &before, sizeof s) == 0);   /* no sample was fabricated */
    r.f.now_fail_from = 0;
    CHECK(oc_light_sample(&l, &s, nullptr) == 0);
    CHECK(s.seq == 1);                           /* the failed attempt consumed seq 0 */
}

static void test_light_samples_survive_the_wire(void)
{
    struct rig r; struct oc_light l; light_rig(&r, &l);
    fake_push(&r.f, -321);
    fake_push(&r.f, 654);
    for (int i = 0; i < 4; i++) {
        if (i == 2) { r.f.fail_call = r.f.calls + 1; r.f.fail_errno = EIO; }
        if (i == 3) r.f.fail_call = 0;
        struct oc_sample s, back; uint8_t buf[OC_MSG_SAMPLE_SIZE]; size_t n = 0;
        CHECK(oc_light_sample(&l, &s, nullptr) == 0);
        CHECK(oc_msg_sample_encode(&s, buf, sizeof buf, &n) == 0 && n == 28);
        CHECK(oc_msg_sample_decode(buf, n, &back) == 0);
        CHECK(s.t_mono_ns == back.t_mono_ns && s.seq == back.seq &&
              s.value == back.value && s.status == back.status &&
              s.aux == back.aux && s.origin == back.origin &&
              s.source == back.source);
    }
}

int main(void)
{
    RUN(test_config_words);
    RUN(test_config_rejects_invalid);
    RUN(test_init_validates_before_hardware);
    RUN(test_read_transactions);
    RUN(test_read_uses_configured_address);
    RUN(test_signed_values);
    RUN(test_no_duplicate_samples);
    RUN(test_conversion_is_never_read_before_ready);
    RUN(test_time_based_conversion);
    RUN(test_timeout_when_never_ready);
    RUN(test_timeout_terminates_with_frozen_clock);
    RUN(test_deadline_holds_when_transfers_are_slow);
    RUN(test_timeout_each_rate);
    RUN(test_sleep_failure_propagates);
    RUN(test_clock_failure_propagates);
    RUN(test_bus_errors_pass_through_each_step);
    RUN(test_config_readback_mismatch);
    RUN(test_system_clock);
    RUN(test_status_mapping);
    RUN(test_light_init_validation);
    RUN(test_light_ok_sample);
    RUN(test_light_timestamp_taken_after_attempt_even_on_failure);
    RUN(test_light_failures_map_and_zero_value);
    RUN(test_light_readback_mismatch_is_io_err);
    RUN(test_light_seq_counts_attempts);
    RUN(test_light_seq_wraps);
    RUN(test_light_digital_clipping);
    RUN(test_light_origin_comes_from_bus);
    RUN(test_light_origin_fixed_at_init);
    RUN(test_light_clock_failure_keeps_attempt_errno);
    RUN(test_light_clock_failure_is_not_fabricated);
    RUN(test_light_samples_survive_the_wire);

    if (failures != 0) {
        fprintf(stderr, "test_light: %d FAILURE(S)\n", failures);
        return 1;
    }
    fprintf(stderr, "test_light: all passed\n");
    return 0;
}
