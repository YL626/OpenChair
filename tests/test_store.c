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

/* ===== 4. recover the valid prefix without fabricating records ===== */

/* Installs `buf` as the image's segment and checks that recovery keeps
 * exactly records 0..k-1, ending at keep_end, and returns nothing else. */
static int check_recovers_prefix(const struct seg_image *img,
                                 const unsigned char *buf, size_t len,
                                 uint64_t k, const char *what)
{
    int before = tu_failures;
    char dir[PATH_MAX] = "";
    char seg[PATH_MAX];
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};
    struct seg_image mod = *img;
    off_t keep_end = k > 0 ? img->ends[k - 1] : 0;

    mod.data = (unsigned char *)buf;
    mod.len = len;
    REQUIRE(seg_image_install(&mod, len, dir, sizeof dir, seg) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, &rep) == 0);
    const struct oc_store_stream_report *r = &rep.stream[OC_STREAM_EVIDENCE];
    CHECK(r->records_kept == k);
    CHECK(r->torn_bytes == (uint64_t)((off_t)len - keep_end));
    CHECK(tu_file_size(seg) == keep_end);
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, tu_len_marked, &c) == 0);
    CHECK(tu_is_exact_range(&c, 0, k));
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(dir);
    if (tu_failures != before)
        fprintf(stderr, "  (case: %s)\n", what);
    return tu_failures != before;
}

/* Builds image bytes [0, prefix) followed by `extra`, into a malloc'd buffer. */
static unsigned char *image_plus(const struct seg_image *img, size_t prefix,
                                 const unsigned char *extra, size_t elen)
{
    unsigned char *buf = malloc(prefix + elen);
    if (buf == NULL)
        return NULL;
    memcpy(buf, img->data, prefix);
    memcpy(buf + prefix, extra, elen);
    return buf;
}

/* Garbage and zero-filled tails (what a filesystem may leave after power
 * loss) are dropped; every real record is kept. */
static void test_prefix_trailing_garbage(void)
{
    enum { N = 6, G = 300 };
    struct seg_image img;
    unsigned char extra[G];
    unsigned char *buf = NULL;
    REQUIRE(seg_image_build(N, &img) == 0);

    uint32_t x = 0x12345678u;   /* fixed xorshift seed: reproducible */
    for (size_t i = 0; i < G; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        extra[i] = (unsigned char)x;
    }
    buf = image_plus(&img, img.len, extra, G);
    REQUIRE(buf != NULL);
    CHECK(check_recovers_prefix(&img, buf, img.len + G, N, "random tail") == 0);
    free(buf);

    memset(extra, 0, sizeof extra);
    buf = image_plus(&img, img.len, extra, G);
    REQUIRE(buf != NULL);
    CHECK(check_recovers_prefix(&img, buf, img.len + G, N, "zero-filled tail") == 0);
out:
    free(buf);
    seg_image_free(&img);
}

/* A byte-exact copy of a valid frame has a correct CRC but a repeated seq.
 * Accepting it would fabricate a record, so it must be rejected. */
static void test_prefix_replayed_frame(void)
{
    enum { N = 6 };
    struct seg_image img;
    unsigned char *buf = NULL;
    REQUIRE(seg_image_build(N, &img) == 0);

    /* The last record, appended a second time. */
    size_t flen = (size_t)(img.ends[N - 1] - img.ends[N - 2]);
    buf = image_plus(&img, img.len, img.data + img.ends[N - 2], flen);
    REQUIRE(buf != NULL);
    CHECK(check_recovers_prefix(&img, buf, img.len + flen, N, "last frame repeated") == 0);
    free(buf);

    /* An earlier record (seq 2) appended at the tail. */
    flen = (size_t)(img.ends[2] - img.ends[1]);
    buf = image_plus(&img, img.len, img.data + img.ends[1], flen);
    REQUIRE(buf != NULL);
    CHECK(check_recovers_prefix(&img, buf, img.len + flen, N, "old frame replayed") == 0);
out:
    free(buf);
    seg_image_free(&img);
}

