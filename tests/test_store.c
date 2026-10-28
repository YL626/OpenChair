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

/* ===== shared fixture: one evidence segment with known record ends ===== */

/* A captured evidence segment and the file offset at which each record ends,
 * measured as the segment size after that record's FORCE_SYNC. */
struct seg_image {
    char name[NAME_MAX + 1];   /* basename, e.g. 00000000.seg */
    unsigned char *data;
    size_t len;
    off_t *ends;               /* ends[i]: end offset of record i */
    uint64_t n;
};

static void seg_image_free(struct seg_image *img)
{
    free(img->data);
    free(img->ends);
    *img = (struct seg_image){0};
}

/* Writes n FORCE_SYNC records (tu_len_marked) to a fresh store, captures the
 * single evidence segment, and removes the store directory. */
static int seg_image_build(uint64_t n, struct seg_image *img)
{
    char dir[PATH_MAX] = "";
    char seg[PATH_MAX];
    struct oc_store *st = NULL;
    int rc = -1;

    *img = (struct seg_image){.n = n};
    img->ends = calloc(n, sizeof *img->ends);
    if (img->ends == NULL || tu_mkdtemp(dir, sizeof dir) != 0)
        goto done;
    struct oc_store_cfg cfg = tu_cfg(dir);
    if (oc_store_open(&cfg, &st, NULL) != 0)
        goto done;
    for (uint64_t i = 0; i < n; i++) {
        if (tu_append(st, OC_STREAM_EVIDENCE, i, tu_len_marked, OC_STORE_FORCE_SYNC) != 0 ||
            tu_newest_segment(dir, OC_STREAM_EVIDENCE, seg) != 0)
            goto done;
        img->ends[i] = tu_file_size(seg);
    }
    /* Capture before close, so the image is exactly what a crash leaves. */
    char paths[TU_MAX_SEGS][PATH_MAX];
    if (tu_list_segments(dir, OC_STREAM_EVIDENCE, paths, TU_MAX_SEGS) != 1)
        goto done;
    const char *base = strrchr(seg, '/');
    base = base != NULL ? base + 1 : seg;
    if (strlen(base) >= sizeof img->name)
        goto done;
    memcpy(img->name, base, strlen(base) + 1);
    if (tu_read_file(seg, &img->data, &img->len) != 0)
        goto done;
    rc = 0;
done:
    (void)oc_store_close(st);
    tu_rmrf(dir);
    if (rc != 0)
        seg_image_free(img);
    return rc;
}

/* Recreates a store directory whose evidence stream holds the first `len`
 * bytes of the image. */
static int seg_image_install(const struct seg_image *img, size_t len,
                             char *dir, size_t cap, char *seg)
{
    char sdir[PATH_MAX];
    if (tu_mkdtemp(dir, cap) != 0 ||
        tu_path(sdir, sizeof sdir, dir, "evidence") != 0 ||
        mkdir(sdir, 0700) != 0 ||
        tu_path(seg, PATH_MAX, sdir, img->name) != 0)
        return -1;
    return tu_write_file(seg, img->data, len, 0);
}

/* Number of records that end at or before offset `cut`. */
static uint64_t records_before(const struct seg_image *img, off_t cut)
{
    uint64_t k = 0;
    while (k < img->n && img->ends[k] <= cut)
        k++;
    return k;
}

/* ===== 2. detect truncated records ===== */

/* Reopens the image truncated at `cut` and checks recovery kept exactly the
 * records that were whole. Returns nonzero if any check failed. */
