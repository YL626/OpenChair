#ifndef OC_READER_BACKEND_H
#define OC_READER_BACKEND_H

/* oc_reader_backend: where a reader process gets its samples (M3-B).
 *
 * One backend per reader process, created before the first IPC connection and
 * closed only at process exit, so a lost and re-established hub connection
 * never re-initializes the sensor adapter or the I2C descriptor.
 *
 *   SIM   oc_sim_reading: synthetic readings, origin SIMULATED.
 *   LIVE  oc_light / oc_distance over the Linux I2C transport (oc_i2c_linux),
 *         origin LIVE (set by the transport, copied by the adapter).
 *   BUS   the same adapters over a caller-supplied oc_i2c_bus (host tests with
 *         a fake bus). Its origin is whatever the bus says; a fake is
 *         SIMULATED, so this path is never LIVE evidence.
 *
 * ---- Sequence ----
 * The backend owns the sample sequence. The adapters (oc_light, oc_distance)
 * already own theirs, so the SIM backend keeps its own counter and the reader
 * keeps none: every acquire consumes exactly one seq, succeeded or failed.
 * The reader reports oc_reader_backend_next_seq.
 *
 * ---- Errors ----
 * oc_reader_backend_acquire returns 0 even when the sensor attempt failed (the
 * failure is the sample's status, value 0; nothing is fabricated and a LIVE
 * backend never turns into SIM). It returns a negative errno, and leaves *out
 * untouched, only when the clock failed (a sample without a timestamp is not
 * fabricated); the reader treats that as fatal.
 *
 * ---- Ownership ----
 * struct oc_reader_backend owns the I2C descriptor in LIVE mode. The adapters
 * hold a pointer to it, so a LIVE backend must not be copied or moved after
 * oc_reader_backend_open. oc_reader_backend_close is idempotent. A bus passed
 * to _open_bus and the clock are caller-owned and must outlive the backend.
 * Not thread-safe. */

#include <stdbool.h>
#include <stdint.h>

#include "oc_clock.h"
#include "oc_distance.h"
#include "oc_i2c.h"
#include "oc_light.h"
#include "oc_role.h"
#include "oc_sample.h"

enum oc_reader_kind {
    OC_READER_CLOSED = 0,
    OC_READER_SIM,
    OC_READER_ADAPTER      /* LIVE (owns i2c) or an injected bus (does not) */
};

struct oc_reader_backend {
    enum oc_reader_kind kind;
    uint8_t source;        /* OC_SOURCE_LIGHT / OC_SOURCE_DISTANCE */
    uint8_t origin;        /* what HELLO and every sample carry */
    struct oc_clock clk;
    uint32_t sim_seq;      /* SIM only */
    uint32_t fault_every;  /* SIM only */
    struct oc_light light;
    struct oc_distance dist;
    struct oc_i2c_linux i2c;   /* fd >= 0 only for LIVE */
};

/* Backend for o->role from the parsed options: SIM for --input sim, LIVE
 * (opens o->i2c_dev) for --input live. Errors: -EINVAL (bad options),
 * -errno from the open or the adapter init; nothing stays open on error. */
int oc_reader_backend_open(struct oc_reader_backend *b, const struct oc_role_opts *o,
                           const struct oc_clock *clk);

/* Adapter backend over an injected bus. source is OC_SOURCE_LIGHT or _DISTANCE. */
int oc_reader_backend_open_bus(struct oc_reader_backend *b, uint8_t source,
                               const struct oc_i2c_bus *bus, const struct oc_clock *clk);

/* One sample attempt; see Errors above. */
int oc_reader_backend_acquire(struct oc_reader_backend *b, struct oc_sample *out, int *io_errno);

/* seq the next attempt will use. */
uint32_t oc_reader_backend_next_seq(const struct oc_reader_backend *b);

void oc_reader_backend_close(struct oc_reader_backend *b);

#endif
