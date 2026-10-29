#ifndef OC_CAPTURE_FMT_H
#define OC_CAPTURE_FMT_H

/* oc_capture_fmt: payload and provenance layout of the sensor samples that the
 * hub stores in OC_STREAM_TRACE as OC_REC_SAMPLES records (M3 capture mode).
 *
 * STATUS: [REVIEW]. Pure functions: no I/O, no allocation, no globals. The
 * layout is the hub's own consumer-side convention on top of oc_store's opaque
 * payload; oc_store itself is not involved and not changed.
 *
 * ---- Payload (little-endian, explicit shifts; never a struct copy) ----
 *     0  u8   fmt     = OC_CAP_FMT_VERSION (1)
 *     1  u8   source  enum oc_source; every sample in the record has it
 *     2  u16  count   1..OC_CAP_MAX_COUNT
 *     4  count x 24 B oc_sample_encode() output (oc_sample.h layout, unchanged)
 * M3-A writes count == 1 (one sample per record); the decoder accepts the
 * batch form so a later change needs no format change.
 *
 * ---- Provenance of the record ----
 *     kind      OC_KIND_SENSOR
 *     origin    the samples' origin (LIVE, SIMULATED or REPLAYED)
 *     emitter   OC_EMIT_LIGHT_READER / OC_EMIT_DISTANCE_READER, matching source
 *     flags     0
 *     instance, session_id   the reader's (HELLO) identity
 *
 * ---- Two different sequence numbers ----
 * oc_store assigns a u64 RECORD seq per stream. Each sample carries its
 * reader's u32 SAMPLE seq (wrapping at 2^32). They are unrelated; this module
 * and the code using it keep them in separate, clearly named variables.
 *
 * Errors: 0 or a negative errno.
 *   -EINVAL    NULL argument, count out of range, mixed sources/origins in an
 *              encode request, or an invalid sample (oc_sample_validate)
 *   -EMSGSIZE  buffer too small (encode) / length != 4 + 24*count (decode)
 *   -EBADMSG   decode: wrong fmt, count 0 or > OC_CAP_MAX_COUNT, undecodable
 *              sample, a sample whose source/origin disagrees with the header
 *              or the provenance, or provenance that is not a sensor record
 *              for that source (kind, emitter, origin)
 * On error the output is left unchanged. */
#include <stddef.h>
#include <stdint.h>

#include "oc_sample.h"
#include "oc_store.h"

#define OC_CAP_FMT_VERSION 1u
#define OC_CAP_HEADER_SIZE 4u
#define OC_CAP_MAX_COUNT 32u
#define OC_CAP_MAX_PAYLOAD (OC_CAP_HEADER_SIZE + OC_CAP_MAX_COUNT * OC_SAMPLE_SIZE)

/* oc_prov.emitter values for sensor-sample records. */
enum oc_capture_emitter {
    OC_EMIT_LIGHT_READER = 1,
    OC_EMIT_DISTANCE_READER = 2
};

struct oc_cap_batch {
    uint8_t source;
    uint16_t count;
    struct oc_sample sample[OC_CAP_MAX_COUNT];
};

/* Emitter for a source, or 0 for an unknown source. */
uint8_t oc_cap_emitter_for(uint8_t source);

/* Provenance for samples of one reader identity (kind SENSOR, flags 0). */
int oc_cap_prov(uint8_t source, uint8_t origin, uint64_t session_id, uint32_t instance,
                struct oc_prov *out);

/* Encodes count samples (all the same source and origin) into out; *n_out is
 * the payload size, 4 + 24*count. */
int oc_cap_encode(const struct oc_sample *s, size_t count, uint8_t *out, size_t cap,
                  size_t *n_out);

/* Decodes and cross-checks one OC_REC_SAMPLES payload against its record
 * provenance. */
int oc_cap_decode(const void *payload, size_t len, const struct oc_prov *prov,
                  struct oc_cap_batch *out);

#endif