static int check_truncated_at(const struct seg_image *img, off_t cut,
                              int header_intact)
{
    int before = tu_failures;
    char dir[PATH_MAX] = "";
    char seg[PATH_MAX];
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};
    uint64_t k = records_before(img, cut);

    REQUIRE(seg_image_install(img, (size_t)cut, dir, sizeof dir, seg) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, &rep) == 0);
    const struct oc_store_stream_report *r = &rep.stream[OC_STREAM_EVIDENCE];
    CHECK(r->records_kept == k);
    CHECK(r->damaged_segments == 0);
    if (header_intact && k > 0) {
        /* The torn tail is cut back to the last whole record, in place. */
        CHECK(r->torn_bytes == (uint64_t)(cut - img->ends[k - 1]));
        CHECK(tu_file_size(seg) == img->ends[k - 1]);
        CHECK(r->next_seq >= k);
    }
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, tu_len_marked, &c) == 0);
    CHECK(tu_is_exact_range(&c, 0, k));
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(dir);
    if (tu_failures != before)
        fprintf(stderr, "  (truncated at offset %lld of %zu)\n", (long long)cut, img->len);
    return tu_failures != before;
}

/* Cut at every byte offset across the last two records, including both
 * exact record boundaries. */
static void test_truncation_sweep_tail(void)
{
    enum { N = 10 };
    struct seg_image img;
    REQUIRE(seg_image_build(N, &img) == 0);
    REQUIRE(img.ends[N - 1] == (off_t)img.len);
    for (off_t cut = img.ends[N - 3]; cut <= img.ends[N - 1]; cut++) {
        if (check_truncated_at(&img, cut, 1) != 0)
            break;   /* one diagnosed cut point is enough */
    }
out:
    seg_image_free(&img);
}

/* Cuts inside the segment header and the first record: nothing is kept,
 * open still succeeds, and the stream accepts new records. */
static void test_truncation_sweep_head(void)
{
    enum { N = 3 };
    struct seg_image img;
    REQUIRE(seg_image_build(N, &img) == 0);
    for (off_t cut = 0; cut < img.ends[0]; cut++) {
        if (check_truncated_at(&img, cut, 0) != 0)
            break;
    }
out:
    seg_image_free(&img);
}

/* After a torn tail is dropped, new records are appended after the kept
 * prefix and the dropped bytes never reappear. */
static void test_truncation_then_append(void)
{
    enum { N = 6 };
    char dir[PATH_MAX] = "";
    char seg[PATH_MAX];
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct seg_image img = {0};
    struct tu_collect c = {0};

    REQUIRE(seg_image_build(N, &img) == 0);
    off_t cut = img.ends[N - 2] + (img.ends[N - 1] - img.ends[N - 2]) / 2;
    REQUIRE(seg_image_install(&img, (size_t)cut, dir, sizeof dir, seg) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, &rep) == 0);
    CHECK(rep.stream[OC_STREAM_EVIDENCE].records_kept == N - 1);

    uint64_t next = rep.stream[OC_STREAM_EVIDENCE].next_seq;
    CHECK(next >= N - 1);
    struct oc_prov prov = tu_prov(0);
    static const char body[] = "after-recovery";
    uint64_t got = 0;
    REQUIRE(oc_store_append(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &prov, body,
                            sizeof body, OC_STORE_FORCE_SYNC, &got) == 0);
    CHECK(got == next);

    REQUIRE(tu_reopen(&st, &cfg, &rep) == 0);
    c = (struct tu_collect){.stream = OC_STREAM_EVIDENCE, .lenf = tu_len_marked};
    REQUIRE(oc_store_read(st, OC_STREAM_EVIDENCE, 0, tu_collect_visit, &c) == 0);
    /* N-1 generated records, then the new one (which the generator does not
     * describe, so it is counted as one mismatch). */
    CHECK(c.n == N);
    CHECK(c.bad == 1);
    for (size_t i = 0; i + 1 < c.n && i < N - 1; i++)
        CHECK(c.seqs[i] == i);
    CHECK(c.n == N && c.seqs[N - 1] == got);
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    seg_image_free(&img);
    tu_rmrf(dir);
}

/* ===== 3. detect corrupted record payloads ===== */

/* Reopens the image with one bit flipped at `off` (inside record k) and
 * checks that recovery keeps exactly records 0..k-1. The image is the
 * newest segment, so everything from the first invalid record on is
 * dropped. Returns nonzero if any check failed. */