/* Removing record 3 leaves record 4 directly after record 2: each frame is
 * individually valid, but the seq jump means 4 and 5 must not be accepted
 * as if nothing were missing from the newest segment. */
static void test_prefix_missing_middle_record(void)
{
    enum { N = 6 };
    struct seg_image img;
    unsigned char *buf = NULL;
    REQUIRE(seg_image_build(N, &img) == 0);
    size_t tail = img.len - (size_t)img.ends[3];
    buf = image_plus(&img, (size_t)img.ends[2], img.data + img.ends[3], tail);
    REQUIRE(buf != NULL);
    CHECK(check_recovers_prefix(&img, buf, (size_t)img.ends[2] + tail, 3,
                                "record 3 spliced out") == 0);
out:
    free(buf);
    seg_image_free(&img);
}

/* Records appended after recovery follow the kept prefix with no gap, and
 * the dropped bytes never come back across further reopens. */
static void test_prefix_append_after_recovery(void)
{
    enum { N = 6, M = 3 };
    char dir[PATH_MAX] = "";
    char seg[PATH_MAX];
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct seg_image img = {0};
    struct tu_collect c = {0};
    static const unsigned char junk[64] = {0xDE, 0xAD, 0xBE, 0xEF};

    REQUIRE(seg_image_build(N, &img) == 0);
    REQUIRE(seg_image_install(&img, img.len, dir, sizeof dir, seg) == 0);
    REQUIRE(tu_write_file(seg, junk, sizeof junk, 1) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, &rep) == 0);
    /* No record was lost, so the sequence continues at N. */
    CHECK(rep.stream[OC_STREAM_EVIDENCE].next_seq == N);
    REQUIRE(tu_append_n(st, OC_STREAM_EVIDENCE, N, M, tu_len_marked, OC_STORE_FORCE_SYNC) == 0);

    for (int round = 0; round < 2; round++) {
        REQUIRE(tu_reopen(&st, &cfg, &rep) == 0);
        CHECK(rep.stream[OC_STREAM_EVIDENCE].records_kept == N + M);
        CHECK(rep.stream[OC_STREAM_EVIDENCE].torn_bytes == 0);
        REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, tu_len_marked, &c) == 0);
        CHECK(tu_is_exact_range(&c, 0, N + M));
        tu_collect_free(&c);
    }
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    seg_image_free(&img);
    tu_rmrf(dir);
}

/* ===== 5. recovery across multiple segments ===== */

/* Segments must hold at least one maximum-size record, so "small" here is
 * 80 KiB with ~4 KiB records: about 20 records per segment. */
#define MS_SEG_BYTES (80u * 1024u)
#define MS_N 70

static size_t len_4k(uint64_t seq)
{
    return 4000 + (size_t)(seq % 7) * 13;
}

/* A closed multi-segment evidence stream and, for each record, which
 * segment (index in id order) holds it and where it ends in that file. */
struct ms_store {
    char dir[PATH_MAX];
    struct oc_store_cfg cfg;
    int seg_of[MS_N];
    off_t end_of[MS_N];
    int nseg;
    char segs[TU_MAX_SEGS][PATH_MAX];
};

static int ms_build(struct ms_store *ms)
{
    struct oc_store *st = NULL;
    int rc = -1;
    memset(ms, 0, sizeof *ms);
    if (tu_mkdtemp(ms->dir, sizeof ms->dir) != 0)
        return -1;
    ms->cfg = tu_cfg(ms->dir);
    ms->cfg.stream[OC_STREAM_EVIDENCE].segment_bytes = MS_SEG_BYTES;
    if (oc_store_open(&ms->cfg, &st, NULL) != 0)
        goto done;
    for (uint64_t i = 0; i < MS_N; i++) {
        if (tu_append(st, OC_STREAM_EVIDENCE, i, len_4k, OC_STORE_FORCE_SYNC) != 0)
            goto done;
        int n = tu_list_segments(ms->dir, OC_STREAM_EVIDENCE, ms->segs, TU_MAX_SEGS);
        if (n <= 0)
            goto done;
        ms->seg_of[i] = n - 1;
        ms->end_of[i] = tu_file_size(ms->segs[n - 1]);
    }
    rc = 0;
done:
    if (oc_store_close(st) != 0)
        rc = -1;
    if (rc == 0) {
        ms->nseg = tu_list_segments(ms->dir, OC_STREAM_EVIDENCE, ms->segs, TU_MAX_SEGS);
        if (ms->nseg != ms->seg_of[MS_N - 1] + 1)
            rc = -1;   /* close must not create or remove segments */
    }
    return rc;
}

