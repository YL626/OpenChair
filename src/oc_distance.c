#include "oc_distance.h"

#include <errno.h>

int oc_distance_init(struct oc_distance *dist, const struct oc_i2c_bus *bus,
                     const struct oc_clock *clk, const struct oc_vl6180x_cfg *cfg)
{
    if (dist == nullptr)
        return -EINVAL;
    struct oc_vl6180x dev;
    int rc = oc_vl6180x_init(&dev, bus, clk, cfg);
    if (rc != 0)
        return rc;
    *dist = (struct oc_distance){.dev = dev, .seq = 0, .origin = bus->origin,
                                 .set_up = false, .stats = {}};
    return 0;
}

uint8_t oc_distance_status_for_errno(int err)
{
    switch (err) {
    case -ENXIO:
    case -EREMOTEIO:
        return OC_ST_NACK;
    case -ETIMEDOUT:
        return OC_ST_TIMEOUT;
    default:
        return OC_ST_IO_ERR;
    }
}

/* Brings the device to a state where a range can be trusted: runs setup if
 * it is needed, or if the chip reset since the last one. */
static int ensure_set_up(struct oc_distance *dist)
{
    if (dist->set_up) {
        bool fresh = false;
        int rc = oc_vl6180x_fresh_out_of_reset(&dist->dev, &fresh);
        if (rc != 0)
            return rc;
        if (!fresh)
            return 0;
        dist->stats.silent_resets++;
        dist->set_up = false;
    }
    dist->stats.setups++;
    int rc = oc_vl6180x_setup(&dist->dev);
    if (rc != 0) {
        dist->stats.setup_failures++;
        return rc;
    }
    dist->set_up = true;
    return 0;
}

int oc_distance_sample(struct oc_distance *dist, struct oc_sample *out, int *io_errno)
{
    if (dist == nullptr || out == nullptr)
        return -EINVAL;

    const uint32_t seq = dist->seq++;   /* one per attempt, success or not */
    dist->stats.attempts++;
    int err = ensure_set_up(dist);
    uint8_t mm = 0, range_status = 0;
    if (err == 0)
        err = oc_vl6180x_range(&dist->dev, &mm, &range_status);
    uint8_t code = 0;
    if (err != 0) {
        dist->set_up = false;   /* after any failure, set the device up again */
        dist->stats.io_failures++;
    } else {
        code = oc_vl6180x_range_error(range_status);
        dist->stats.range_errors[code]++;
        if (oc_vl6180x_range_error_is_system(code)) {
            dist->stats.system_errors++;
            dist->set_up = false;   /* the chip reported its own fault */
        }
    }

    uint64_t t;
    int crc = dist->dev.clk.now_ns(dist->dev.clk.ctx, &t);   /* after the attempt */
    if (io_errno != nullptr)
        *io_errno = err;   /* keep the attempt's errno even if the clock failed */
    if (crc != 0)
        return crc;

    uint8_t status = OC_ST_OK;
    int32_t value = mm;
    if (err != 0) {
        status = oc_distance_status_for_errno(err);
        value = 0;
    } else if (code != OC_VL6180X_ERR_NONE) {
        status = OC_ST_RANGE_ERR;
        value = 0;
    }

    *out = (struct oc_sample){
        .t_mono_ns = t,
        .seq = seq,
        .source = OC_SOURCE_DISTANCE,
        .origin = dist->origin,
        .status = status,
        .value = value,
        .aux = err == 0 ? range_status : 0,
    };
    return 0;
}

int oc_distance_get_stats(const struct oc_distance *dist, struct oc_distance_stats *out)
{
    if (dist == nullptr || out == nullptr)
        return -EINVAL;
    *out = dist->stats;
    return 0;
}
