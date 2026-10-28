/* STUB: every call fails with -ENOSYS so tests/test_store*.c compile and
 * fail until the real implementation lands. See include/oc_store.h. */
#include <errno.h>
#include <stddef.h>

#include "oc_store.h"

int oc_store_open(const struct oc_store_cfg *cfg, struct oc_store **out,
                  struct oc_store_report *report)
{
    (void)cfg;
    (void)report;
    if (out != NULL)
        *out = NULL;
    return -ENOSYS;
}

int oc_store_append(struct oc_store *st, enum oc_stream stream,
                    enum oc_rec_type type, const struct oc_prov *prov,
                    const void *payload, size_t len, unsigned flags,
                    uint64_t *seq_out)
{
    (void)st;
    (void)stream;
    (void)type;
    (void)prov;
    (void)payload;
    (void)len;
    (void)flags;
    (void)seq_out;
    return -ENOSYS;
}

int oc_store_tick(struct oc_store *st, uint64_t now_mono_ns)
{
    (void)st;
    (void)now_mono_ns;
    return -ENOSYS;
}

int oc_store_sync(struct oc_store *st, enum oc_stream stream,
                  uint64_t *durable_seq)
{
    (void)st;
    (void)stream;
    (void)durable_seq;
    return -ENOSYS;
}

int oc_store_status(const struct oc_store *st, struct oc_store_status *out)
{
    (void)st;
    (void)out;
    return -ENOSYS;
}

int oc_store_read(struct oc_store *st, enum oc_stream stream, uint64_t from_seq,
                  oc_store_visit_fn visit, void *ctx)
{
    (void)st;
    (void)stream;
    (void)from_seq;
    (void)visit;
    (void)ctx;
    return -ENOSYS;
}

int oc_store_close(struct oc_store *st)
{
    if (st == NULL)
        return 0;
    return -ENOSYS;
}