/* First and one-past-last record held by segment s. */
static void ms_range(const struct ms_store *ms, int s, uint64_t *first, uint64_t *end)
{
    *first = MS_N;
    *end = 0;
    for (uint64_t i = 0; i < MS_N; i++) {
        if (ms->seg_of[i] == s) {
            if (i < *first)
                *first = i;
            *end = i + 1;
        }
    }
}

/* Baseline: records span at least three segments, seq is contiguous across
 * them, and a read can start in a middle segment. */
static void test_multiseg_roundtrip(void)
{
    struct ms_store ms;
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};

    REQUIRE(ms_build(&ms) == 0);
    CHECK(ms.nseg >= 3);
    REQUIRE(oc_store_open(&ms.cfg, &st, &rep) == 0);
    CHECK(rep.stream[OC_STREAM_EVIDENCE].records_kept == MS_N);
    CHECK(rep.stream[OC_STREAM_EVIDENCE].damaged_segments == 0);
    CHECK(rep.stream[OC_STREAM_EVIDENCE].seq_gaps == 0);
    CHECK(rep.stream[OC_STREAM_EVIDENCE].next_seq == MS_N);
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, len_4k, &c) == 0);
    CHECK(tu_is_exact_range(&c, 0, MS_N));
    tu_collect_free(&c);

    uint64_t first, end;
    ms_range(&ms, 1, &first, &end);
    c = (struct tu_collect){.stream = OC_STREAM_EVIDENCE, .lenf = len_4k};
    REQUIRE(oc_store_read(st, OC_STREAM_EVIDENCE, first + 1, tu_collect_visit, &c) == 0);
    CHECK(tu_is_exact_range(&c, first + 1, MS_N - first - 1));
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(ms.dir);
}

/* (a) A torn tail in the newest segment drops only its last record; every
 * older segment is untouched. */
static void test_multiseg_torn_newest(void)
{
    struct ms_store ms;
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};

    REQUIRE(ms_build(&ms) == 0);
    REQUIRE(ms.nseg >= 3);
    REQUIRE(ms.seg_of[MS_N - 2] == ms.nseg - 1);   /* last two records share the newest segment */
    REQUIRE(tu_truncate(ms.segs[ms.nseg - 1], ms.end_of[MS_N - 1] - 5) == 0);
    REQUIRE(oc_store_open(&ms.cfg, &st, &rep) == 0);
    const struct oc_store_stream_report *r = &rep.stream[OC_STREAM_EVIDENCE];
    CHECK(r->records_kept == MS_N - 1);
    CHECK(r->damaged_segments == 0);
    CHECK(r->torn_bytes > 0);
    CHECK(tu_file_size(ms.segs[ms.nseg - 1]) == ms.end_of[MS_N - 2]);
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, len_4k, &c) == 0);
    CHECK(tu_is_exact_range(&c, 0, MS_N - 1));
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(ms.dir);
}

/* (b) Damage inside an older, sealed segment is not a torn write: the file
 * is kept byte-for-byte, marked damaged, and recovery continues with the
 * later segments. The damaged record is never returned. */
