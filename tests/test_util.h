/* Shared helpers for the oc_store tests (test_store*.c).
 *
 * All inputs are synthetic and generated deterministically from
 * (stream, seq, len): nothing here is a live sensor measurement.
 *
 * Tests that find bytes in segment files rely on only two layout facts:
 * segments live in <dir>/{evidence,trace}/NNNNNNNN.seg, and a payload is
 * stored as its raw bytes (so its 16-byte marker can be found by memmem).
 * Record boundaries are measured as the segment size after each sync. */
#ifndef TEST_UTIL_H
#define TEST_UTIL_H

#define _GNU_SOURCE   /* memmem; include this header before any other */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "oc_store.h"

[[maybe_unused]] static int tu_failures;

/* CHECK records a failure and continues. REQUIRE records a failure and jumps
 * to the test's `out:` cleanup label, so a stubbed store yields a clean FAIL
 * line instead of a crash on a NULL handle. */
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            tu_failures++;                                                   \
        }                                                                    \
    } while (0)

#define REQUIRE(cond)                                                        \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            tu_failures++;                                                   \
            goto out;                                                        \
        }                                                                    \
    } while (0)

#define RUN(test)                                \
    do {                                         \
        fprintf(stderr, "== %s\n", #test);       \
        test();                                  \
    } while (0)

/* Prints the summary and returns the process exit status. */
[[maybe_unused]] static int tu_finish(const char *suite)
{
    if (tu_failures != 0) {
        fprintf(stderr, "%s: %d check(s) failed\n", suite, tu_failures);
        return 1;
    }
    printf("%s: all tests passed\n", suite);
    return 0;
}

/* ---------- filesystem ---------- */

/* Creates a fresh directory under $TMPDIR (or /tmp) into buf. */
[[maybe_unused]] static int tu_mkdtemp(char *buf, size_t cap)
{
    const char *base = getenv("TMPDIR");
    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    int n = snprintf(buf, cap, "%s/oc_store_test.XXXXXX", base);
    if (n < 0 || (size_t)n >= cap)
        return -1;
    return mkdtemp(buf) != NULL ? 0 : -1;
}

[[maybe_unused]] static int tu_rm_entry(const char *path, const struct stat *sb, int flag,
                       struct FTW *ftw)
{
    (void)sb;
    (void)flag;
    (void)ftw;
    if (remove(path) != 0)
        fprintf(stderr, "warning: cannot remove %s: %s\n", path, strerror(errno));
    return 0;
}

/* rm -rf; a no-op for an empty path. Restores write permission first so
 * tests that chmod a directory can still be cleaned up. */
[[maybe_unused]] static void tu_rmrf(const char *path)
{
    if (path == NULL || path[0] == '\0')
        return;
    (void)chmod(path, 0700);
    (void)nftw(path, tu_rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* Builds dir/sub into buf. */
[[maybe_unused]] static int tu_path(char *buf, size_t cap, const char *dir,
                                    const char *sub)
{
    int n = snprintf(buf, cap, "%s/%s", dir, sub);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

[[maybe_unused]] static off_t tu_file_size(const char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 ? sb.st_size : (off_t)-1;
}

[[maybe_unused]] static int tu_truncate(const char *path, off_t size)
{
    return truncate(path, size);
}

/* Reads a whole file into a malloc'd buffer; caller frees *data. */
[[maybe_unused]] static int tu_read_file(const char *path, unsigned char **data,
                                         size_t *len)
{
    *data = NULL;
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    int rc = -1;
    unsigned char *buf = NULL;
    if (fseeko(f, 0, SEEK_END) != 0)
        goto done;
    off_t size = ftello(f);
    if (size < 0 || fseeko(f, 0, SEEK_SET) != 0)
        goto done;
    buf = malloc(size > 0 ? (size_t)size : 1);
    if (buf == NULL)
        goto done;
    if ((size_t)size != 0 && fread(buf, 1, (size_t)size, f) != (size_t)size)
        goto done;
    *data = buf;
    *len = (size_t)size;
    buf = NULL;
    rc = 0;
done:
    free(buf);
    if (fclose(f) != 0)
        rc = -1;
    return rc;
}

/* Overwrites (O_TRUNC) or appends a file with the given bytes. */
[[maybe_unused]] static int tu_write_file(const char *path, const void *data,
                                          size_t len, int append)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0600);
    if (fd < 0)
        return -1;
    const unsigned char *p = data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return close(fd);
}

/* Flips one bit of the byte at offset. */
[[maybe_unused]] static int tu_flip_bit(const char *path, off_t offset, int bit)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    unsigned char b;
    int rc = -1;
    if (pread(fd, &b, 1, offset) == 1) {
        b ^= (unsigned char)(1u << bit);
        if (pwrite(fd, &b, 1, offset) == 1)
            rc = 0;
    }
    if (close(fd) != 0)
        rc = -1;
    return rc;
}

/* Offset of the first occurrence of needle in the file, or -1. */
[[maybe_unused]] static off_t tu_find(const char *path, const void *needle,
                                      size_t nlen)
{
    unsigned char *data;
    size_t len;
    if (tu_read_file(path, &data, &len) != 0)
        return -1;
    const unsigned char *hit = memmem(data, len, needle, nlen);
    off_t off = hit != NULL ? (off_t)(hit - data) : (off_t)-1;
    free(data);
    return off;
}

/* Number of open descriptors of this process (excluding the one used to
 * list them), or -1. */
[[maybe_unused]] static int tu_fd_count(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (d == NULL)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] != '.')
            n++;
    }
    closedir(d);
    return n - 1;
}

