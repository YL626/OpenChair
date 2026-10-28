#ifndef OC_LIGHT_H
#define OC_LIGHT_H

/* oc_light: adapts ADS1115 conversions to the shared oc_sample contract
 * (source LIGHT). Raw ADC counts only; no thresholds, no optical decisions.
 *
 * One call to oc_light_sample is one sample attempt and always consumes one
 * seq (0, 1, 2, ... wrapping at 2^32), whether it succeeds or fails.
 * t_mono_ns is CLOCK_MONOTONIC read AFTER the attempt returned (success or
 * failure). origin is copied from the bus, which is the only trusted source.
 *
 * Status mapping:
 *   conversion ok                         -> OC_ST_OK, value = signed code
 *   -ENXIO, -EREMOTEIO (NACK)             -> OC_ST_NACK
 *   -ETIMEDOUT or conversion deadline     -> OC_ST_TIMEOUT
 *   any other failure                     -> OC_ST_IO_ERR (see oc_sample.h:
 *                                            pending shared change)
 *   code is INT16_MAX or INT16_MIN        -> OC_ST_SATURATED
 * The last row is a DIGITAL CLIPPING check on the ADC code only. With
 * VDD = 3.3 V and +/-4.096 V it should not occur on a healthy input; it says
 * nothing about optical saturation of the ALS-PT19, whose thresholds are
 * [MEASURE] and belong to later analysis.
 * Every non-OK sample has value 0. aux is the idle config word (0x4383 for
 * the defaults). The original negative errno is returned through io_errno.
 *
 * Ownership: struct oc_light owns no resources (see oc_ads1115.h). */

#include <stdint.h>

#include "oc_ads1115.h"
#include "oc_sample.h"

struct oc_light {
    struct oc_ads1115 adc;
    uint32_t seq;      /* seq of the next sample */
    uint8_t origin;    /* copied from the bus at init */
};

/* -EINVAL for NULL/invalid arguments (see oc_ads1115_init). No hardware access. */
int oc_light_init(struct oc_light *l, const struct oc_i2c_bus *bus,
                  const struct oc_clock *clk, const struct oc_ads1115_cfg *cfg);

/* Errno-to-status table above for a failed read (err < 0). */
uint8_t oc_light_status_for_errno(int err);

/* Performs one attempt and fills *out. Returns 0 even when the attempt
 * failed (the failure is in out->status); *io_errno (may be NULL) gets the
 * original negative errno, or 0 if there was none. Returns a negative errno
 * and leaves *out untouched only if the clock itself failed (*io_errno still
 * reports the attempt's errno), because a sample
 * without a timestamp must not be fabricated; seq is still consumed. */
int oc_light_sample(struct oc_light *l, struct oc_sample *out, int *io_errno);

#endif