static void test_multiseg_damaged_sealed(void)
{
    struct ms_store ms;
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};
    unsigned char *before = NULL, *after = NULL;
    size_t blen, alen;

    REQUIRE(ms_build(&ms) == 0);
    REQUIRE(ms.nseg >= 3);
    uint64_t first, end;
    ms_range(&ms, 1, &first, &end);
    uint64_t victim = first + (end - first) / 2;
    unsigned char m[TU_MARKER_LEN];
    tu_marker(OC_STREAM_EVIDENCE, victim, m);
    off_t off = tu_find(ms.segs[1], m, sizeof m);
    REQUIRE(off >= 0);
    REQUIRE(tu_flip_bit(ms.segs[1], off + 6, 2) == 0);
    REQUIRE(tu_read_file(ms.segs[1], &before, &blen) == 0);

    REQUIRE(oc_store_open(&ms.cfg, &st, &rep) == 0);
    const struct oc_store_stream_report *r = &rep.stream[OC_STREAM_EVIDENCE];
    CHECK(r->damaged_segments == 1);
    CHECK(r->seq_gaps >= 1);
    CHECK(r->next_seq == MS_N);
    REQUIRE(tu_read_file(ms.segs[1], &after, &alen) == 0);
    CHECK(alen == blen && memcmp(before, after, blen) == 0);

    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, len_4k, &c) == 0);
    CHECK(c.bad == 0);
    CHECK(r->records_kept == c.n);
    /* Strictly increasing, the victim absent, and every record outside the
     * damaged segment present. (Whether records of segment 1 before the
     * damage are returned is open decision 6.) */
    size_t j = 0;
    for (uint64_t i = 0; i < MS_N; i++) {
        int present = j < c.n && c.seqs[j] == i;
        if (i == victim)
            CHECK(!present);
        else if (ms.seg_of[i] != 1)
            CHECK(present);
        if (present)
            j++;
    }
    CHECK(j == c.n);
out:
    free(before);
    free(after);
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(ms.dir);
}

/* (c) The newest segment's header is damaged: it was never durably
 * created, so it is removed and the older segments are kept. */
static void test_multiseg_bad_newest_header(void)
{
    struct ms_store ms;
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};

    REQUIRE(ms_build(&ms) == 0);
    REQUIRE(ms.nseg >= 3);
    uint64_t first, end;
    ms_range(&ms, ms.nseg - 1, &first, &end);
    REQUIRE(tu_flip_bit(ms.segs[ms.nseg - 1], 0, 0) == 0);
    REQUIRE(oc_store_open(&ms.cfg, &st, &rep) == 0);
    const struct oc_store_stream_report *r = &rep.stream[OC_STREAM_EVIDENCE];
    CHECK(r->removed_segments == 1);
    CHECK(r->damaged_segments == 0);
    CHECK(r->records_kept == first);
    CHECK(r->next_seq >= first);
    CHECK(tu_file_size(ms.segs[ms.nseg - 1]) < 0);   /* file removed */
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, len_4k, &c) == 0);
    CHECK(tu_is_exact_range(&c, 0, first));
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(ms.dir);
}

/* (d) A whole middle segment is missing: the gap is reported and nothing
 * is invented to fill it. */
static void test_multiseg_missing_middle(void)
{
    struct ms_store ms;
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};

    REQUIRE(ms_build(&ms) == 0);
    REQUIRE(ms.nseg >= 3);
    uint64_t first, end;
    ms_range(&ms, 1, &first, &end);
    REQUIRE(unlink(ms.segs[1]) == 0);
    REQUIRE(oc_store_open(&ms.cfg, &st, &rep) == 0);
    const struct oc_store_stream_report *r = &rep.stream[OC_STREAM_EVIDENCE];
    CHECK(r->seq_gaps == end - first);
    CHECK(r->records_kept == MS_N - (end - first));
    CHECK(r->next_seq == MS_N);
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, len_4k, &c) == 0);
    CHECK(c.bad == 0);
    CHECK(c.n == MS_N - (end - first));
    size_t j = 0;
    for (uint64_t i = 0; i < MS_N && j < c.n; i++) {
        if (i >= first && i < end)
            continue;
        CHECK(c.seqs[j] == i);
        j++;
    }
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(ms.dir);
}

/* ===== 6. invalid inputs ===== */

/* Number of entries in a directory (excluding . and ..), or -1. */
static int dir_entries(const char *path)
{
    DIR *d = opendir(path);
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            n++;
    }
    closedir(d);
    return n;
}