/* ---------- segments ---------- */

#define TU_MAX_SEGS 64

[[maybe_unused]] static const char *tu_stream_dir_name(enum oc_stream s)
{
    return s == OC_STREAM_EVIDENCE ? "evidence" : "trace";
}

[[maybe_unused]] static int tu_is_seg(const struct dirent *e)
{
    size_t n = strlen(e->d_name);
    return n > 4 && strcmp(e->d_name + n - 4, ".seg") == 0;
}

/* Fills paths[] with the stream's segment files in name (= id) order and
 * returns how many, or -1. */
[[maybe_unused]] static int tu_list_segments(const char *dir, enum oc_stream s,
                                             char paths[][PATH_MAX], int max)
{
    char sdir[PATH_MAX];
    if (tu_path(sdir, sizeof sdir, dir, tu_stream_dir_name(s)) != 0)
        return -1;
    struct dirent **list;
    int n = scandir(sdir, &list, tu_is_seg, alphasort);
    if (n < 0)
        return -1;
    int kept = 0;
    for (int i = 0; i < n; i++) {
        if (kept < max && tu_path(paths[kept], PATH_MAX, sdir, list[i]->d_name) == 0)
            kept++;
        free(list[i]);
    }
    free(list);
    return n > max ? -1 : kept;
}

/* Path of the newest (highest id) segment of a stream. */
[[maybe_unused]] static int tu_newest_segment(const char *dir, enum oc_stream s,
                                              char *out)
{
    char paths[TU_MAX_SEGS][PATH_MAX];
    int n = tu_list_segments(dir, s, paths, TU_MAX_SEGS);
    if (n <= 0)
        return -1;
    memcpy(out, paths[n - 1], PATH_MAX);
    return 0;
}

/* ---------- synthetic records ---------- */

#define TU_MARKER_LEN 16
#define TU_SESSION 0xFEEDFACECAFEBEEFull

/* Payload length policy: a function of seq so the verifier can recompute it. */
typedef size_t (*tu_len_fn)(uint64_t seq);

/* Mixed lengths including the edges 0 and OC_STORE_MAX_PAYLOAD. */
[[maybe_unused]] static size_t tu_len_mixed(uint64_t seq)
{
    static const size_t lens[] = {0, 1, 15, 16, 17, 100, 1000, 4096, OC_STORE_MAX_PAYLOAD};
    return lens[seq % (sizeof lens / sizeof lens[0])];
}

/* Every record carries a full marker plus some body. */
[[maybe_unused]] static size_t tu_len_marked(uint64_t seq)
{
    return 64 + (size_t)(seq % 7) * 13;
}

/* 16-byte marker unique to (stream, seq). */
[[maybe_unused]] static void tu_marker(enum oc_stream s, uint64_t seq,
                                       unsigned char m[TU_MARKER_LEN])
{
    m[0] = 'M';
    m[1] = 'K';
    m[2] = (unsigned char)s;
    m[3] = 0xA5;
    for (int i = 0; i < 8; i++)
        m[4 + i] = (unsigned char)(seq >> (8 * i));
    m[12] = 0xC3;
    m[13] = 0x3C;
    m[14] = 0x96;
    m[15] = 0x69;
}

/* Deterministic payload: marker (truncated if len < 16), then a pattern. */
[[maybe_unused]] static void tu_payload(enum oc_stream s, uint64_t seq,
                                        unsigned char *buf, size_t len)
{
    unsigned char m[TU_MARKER_LEN];
    tu_marker(s, seq, m);
    for (size_t i = 0; i < len; i++)
        buf[i] = i < TU_MARKER_LEN ? m[i] : (unsigned char)(seq * 31u + i * 7u);
}

[[maybe_unused]] static enum oc_rec_type tu_type(uint64_t seq)
{
    return (enum oc_rec_type)(OC_REC_EVENT + (int)(seq % 4));
}

/* Provenance varies with seq to exercise every field, including the
 * extreme session_id and the INJECTED flag. */
[[maybe_unused]] static struct oc_prov tu_prov(uint64_t seq)
{
    static const uint8_t origins[] = {
        OC_ORIGIN_LIVE, OC_ORIGIN_SIMULATED, OC_ORIGIN_REPLAYED,
        OC_ORIGIN_MIXED, OC_ORIGIN_NONE,
    };
    return (struct oc_prov){
        .kind = (uint8_t)(OC_KIND_SENSOR + seq % 3),
        .origin = origins[seq % 5],
        .emitter = (uint8_t)(seq % 4),
        .flags = (seq % 11 == 0) ? OC_PROV_INJECTED : 0,
        .instance = (uint32_t)(seq % 3 == 0 ? UINT32_MAX : seq),
        .session_id = seq % 2 == 0 ? TU_SESSION : UINT64_MAX,
    };
}

