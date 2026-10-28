#ifndef OC_ADS1115_H
#define OC_ADS1115_H

/* oc_ads1115: hardware-independent ADS1115 driver, channel A0 vs GND,
 * single-shot conversions only.
 *
 * STATUS: gain and rate are configurable STARTING DEFAULTS, not verified
 * hardware settings. Nothing here is measured on a Pi.
 *
 * Single-shot: every oc_ads1115_read starts one new conversion, waits for the
 * OS bit (config register bit 15) to report completion, then reads the
 * result. Every returned value is therefore a fresh conversion; duplicates
 * cannot occur. (Continuous mode has no data-ready flag without the ALERT/RDY
 * pin, so it is deliberately not offered.) The comparator is disabled.
 *
 * Register values are big-endian on the wire. Register reads are one combined
 * transaction (pointer write + read).
 *
 * Errors: 0 or a negative errno. Bus errors pass through unchanged
 * (-ENXIO/-EREMOTEIO NACK, -ETIMEDOUT, ...). -ETIMEDOUT is also returned when
 * conversion does not complete within oc_ads1115_timeout_ns(). -EPROTO means the
 * device answered but its configuration read back differently from what was
 * written. *raw is written only on success.
 *
 * Bounds: the wait loop is bounded by time AND by a poll-count cap, so a
 * clock that does not advance still terminates. Sleeps never exceed the
 * remaining deadline. Interrupted sleeps are the clock's job (oc_clock.h).
 *
 * Ownership: struct oc_ads1115 copies the bus and clock descriptors; it owns
 * no resources. The bus ctx and clock ctx must outlive it. Not thread-safe. */

#include <stdbool.h>
#include <stdint.h>

#include "oc_clock.h"
#include "oc_i2c.h"

#define OC_ADS1115_ADDR_DEFAULT 0x48u
#define OC_ADS1115_ADDR_MIN 0x48u    /* ADDR pin: GND, VDD, SDA, SCL */
#define OC_ADS1115_ADDR_MAX 0x4Bu

enum oc_ads1115_pga {            /* full-scale range, config bits 11:9 */
    OC_ADS1115_PGA_6V144 = 0,
    OC_ADS1115_PGA_4V096 = 1,
    OC_ADS1115_PGA_2V048 = 2,
    OC_ADS1115_PGA_1V024 = 3,
    OC_ADS1115_PGA_0V512 = 4,
    OC_ADS1115_PGA_0V256 = 5
};

enum oc_ads1115_rate {           /* data rate, config bits 7:5 */
    OC_ADS1115_RATE_8SPS = 0,
    OC_ADS1115_RATE_16SPS = 1,
    OC_ADS1115_RATE_32SPS = 2,
    OC_ADS1115_RATE_64SPS = 3,
    OC_ADS1115_RATE_128SPS = 4,
    OC_ADS1115_RATE_250SPS = 5,
    OC_ADS1115_RATE_475SPS = 6,
    OC_ADS1115_RATE_860SPS = 7
};

struct oc_ads1115_cfg {
    uint8_t addr;   /* 0x48..0x4B */
    uint8_t pga;    /* enum oc_ads1115_pga */
    uint8_t rate;   /* enum oc_ads1115_rate */
};

/* Starting defaults: 0x48, +/-4.096 V, 128 SPS. Not verified on hardware. */
#define OC_ADS1115_CFG_DEFAULT                                              \
    { .addr = OC_ADS1115_ADDR_DEFAULT, .pga = OC_ADS1115_PGA_4V096,         \
      .rate = OC_ADS1115_RATE_128SPS }

struct oc_ads1115 {
    struct oc_i2c_bus bus;
    struct oc_clock clk;
    struct oc_ads1115_cfg cfg;
    uint16_t start_word;   /* config word written to start a conversion (OS=1) */
    uint16_t idle_word;    /* the same with OS=0; identifies the setting */
};

/* Samples per second for a rate code; 0 if the code is invalid. */
uint32_t oc_ads1115_rate_sps(uint8_t rate);

/* Longest time one read waits for a conversion (2 x conversion time + a fixed
 * margin; the factor and margin are unmeasured starting values). 0 if the
 * rate code is invalid. */
uint64_t oc_ads1115_timeout_ns(uint8_t rate);

/* Builds the config word: AIN0/GND, chosen PGA, single-shot, chosen rate,
 * comparator disabled; OS bit set when start is true. Defaults give 0xC383
 * (start) and 0x4383 (idle). -EINVAL for an invalid cfg or NULL. */
int oc_ads1115_config_word(const struct oc_ads1115_cfg *cfg, bool start,
                           uint16_t *out);

/* Validates everything; touches no hardware. -EINVAL for NULL, an invalid
 * config, a bus without xfer, an origin that is not LIVE/SIMULATED/REPLAYED,
 * or a clock without both functions. */
int oc_ads1115_init(struct oc_ads1115 *d, const struct oc_i2c_bus *bus,
                    const struct oc_clock *clk, const struct oc_ads1115_cfg *cfg);

/* One fresh conversion. *raw is the signed 16-bit code. */
int oc_ads1115_read(struct oc_ads1115 *d, int16_t *raw);

#endif
