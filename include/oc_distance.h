#ifndef OC_DISTANCE_H
#define OC_DISTANCE_H

/* oc_distance: adapts VL6180X ranges to the shared oc_sample contract
 * (source DISTANCE). Raw millimetres only; no placement or stability
 * decisions (those belong to the hub's analysis).
 *
 * One call to oc_distance_sample is one sample attempt and always consumes
 * one seq (0, 1, 2, ... wrapping at 2^32), whether it succeeds or fails.
 * t_mono_ns is CLOCK_MONOTONIC read AFTER the attempt returned. origin is
 * copied from the bus at init, which is the only trusted source.
 *
 * Status mapping (same errno table as oc_light, so the hub reads both
 * sensors' failures the same way):
 *   range valid (error code 0)            -> OC_ST_OK, value = mm (0..255)
 *   device-flagged range error (1..15)    -> OC_ST_RANGE_ERR
 *   -ENXIO, -EREMOTEIO (NACK)             -> OC_ST_NACK
 *   -ETIMEDOUT (not ready / no result)    -> OC_ST_TIMEOUT
 *   any other failure, incl. -EPROTO      -> OC_ST_IO_ERR
 * Every non-OK sample has value 0. aux is the raw RESULT__RANGE_STATUS byte
 * when the device produced one (error code in bits 7:4), otherwise 0. The
 * original negative errno is returned through io_errno.
 *
 * Recovery: the device is set up (oc_vl6180x_setup) on the first attempt and
 * again on the attempt after any failed one, so a sensor that was unplugged
 * and re-plugged (power-cycled, so its settings are gone) is brought back
 * without a restart. That attempt's setup is part of the sample attempt.
 *
 * Ownership: struct oc_distance owns no resources (see oc_vl6180x.h). */

#include <stdint.h>

#include "oc_sample.h"
#include "oc_vl6180x.h"

struct oc_distance {
    struct oc_vl6180x dev;
    uint32_t seq;      /* seq of the next sample */
    uint8_t origin;    /* copied from the bus at init */
    bool set_up;       /* false until a setup succeeds, and after any failure */
};

/* -EINVAL for NULL/invalid arguments (see oc_vl6180x_init). No hardware access. */
int oc_distance_init(struct oc_distance *dist, const struct oc_i2c_bus *bus,
                     const struct oc_clock *clk, const struct oc_vl6180x_cfg *cfg);

/* Errno-to-status table above for a failed attempt (err < 0). */
uint8_t oc_distance_status_for_errno(int err);

/* Performs one attempt and fills *out. Returns 0 even when the attempt failed
 * (the failure is in out->status); *io_errno (may be NULL) gets the original
 * negative errno, or 0. Returns a negative errno and leaves *out untouched
 * only if the clock itself failed, because a sample without a timestamp must
 * not be fabricated; seq is still consumed. */
int oc_distance_sample(struct oc_distance *dist, struct oc_sample *out, int *io_errno);

#endif
