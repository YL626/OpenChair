/* Scripted VL6180X-on-a-bus and fake clock for host tests of oc_vl6180x and
 * oc_distance. Everything here is SIMULATED: no hardware, no real time.
 * The model follows the register behaviour the driver relies on (see
 * src/oc_vl6180x.c); it is not evidence about a real VL6180X.
 * Include after the oc_* headers. Header-only; static functions. */
#ifndef FAKE_VL6180X_H
#define FAKE_VL6180X_H

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "oc_clock.h"
#include "oc_i2c.h"
#include "oc_store.h"

#define FV_NREGS 0x0300u
#define FV_MAX_MEAS 16
#define FV_MAX_LOG 256

struct fv_meas {
    uint8_t mm;
    uint8_t err;   /* range error code, bits 7:4 of RESULT__RANGE_STATUS */
};

struct fv_rec {
    size_t n;
    uint16_t addr;
    uint16_t reg;      /* register address from the first message */
    bool read;         /* register read (2 messages) vs. write */
    uint8_t value;     /* written or returned */
};

struct fv {
    uint8_t regs[FV_NREGS];   /* plain register file; special ones below */
    uint8_t model_id;
    bool unplugged;           /* every transfer NACKs (-ENXIO) */

    /* ranging model */
    int not_ready_polls;      /* RESULT__RANGE_STATUS reads reporting "not ready" */
    bool measuring;
    int meas_polls;           /* interrupt-status reads before a started range completes */
    int meas_left;
    bool stuck;               /* a started range never completes */
    uint8_t int_status;       /* RESULT__INTERRUPT_STATUS_GPIO range bits */
    struct fv_meas meas[FV_MAX_MEAS];
    size_t meas_n, meas_next;
    struct fv_meas latched;   /* result of the last completed range */
    unsigned starts;
    unsigned early_value_reads;   /* range value read while not "new sample ready" */
    unsigned private_writes;      /* writes to the AN4545 private settings */

    /* error injection: the fail_call-th xfer (1-based) fails with fail_errno */
    unsigned fail_call;
    int fail_errno;

    struct fv_rec log[FV_MAX_LOG];
    unsigned calls;

    /* clock */
    uint64_t now;
    bool frozen;              /* sleeps do not advance time */
    uint64_t max_sleep_ns;
    unsigned now_calls;
    unsigned now_fail_from;   /* now_ns fails from this call on (1-based) */
    int now_errno;
};

/* Power-on state: model ID, fresh out of reset, device ready, no settings. */
static void fv_power_on(struct fv *f)
{
    memset(f->regs, 0, sizeof f->regs);
    f->regs[0x0000] = f->model_id;
    f->regs[0x0016] = 0x01;   /* SYSTEM__FRESH_OUT_OF_RESET */
    f->measuring = false;
    f->int_status = 0;
}

static bool fv_is_private_reg(uint16_t reg)
{
    static const uint16_t priv[] = {
        0x0207, 0x0208, 0x0096, 0x0097, 0x00E3, 0x00E4, 0x00E5, 0x00E6,
        0x00E7, 0x00F5, 0x00D9, 0x00DB, 0x00DC, 0x00DD, 0x009F, 0x00A3,
        0x00B7, 0x00BB, 0x00B2, 0x00CA, 0x0198, 0x01B0, 0x01AD, 0x00FF,
        0x0100, 0x0199, 0x01A6, 0x01AC, 0x01A7, 0x0030,
    };
    for (size_t i = 0; i < sizeof priv / sizeof priv[0]; i++)
        if (priv[i] == reg)
            return true;
    return false;
}

static void fv_write(struct fv *f, uint16_t reg, uint8_t v)
{
    if (fv_is_private_reg(reg))
        f->private_writes++;
    if (reg == 0x0015) {                 /* SYSTEM__INTERRUPT_CLEAR */
        if (v & 0x01)
            f->int_status = 0;
        return;
    }
    if (reg == 0x0018 && (v & 0x01)) {   /* SYSRANGE__START */
        f->starts++;
        f->measuring = true;
        f->meas_left = f->meas_polls;
        f->latched = f->meas_next < f->meas_n ? f->meas[f->meas_next++]
                                              : (struct fv_meas){0, 0};
        return;
    }
    f->regs[reg] = v;
}

