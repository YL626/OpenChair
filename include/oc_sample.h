#ifndef OC_SAMPLE_H
#define OC_SAMPLE_H

/* oc_sample: shared sensor-sample record and IPC message envelope.
 *
 * STATUS: [REVIEW] follows DESIGN.md "Interface" and docs/IMPLEMENTATION_NOTES.md
 * section 1. The numeric values of enum oc_sample_status and enum oc_msg_type
 * are proposals not fixed by those documents.
 *
 * Pure functions: no I/O, no allocation, no globals, no hardware. Wire bytes
 * are produced and parsed with explicit little-endian shifts; neither
 * sizeof(struct oc_sample) nor its memory layout is ever used on the wire.
 *
 * Errors: every function returns 0 or a negative errno:
 *   -EINVAL            NULL argument, or an invalid value passed to an encoder
 *                      (encoders refuse to emit bytes a decoder would reject)
 *   -EMSGSIZE          buffer too small (encode) or wrong length (decode)
 *   -EPROTONOSUPPORT   envelope version is not OC_PROTO_VERSION; a receiver
 *                      closes the connection on this error
 *   -EBADMSG           decoded bytes are malformed: unknown or illegal enum,
 *                      nonzero reserved byte, bad payload length, or a non-OK
 *                      status with a nonzero value
 * On any error the output buffer or struct is left unchanged, so a failure
 * can never be mistaken for a reading.
 *
 * Ownership: all buffers and structs belong to the caller. Nothing is retained.
 *
 * This layer checks structure and protocol only. Whether a value is plausible
 * for the sensor (range, saturation level, stability) is decided by the
 * readers and by analysis, not here.
 *
 * Session identity ((session_id, instance), see Notes section 2) is carried by
 * HELLO and the supervisor, not by the 24-byte sample payload. */

#include <stddef.h>
#include <stdint.h>

#include "oc_store.h"   /* enum oc_origin, shared with storage; valid on a
                         * sample: LIVE, SIMULATED, REPLAYED only */

#define OC_PROTO_VERSION 1u
#define OC_SAMPLE_SIZE 24u        /* payload bytes of a SAMPLE message */
#define OC_MSG_HEADER_SIZE 4u     /* version u8, type u8, payload_len u16 LE */
#define OC_MSG_SAMPLE_SIZE (OC_MSG_HEADER_SIZE + OC_SAMPLE_SIZE)   /* 28 */

enum oc_source {
    OC_SOURCE_LIGHT = 1,
    OC_SOURCE_DISTANCE = 2
};

enum oc_sample_status {
    OC_ST_OK = 1,
    OC_ST_SATURATED = 2,
    OC_ST_NACK = 3,
    OC_ST_TIMEOUT = 4,
    OC_ST_RANGE_ERR = 5,
    OC_ST_STALE = 6
};

enum oc_msg_type {
    OC_MSG_HELLO = 1,
    OC_MSG_HELLO_ACK = 2,
    OC_MSG_HELLO_REJECT = 3,
    OC_MSG_SAMPLE = 4,
    OC_MSG_BYE = 5
};

/* Logical sample. Raw counts only. The wire reserved byte has no member here:
 * it is always written as 0 and must be 0 on decode. Enumerated fields are
 * uint8_t so that an out-of-range wire byte can be represented and rejected. */
struct oc_sample {
    uint64_t t_mono_ns;   /* CLOCK_MONOTONIC when the read returned */
    uint32_t seq;         /* per source and instance, +1 per sample attempt */
    uint8_t source;       /* enum oc_source */
    uint8_t origin;       /* enum oc_origin: LIVE, SIMULATED or REPLAYED */
    uint8_t status;       /* enum oc_sample_status */
    int32_t value;        /* signed counts or millimetres; 0 unless status is OK */
    uint32_t aux;         /* ADS1115 config word or raw VL6180X status code */
};

struct oc_msg_header {
    uint8_t version;
    uint8_t type;           /* enum oc_msg_type */
    uint16_t payload_len;   /* bytes after the header */
};

/* Checks field legality only. Returns 0 or -EINVAL. */
int oc_sample_validate(const struct oc_sample *s);

/* 24-byte payload only. Writes exactly OC_SAMPLE_SIZE bytes. */
int oc_sample_encode(const struct oc_sample *s, uint8_t *out, size_t cap);
int oc_sample_decode(const uint8_t *buf, size_t len, struct oc_sample *out);

/* 4-byte envelope. Accepts version OC_PROTO_VERSION and the five known types.
 * A SAMPLE header must have payload_len == OC_SAMPLE_SIZE (24). Other types
 * accept any length: their payload layouts are not yet defined (issue #16).
 * The decoder requires len == OC_MSG_HEADER_SIZE exactly. */
int oc_msg_header_encode(const struct oc_msg_header *h, uint8_t *out, size_t cap);
int oc_msg_header_decode(const uint8_t *buf, size_t len, struct oc_msg_header *out);

/* Complete SAMPLE datagram: header (01 04 18 00) + 24-byte payload = 28 bytes.
 * Encode sets *n_out to OC_MSG_SAMPLE_SIZE. Decode requires len == 28 exactly
 * (SEQPACKET delivers one message per datagram). */
int oc_msg_sample_encode(const struct oc_sample *s, uint8_t *out, size_t cap,
                         size_t *n_out);
int oc_msg_sample_decode(const uint8_t *buf, size_t len, struct oc_sample *out);

#endif
