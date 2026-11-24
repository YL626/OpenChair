#ifndef OC_VL6180X_H
#define OC_VL6180X_H

/* oc_vl6180x: hardware-independent VL6180X time-of-flight driver, single-shot
 * ranging only (ALS is not used).
 *
 * STATUS: register addresses and the 30 AN4545 private settings were
 * cross-checked against Adafruit_VL6180X (identical), the Linux kernel's
 * vl6180 IIO driver and Pololu's vl6180x-arduino (same addresses). Still
 * [REVIEW] against the ST datasheet itself: Pololu writes 0x0E3=0x01 and
 * 0x0E4=0x03 where Adafruit and this driver write 0x00 and 0x04; the meaning
 * of range error codes 1-5 beyond "system error"; the offset encoding. Timing
 * values are [MEASURE]. Host tests use a SIMULATED register model
 * (tests/fake_vl6180x.h); docs/vl6180x.md has the live procedure.
 *
 * Wire format: registers have 16-bit addresses, sent high byte first, and
 * 8-bit values. A register read is one combined transaction (address write +
 * read with a repeated start), like the Linux kernel's vl6180 driver.
 *
 * Setup (oc_vl6180x_setup): model-ID gate (nothing is written to anything
 * that is not a VL6180X), the private settings when fresh out of reset, then
 * every ranging setting this driver relies on is written explicitly and read
 * back. A readback mismatch is -EPROTO: the device did not take the setting,
 * so its results could not be trusted.
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
 * model ID is not 0xB4 or a setting does not read back. A range that the
 * device itself flags as invalid (no target, overflow, ...) is NOT an error
 * here: it returns 0 with the error code in bits 7:4 of *range_status, and
 * the caller decides.
 *
 * Bounds: every wait is bounded by time AND by a poll-count cap, so a clock
 * that does not advance still terminates. Sleeps never exceed the deadline.
 * A transfer blocked inside the kernel cannot be interrupted (see oc_i2c.h).
 *
 * Ownership: struct oc_vl6180x copies the bus and clock descriptors and owns
 * no resources. The bus ctx and clock ctx must outlive it. Not thread-safe. */

#include <stddef.h>
#include <stdint.h>

#include "oc_clock.h"
#include "oc_i2c.h"

#define OC_VL6180X_ADDR_DEFAULT 0x29u   /* fixed at power-up; see CLAUDE.md bus map */
#define OC_VL6180X_MODEL_ID 0xB4u

/* SYSRANGE__MAX_CONVERGENCE_TIME, written by setup (the power-on default,
 * now guaranteed rather than assumed). A range with no target gives up after
 * this long. */
#define OC_VL6180X_MAX_CONVERGENCE_MS 49u
/* Readout averaging: 1.3 ms + 48 x 64.5 us = 4.4 ms (register value 0x30). [REVIEW] */
#define OC_VL6180X_AVERAGING_NS 4400000ull

/* Deadline for one measurement, from the ready wait to the result.
 * Convergence + averaging is about 53.4 ms; 100 ms leaves margin for
 * scheduling and the bus. [MEASURE] */
#define OC_VL6180X_TIMEOUT_NS 100000000ull
#define OC_VL6180X_POLL_NS 1000000ull   /* status poll interval [MEASURE] */

static_assert(OC_VL6180X_TIMEOUT_NS >
                  OC_VL6180X_MAX_CONVERGENCE_MS * 1000000ull + OC_VL6180X_AVERAGING_NS,
              "the deadline must cover a full convergence plus readout averaging");

/* Range error codes: bits 7:4 of RESULT__RANGE_STATUS. Names follow
 * Adafruit_VL6180X. 1-5 are device system errors (the datasheet attributes
 * them to the VCSEL/PLL; exact meanings [REVIEW]); 6-15 describe the
 * measurement (no target, out of range, ...). 9 and 10 are not defined. */
enum oc_vl6180x_range_err : uint8_t {
    OC_VL6180X_ERR_NONE = 0,
    OC_VL6180X_ERR_SYSERR_1 = 1,
    OC_VL6180X_ERR_SYSERR_2 = 2,
    OC_VL6180X_ERR_SYSERR_3 = 3,
    OC_VL6180X_ERR_SYSERR_4 = 4,
    OC_VL6180X_ERR_SYSERR_5 = 5,
    OC_VL6180X_ERR_ECE_FAIL = 6,       /* early convergence estimate failed */
    OC_VL6180X_ERR_NO_CONVERGE = 7,    /* max convergence time reached */
    OC_VL6180X_ERR_RANGE_IGNORE = 8,   /* below the ignore threshold */
    OC_VL6180X_ERR_SNR = 11,           /* ambient too high / signal too weak */
    OC_VL6180X_ERR_RAW_UFLOW = 12,
    OC_VL6180X_ERR_RAW_OFLOW = 13,
    OC_VL6180X_ERR_RANGE_UFLOW = 14,   /* target too close */
    OC_VL6180X_ERR_RANGE_OFLOW = 15,   /* target too far / nothing in range */
};
#define OC_VL6180X_RANGE_OK OC_VL6180X_ERR_NONE

