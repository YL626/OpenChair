/* Private to oc_store: the on-disk byte layout of segments, record frames
 * and footers (docs/oc_store.md). Pure encode/decode with no I/O. Every
 * integer is little-endian and is written byte by byte, never by copying a
 * struct, so the layout does not depend on compiler padding or host byte
 * order. Not part of the public API; include only from src/oc_store*.c. */
#ifndef OC_STORE_FORMAT_H
#define OC_STORE_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#include "oc_store.h"

#define OCF_VERSION 1u

#define OCF_SEG_HDR_BYTES 32u   /* segment header */
#define OCF_FRAME_HEAD_BYTES 32u   /* frame header (16) + provenance block (16) */
#define OCF_CRC_BYTES 4u
#define OCF_FRAME_OVERHEAD (OCF_FRAME_HEAD_BYTES + OCF_CRC_BYTES)
#define OCF_FOOTER_BYTES 20u

/* Largest encoded frame: what a buffer must hold to decode any record. */
#define OCF_FRAME_MAX_BYTES (OCF_FRAME_OVERHEAD + OC_STORE_MAX_PAYLOAD)

struct ocf_seg_hdr {
    uint8_t stream;       /* enum oc_stream */
    uint32_t id;          /* equals the NNNNNNNN of the file name */
    uint64_t first_seq;   /* seq of the first record in the segment */
};

/* Written only when a segment is sealed, and only if it holds >= 1 record. */
struct ocf_footer {
    uint32_t count;       /* records in the segment */
    uint64_t last_seq;    /* first_seq + count - 1 */
};

struct ocf_frame_head {
    uint8_t type;         /* enum oc_rec_type */
    uint32_t len;         /* payload bytes */
    uint64_t seq;
    struct oc_prov prov;
};

/* Field checks shared by oc_store_append (input validation) and the frame
 * decoder (on-disk validation), so the store never writes a frame it would
 * later reject, and never accepts one it could not have written. */
bool ocf_type_valid(unsigned type);
bool ocf_prov_valid(const struct oc_prov *prov);

void ocf_put_seg_hdr(unsigned char out[OCF_SEG_HDR_BYTES], const struct ocf_seg_hdr *h);
/* Returns true if the header's magic, version, reserved bytes and CRC are valid. */
bool ocf_get_seg_hdr(const unsigned char in[OCF_SEG_HDR_BYTES], struct ocf_seg_hdr *h);

void ocf_put_footer(unsigned char out[OCF_FOOTER_BYTES], const struct ocf_footer *f);
bool ocf_get_footer(const unsigned char in[OCF_FOOTER_BYTES], struct ocf_footer *f);

/* Encodes a whole frame (OCF_FRAME_OVERHEAD + h->len bytes) into out,
 * including its CRC. payload may be NULL when h->len is 0. */
void ocf_put_frame(unsigned char *out, const struct ocf_frame_head *h, const void *payload);

/* Decodes the first OCF_FRAME_HEAD_BYTES of a frame. Returns true if magic,
 * version, type, length and provenance are all valid; the CRC is checked
 * separately, once the payload has been read. */
bool ocf_get_frame_head(const unsigned char in[OCF_FRAME_HEAD_BYTES], struct ocf_frame_head *h);

/* True if the CRC stored after the payload matches the frame's contents.
 * frame points at the frame's first byte and holds OCF_FRAME_OVERHEAD + len. */
bool ocf_frame_crc_ok(const unsigned char *frame, uint32_t len);

#endif
