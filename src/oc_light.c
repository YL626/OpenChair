#include "oc_light.h"

#include <errno.h>
#include <limits.h>

int oc_light_init(struct oc_light *l, const struct oc_i2c_bus *bus,
                  const struct oc_clock *clk, const struct oc_ads1115_cfg *cfg)
{
    if (l == nullptr)
        return -EINVAL;
    struct oc_ads1115 adc;
    int rc = oc_ads1115_init(&adc, bus, clk, cfg);
    if (rc != 0)
        return rc;
    *l = (struct oc_light){.adc = adc, .seq = 0, .origin = bus->origin};
    return 0;
}

uint8_t oc_light_status_for_errno(int err)
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

int oc_light_sample(struct oc_light *l, struct oc_sample *out, int *io_errno)
{
    if (l == nullptr || out == nullptr)
        return -EINVAL;

    const uint32_t seq = l->seq++;   /* one per attempt, success or not */
    int16_t raw = 0;
    int err = oc_ads1115_read(&l->adc, &raw);

    uint64_t t;
    int crc = l->adc.clk.now_ns(l->adc.clk.ctx, &t);   /* after the attempt */
    if (io_errno != nullptr)
        *io_errno = err;   /* keep the attempt's errno even if the clock failed */
    if (crc != 0)
        return crc;

    uint8_t status = OC_ST_OK;
    int32_t value = raw;
    if (err != 0) {
        status = oc_light_status_for_errno(err);
        value = 0;
    } else if (raw == INT16_MAX || raw == INT16_MIN) {
        status = OC_ST_SATURATED;   /* digital clipping of the ADC code only */
        value = 0;
    }

    *out = (struct oc_sample){
        .t_mono_ns = t,
        .seq = seq,
        .source = OC_SOURCE_LIGHT,
        .origin = l->origin,
        .status = status,
        .value = value,
        .aux = l->adc.idle_word,
    };
    return 0;
}
