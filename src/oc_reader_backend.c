#include "oc_reader_backend.h"

#include <errno.h>
#include <string.h>

#include "oc_ads1115.h"
#include "oc_sim_source.h"
#include "oc_store.h"   /* enum oc_origin */
#include "oc_vl6180x.h"

static void reset(struct oc_reader_backend *b)
{
    memset(b, 0, sizeof *b);
    b->kind = OC_READER_CLOSED;
    b->i2c.fd = -1;
}

static int init_adapter(struct oc_reader_backend *b, uint8_t source, const struct oc_i2c_bus *bus)
{
    int rc;
    if (source == OC_SOURCE_LIGHT) {
        const struct oc_ads1115_cfg cfg = OC_ADS1115_CFG_DEFAULT;
        rc = oc_light_init(&b->light, bus, &b->clk, &cfg);
    } else {
        const struct oc_vl6180x_cfg cfg = OC_VL6180X_CFG_DEFAULT;
        rc = oc_distance_init(&b->dist, bus, &b->clk, &cfg);
    }
    if (rc != 0)
        return rc;
    b->kind = OC_READER_ADAPTER;
    b->source = source;
    b->origin = bus->origin;
    return 0;
}

int oc_reader_backend_open_bus(struct oc_reader_backend *b, uint8_t source,
                               const struct oc_i2c_bus *bus, const struct oc_clock *clk)
{
    if (b == nullptr || bus == nullptr || clk == nullptr ||
        (source != OC_SOURCE_LIGHT && source != OC_SOURCE_DISTANCE))
        return -EINVAL;
    reset(b);
    b->clk = *clk;
    int rc = init_adapter(b, source, bus);
    if (rc != 0)
        reset(b);
    return rc;
}

int oc_reader_backend_open(struct oc_reader_backend *b, const struct oc_role_opts *o,
                           const struct oc_clock *clk)
{
    if (b == nullptr || o == nullptr || clk == nullptr ||
        (o->role != OC_ROLE_LIGHT && o->role != OC_ROLE_DISTANCE))
        return -EINVAL;
    const uint8_t source = o->role == OC_ROLE_LIGHT ? OC_SOURCE_LIGHT : OC_SOURCE_DISTANCE;
    reset(b);
    b->clk = *clk;
    if (o->input == OC_INPUT_SIM) {
        b->kind = OC_READER_SIM;
        b->source = source;
        b->origin = OC_ORIGIN_SIMULATED;
        b->fault_every = o->fault_every;
        return 0;
    }
    if (o->input != OC_INPUT_LIVE || o->i2c_dev == nullptr)
        return -EINVAL;
    struct oc_i2c_bus bus;
    int rc = oc_i2c_linux_open(&b->i2c, o->i2c_dev, &bus);   /* bus.ctx = &b->i2c */
    if (rc != 0) {
        reset(b);
        return rc;
    }
    rc = init_adapter(b, source, &bus);
    if (rc != 0) {
        oc_i2c_linux_close(&b->i2c);
        reset(b);
    }
    return rc;
}

static int acquire_sim(struct oc_reader_backend *b, struct oc_sample *out, int *io_errno)
{
    struct oc_sim_reading rd;
    const uint32_t seq = b->sim_seq;
    if (io_errno != nullptr)
        *io_errno = 0;
    b->sim_seq++;                            /* every attempt consumes a seq */
    int rc = oc_sim_reading(b->source, seq, b->fault_every, &rd);
    if (rc != 0)
        return rc;
    uint64_t t = 0;
    rc = b->clk.now_ns(b->clk.ctx, &t);      /* after the "acquisition" */
    if (rc != 0)
        return rc;
    *out = (struct oc_sample){.t_mono_ns = t, .seq = seq, .source = b->source,
                              .origin = b->origin, .status = rd.status, .value = rd.value,
                              .aux = rd.aux};
    return 0;
}

int oc_reader_backend_acquire(struct oc_reader_backend *b, struct oc_sample *out, int *io_errno)
{
    if (b == nullptr || out == nullptr)
        return -EINVAL;
    switch (b->kind) {
    case OC_READER_SIM:
        return acquire_sim(b, out, io_errno);
    case OC_READER_ADAPTER:
        return b->source == OC_SOURCE_LIGHT ? oc_light_sample(&b->light, out, io_errno)
                                            : oc_distance_sample(&b->dist, out, io_errno);
    case OC_READER_CLOSED:
        break;
    }
    return -EINVAL;
}

uint32_t oc_reader_backend_next_seq(const struct oc_reader_backend *b)
{
    switch (b->kind) {
    case OC_READER_SIM: return b->sim_seq;
    case OC_READER_ADAPTER: return b->source == OC_SOURCE_LIGHT ? b->light.seq : b->dist.seq;
    case OC_READER_CLOSED: break;
    }
    return 0;
}

void oc_reader_backend_close(struct oc_reader_backend *b)
{
    if (b == nullptr || b->kind == OC_READER_CLOSED)
        return;                       /* never opened (possibly zeroed: fd 0 is not ours) or closed */
    oc_i2c_linux_close(&b->i2c);
    reset(b);
}
