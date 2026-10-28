/* Functional tests for oc_store (Mechanism D). Inputs are synthetic
 * (see test_util.h); no sensor or hardware is involved.
 *
 * Written before the implementation: against the stub in src/oc_store.c
 * every case is expected to FAIL. Run with `make test-store`. */
#include "test_util.h"

#include "oc_crc32.h"

/* ===== 1. write and read back valid records ===== */

static void test_crc32_known_answers(void)
{
    static const char check[] = "123456789";
    CHECK(oc_crc32(0, "", 0) == 0x00000000u);
    CHECK(oc_crc32(0, check, 9) == 0xCBF43926u);
    /* Incremental use gives the same answer as one call. */
    CHECK(oc_crc32(oc_crc32(0, check, 4), check + 4, 5) == 0xCBF43926u);
    CHECK(oc_crc32(0, "a", 1) == 0xE8B7BE43u);
}

static void test_open_fresh_dir(void)
{
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    memset(&rep, 0xFF, sizeof rep);

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, &rep) == 0);
    REQUIRE(st != NULL);
    for (int s = 0; s < OC_STREAM_COUNT; s++) {
        CHECK(rep.stream[s].records_kept == 0);
        CHECK(rep.stream[s].torn_bytes == 0);
        CHECK(rep.stream[s].damaged_segments == 0);
        CHECK(rep.stream[s].removed_segments == 0);
        CHECK(rep.stream[s].seq_gaps == 0);
        CHECK(rep.stream[s].next_seq == 0);
    }

    struct oc_store_status ss;
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.state == OC_STORE_OK);
    CHECK(ss.last_errno == 0);
    for (int s = 0; s < OC_STREAM_COUNT; s++) {
        CHECK(ss.stream[s].buffered == 0);
        CHECK(ss.stream[s].durable == 0);
        CHECK(ss.stream[s].has_durable == 0);
    }

    /* An empty stream reads back nothing and syncs to nothing. */
    struct tu_collect c;
    CHECK(tu_read_all(st, OC_STREAM_EVIDENCE, tu_len_mixed, &c) == 0);
    CHECK(c.n == 0);
    tu_collect_free(&c);
    uint64_t dseq;
    CHECK(oc_store_sync(st, OC_STREAM_TRACE, &dseq) == -ENODATA);
out:
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(dir);
}

/* 100 records per stream, lengths 0..OC_STORE_MAX_PAYLOAD, every provenance
 * field varied; read back, close, reopen, read back again, keep appending. */
static void test_roundtrip_both_streams(void)
{
    enum { N = 100 };
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    cfg.stream[OC_STREAM_TRACE].segment_bytes = 4u << 20;   /* fits 100 mixed records */
    cfg.stream[OC_STREAM_EVIDENCE].segment_bytes = 4u << 20;
    REQUIRE(oc_store_open(&cfg, &st, &rep) == 0);

    for (int s = 0; s < OC_STREAM_COUNT; s++) {
        REQUIRE(tu_append_n(st, (enum oc_stream)s, 0, N, tu_len_mixed, 0) == 0);
        uint64_t dseq = 0;
        REQUIRE(oc_store_sync(st, (enum oc_stream)s, &dseq) == 0);
        CHECK(dseq == N - 1);
    }

    struct oc_store_status ss;
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.state == OC_STORE_OK);
    for (int s = 0; s < OC_STREAM_COUNT; s++) {
        CHECK(ss.stream[s].durable == N);
        CHECK(ss.stream[s].buffered == 0);
        CHECK(ss.stream[s].uncertain == 0);
        CHECK(ss.stream[s].has_durable && ss.stream[s].durable_seq == N - 1);
    }

    for (int s = 0; s < OC_STREAM_COUNT; s++) {
        REQUIRE(tu_read_all(st, (enum oc_stream)s, tu_len_mixed, &c) == 0);
        CHECK(tu_is_exact_range(&c, 0, N));
        tu_collect_free(&c);
    }

    /* Reading from the middle starts at from_seq. */
    c = (struct tu_collect){.stream = OC_STREAM_TRACE, .lenf = tu_len_mixed};
    REQUIRE(oc_store_read(st, OC_STREAM_TRACE, 40, tu_collect_visit, &c) == 0);
    CHECK(tu_is_exact_range(&c, 40, N - 40));
    tu_collect_free(&c);

    REQUIRE(tu_reopen(&st, &cfg, &rep) == 0);
    for (int s = 0; s < OC_STREAM_COUNT; s++) {
        CHECK(rep.stream[s].records_kept == N);
        CHECK(rep.stream[s].torn_bytes == 0);
        CHECK(rep.stream[s].damaged_segments == 0);
        CHECK(rep.stream[s].seq_gaps == 0);
        CHECK(rep.stream[s].next_seq == N);
        REQUIRE(tu_read_all(st, (enum oc_stream)s, tu_len_mixed, &c) == 0);
        CHECK(tu_is_exact_range(&c, 0, N));
        tu_collect_free(&c);
    }

    /* Appends after reopen continue the sequence. */
    REQUIRE(tu_append(st, OC_STREAM_EVIDENCE, N, tu_len_mixed, OC_STORE_FORCE_SYNC) == 0);
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, tu_len_mixed, &c) == 0);
    CHECK(tu_is_exact_range(&c, 0, N + 1));
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(dir);
}