static int check_flipped_at(const struct seg_image *img, off_t off, uint64_t k)
{
    int before = tu_failures;
    char dir[PATH_MAX] = "";
    char seg[PATH_MAX];
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};
    off_t keep_end = k > 0 ? img->ends[k - 1] : 0;

    REQUIRE(seg_image_install(img, img->len, dir, sizeof dir, seg) == 0);
    REQUIRE(tu_flip_bit(seg, off, (int)(off % 8)) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, &rep) == 0);
    const struct oc_store_stream_report *r = &rep.stream[OC_STREAM_EVIDENCE];
    CHECK(r->records_kept == k);
    CHECK(r->damaged_segments == 0);
    if (k > 0) {
        CHECK(r->torn_bytes == (uint64_t)((off_t)img->len - keep_end));
        CHECK(tu_file_size(seg) == keep_end);
    }
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, tu_len_marked, &c) == 0);
    CHECK(tu_is_exact_range(&c, 0, k));
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(dir);
    if (tu_failures != before)
        fprintf(stderr, "  (bit %d flipped at offset %lld, record %llu)\n",
                (int)(off % 8), (long long)off, (unsigned long long)k);
    return tu_failures != before;
}

/* The payload marker is located by content, so this does not depend on the
 * frame format: flipping any bit of it must invalidate the record. */
static void test_corrupt_payload_marker(void)
{
    enum { N = 8, K = 4 };
    struct seg_image img;
    unsigned char m[TU_MARKER_LEN];
    REQUIRE(seg_image_build(N, &img) == 0);
    tu_marker(OC_STREAM_EVIDENCE, K, m);
    const unsigned char *hit = memmem(img.data, img.len, m, sizeof m);
    REQUIRE(hit != NULL);
    off_t off = (off_t)(hit - img.data);
    REQUIRE(off >= img.ends[K - 1] && off + TU_MARKER_LEN <= img.ends[K]);
    for (int i = 0; i < TU_MARKER_LEN; i += 5)
        CHECK(check_flipped_at(&img, off + i, K) == 0);
    /* A byte in the body after the marker. */
    CHECK(check_flipped_at(&img, off + TU_MARKER_LEN + 10, K) == 0);
out:
    seg_image_free(&img);
}

/* Every byte of a middle record, including its frame header, provenance
 * block and CRC: a single flipped bit anywhere must be detected. */
static void test_corrupt_every_byte_of_record(void)
{
    enum { N = 8, K = 5 };
    struct seg_image img;
    REQUIRE(seg_image_build(N, &img) == 0);
    for (off_t off = img.ends[K - 1]; off < img.ends[K]; off++) {
        if (check_flipped_at(&img, off, K) != 0)
            break;   /* one diagnosed offset is enough */
    }
out:
    seg_image_free(&img);
}

/* Damage to the first and the last record. */
static void test_corrupt_first_and_last_record(void)
{
    enum { N = 5 };
    struct seg_image img;
    unsigned char m[TU_MARKER_LEN];
    REQUIRE(seg_image_build(N, &img) == 0);
    tu_marker(OC_STREAM_EVIDENCE, 0, m);
    const unsigned char *hit = memmem(img.data, img.len, m, sizeof m);
    REQUIRE(hit != NULL);
    CHECK(check_flipped_at(&img, (off_t)(hit - img.data) + 3, 0) == 0);
    CHECK(check_flipped_at(&img, img.ends[N - 1] - 1, N - 1) == 0);
out:
    seg_image_free(&img);
}

int main(void)
{
    RUN(test_crc32_known_answers);
    RUN(test_open_fresh_dir);
    RUN(test_roundtrip_both_streams);
    RUN(test_force_sync_vs_buffered);
    RUN(test_batching_policy);

    RUN(test_truncation_sweep_tail);
    RUN(test_truncation_sweep_head);
    RUN(test_truncation_then_append);

    RUN(test_corrupt_payload_marker);
    RUN(test_corrupt_every_byte_of_record);
    RUN(test_corrupt_first_and_last_record);
    return tu_finish("test_store");
}