/* SYSRANGE__PART_TO_PART_RANGE_OFFSET handling. FACTORY leaves the value the
 * part was calibrated with and writes nothing; SET writes offset_mm (two's
 * complement, [REVIEW] against the datasheet) and verifies it by readback. */
enum oc_vl6180x_offset_mode : uint8_t {
    OC_VL6180X_OFFSET_FACTORY = 0,
    OC_VL6180X_OFFSET_SET = 1,
};

struct oc_vl6180x_cfg {
    uint8_t addr;          /* 7-bit, 0x08..0x77 */
    uint8_t offset_mode;   /* enum oc_vl6180x_offset_mode */
    int8_t offset_mm;      /* used only with OC_VL6180X_OFFSET_SET */
};

/* Factory offset: identical behaviour to a configuration without calibration. */
#define OC_VL6180X_CFG_DEFAULT \
    { .addr = OC_VL6180X_ADDR_DEFAULT, .offset_mode = OC_VL6180X_OFFSET_FACTORY, .offset_mm = 0 }

struct oc_vl6180x {
    struct oc_i2c_bus bus;
    struct oc_clock clk;
    struct oc_vl6180x_cfg cfg;
};

/* Validates everything; touches no hardware. -EINVAL for NULL, an invalid
 * address or offset mode, a bus without xfer, an origin that is not
 * LIVE/SIMULATED/REPLAYED, or a clock without both functions. */
int oc_vl6180x_init(struct oc_vl6180x *d, const struct oc_i2c_bus *bus,
                    const struct oc_clock *clk, const struct oc_vl6180x_cfg *cfg);

/* Brings the device up: checks the model ID, loads ST's mandatory settings if
 * the device is fresh out of reset (then clears that flag), writes and reads
 * back every ranging setting this driver relies on, and applies the offset
 * configuration. Safe to call again, e.g. after the sensor was power-cycled. */
int oc_vl6180x_setup(struct oc_vl6180x *d);

/* Reads SYSTEM__FRESH_OUT_OF_RESET. *fresh is true when the device has reset
 * since the last successful setup (its settings are gone). One bus read.
 * *fresh is written only on success. */
int oc_vl6180x_fresh_out_of_reset(struct oc_vl6180x *d, bool *fresh);

/* One fresh single-shot range. On 0, *mm is the range in millimetres (only
 * meaningful when the error code is OC_VL6180X_ERR_NONE) and *range_status is
 * the raw RESULT__RANGE_STATUS byte. Neither is written on error. */
int oc_vl6180x_range(struct oc_vl6180x *d, uint8_t *mm, uint8_t *range_status);

/* Reads the current part-to-part range offset (two's complement mm). */
int oc_vl6180x_read_offset(struct oc_vl6180x *d, int8_t *offset_mm);

/* The error code in a RESULT__RANGE_STATUS byte. */
static inline uint8_t oc_vl6180x_range_error(uint8_t range_status)
{
    return (uint8_t)(range_status >> 4);
}

/* True for codes 1-5: the device reports a fault of its own, not a property
 * of the scene. The adapter re-runs setup after one. */
static inline bool oc_vl6180x_range_error_is_system(uint8_t code)
{
    return code >= OC_VL6180X_ERR_SYSERR_1 && code <= OC_VL6180X_ERR_SYSERR_5;
}

/* Stable upper-case name for a range error code ("NONE", "SNR", ...);
 * "UNDEFINED_9"/"UNDEFINED_10" for the unused codes, "?" above 15. */
const char *oc_vl6180x_range_error_name(uint8_t code);

/* ---- offset calibration (pure computation; no hardware access) ----
 *
 * Procedure (docs/vl6180x.md): hold a flat target at a known distance, take
 * at least OC_VL6180X_CAL_MIN_SAMPLES valid ranges with the current offset in
 * place, then compute offset = current + (known - mean measured). The device
 * applies the offset before reporting RESULT__RANGE_VAL, which is why the
 * current offset is part of the formula. */
#define OC_VL6180X_CAL_MIN_SAMPLES 10u
#define OC_VL6180X_CAL_MAX_SPREAD_MM 10u   /* noisier than this: the target moved [MEASURE] */

struct oc_vl6180x_offset_fit {
    int8_t suggested_mm;   /* new offset to configure */
    int32_t mean_x10_mm;   /* mean measured range, tenths of a mm, rounded */
    uint8_t spread_mm;     /* max - min of the samples */
    size_t n;
};

/* -EINVAL: NULL pointers or known_mm == 0. -ENODATA: fewer than
 * OC_VL6180X_CAL_MIN_SAMPLES samples. -EDOM: spread above
 * OC_VL6180X_CAL_MAX_SPREAD_MM (the fit is still filled in for reporting).
 * -ERANGE: the suggested offset does not fit in int8 (fit filled in, with
 * suggested_mm 0). Only valid (error code 0) ranges may be passed. */
int oc_vl6180x_offset_suggest(uint8_t known_mm, const uint8_t *mm, size_t n,
                              int8_t current_mm, struct oc_vl6180x_offset_fit *fit);

#endif
