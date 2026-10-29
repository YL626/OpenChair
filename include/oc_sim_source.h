#ifndef OC_SIM_SOURCE_H
#define OC_SIM_SOURCE_H

/* oc_sim_source: deterministic SYNTHETIC readings for the S3C simulated
 * reader processes. Pure function of (source, seq, fault_every): no clock, no
 * I/O, no randomness, no hardware. Nothing produced here is a measurement of
 * anything; every sample built from it carries origin SIMULATED.
 *
 *   LIGHT     a triangle wave of positive counts (about 1000..7400)
 *   DISTANCE  about 40 mm with a small deterministic wobble (38..42)
 *   fault_every = N > 0: attempt seq with (seq % N) == N - 1 is reported as
 *             OC_ST_TIMEOUT with value 0 (a failed attempt still consumes its
 *             seq, as for a real reader). N == 0 disables faults.
 *
 * Returns 0, or -EINVAL for an unknown source (out is left unchanged). The
 * result satisfies oc_sample_validate once combined with a timestamp, seq,
 * source and origin by the caller. */
#include <stdint.h>

struct oc_sim_reading {
    uint8_t status;     /* enum oc_sample_status: OK or TIMEOUT */
    int32_t value;      /* 0 unless status is OK */
    uint32_t aux;       /* always 0 in simulation */
};

int oc_sim_reading(uint8_t source, uint32_t seq, uint32_t fault_every,
                   struct oc_sim_reading *out);

#endif
