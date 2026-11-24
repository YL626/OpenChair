#include "oc_vl6180x.h"

#include <errno.h>
#include <stdint.h>

#include "oc_store.h"   /* enum oc_origin */

/* Registers (datasheet names). Addresses cross-checked against Adafruit,
 * Linux vl6180 and Pololu (see oc_vl6180x.h). */
#define REG_MODEL_ID 0x0000u                  /* IDENTIFICATION__MODEL_ID */
#define REG_GPIO1_MODE 0x0011u                /* SYSTEM__MODE_GPIO1 */
#define REG_MAX_CONVERGENCE 0x001Cu           /* SYSRANGE__MAX_CONVERGENCE_TIME */
#define REG_PART_TO_PART_OFFSET 0x0024u       /* SYSRANGE__PART_TO_PART_RANGE_OFFSET */
#define REG_RANGE_SCALER_HI 0x0096u           /* RANGE_SCALER (16-bit) */
#define REG_RANGE_SCALER_LO 0x0097u
#define REG_INTERLEAVED_MODE 0x02A3u          /* INTERLEAVED_MODE__ENABLE */
#define REG_INTERRUPT_CONFIG 0x0014u          /* SYSTEM__INTERRUPT_CONFIG_GPIO */
#define REG_INTERRUPT_CLEAR 0x0015u           /* SYSTEM__INTERRUPT_CLEAR */
#define REG_FRESH_OUT_OF_RESET 0x0016u        /* SYSTEM__FRESH_OUT_OF_RESET */
#define REG_RANGE_START 0x0018u               /* SYSRANGE__START */
#define REG_VHV_RECALIBRATE 0x002Eu           /* SYSRANGE__VHV_RECALIBRATE */
#define REG_VHV_REPEAT_RATE 0x0031u           /* SYSRANGE__VHV_REPEAT_RATE */
#define REG_AVERAGING_PERIOD 0x010Au          /* READOUT__AVERAGING_SAMPLE_PERIOD */
#define REG_RANGE_STATUS 0x004Du              /* RESULT__RANGE_STATUS */
#define REG_INTERRUPT_STATUS 0x004Fu          /* RESULT__INTERRUPT_STATUS_GPIO */
#define REG_RANGE_VAL 0x0062u                 /* RESULT__RANGE_VAL */

#define RANGE_STATUS_DEVICE_READY 0x01u
#define INTERRUPT_RANGE_MASK 0x07u
#define INTERRUPT_NEW_SAMPLE_READY 0x04u
#define INTERRUPT_CLEAR_ALL 0x07u
#define RANGE_START_SINGLE_SHOT 0x01u

struct reg_value {
    uint16_t reg;
    uint8_t value;
};

/* Mandatory "private register" settings loaded after a reset, from ST
 * application note AN4545. Identical to Adafruit_VL6180X's loadSettings().
 * Undocumented tuning values; copied, not derived. [REVIEW: Pololu's copy
 * has 0x00E3=0x01 and 0x00E4=0x03; check AN4545 itself.] */
static const struct reg_value private_settings[] = {
    {0x0207, 0x01}, {0x0208, 0x01}, {0x0096, 0x00}, {0x0097, 0xFD},
    {0x00E3, 0x00}, {0x00E4, 0x04}, {0x00E5, 0x02}, {0x00E6, 0x01},
    {0x00E7, 0x03}, {0x00F5, 0x02}, {0x00D9, 0x05}, {0x00DB, 0xCE},
    {0x00DC, 0x03}, {0x00DD, 0xF8}, {0x009F, 0x00}, {0x00A3, 0x3C},
    {0x00B7, 0x00}, {0x00BB, 0x3C}, {0x00B2, 0x09}, {0x00CA, 0x09},
    {0x0198, 0x01}, {0x01B0, 0x17}, {0x01AD, 0x00}, {0x00FF, 0x05},
    {0x0100, 0x05}, {0x0199, 0x05}, {0x01A6, 0x1B}, {0x01AC, 0x3E},
    {0x01A7, 0x1F}, {0x0030, 0x00},
};

