#include "oc_sim_source.h"

#include <errno.h>

#include "oc_sample.h"

int oc_sim_reading(uint8_t source, uint32_t seq, uint32_t fault_every,
                   struct oc_sim_reading *out)
{
    if (out == nullptr || (source != OC_SOURCE_LIGHT && source != OC_SOURCE_DISTANCE))
        return -EINVAL;

    struct oc_sim_reading r = {.status = OC_ST_OK, .value = 0, .aux = 0};
    if (fault_every > 0 && seq % fault_every == fault_every - 1) {
        r.status = OC_ST_TIMEOUT;               /* failed attempt: value stays 0 */
        *out = r;
        return 0;
    }
    if (source == OC_SOURCE_LIGHT) {
        uint32_t phase = seq % 200u;            /* triangle: 0..100..0 */
        uint32_t tri = phase < 100u ? phase : 200u - phase;
        r.value = (int32_t)(1000u + tri * 64u);
    } else {
        static const int8_t wobble[5] = {0, 2, -1, 1, -2};
        r.value = 40 + wobble[seq % 5u];
    }
    *out = r;
    return 0;
}