/* Opens with cfg, expecting `want`; a failed open leaves *out NULL and
 * creates nothing in the store directory. */
static void expect_open_fails(const struct oc_store_cfg *cfg, int want,
                              const char *dir, const char *what)
{
    struct oc_store *st = (struct oc_store *)&st;   /* non-NULL sentinel */
    int rc = oc_store_open(cfg, &st, NULL);
    CHECK(rc == want);
    CHECK(st == NULL);
    if (rc == 0)
        (void)oc_store_close(st);
    if (dir != NULL)
        CHECK(dir_entries(dir) == 0);
    if (rc != want)
        fprintf(stderr, "  (case: %s: got %d, want %d)\n", what, rc, want);
}

static void test_invalid_open(void)
{
    char dir[PATH_MAX] = "";
    char path[PATH_MAX];
    struct oc_store *st = NULL;

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg good = tu_cfg(dir);
    struct oc_store_cfg cfg;

    CHECK(oc_store_open(NULL, &st, NULL) == -EINVAL);
    CHECK(st == NULL);
    CHECK(oc_store_open(&good, NULL, NULL) == -EINVAL);
    CHECK(dir_entries(dir) == 0);

    cfg = good;
    cfg.dir = NULL;
    expect_open_fails(&cfg, -EINVAL, NULL, "dir NULL");
    cfg.dir = "";
    expect_open_fails(&cfg, -EINVAL, NULL, "dir empty");

    /* Too small to hold one maximum-size record. */
    cfg = good;
    cfg.stream[OC_STREAM_TRACE].segment_bytes = OC_STORE_MAX_PAYLOAD;
    expect_open_fails(&cfg, -EINVAL, dir, "segment_bytes == max payload");
    cfg.stream[OC_STREAM_TRACE].segment_bytes = 0;
    expect_open_fails(&cfg, -EINVAL, dir, "segment_bytes 0");
    cfg = good;
    cfg.stream[OC_STREAM_EVIDENCE].max_segments = 0;
    expect_open_fails(&cfg, -EINVAL, dir, "max_segments 0");
    cfg = good;
    cfg.batch_records = 0;
    expect_open_fails(&cfg, -EINVAL, dir, "batch_records 0");
    cfg = good;
    cfg.batch_ms = 0;
    expect_open_fails(&cfg, -EINVAL, dir, "batch_ms 0");

    /* OS errors are passed through as negative errno. */
    REQUIRE(tu_path(path, sizeof path, dir, "missing") == 0);
    cfg = good;
    cfg.dir = path;
    expect_open_fails(&cfg, -ENOENT, NULL, "dir missing");
    CHECK(tu_file_size(path) < 0);   /* not created behind our back */

    REQUIRE(tu_path(path, sizeof path, dir, "plain-file") == 0);
    REQUIRE(tu_write_file(path, "x", 1, 0) == 0);
    cfg.dir = path;
    expect_open_fails(&cfg, -ENOTDIR, NULL, "dir is a regular file");
out:
    tu_rmrf(dir);
}

/* Every invalid append returns -EINVAL, leaves *seq_out alone, consumes no
 * seq, and leaves the store OK with nothing buffered. */
static void test_invalid_append(void)
{
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    unsigned char *big = NULL;
    static const unsigned char body[8] = "payload";
    const struct oc_prov good = tu_prov(1);
    struct oc_prov prov;
    uint64_t seq;

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);
    big = calloc(1, OC_STORE_MAX_PAYLOAD + 1);
    REQUIRE(big != NULL);