/* Ranging settings, written on every setup so behaviour does not depend on
 * whether the device was just reset or on power-on defaults. The first five
 * are AN4545's recommended public settings (ALS ones omitted: the ALS is
 * never used); the rest pin values the driver's timing and 0..255 mm claim
 * depend on (Pololu's configureDefault() writes the same ones). */
static const struct reg_value range_settings[] = {
    {REG_GPIO1_MODE, 0x10},          /* GPIO1: interrupt output, active low */
    {REG_AVERAGING_PERIOD, 0x30},    /* readout averaging: 48 (~4.4 ms) */
    {REG_VHV_REPEAT_RATE, 0xFF},     /* temperature recalibration every 255 ranges */
    {REG_VHV_RECALIBRATE, 0x01},     /* one recalibration now (self-clearing) */
    {REG_INTERRUPT_CONFIG, 0x24},    /* range and ALS: "new sample ready" */
    {REG_MAX_CONVERGENCE, OC_VL6180X_MAX_CONVERGENCE_MS},   /* 0x31 = 49 ms */
    {REG_INTERLEAVED_MODE, 0x00},    /* no interleaved ALS/range */
    {REG_RANGE_SCALER_HI, 0x00},     /* RANGE_SCALER = 253: 1x scaling, */
    {REG_RANGE_SCALER_LO, 0xFD},     /* so RESULT__RANGE_VAL is plain mm */
};

/* Settings read back after setup. VHV_RECALIBRATE is excluded: the device
 * clears it when the recalibration is done. */
static bool verified_after_write(uint16_t reg)
{
    return reg != REG_VHV_RECALIBRATE;
}

static const char *const range_error_names[16] = {
    "NONE",       "SYSERR_1",    "SYSERR_2",  "SYSERR_3",
    "SYSERR_4",   "SYSERR_5",    "ECE_FAIL",  "NO_CONVERGE",
    "RANGE_IGNORE", "UNDEFINED_9", "UNDEFINED_10", "SNR",
    "RAW_UFLOW",  "RAW_OFLOW",   "RANGE_UFLOW", "RANGE_OFLOW",
};

const char *oc_vl6180x_range_error_name(uint8_t code)
{
    return code < 16 ? range_error_names[code] : "?";
}

static bool origin_ok(uint8_t o)
{
    return o == OC_ORIGIN_LIVE || o == OC_ORIGIN_SIMULATED ||
           o == OC_ORIGIN_REPLAYED;
}

int oc_vl6180x_init(struct oc_vl6180x *d, const struct oc_i2c_bus *bus,
                    const struct oc_clock *clk, const struct oc_vl6180x_cfg *cfg)
{
    if (d == nullptr || bus == nullptr || clk == nullptr || cfg == nullptr)
        return -EINVAL;
    if (bus->xfer == nullptr || !origin_ok(bus->origin) ||
        clk->now_ns == nullptr || clk->sleep_until_ns == nullptr ||
        cfg->addr < 0x08 || cfg->addr > 0x77 ||
        (cfg->offset_mode != OC_VL6180X_OFFSET_FACTORY &&
         cfg->offset_mode != OC_VL6180X_OFFSET_SET))
        return -EINVAL;
    *d = (struct oc_vl6180x){.bus = *bus, .clk = *clk, .cfg = *cfg};
    return 0;
}

static int write_reg(const struct oc_vl6180x *d, uint16_t reg, uint8_t value)
{
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)reg, value};
    struct oc_i2c_msg m = {.addr = d->cfg.addr, .flags = 0, .len = 3, .buf = buf};
    return d->bus.xfer(d->bus.ctx, &m, 1);
}

/* Register address write + 1-byte read as one transaction. */
static int read_reg(const struct oc_vl6180x *d, uint16_t reg, uint8_t *value)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)reg};
    uint8_t rx = 0;
    struct oc_i2c_msg m[2] = {
        {.addr = d->cfg.addr, .flags = 0, .len = 2, .buf = addr},
        {.addr = d->cfg.addr, .flags = OC_I2C_M_RD, .len = 1, .buf = &rx},
    };
    int rc = d->bus.xfer(d->bus.ctx, m, 2);
    if (rc != 0)
        return rc;
    *value = rx;
    return 0;
}

