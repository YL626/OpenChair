#include "oc_ads1115.h"

#include <errno.h>

#include "oc_store.h"   /* enum oc_origin */

/* Registers (pointer register values). */
#define REG_CONVERSION 0x00u
#define REG_CONFIG 0x01u

/* Config word fields. */
#define CFG_OS 0x8000u          /* write 1: start; read 1: idle / conversion done */
#define CFG_MUX_AIN0_GND 0x4000u   /* 100 */
#define CFG_MODE_SINGLE 0x0100u
#define CFG_COMP_OFF 0x0003u       /* COMP_QUE 11: comparator disabled */

/* Wait policy. Starting values, all [MEASURE]:
 *   first sleep: 3/4 of the nominal conversion time
 *   poll interval: 1/8 of it, never below POLL_MIN_NS
 *   deadline: 2 x conversion time + TIMEOUT_MARGIN_NS (covers the datasheet
 *   clock tolerance and scheduling delay) */
#define POLL_MIN_NS 100000ull
#define TIMEOUT_MARGIN_NS 10000000ull

static const uint32_t rate_sps[8] = {8, 16, 32, 64, 128, 250, 475, 860};

uint32_t oc_ads1115_rate_sps(uint8_t rate)
{
    return rate < 8 ? rate_sps[rate] : 0;
}

static uint64_t conversion_ns(uint8_t rate)
{
    uint32_t sps = oc_ads1115_rate_sps(rate);
    return sps == 0 ? 0 : 1000000000ull / sps;
}

uint64_t oc_ads1115_timeout_ns(uint8_t rate)
{
    uint64_t c = conversion_ns(rate);
    return c == 0 ? 0 : 2 * c + TIMEOUT_MARGIN_NS;
}

static bool origin_ok(uint8_t o)
{
    return o == OC_ORIGIN_LIVE || o == OC_ORIGIN_SIMULATED ||
           o == OC_ORIGIN_REPLAYED;
}

int oc_ads1115_config_word(const struct oc_ads1115_cfg *cfg, bool start,
                           uint16_t *out)
{
    if (cfg == nullptr || out == nullptr)
        return -EINVAL;
    if (cfg->addr < OC_ADS1115_ADDR_MIN || cfg->addr > OC_ADS1115_ADDR_MAX ||
        cfg->pga > OC_ADS1115_PGA_0V256 || cfg->rate > OC_ADS1115_RATE_860SPS)
        return -EINVAL;
    *out = (uint16_t)(CFG_MUX_AIN0_GND | ((unsigned)cfg->pga << 9) |
                      CFG_MODE_SINGLE | ((unsigned)cfg->rate << 5) |
                      CFG_COMP_OFF | (start ? CFG_OS : 0u));
    return 0;
}

int oc_ads1115_init(struct oc_ads1115 *d, const struct oc_i2c_bus *bus,
                    const struct oc_clock *clk, const struct oc_ads1115_cfg *cfg)
{
    if (d == nullptr || bus == nullptr || clk == nullptr || cfg == nullptr)
        return -EINVAL;
    if (bus->xfer == nullptr || !origin_ok(bus->origin) ||
        clk->now_ns == nullptr || clk->sleep_until_ns == nullptr)
        return -EINVAL;

    uint16_t start, idle;
    int rc = oc_ads1115_config_word(cfg, true, &start);
    if (rc == 0)
        rc = oc_ads1115_config_word(cfg, false, &idle);
    if (rc != 0)
        return rc;

    *d = (struct oc_ads1115){.bus = *bus, .clk = *clk, .cfg = *cfg,
                             .start_word = start, .idle_word = idle};
    return 0;
}

static int write_config(const struct oc_ads1115 *d, uint16_t word)
{
    uint8_t buf[3] = {REG_CONFIG, (uint8_t)(word >> 8), (uint8_t)word};
    struct oc_i2c_msg m = {.addr = d->cfg.addr, .flags = 0, .len = 3, .buf = buf};
    return d->bus.xfer(d->bus.ctx, &m, 1);
}

/* Pointer write + 2-byte read as one transaction; value is big-endian. */
static int read_reg(const struct oc_ads1115 *d, uint8_t reg, uint16_t *v)
{
    uint8_t ptr = reg, rx[2] = {0, 0};
    struct oc_i2c_msg m[2] = {
        {.addr = d->cfg.addr, .flags = 0, .len = 1, .buf = &ptr},
        {.addr = d->cfg.addr, .flags = OC_I2C_M_RD, .len = 2, .buf = rx},
    };
    int rc = d->bus.xfer(d->bus.ctx, m, 2);
    if (rc != 0)
        return rc;
    *v = (uint16_t)((rx[0] << 8) | rx[1]);
    return 0;
}

/* Waits until the OS bit reports completion. The config readback (minus OS)
 * must equal what was written; otherwise the device is not what we configured
 * (wrong chip, bus corruption, power reset) and the result is not trusted. */
static int wait_ready(const struct oc_ads1115 *d)
{
    const uint64_t conv = conversion_ns(d->cfg.rate);
    const uint64_t interval = conv / 8 > POLL_MIN_NS ? conv / 8 : POLL_MIN_NS;
    const uint64_t budget = oc_ads1115_timeout_ns(d->cfg.rate);
    const uint64_t max_polls = budget / interval + 2;   /* terminates even if the clock stalls */

    uint64_t start;
    int rc = d->clk.now_ns(d->clk.ctx, &start);
    if (rc != 0)
        return rc;
    const uint64_t deadline = start + budget;
    uint64_t wait = conv * 3 / 4;

    for (uint64_t polls = 0; polls < max_polls; polls++) {
        uint64_t now;
        rc = d->clk.now_ns(d->clk.ctx, &now);
        if (rc != 0)
            return rc;
        if (now < deadline) {
            uint64_t left = deadline - now;
            rc = d->clk.sleep_until_ns(d->clk.ctx, now + (wait < left ? wait : left));
            if (rc != 0)
                return rc;
        }

        uint16_t cfg;
        rc = read_reg(d, REG_CONFIG, &cfg);
        if (rc != 0)
            return rc;
        if (cfg & CFG_OS) {
            return (uint16_t)(cfg & ~CFG_OS) == d->idle_word ? 0 : -EPROTO;
        }

        rc = d->clk.now_ns(d->clk.ctx, &now);
        if (rc != 0)
            return rc;
        if (now >= deadline)
            break;
        wait = interval;
    }
    return -ETIMEDOUT;
}

int oc_ads1115_read(struct oc_ads1115 *d, int16_t *raw)
{
    if (d == nullptr || raw == nullptr)
        return -EINVAL;

    uint64_t unused;   /* a dead clock must stop us before the device is touched */
    int rc = d->clk.now_ns(d->clk.ctx, &unused);
    if (rc == 0)
        rc = write_config(d, d->start_word);
    if (rc == 0)
        rc = wait_ready(d);
    uint16_t v = 0;
    if (rc == 0)
        rc = read_reg(d, REG_CONVERSION, &v);
    if (rc != 0)
        return rc;
    *raw = (int16_t)v;
    return 0;
}
