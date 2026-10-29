#ifndef OC_VL6180X_H
#define OC_VL6180X_H

/* oc_vl6180x: hardware-independent VL6180X time-of-flight driver, single-shot
 * ranging only (ALS is not used).
 *
 * STATUS: register addresses, the mandatory settings and the timing below
 * come from the ST VL6180X datasheet and application note AN4545 [REVIEW:
 * check against the datasheet]. The deadline is an unmeasured starting value
 * [MEASURE]. Nothing here is verified on a Pi until the live probe has run
 * (docs/vl6180x.md).
 *
 * Wire format: registers have 16-bit addresses, sent high byte first, and
 * 8-bit values. A register read is one combined transaction (address write +
 * read with a repeated start), like the Linux kernel's vl6180 driver.
 *
 * Measurement (oc_vl6180x_range): wait until the device reports ready, clear
 * any pending interrupt, start one range, poll until "new sample ready", read
 * the range (mm) and the range status, clear the interrupt. Clearing BEFORE
 * the start means a result left over from an interrupted attempt can never be
 * returned as this attempt's: every returned value is a fresh measurement.
 *
 * Errors: 0 or a negative errno. Bus errors pass through unchanged
 * (-ENXIO/-EREMOTEIO NACK, ...). -ETIMEDOUT when the device is not ready or
 * the range does not complete within OC_VL6180X_TIMEOUT_NS. -EPROTO when the
 * device answers but its model ID is not 0xB4. A range that the device itself
 * flags as invalid (no target, overflow, ...) is NOT an error here: it returns
 * 0 with the error code in bits 7:4 of *range_status, and the caller decides.
 *
 * Bounds: every wait is bounded by time AND by a poll-count cap, so a clock
 * that does not advance still terminates. Sleeps never exceed the deadline.
 * A transfer blocked inside the kernel cannot be interrupted (see oc_i2c.h).
 *
 * Ownership: struct oc_vl6180x copies the bus and clock descriptors and owns
 * no resources. The bus ctx and clock ctx must outlive it. Not thread-safe. */

#include <stdint.h>

#include "oc_clock.h"
#include "oc_i2c.h"

#define OC_VL6180X_ADDR_DEFAULT 0x29u   /* fixed at power-up; see CLAUDE.md bus map */
#define OC_VL6180X_MODEL_ID 0xB4u

/* Deadline for one measurement, from the ready wait to the result. The
 * default max convergence time is about 49 ms plus readout averaging of about
 * 4.3 ms; 100 ms leaves margin for scheduling. [MEASURE] */
#define OC_VL6180X_TIMEOUT_NS 100000000ull
#define OC_VL6180X_POLL_NS 1000000ull   /* status poll interval [MEASURE] */

/* Range error codes: bits 7:4 of RESULT__RANGE_STATUS (datasheet). 0 means
 * the range is valid; for example 11 is "max signal-to-noise" (often no
 * target) and 15 is "range overflow" (target beyond range). */
#define OC_VL6180X_RANGE_OK 0u

struct oc_vl6180x_cfg {
    uint8_t addr;   /* 7-bit, 0x08..0x77 */
};

#define OC_VL6180X_CFG_DEFAULT { .addr = OC_VL6180X_ADDR_DEFAULT }

struct oc_vl6180x {
    struct oc_i2c_bus bus;
    struct oc_clock clk;
    struct oc_vl6180x_cfg cfg;
};

/* Validates everything; touches no hardware. -EINVAL for NULL, an invalid
 * address, a bus without xfer, an origin that is not LIVE/SIMULATED/REPLAYED,
 * or a clock without both functions. */
int oc_vl6180x_init(struct oc_vl6180x *d, const struct oc_i2c_bus *bus,
                    const struct oc_clock *clk, const struct oc_vl6180x_cfg *cfg);

/* Brings the device up: checks the model ID, loads ST's mandatory settings if
 * the device is fresh out of reset (then clears that flag), and writes the
 * configuration this driver relies on ("new sample ready" interrupt). Safe to
 * call again, e.g. after the sensor was unplugged and power-cycled. */
int oc_vl6180x_setup(struct oc_vl6180x *d);

/* One fresh single-shot range. On 0, *mm is the range in millimetres (only
 * meaningful when the error code is OC_VL6180X_RANGE_OK) and *range_status is
 * the raw RESULT__RANGE_STATUS byte. Neither is written on error. */
int oc_vl6180x_range(struct oc_vl6180x *d, uint8_t *mm, uint8_t *range_status);

/* The error code in a RESULT__RANGE_STATUS byte. */
static inline uint8_t oc_vl6180x_range_error(uint8_t range_status)
{
    return (uint8_t)(range_status >> 4);
}

#endif