static int write_all(const struct oc_vl6180x *d, const struct reg_value *rv, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int rc = write_reg(d, rv[i].reg, rv[i].value);
        if (rc != 0)
            return rc;
    }
    return 0;
}

/* Reads every entry back; -EPROTO on the first value that differs. */
static int verify_all(const struct oc_vl6180x *d, const struct reg_value *rv, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!verified_after_write(rv[i].reg))
            continue;
        uint8_t v = 0;
        int rc = read_reg(d, rv[i].reg, &v);
        if (rc != 0)
            return rc;
        if (v != rv[i].value)
            return -EPROTO;
    }
    return 0;
}

static int apply_offset(const struct oc_vl6180x *d)
{
    if (d->cfg.offset_mode != OC_VL6180X_OFFSET_SET)
        return 0;   /* FACTORY: keep the part's own calibration, write nothing */
    const uint8_t raw = (uint8_t)d->cfg.offset_mm;   /* two's complement [REVIEW] */
    int rc = write_reg(d, REG_PART_TO_PART_OFFSET, raw);
    uint8_t back = 0;
    if (rc == 0)
        rc = read_reg(d, REG_PART_TO_PART_OFFSET, &back);
    if (rc == 0 && back != raw)
        rc = -EPROTO;
    return rc;
}

int oc_vl6180x_setup(struct oc_vl6180x *d)
{
    if (d == nullptr)
        return -EINVAL;
    uint8_t model = 0;
    int rc = read_reg(d, REG_MODEL_ID, &model);
    if (rc != 0)
        return rc;
    if (model != OC_VL6180X_MODEL_ID)
        return -EPROTO;   /* something answered at this address, but not a VL6180X */

    uint8_t fresh = 0;
    rc = read_reg(d, REG_FRESH_OUT_OF_RESET, &fresh);
    if (rc == 0 && (fresh & 0x01u) != 0) {
        rc = write_all(d, private_settings,
                       sizeof private_settings / sizeof private_settings[0]);
        if (rc == 0)
            rc = write_reg(d, REG_FRESH_OUT_OF_RESET, 0x00);
    }
    const size_t n = sizeof range_settings / sizeof range_settings[0];
    if (rc == 0)
        rc = write_all(d, range_settings, n);
    if (rc == 0)
        rc = apply_offset(d);
    if (rc == 0)
        rc = verify_all(d, range_settings, n);
    if (rc == 0) {   /* the fresh flag must stay cleared, or reset detection is blind */
        rc = read_reg(d, REG_FRESH_OUT_OF_RESET, &fresh);
        if (rc == 0 && (fresh & 0x01u) != 0)
            rc = -EPROTO;
    }
    return rc;
}

int oc_vl6180x_fresh_out_of_reset(struct oc_vl6180x *d, bool *fresh)
{
    if (d == nullptr || fresh == nullptr)
        return -EINVAL;
    uint8_t v = 0;
    int rc = read_reg(d, REG_FRESH_OUT_OF_RESET, &v);
    if (rc != 0)
        return rc;
    *fresh = (v & 0x01u) != 0;
    return 0;
}

int oc_vl6180x_read_offset(struct oc_vl6180x *d, int8_t *offset_mm)
{
    if (d == nullptr || offset_mm == nullptr)
        return -EINVAL;
    uint8_t v = 0;
    int rc = read_reg(d, REG_PART_TO_PART_OFFSET, &v);
    if (rc != 0)
        return rc;
    *offset_mm = (int8_t)v;
    return 0;
}

/* Polls reg until (value & mask) == want, or the deadline passes. The poll
 * count is capped as well, so a clock that never advances still ends. */
