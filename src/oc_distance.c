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
                                 .set_up = false};
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

int oc_distance_sample(struct oc_distance *dist, struct oc_sample *out, int *io_errno)
{
    if (dist == nullptr || out == nullptr)
        return -EINVAL;

    const uint32_t seq = dist->seq++;   /* one per attempt, success or not */
    int err = 0;
    if (!dist->set_up)
        err = oc_vl6180x_setup(&dist->dev);
    uint8_t mm = 0, range_status = 0;
    if (err == 0)
        err = oc_vl6180x_range(&dist->dev, &mm, &range_status);
    dist->set_up = err == 0;   /* after any failure, set the device up again */

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
    } else if (oc_vl6180x_range_error(range_status) != OC_VL6180X_RANGE_OK) {
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
