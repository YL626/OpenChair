/* On-disk byte layout for oc_store. See oc_store_format.h and docs/oc_store.md. */
#include <string.h>

#include "oc_crc32.h"
#include "oc_store_format.h"

static const unsigned char seg_magic[4] = {'O', 'C', 'S', 'G'};
static const unsigned char footer_magic[4] = {'O', 'C', 'F', 'T'};
#define FRAME_MAGIC 0x5243u   /* "CR" when read as bytes: 0x43 0x52 */

/* ---------- little-endian helpers ---------- */

static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put_u32(unsigned char *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static void put_u64(unsigned char *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}

static uint16_t get_u16(const unsigned char *p)
{
    return (uint16_t)(p[0] | (unsigned)p[1] << 8);
}

static uint32_t get_u32(const unsigned char *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static uint64_t get_u64(const unsigned char *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

/* ---------- field checks ---------- */

bool ocf_type_valid(unsigned type)
{
    return type >= OC_REC_EVENT && type <= OC_REC_SAMPLES;
}

bool ocf_prov_valid(const struct oc_prov *prov)
{
    return prov->kind >= OC_KIND_SENSOR && prov->kind <= OC_KIND_SYSTEM &&
           prov->origin >= OC_ORIGIN_LIVE && prov->origin <= OC_ORIGIN_NONE &&
           (prov->flags & ~OC_PROV_INJECTED) == 0;
}

/* ---------- segment header ----------
 *  0  magic "OCSG"      4  version u16    6  stream u8    7  reserved u8 (0)
 *  8  segment id u32   12  first_seq u64 20  reserved u32 x2 (0)
 * 28  CRC32 of bytes 0..27 */

void ocf_put_seg_hdr(unsigned char out[OCF_SEG_HDR_BYTES], const struct ocf_seg_hdr *h)
{
    memset(out, 0, OCF_SEG_HDR_BYTES);
    memcpy(out, seg_magic, sizeof seg_magic);
    put_u16(out + 4, OCF_VERSION);
    out[6] = h->stream;
    put_u32(out + 8, h->id);
    put_u64(out + 12, h->first_seq);
    put_u32(out + 28, oc_crc32(0, out, 28));
}

bool ocf_get_seg_hdr(const unsigned char in[OCF_SEG_HDR_BYTES], struct ocf_seg_hdr *h)
{
    if (memcmp(in, seg_magic, sizeof seg_magic) != 0 || get_u16(in + 4) != OCF_VERSION ||
        in[7] != 0 || get_u32(in + 20) != 0 || get_u32(in + 24) != 0 ||
        get_u32(in + 28) != oc_crc32(0, in, 28))
        return false;
    h->stream = in[6];
    h->id = get_u32(in + 8);
    h->first_seq = get_u64(in + 12);
    return true;
}

/* ---------- footer ----------
 *  0  magic "OCFT"   4  count u32   8  last_seq u64   16  CRC32 of bytes 0..15 */

void ocf_put_footer(unsigned char out[OCF_FOOTER_BYTES], const struct ocf_footer *f)
{
    memcpy(out, footer_magic, sizeof footer_magic);
    put_u32(out + 4, f->count);
    put_u64(out + 8, f->last_seq);
    put_u32(out + 16, oc_crc32(0, out, 16));
}

bool ocf_get_footer(const unsigned char in[OCF_FOOTER_BYTES], struct ocf_footer *f)
{
    if (memcmp(in, footer_magic, sizeof footer_magic) != 0 ||
        get_u32(in + 16) != oc_crc32(0, in, 16))
        return false;
    f->count = get_u32(in + 4);
    f->last_seq = get_u64(in + 8);
    return true;
}

/* ---------- record frame ----------
 * frame header:     0 magic u16 0x5243   2 type u8   3 version u8
 *                   4 payload len u32    8 seq u64
 * provenance block: 16 kind u8  17 origin u8  18 emitter u8  19 flags u8
 *                   20 instance u32      24 session_id u64
 * 32 payload (len bytes, raw), then CRC32 u32 over bytes [0, 32 + len). */

void ocf_put_frame(unsigned char *out, const struct ocf_frame_head *h, const void *payload)
{
    put_u16(out, FRAME_MAGIC);
    out[2] = h->type;
    out[3] = OCF_VERSION;
    put_u32(out + 4, h->len);
    put_u64(out + 8, h->seq);
    out[16] = h->prov.kind;
    out[17] = h->prov.origin;
    out[18] = h->prov.emitter;
    out[19] = h->prov.flags;
    put_u32(out + 20, h->prov.instance);
    put_u64(out + 24, h->prov.session_id);
    if (h->len > 0)
        memcpy(out + OCF_FRAME_HEAD_BYTES, payload, h->len);
    size_t body = OCF_FRAME_HEAD_BYTES + (size_t)h->len;
    put_u32(out + body, oc_crc32(0, out, body));
}

bool ocf_get_frame_head(const unsigned char in[OCF_FRAME_HEAD_BYTES], struct ocf_frame_head *h)
{
    if (get_u16(in) != FRAME_MAGIC || in[3] != OCF_VERSION)
        return false;
    h->type = in[2];
    h->len = get_u32(in + 4);
    h->seq = get_u64(in + 8);
    h->prov = (struct oc_prov){
        .kind = in[16],
        .origin = in[17],
        .emitter = in[18],
        .flags = in[19],
        .instance = get_u32(in + 20),
        .session_id = get_u64(in + 24),
    };
    return ocf_type_valid(h->type) && h->len <= OC_STORE_MAX_PAYLOAD &&
           ocf_prov_valid(&h->prov);
}

bool ocf_frame_crc_ok(const unsigned char *frame, uint32_t len)
{
    size_t body = OCF_FRAME_HEAD_BYTES + (size_t)len;
    return get_u32(frame + body) == oc_crc32(0, frame, body);
}
