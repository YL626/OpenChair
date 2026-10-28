/* Scripted ADS1115-on-a-bus and fake clock for host tests of oc_ads1115 and
 * oc_light. Everything here is SIMULATED: no hardware, no real time.
 * Include after the oc_* headers. Header-only; static functions. */
#ifndef FAKE_I2C_H
#define FAKE_I2C_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "oc_clock.h"
#include "oc_i2c.h"
#include "oc_store.h"

#define FAKE_MAX_LOG 512
#define FAKE_MAX_CONV 16

struct fake_rec {
    size_t n;
    uint16_t addr[OC_I2C_MAX_MSGS];
    uint16_t flags[OC_I2C_MAX_MSGS];
    uint16_t len[OC_I2C_MAX_MSGS];
    uint8_t data[OC_I2C_MAX_MSGS][4];   /* write: as sent; read: as returned */
};

struct fake {
    /* ADS1115 model */
    uint16_t cfg_reg;            /* low 15 bits as last written; OS is modelled */
    bool converting;
    int busy_polls;              /* config reads that report busy after a start */
    int busy_left;
    bool stuck_busy;             /* never completes */
    uint64_t conv_ns;            /* if > 0: ready only once fake time passes start+conv_ns */
    uint64_t conv_start;
    unsigned early_conv_reads;   /* conversion reads before the conversion finished */
    uint16_t readback_xor;       /* corrupts the config readback */
    int16_t conv[FAKE_MAX_CONV]; /* results, consumed one per started conversion */
    size_t conv_n, conv_next;
    int16_t latched;
    unsigned starts;             /* config writes with OS=1 */
    unsigned conv_reads;         /* conversion-register reads */
    unsigned cfg_reads;

    /* error injection: the fail_call-th xfer (1-based) fails with fail_errno;
     * fail_from_call>0 fails every call from that index on. */
    unsigned fail_call;
    unsigned fail_from_call;
    int fail_errno;

    /* log */
    struct fake_rec log[FAKE_MAX_LOG];
    unsigned calls;

    /* clock */
    uint64_t now;
    bool frozen;                 /* sleeps do not advance time */
    uint64_t xfer_cost_ns;       /* time added by each xfer */
    unsigned sleeps;
    uint64_t max_sleep_ns;       /* longest requested sleep (target - now) */
    int sleep_errno;             /* sleep fails with this */
    unsigned now_calls;
    unsigned now_fail_from;      /* now_ns fails from this call on (1-based) */
    int now_errno;
};

static int fake_xfer(void *ctx, struct oc_i2c_msg *m, size_t n)
{
    struct fake *f = ctx;
    f->calls++;
    f->now += f->xfer_cost_ns;
    struct fake_rec *r = f->calls <= FAKE_MAX_LOG ? &f->log[f->calls - 1] : nullptr;
    if (r != nullptr) {
        memset(r, 0, sizeof *r);
        r->n = n;
        for (size_t i = 0; i < n && i < OC_I2C_MAX_MSGS; i++) {
            r->addr[i] = m[i].addr;
            r->flags[i] = m[i].flags;
            r->len[i] = m[i].len;
            if (!(m[i].flags & OC_I2C_M_RD) && m[i].len <= 4)
                memcpy(r->data[i], m[i].buf, m[i].len);
        }
    }
    if ((f->fail_call != 0 && f->calls == f->fail_call) ||
        (f->fail_from_call != 0 && f->calls >= f->fail_from_call))
        return -f->fail_errno;

    if (n == 1 && m[0].len == 3 && m[0].buf[0] == 1) {   /* config write */
        uint16_t w = (uint16_t)(m[0].buf[1] << 8 | m[0].buf[2]);
        f->cfg_reg = w & 0x7FFFu;
        if (w & 0x8000u) {
            f->starts++;
            f->converting = true;
            f->busy_left = f->busy_polls;
            f->conv_start = f->now;
            f->latched = f->conv_next < f->conv_n ? f->conv[f->conv_next++] : 0;
        }
        return 0;
    }
    if (n == 2 && m[0].len == 1 && m[1].len == 2 && (m[1].flags & OC_I2C_M_RD)) {
        uint16_t v;
        if (m[0].buf[0] == 1) {                           /* config read */
            f->cfg_reads++;
            bool ready = true;
            if (f->converting) {
                if (f->conv_ns > 0 ? f->now < f->conv_start + f->conv_ns
                                   : (f->stuck_busy || f->busy_left > 0)) {
                    ready = false;
                    if (f->conv_ns == 0 && !f->stuck_busy)
                        f->busy_left--;
                } else {
                    f->converting = false;
                }
            }
            v = (uint16_t)(f->cfg_reg | (ready ? 0x8000u : 0u));
            v ^= f->readback_xor;
        } else if (m[0].buf[0] == 0) {                    /* conversion read */
            f->conv_reads++;
            if (f->converting)
                f->early_conv_reads++;   /* read before OS reported done */
            v = (uint16_t)f->latched;
        } else {
            return -EINVAL;
        }
        m[1].buf[0] = (uint8_t)(v >> 8);                  /* big-endian */
        m[1].buf[1] = (uint8_t)v;
        if (r != nullptr)
            memcpy(r->data[1], m[1].buf, 2);
        return 0;
    }
    return -EINVAL;   /* anything else is a driver bug: fail loudly */
}

static int fake_now(void *ctx, uint64_t *out)
{
    struct fake *f = ctx;
    f->now_calls++;
    if (f->now_fail_from != 0 && f->now_calls >= f->now_fail_from)
        return -f->now_errno;
    *out = f->now;
    return 0;
}

static int fake_sleep_until(void *ctx, uint64_t abs_ns)
{
    struct fake *f = ctx;
    f->sleeps++;
    if (abs_ns > f->now && abs_ns - f->now > f->max_sleep_ns)
        f->max_sleep_ns = abs_ns - f->now;
    if (f->sleep_errno != 0)
        return -f->sleep_errno;
    if (!f->frozen && abs_ns > f->now)
        f->now = abs_ns;
    return 0;
}

static void fake_init(struct fake *f)
{
    memset(f, 0, sizeof *f);
    f->now = 1000000;   /* arbitrary non-zero start */
}

static void fake_bus(struct fake *f, struct oc_i2c_bus *bus, struct oc_clock *clk)
{
    *bus = (struct oc_i2c_bus){.xfer = fake_xfer, .ctx = f,
                               .origin = OC_ORIGIN_SIMULATED};
    *clk = (struct oc_clock){.now_ns = fake_now, .sleep_until_ns = fake_sleep_until,
                             .ctx = f};
}

static void fake_push(struct fake *f, int16_t v)
{
    if (f->conv_n < FAKE_MAX_CONV)
        f->conv[f->conv_n++] = v;
}

#endif