#define BAD_APPEND(st_, stream_, type_, prov_, data_, len_, flags_)                 \
    do {                                                                            \
        seq = 777;                                                                  \
        CHECK(oc_store_append(st_, stream_, type_, prov_, data_, len_, flags_, &seq) \
              == -EINVAL);                                                          \
        CHECK(seq == 777);                                                          \
    } while (0)

    BAD_APPEND(NULL, OC_STREAM_EVIDENCE, OC_REC_EVENT, &good, body, sizeof body, 0);
    BAD_APPEND(st, OC_STREAM_COUNT, OC_REC_EVENT, &good, body, sizeof body, 0);
    BAD_APPEND(st, (enum oc_stream)-1, OC_REC_EVENT, &good, body, sizeof body, 0);
    BAD_APPEND(st, OC_STREAM_EVIDENCE, (enum oc_rec_type)0, &good, body, sizeof body, 0);
    BAD_APPEND(st, OC_STREAM_EVIDENCE, (enum oc_rec_type)5, &good, body, sizeof body, 0);
    BAD_APPEND(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, NULL, body, sizeof body, 0);
    BAD_APPEND(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &good, NULL, 5, 0);
    BAD_APPEND(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &good, big, OC_STORE_MAX_PAYLOAD + 1, 0);
    BAD_APPEND(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &good, body, sizeof body, 0x80u);

    prov = good;
    prov.kind = 0;
    BAD_APPEND(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &prov, body, sizeof body, 0);
    prov.kind = OC_KIND_SYSTEM + 1;
    BAD_APPEND(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &prov, body, sizeof body, 0);
    prov = good;
    prov.origin = 0;
    BAD_APPEND(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &prov, body, sizeof body, 0);
    prov.origin = OC_ORIGIN_NONE + 1;
    BAD_APPEND(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &prov, body, sizeof body, 0);
    prov = good;
    prov.flags = 0x80u;
    BAD_APPEND(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &prov, body, sizeof body, 0);
#undef BAD_APPEND

    struct oc_store_status ss;
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.state == OC_STORE_OK);
    CHECK(ss.last_errno == 0);
    for (int s = 0; s < OC_STREAM_COUNT; s++)
        CHECK(ss.stream[s].buffered == 0 && ss.stream[s].durable == 0);

    /* The edges that are valid: an empty payload (NULL allowed), and exactly
     * the maximum. No seq was consumed by the rejected calls. */
    CHECK(oc_store_append(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &good, NULL, 0, 0, &seq) == 0);
    CHECK(seq == 0);
    CHECK(oc_store_append(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &good, big,
                          OC_STORE_MAX_PAYLOAD, 0, NULL) == 0);
    CHECK(oc_store_sync(st, OC_STREAM_EVIDENCE, &seq) == 0);
    CHECK(seq == 1);
out:
    free(big);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(dir);
}

/* A visitor that stops after `limit` records by returning `stop`. */
struct stop_ctx {
    int seen, limit, stop;
};

static int stop_visit(const struct oc_record_view *r, void *ctx)
{
    (void)r;
    struct stop_ctx *s = ctx;
    return ++s->seen == s->limit ? s->stop : 0;
}

static void test_invalid_other_calls(void)
{
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct oc_store_status ss;
    uint64_t seq;

    /* NULL handles. close(NULL) is a no-op, like free(NULL). */
    CHECK(oc_store_tick(NULL, 0) == -EINVAL);
    CHECK(oc_store_sync(NULL, OC_STREAM_EVIDENCE, &seq) == -EINVAL);
    CHECK(oc_store_status(NULL, &ss) == -EINVAL);
    CHECK(oc_store_read(NULL, OC_STREAM_EVIDENCE, 0, stop_visit, NULL) == -EINVAL);
    CHECK(oc_store_close(NULL) == 0);

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);
    REQUIRE(tu_append_n(st, OC_STREAM_TRACE, 0, 10, tu_len_marked, OC_STORE_FORCE_SYNC) == 0);

    CHECK(oc_store_status(st, NULL) == -EINVAL);
    CHECK(oc_store_sync(st, OC_STREAM_COUNT, &seq) == -EINVAL);
    CHECK(oc_store_read(st, OC_STREAM_COUNT, 0, stop_visit, NULL) == -EINVAL);
    CHECK(oc_store_read(st, OC_STREAM_TRACE, 0, NULL, NULL) == -EINVAL);

    /* Reading past the end visits nothing and succeeds. */
    struct stop_ctx sc = {.limit = 1, .stop = 1};
    CHECK(oc_store_read(st, OC_STREAM_TRACE, 10, stop_visit, &sc) == 0);
    CHECK(sc.seen == 0);

    /* A visitor's nonzero return stops the read and is propagated. */
    sc = (struct stop_ctx){.limit = 3, .stop = 42};
    CHECK(oc_store_read(st, OC_STREAM_TRACE, 0, stop_visit, &sc) == 42);
    CHECK(sc.seen == 3);
    sc = (struct stop_ctx){.limit = 2, .stop = -7};
    CHECK(oc_store_read(st, OC_STREAM_TRACE, 0, stop_visit, &sc) == -7);
    CHECK(sc.seen == 2);

    /* None of the above changed the store. */
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.state == OC_STORE_OK);
    CHECK(ss.stream[OC_STREAM_TRACE].durable == 10);