/* FORCE_SYNC makes the record durable before append returns; a plain append
 * is only buffered. */
static void test_force_sync_vs_buffered(void)
{
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct oc_store_status ss;

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);

    REQUIRE(tu_append(st, OC_STREAM_EVIDENCE, 0, tu_len_marked, 0) == 0);
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].buffered == 1);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].durable == 0);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].has_durable == 0);

    REQUIRE(tu_append(st, OC_STREAM_EVIDENCE, 1, tu_len_marked, OC_STORE_FORCE_SYNC) == 0);
    REQUIRE(oc_store_status(st, &ss) == 0);
    /* Syncing is in order, so the earlier buffered record is durable too. */
    CHECK(ss.stream[OC_STREAM_EVIDENCE].buffered == 0);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].durable == 2);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].has_durable);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].durable_seq == 1);
    /* The other stream is untouched. */
    CHECK(ss.stream[OC_STREAM_TRACE].durable == 0);
out:
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(dir);
}

/* tick() syncs once batch_records have accumulated, or batch_ms after it
 * first saw pending records, and not before. */
static void test_batching_policy(void)
{
    const uint64_t ms = 1000000ull;
    const uint64_t t0 = 5000 * ms;
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct oc_store_status ss;

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);   /* 32 records / 500 ms */
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);

    /* Count trigger: 31 records are not enough, 32 are. */
    REQUIRE(tu_append_n(st, OC_STREAM_TRACE, 0, 31, tu_len_marked, 0) == 0);
    REQUIRE(oc_store_tick(st, t0) == 0);
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.stream[OC_STREAM_TRACE].durable == 0);
    REQUIRE(tu_append(st, OC_STREAM_TRACE, 31, tu_len_marked, 0) == 0);
    REQUIRE(oc_store_tick(st, t0) == 0);
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.stream[OC_STREAM_TRACE].durable == 32);
    CHECK(ss.stream[OC_STREAM_TRACE].durable_seq == 31);

    /* Time trigger: one record becomes durable 500 ms later, not at 400. */
    REQUIRE(tu_append(st, OC_STREAM_TRACE, 32, tu_len_marked, 0) == 0);
    REQUIRE(oc_store_tick(st, t0 + 1000 * ms) == 0);
    REQUIRE(oc_store_tick(st, t0 + 1400 * ms) == 0);
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.stream[OC_STREAM_TRACE].durable == 32);
    REQUIRE(oc_store_tick(st, t0 + 1600 * ms) == 0);
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.stream[OC_STREAM_TRACE].durable == 33);
    CHECK(ss.stream[OC_STREAM_TRACE].buffered == 0);
out:
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(dir);
}

int main(void)
{
    RUN(test_crc32_known_answers);
    RUN(test_open_fresh_dir);
    RUN(test_roundtrip_both_streams);
    RUN(test_force_sync_vs_buffered);
    RUN(test_batching_policy);
    return tu_finish("test_store");
}