[[maybe_unused]] static int tu_prov_eq(const struct oc_prov *a, const struct oc_prov *b)
{
    return a->kind == b->kind && a->origin == b->origin &&
           a->emitter == b->emitter && a->flags == b->flags &&
           a->instance == b->instance && a->session_id == b->session_id;
}

/* Appends one synthetic record whose expected seq is `seq`. Returns the
 * oc_store_append result; a seq mismatch counts as a failed check. */
[[maybe_unused]] static int tu_append(struct oc_store *st, enum oc_stream s,
                                      uint64_t seq, tu_len_fn lenf, unsigned flags)
{
    size_t len = lenf(seq);
    unsigned char *buf = malloc(len > 0 ? len : 1);
    if (buf == NULL) {
        fprintf(stderr, "FAIL %s:%d: malloc\n", __FILE__, __LINE__);
        tu_failures++;
        return -ENOMEM;
    }
    tu_payload(s, seq, buf, len);
    struct oc_prov prov = tu_prov(seq);
    uint64_t got = UINT64_MAX;
    int rc = oc_store_append(st, s, tu_type(seq), &prov, buf, len, flags, &got);
    free(buf);
    if (rc == 0)
        CHECK(got == seq);
    return rc;
}

/* Appends seqs [first, first + n); stops at the first error. */
[[maybe_unused]] static int tu_append_n(struct oc_store *st, enum oc_stream s,
                                        uint64_t first, uint64_t n,
                                        tu_len_fn lenf, unsigned flags)
{
    for (uint64_t i = 0; i < n; i++) {
        int rc = tu_append(st, s, first + i, lenf, flags);
        if (rc != 0)
            return rc;
    }
    return 0;
}

/* ---------- reading back ---------- */

/* Visitor context: checks every visited record against the generator and
 * collects the seqs seen, in order. */
struct tu_collect {
    enum oc_stream stream;
    tu_len_fn lenf;
    uint64_t *seqs;
    size_t n, cap;
    size_t bad;    /* records whose content did not match the generator */
};

[[maybe_unused]] static int tu_collect_visit(const struct oc_record_view *r, void *ctx)
{
    struct tu_collect *c = ctx;
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 64;
        uint64_t *p = realloc(c->seqs, cap * sizeof *p);
        if (p == NULL)
            return -ENOMEM;
        c->seqs = p;
        c->cap = cap;
    }
    c->seqs[c->n++] = r->seq;

    size_t len = c->lenf(r->seq);
    struct oc_prov prov = tu_prov(r->seq);
    int ok = r->stream == c->stream && r->type == tu_type(r->seq) &&
             tu_prov_eq(&r->prov, &prov) && r->len == len &&
             (len == 0 || r->payload != NULL);
    if (ok && len > 0) {
        unsigned char *want = malloc(len);
        if (want == NULL)
            return -ENOMEM;
        tu_payload(c->stream, r->seq, want, len);
        ok = memcmp(want, r->payload, len) == 0;
        free(want);
    }
    if (!ok) {
        fprintf(stderr, "  mismatch: stream %d seq %llu\n", (int)c->stream,
                (unsigned long long)r->seq);
        c->bad++;
    }
    return 0;
}

/* Reads the whole stream into c. */
[[maybe_unused]] static int tu_read_all(struct oc_store *st, enum oc_stream s,
                                        tu_len_fn lenf, struct tu_collect *c)
{
    *c = (struct tu_collect){.stream = s, .lenf = lenf};
    return oc_store_read(st, s, 0, tu_collect_visit, c);
}

[[maybe_unused]] static void tu_collect_free(struct tu_collect *c)
{
    free(c->seqs);
    *c = (struct tu_collect){0};
}

/* True if c holds exactly seqs [first, first + n), in order, all intact. */
[[maybe_unused]] static int tu_is_exact_range(const struct tu_collect *c,
                                              uint64_t first, uint64_t n)
{
    if (c->bad != 0 || c->n != n)
        return 0;
    for (size_t i = 0; i < n; i++) {
        if (c->seqs[i] != first + i)
            return 0;
    }
    return 1;
}

/* ---------- configuration ---------- */

[[maybe_unused]] static struct oc_store_cfg tu_cfg(const char *dir)
{
    struct oc_store_cfg cfg = {
        .dir = dir,
        .batch_records = 32,
        .batch_ms = 500,
    };
    for (int i = 0; i < OC_STREAM_COUNT; i++) {
        cfg.stream[i].segment_bytes = 1u << 20;
        cfg.stream[i].max_segments = 16;
    }
    return cfg;
}

/* Closes a store and reopens it with the same cfg, filling *rep. */
[[maybe_unused]] static int tu_reopen(struct oc_store **st, const struct oc_store_cfg *cfg,
                                      struct oc_store_report *rep)
{
    int rc = oc_store_close(*st);
    *st = NULL;
    if (rc != 0)
        return rc;
    return oc_store_open(cfg, st, rep);
}

#endif