/* A started range completes after meas_polls status reads (of either
 * status register), unless stuck: the real device finishes on its own time. */
static void fv_advance(struct fv *f)
{
    if (!f->measuring || f->stuck)
        return;
    if (f->meas_left > 0) {
        f->meas_left--;
        return;
    }
    f->measuring = false;
    f->int_status = 0x04;   /* new sample ready */
}

static uint8_t fv_read(struct fv *f, uint16_t reg)
{
    switch (reg) {
    case 0x004D:                         /* RESULT__RANGE_STATUS */
        fv_advance(f);
        if (f->not_ready_polls > 0) {
            f->not_ready_polls--;
            return (uint8_t)(f->latched.err << 4);
        }
        return (uint8_t)(f->latched.err << 4 | (f->measuring ? 0 : 0x01));
    case 0x004F:                         /* RESULT__INTERRUPT_STATUS_GPIO */
        fv_advance(f);
        return f->int_status;
    case 0x0062:                         /* RESULT__RANGE_VAL */
        if (f->int_status != 0x04)
            f->early_value_reads++;
        return f->latched.mm;
    default:
        return f->regs[reg];
    }
}

static int fv_xfer(void *ctx, struct oc_i2c_msg *m, size_t n)
{
    struct fv *f = ctx;
    f->calls++;
    if ((f->fail_call != 0 && f->calls == f->fail_call))
        return -f->fail_errno;
    if (f->unplugged)
        return -ENXIO;
    if (n < 1 || m[0].len < 2 || (m[0].flags & OC_I2C_M_RD))
        return -EINVAL;   /* anything unexpected is a driver bug: fail loudly */
    uint16_t reg = (uint16_t)(m[0].buf[0] << 8 | m[0].buf[1]);   /* big-endian */
    if (reg >= FV_NREGS)
        return -EINVAL;
    struct fv_rec *r = f->calls <= FV_MAX_LOG ? &f->log[f->calls - 1] : nullptr;

    if (n == 1 && m[0].len == 3) {                                  /* write */
        fv_write(f, reg, m[0].buf[2]);
        if (r != nullptr)
            *r = (struct fv_rec){.n = 1, .addr = m[0].addr, .reg = reg, .value = m[0].buf[2]};
        return 0;
    }
    if (n == 2 && m[0].len == 2 && (m[1].flags & OC_I2C_M_RD) && m[1].len == 1 &&
        m[1].addr == m[0].addr) {                                   /* read */
        m[1].buf[0] = fv_read(f, reg);
        if (r != nullptr)
            *r = (struct fv_rec){.n = 2, .addr = m[0].addr, .reg = reg, .read = true,
                                 .value = m[1].buf[0]};
        return 0;
    }
    return -EINVAL;
}

static int fv_now(void *ctx, uint64_t *out)
{
    struct fv *f = ctx;
    f->now_calls++;
    if (f->now_fail_from != 0 && f->now_calls >= f->now_fail_from)
        return -f->now_errno;
    *out = f->now;
    return 0;
}

static int fv_sleep_until(void *ctx, uint64_t abs_ns)
{
    struct fv *f = ctx;
    if (abs_ns > f->now && abs_ns - f->now > f->max_sleep_ns)
        f->max_sleep_ns = abs_ns - f->now;
    if (!f->frozen && abs_ns > f->now)
        f->now = abs_ns;
    return 0;
}

static void fv_init(struct fv *f)
{
    memset(f, 0, sizeof *f);
    f->model_id = 0xB4;
    fv_power_on(f);
    f->now = 1000000;   /* arbitrary non-zero start */
}

static void fv_bus(struct fv *f, struct oc_i2c_bus *bus, struct oc_clock *clk)
{
    *bus = (struct oc_i2c_bus){.xfer = fv_xfer, .ctx = f, .origin = OC_ORIGIN_SIMULATED};
    *clk = (struct oc_clock){.now_ns = fv_now, .sleep_until_ns = fv_sleep_until, .ctx = f};
}

static void fv_push(struct fv *f, uint8_t mm, uint8_t err)
{
    if (f->meas_n < FV_MAX_MEAS)
        f->meas[f->meas_n++] = (struct fv_meas){mm, err};
}

#endif