out:
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(dir);
}

/* ===== 9. bounded storage: trace ring, evidence cap ===== */

#define RING_SEGS 3

/* Number of segment files of a stream, or -1. */
static int seg_count(const char *dir, enum oc_stream s)
{
    char paths[TU_MAX_SEGS][PATH_MAX];
    return tu_list_segments(dir, s, paths, TU_MAX_SEGS);
}

/* The trace stream is a ring of max_segments files: the oldest segment is
 * deleted to make room, the stream never holds more files than that, what
 * remains is a contiguous run ending at the newest record, and dropping
 * the oldest records is not reported as a gap. Rotation continues across a
 * reopen. */
static void test_trace_ring_rotation(void)
{
    enum { N1 = 200, N2 = 60 };
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    cfg.stream[OC_STREAM_TRACE].segment_bytes = MS_SEG_BYTES;   /* ~20 records each */
    cfg.stream[OC_STREAM_TRACE].max_segments = RING_SEGS;
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);

    int max_seen = 0;
    for (uint64_t i = 0; i < N1; i++) {
        REQUIRE(tu_append(st, OC_STREAM_TRACE, i, len_4k, OC_STORE_FORCE_SYNC) == 0);
        int n = seg_count(dir, OC_STREAM_TRACE);
        if (n > max_seen)
            max_seen = n;
    }
    CHECK(max_seen == RING_SEGS);   /* reached the ring length, never exceeded it */

    REQUIRE(tu_read_all(st, OC_STREAM_TRACE, len_4k, &c) == 0);
    REQUIRE(c.n > 0);
    uint64_t first1 = c.seqs[0];
    CHECK(first1 > 0);   /* the oldest records were rotated out */
    CHECK(tu_is_exact_range(&c, first1, N1 - first1));
    tu_collect_free(&c);

    /* The evidence stream is unaffected by trace rotation. */
    CHECK(seg_count(dir, OC_STREAM_EVIDENCE) == 1);

    REQUIRE(tu_reopen(&st, &cfg, &rep) == 0);
    const struct oc_store_stream_report *r = &rep.stream[OC_STREAM_TRACE];
    CHECK(r->records_kept == N1 - first1);
    CHECK(r->seq_gaps == 0);
    CHECK(r->damaged_segments == 0);
    CHECK(r->next_seq == N1);

    REQUIRE(tu_append_n(st, OC_STREAM_TRACE, N1, N2, len_4k, OC_STORE_FORCE_SYNC) == 0);
    CHECK(seg_count(dir, OC_STREAM_TRACE) == RING_SEGS);
    REQUIRE(tu_read_all(st, OC_STREAM_TRACE, len_4k, &c) == 0);
    REQUIRE(c.n > 0);
    CHECK(c.seqs[0] > first1);
    CHECK(tu_is_exact_range(&c, c.seqs[0], N1 + N2 - c.seqs[0]));
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    tu_rmrf(dir);
}

/* The evidence stream has a hard cap of max_segments files and never
 * deletes: an append that would need a segment past the cap is refused
 * with -EDQUOT, consumes no seq and leaves *seq_out alone, while the store
 * stays OK, every accepted record stays readable, and the trace stream
 * keeps working. The cap still holds after a reopen. */