static int wait_reg(const struct oc_vl6180x *d, uint16_t reg, uint8_t mask,
                    uint8_t want, uint64_t deadline)
{
    const uint64_t max_polls = OC_VL6180X_TIMEOUT_NS / OC_VL6180X_POLL_NS + 2;
    for (uint64_t polls = 0; polls < max_polls; polls++) {
        uint8_t v = 0;
        int rc = read_reg(d, reg, &v);
        if (rc != 0)
            return rc;
        if ((v & mask) == want)
            return 0;

        uint64_t now;
        rc = d->clk.now_ns(d->clk.ctx, &now);
        if (rc != 0)
            return rc;
        if (now >= deadline)
            break;
        uint64_t left = deadline - now;
        rc = d->clk.sleep_until_ns(d->clk.ctx,
                                   now + (OC_VL6180X_POLL_NS < left ? OC_VL6180X_POLL_NS : left));
        if (rc != 0)
            return rc;
    }
    return -ETIMEDOUT;
}

int oc_vl6180x_range(struct oc_vl6180x *d, uint8_t *mm, uint8_t *range_status)
{
    if (d == nullptr || mm == nullptr || range_status == nullptr)
        return -EINVAL;

    uint64_t start;   /* a dead clock must stop us before the device is touched */
    int rc = d->clk.now_ns(d->clk.ctx, &start);
    if (rc != 0)
        return rc;
    const uint64_t deadline = start + OC_VL6180X_TIMEOUT_NS;

    rc = wait_reg(d, REG_RANGE_STATUS, RANGE_STATUS_DEVICE_READY,
                  RANGE_STATUS_DEVICE_READY, deadline);
    if (rc == 0)
        rc = write_reg(d, REG_INTERRUPT_CLEAR, INTERRUPT_CLEAR_ALL);
    if (rc == 0)
        rc = write_reg(d, REG_RANGE_START, RANGE_START_SINGLE_SHOT);
    if (rc == 0)
        rc = wait_reg(d, REG_INTERRUPT_STATUS, INTERRUPT_RANGE_MASK,
                      INTERRUPT_NEW_SAMPLE_READY, deadline);
    uint8_t val = 0, status = 0;
    if (rc == 0)
        rc = read_reg(d, REG_RANGE_VAL, &val);
    if (rc == 0)
        rc = read_reg(d, REG_RANGE_STATUS, &status);
    if (rc == 0)
        rc = write_reg(d, REG_INTERRUPT_CLEAR, INTERRUPT_CLEAR_ALL);
    if (rc != 0)
        return rc;
    *mm = val;
    *range_status = status;
    return 0;
}

int oc_vl6180x_offset_suggest(uint8_t known_mm, const uint8_t *mm, size_t n,
                              int8_t current_mm, struct oc_vl6180x_offset_fit *fit)
{
    if (mm == nullptr || fit == nullptr || known_mm == 0)
        return -EINVAL;
    if (n < OC_VL6180X_CAL_MIN_SAMPLES)
        return -ENODATA;
    uint64_t sum = 0;
    uint8_t lo = mm[0], hi = mm[0];
    for (size_t i = 0; i < n; i++) {
        sum += mm[i];
        if (mm[i] < lo)
            lo = mm[i];
        if (mm[i] > hi)
            hi = mm[i];
    }
    /* mean in tenths, rounded half up (sum <= 255 n, so sum * 10 fits in u64) */
    const int64_t mean_x10 = (int64_t)((sum * 10u + n / 2u) / n);
    const int64_t delta_x10 = (int64_t)known_mm * 10 - mean_x10;
    /* round half away from zero to whole mm */
    const int64_t delta = delta_x10 >= 0 ? (delta_x10 + 5) / 10 : -((-delta_x10 + 5) / 10);
    const int64_t suggested = (int64_t)current_mm + delta;
    *fit = (struct oc_vl6180x_offset_fit){
        .suggested_mm = 0, .mean_x10_mm = (int32_t)mean_x10,
        .spread_mm = (uint8_t)(hi - lo), .n = n,
    };
    if ((unsigned)(hi - lo) > OC_VL6180X_CAL_MAX_SPREAD_MM)
        return -EDOM;
    if (suggested < INT8_MIN || suggested > INT8_MAX)
        return -ERANGE;
    fit->suggested_mm = (int8_t)suggested;
    return 0;
}