static void test_evidence_cap(void)
{
    enum { CAP_SEGS = 2, LIMIT = 200 };
    char dir[PATH_MAX] = "";
    char paths[TU_MAX_SEGS][PATH_MAX];
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct oc_store_status ss;
    struct tu_collect c = {0};
    unsigned char body[4000];
    const struct oc_prov prov = tu_prov(1);

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    cfg.stream[OC_STREAM_EVIDENCE].segment_bytes = MS_SEG_BYTES;
    cfg.stream[OC_STREAM_EVIDENCE].max_segments = CAP_SEGS;
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);

    uint64_t n = 0;
    int rc = 0;
    while (n < LIMIT && (rc = tu_append(st, OC_STREAM_EVIDENCE, n, len_4k,
                                        OC_STORE_FORCE_SYNC)) == 0)
        n++;
    CHECK(rc == -EDQUOT);
    CHECK(n > 20 && n < LIMIT);   /* more than one segment's worth was accepted */
    CHECK(seg_count(dir, OC_STREAM_EVIDENCE) == CAP_SEGS);

    /* Refused again; no seq consumed, *seq_out untouched. */
    uint64_t seq = 777;
    tu_payload(OC_STREAM_EVIDENCE, n, body, sizeof body);
    CHECK(oc_store_append(st, OC_STREAM_EVIDENCE, OC_REC_EVENT, &prov, body, sizeof body,
                          OC_STORE_FORCE_SYNC, &seq) == -EDQUOT);
    CHECK(seq == 777);

    /* Not a failure: the store is OK, nothing is uncertain, every accepted
     * record is durable, and the trace stream still accepts records. */
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.state == OC_STORE_OK);
    CHECK(ss.last_errno == 0);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].durable == n);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].uncertain == 0);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].durable_seq == n - 1);
    CHECK(tu_append(st, OC_STREAM_TRACE, 0, len_4k, OC_STORE_FORCE_SYNC) == 0);

    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, len_4k, &c) == 0);
    CHECK(tu_is_exact_range(&c, 0, n));
    tu_collect_free(&c);

    /* After a reopen: nothing was deleted, everything is kept, and the cap
     * still refuses the next record. */
    REQUIRE(tu_reopen(&st, &cfg, &rep) == 0);
    CHECK(rep.stream[OC_STREAM_EVIDENCE].records_kept == n);
    CHECK(rep.stream[OC_STREAM_EVIDENCE].next_seq == n);
    REQUIRE(tu_list_segments(dir, OC_STREAM_EVIDENCE, paths, TU_MAX_SEGS) == CAP_SEGS);
    CHECK(strcmp(strrchr(paths[0], '/') + 1, "00000000.seg") == 0);
    CHECK(tu_append(st, OC_STREAM_EVIDENCE, n, len_4k, OC_STORE_FORCE_SYNC) == -EDQUOT);
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, len_4k, &c) == 0);
    CHECK(tu_is_exact_range(&c, 0, n));
out:
    tu_collect_free(&c);
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

    RUN(test_truncation_sweep_tail);
    RUN(test_truncation_sweep_head);
    RUN(test_truncation_then_append);

    RUN(test_corrupt_payload_marker);
    RUN(test_corrupt_every_byte_of_record);
    RUN(test_corrupt_first_and_last_record);

    RUN(test_prefix_trailing_garbage);
    RUN(test_prefix_replayed_frame);
    RUN(test_prefix_missing_middle_record);
    RUN(test_prefix_append_after_recovery);

    RUN(test_multiseg_roundtrip);
    RUN(test_multiseg_torn_newest);
    RUN(test_multiseg_damaged_sealed);
    RUN(test_multiseg_bad_newest_header);
    RUN(test_multiseg_missing_middle);

    RUN(test_invalid_open);
    RUN(test_invalid_append);
    RUN(test_invalid_other_calls);

    RUN(test_trace_ring_rotation);
    RUN(test_evidence_cap);
    return tu_finish("test_store");
}
