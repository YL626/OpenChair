/* oc_store: crash-consistent append-only storage (Mechanism D).
 *
 * Public API and data states: include/oc_store.h. File format, sync policy
 * and recovery rules: docs/oc_store.md. Byte layout: oc_store_format.c.
 *
 * Ownership: struct oc_store owns every descriptor and buffer below; nothing
 * is shared with the caller except the payload pointer handed to a read
 * visitor, which is valid only during that call. All descriptors are
 * O_CLOEXEC. Single writer, not thread-safe (only the hub uses it). */
#define _POSIX_C_SOURCE 200809L   /* fdatasync, openat, fdopendir, pread/pwrite */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "oc_store.h"
#include "oc_store_format.h"

/* Pending-record buffer per stream. Holds at least one maximum frame; when
 * an append would overflow it, the buffer is written out first, so memory
 * stays bounded even if the caller never ticks. */
#define WBUF_BYTES (256u * 1024u)
static_assert(WBUF_BYTES >= OCF_FRAME_MAX_BYTES, "buffer must hold one maximum frame");

/* A segment must hold its header, one maximum frame and its footer. The
 * upper bound keeps the footer's u32 record count from overflowing. */
#define SEG_MIN_BYTES (OCF_SEG_HDR_BYTES + OCF_FRAME_MAX_BYTES + OCF_FOOTER_BYTES)
#define SEG_MAX_BYTES ((size_t)1 << 30)

/* Segment files are NNNNNNNN.seg: 8 decimal digits. */
#define SEG_NAME_LEN 12
#define SEG_ID_MAX 99999999u

static const char *const stream_dir_name[OC_STREAM_COUNT] = {"evidence", "trace"};

/* One segment file, as validated by recovery or extended by this process.
 * Records [first_seq, first_seq + nrec) occupy bytes [OCF_SEG_HDR_BYTES,
 * valid_end). Bytes past valid_end are the footer (sealed) or nothing. */
struct seg {
    uint32_t id;
    uint64_t first_seq;
    uint64_t nrec;
    off_t valid_end;
    bool damaged;   /* older segment that failed validation; never modified */
};

struct stream {
    enum oc_stream id;
    struct oc_store_stream_cfg cfg;
    int dirfd;               /* <dir>/<stream>, O_DIRECTORY */
    struct seg *segs;        /* id order; segs[nseg - 1] is the active segment */
    size_t nseg, cap;
    int fd;                  /* active segment, O_RDWR; -1 if none */

    /* Records appended but not yet written: buf_nrec frames in buf_len bytes,
     * to be written at segs[nseg - 1].valid_end. */
    unsigned char *buf;
    size_t buf_len;
    uint64_t buf_nrec;

    uint64_t next_seq;
    /* Counts of records appended through this handle (oc_store_status). */
    uint64_t n_buffered, n_written, n_durable, n_uncertain;
    /* Highest durable seq, including records kept by recovery at open. */
    bool has_durable;
    uint64_t durable_seq;
    /* Batch timer: started by the first tick that sees pending records. */
    bool timer_running;
    uint64_t pending_since;
};

struct oc_store {
    int dirfd;
    uint32_t batch_records;
    uint64_t batch_ns;
    enum oc_store_state state;
    int last_errno;
    struct stream st[OC_STREAM_COUNT];
};

/* ---------- small helpers ---------- */

static struct seg *active_seg(struct stream *s)
{
    return &s->segs[s->nseg - 1];
}

/* Builds a segment file name; returns -EOVERFLOW if the id has run out. */
static int seg_name(uint32_t id, char name[SEG_NAME_LEN + 1])
{
    if (id > SEG_ID_MAX)
        return -EOVERFLOW;
    int n = snprintf(name, SEG_NAME_LEN + 1, "%08" PRIu32 ".seg", id);
    return n == SEG_NAME_LEN ? 0 : -EOVERFLOW;
}

/* Parses NNNNNNNN.seg; returns false for any other name. */
static bool seg_parse_name(const char *name, uint32_t *id)
{
    if (strlen(name) != SEG_NAME_LEN || strcmp(name + 8, ".seg") != 0)
        return false;
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9')
            return false;
        v = v * 10 + (uint32_t)(name[i] - '0');
    }
    *id = v;
    return true;
}

/* Writes all of buf at off, continuing after short writes and EINTR.
 * Returns 0 or -errno. */
static int pwrite_full(int fd, const void *buf, size_t len, off_t off)
{
    const unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = pwrite(fd, p, len, off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            return -EIO;   /* no progress on a regular file: treat as I/O error */
        p += n;
        len -= (size_t)n;
        off += n;
    }
    return 0;
}

/* Reads up to len bytes at off. Returns the number read (less than len only
 * at end of file) or -errno. */
static ssize_t pread_full(int fd, void *buf, size_t len, off_t off)
{
    unsigned char *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(fd, p + got, len - got, off + (off_t)got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    return (ssize_t)got;
}

static int close_checked(int fd)
{
    return close(fd) == 0 ? 0 : -errno;
}

/* Grows the segment table to hold one more entry. */
static int segs_reserve(struct stream *s)
{
    if (s->nseg < s->cap)
        return 0;
    size_t cap = s->cap ? s->cap * 2 : 16;
    struct seg *p = realloc(s->segs, cap * sizeof *p);
    if (p == NULL)
        return -ENOMEM;
    s->segs = p;
    s->cap = cap;
    return 0;
}

static bool stream_has_records(const struct stream *s)
{
    if (s->buf_nrec > 0)
        return true;
    for (size_t i = 0; i < s->nseg; i++) {
        if (s->segs[i].nrec > 0)
            return true;
    }
    return false;
}

/* ---------- failure ---------- */

/* Enters FAILED (DESIGN.md §4 "Failure semantics"): every record not yet
 * confirmed durable becomes uncertain, pending records are discarded, and
 * no later call acknowledges anything. Returns err (a negative errno) so
 * callers can `return fail(st, rc);`. The first failure's errno is kept. */
static int fail(struct oc_store *st, int err)
{
    if (st->state == OC_STORE_OK) {
        st->state = OC_STORE_FAILED;
        st->last_errno = -err;
    }
    for (int i = 0; i < OC_STREAM_COUNT; i++) {
        struct stream *s = &st->st[i];
        s->n_uncertain += s->n_buffered + s->n_written;
        s->n_buffered = 0;
        s->n_written = 0;
        s->buf_len = 0;
        s->buf_nrec = 0;
        s->timer_running = false;
    }
    return err;
}

/* ---------- write path ---------- */

/* Writes the pending buffer at the end of the active segment: buffered
 * records become written (not yet durable). */
static int stream_write_buf(struct oc_store *st, struct stream *s)
{
    if (s->buf_len == 0)
        return 0;
    struct seg *a = active_seg(s);
    int rc = pwrite_full(s->fd, s->buf, s->buf_len, a->valid_end);
    if (rc != 0)
        return fail(st, rc);
    a->valid_end += (off_t)s->buf_len;
    a->nrec += s->buf_nrec;
    s->n_written += s->buf_nrec;
    s->n_buffered -= s->buf_nrec;
    s->buf_len = 0;
    s->buf_nrec = 0;
    return 0;
}

/* Called after a successful fdatasync of the active segment. Every record
 * appended so far is in it, so all of them are now durable. */
static void stream_mark_durable(struct stream *s)
{
    if (s->n_written > 0) {
        s->n_durable += s->n_written;
        s->n_written = 0;
        s->has_durable = true;
        s->durable_seq = s->next_seq - 1;
    }
    s->timer_running = false;
}

/* Writes and syncs everything pending in the stream. */
static int stream_flush(struct oc_store *st, struct stream *s)
{
    int rc = stream_write_buf(st, s);
    if (rc != 0)
        return rc;
    if (s->n_written > 0 && fdatasync(s->fd) != 0)
        return fail(st, -errno);
    stream_mark_durable(s);
    return 0;
}

/* Writes pending records and, if the active segment holds any record, its
 * footer; then syncs both with one fdatasync. The footer is not a record,
 * so valid_end stays at the end of the last record. */
static int stream_seal(struct oc_store *st, struct stream *s)
{
    int rc = stream_write_buf(st, s);
    if (rc != 0)
        return rc;
    struct seg *a = active_seg(s);
    if (a->nrec == 0)
        return 0;   /* nothing to seal; an empty segment has no footer */
    unsigned char f[OCF_FOOTER_BYTES];
    ocf_put_footer(f, &(struct ocf_footer){
        .count = (uint32_t)a->nrec,
        .last_seq = a->first_seq + a->nrec - 1,
    });
    rc = pwrite_full(s->fd, f, sizeof f, a->valid_end);
    if (rc != 0)
        return fail(st, rc);
    if (fdatasync(s->fd) != 0)
        return fail(st, -errno);
    stream_mark_durable(s);
    return 0;
}

/* Creates segment `id` starting at first_seq and makes it the active
 * segment: header written and synced, then the directory synced, before any
 * record goes in (DESIGN.md §4). Returns -errno and leaves s->fd at -1 on
 * failure; a file left behind is either a valid empty segment or one with an
 * invalid header, which recovery removes. */
static int seg_create(struct stream *s, uint32_t id, uint64_t first_seq)
{
    char name[SEG_NAME_LEN + 1];
    int rc = seg_name(id, name);
    if (rc != 0)
        return rc;
    rc = segs_reserve(s);
    if (rc != 0)
        return rc;
    int fd = openat(s->dirfd, name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return -errno;
    unsigned char hdr[OCF_SEG_HDR_BYTES];
    ocf_put_seg_hdr(hdr, &(struct ocf_seg_hdr){
        .stream = (uint8_t)s->id, .id = id, .first_seq = first_seq,
    });
    rc = pwrite_full(fd, hdr, sizeof hdr, 0);
    if (rc == 0 && fdatasync(fd) != 0)
        rc = -errno;
    if (rc == 0 && fsync(s->dirfd) != 0)
        rc = -errno;
    if (rc != 0) {
        (void)close(fd);   /* already failing; the first error is reported */
        return rc;
    }
    s->segs[s->nseg++] = (struct seg){
        .id = id, .first_seq = first_seq, .nrec = 0, .valid_end = OCF_SEG_HDR_BYTES,
    };
    s->fd = fd;
    return 0;
}

/* Deletes the oldest segments of a ring stream until one more fits in
 * max_segments. The directory is synced by the seg_create that follows; a
 * crash in between at worst leaves a deleted segment in place. */
static int stream_drop_oldest(struct stream *s)
{
    size_t drop = 0;
    int rc = 0;
    while (rc == 0 && s->nseg - drop >= s->cfg.max_segments) {
        char name[SEG_NAME_LEN + 1];
        rc = seg_name(s->segs[drop].id, name);
        if (rc == 0 && unlinkat(s->dirfd, name, 0) != 0)
            rc = -errno;
        if (rc == 0)
            drop++;
    }
    memmove(s->segs, s->segs + drop, (s->nseg - drop) * sizeof *s->segs);
    s->nseg -= drop;
    return rc;
}

/* Seals the full active segment and opens the next one. Bounded storage:
 * evidence is never deleted, so past its cap the record is refused with
 * -EDQUOT before any I/O and the store stays OK; trace is a ring, so its
 * oldest segments are deleted to make room. */
static int stream_rollover(struct oc_store *st, struct stream *s)
{
    if (s->id == OC_STREAM_EVIDENCE && s->nseg >= s->cfg.max_segments)
        return -EDQUOT;
    int rc = stream_seal(st, s);
    if (rc != 0)
        return rc;
    rc = close_checked(s->fd);
    s->fd = -1;
    if (rc != 0)
        return fail(st, rc);
    uint32_t id = active_seg(s)->id + 1;   /* before the ring may delete it */
    if (s->id == OC_STREAM_TRACE) {
        rc = stream_drop_oldest(s);
        if (rc != 0)
            return fail(st, rc);
    }
    rc = seg_create(s, id, s->next_seq);
    if (rc != 0)
        return fail(st, rc);
    return 0;
}

/* ---------- recovery ---------- */

/* Fills s->segs with the stream's segment files in id order. Names that are
 * not NNNNNNNN.seg are ignored. Header fields are filled in later. */
static int segs_list(struct stream *s)
{
    int fd = fcntl(s->dirfd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
        return -errno;
    DIR *d = fdopendir(fd);   /* owns fd from here on */
    if (d == NULL) {
        int err = -errno;
        (void)close(fd);
        return err;
    }
    rewinddir(d);
    int rc = 0;
    for (;;) {
        errno = 0;
        struct dirent *e = readdir(d);
        if (e == NULL) {
            rc = -errno;   /* 0 at the end of the directory */
            break;
        }
        uint32_t id;
        if (!seg_parse_name(e->d_name, &id))
            continue;
        rc = segs_reserve(s);
        if (rc != 0)
            break;
        s->segs[s->nseg++] = (struct seg){.id = id};
    }
    if (closedir(d) != 0 && rc == 0)
        rc = -errno;
    if (rc != 0)
        return rc;
    for (size_t i = 1; i < s->nseg; i++) {   /* insertion sort: few segments */
        struct seg cur = s->segs[i];
        size_t j = i;
        for (; j > 0 && s->segs[j - 1].id > cur.id; j--)
            s->segs[j] = s->segs[j - 1];
        s->segs[j] = cur;
    }
    return 0;
}

static int seg_open(const struct stream *s, uint32_t id, int flags)
{
    char name[SEG_NAME_LEN + 1];
    int rc = seg_name(id, name);
    if (rc != 0)
        return rc;
    int fd = openat(s->dirfd, name, flags | O_CLOEXEC);
    return fd < 0 ? -errno : fd;
}

/* Reads and validates a segment header. Returns 1 if valid (filling
 * first_seq), 0 if invalid, or -errno. */
static int seg_read_hdr(const struct stream *s, int fd, struct seg *g)
{
    unsigned char hdr[OCF_SEG_HDR_BYTES];
    ssize_t n = pread_full(fd, hdr, sizeof hdr, 0);
    if (n < 0)
        return (int)n;
    struct ocf_seg_hdr h;
    if ((size_t)n != sizeof hdr || !ocf_get_seg_hdr(hdr, &h) ||
        h.stream != (uint8_t)s->id || h.id != g->id)
        return 0;
    g->first_seq = h.first_seq;
    return 1;
}

/* Scans records from the end of the header, accepting each one only if its
 * frame header is valid, its seq is exactly the next expected one, it lies
 * wholly inside the file and its CRC matches. Stops at the first record that
 * fails; sets g->nrec and g->valid_end to the valid prefix. buf must hold
 * OCF_FRAME_MAX_BYTES. */
static int seg_scan(int fd, off_t size, struct seg *g, unsigned char *buf)
{
    off_t off = OCF_SEG_HDR_BYTES;
    uint64_t n = 0;
    while (size - off >= (off_t)OCF_FRAME_OVERHEAD) {
        ssize_t got = pread_full(fd, buf, OCF_FRAME_HEAD_BYTES, off);
        if (got < 0)
            return (int)got;
        struct ocf_frame_head h;
        if (!ocf_get_frame_head(buf, &h) || h.seq != g->first_seq + n)
            break;
        off_t flen = (off_t)OCF_FRAME_OVERHEAD + (off_t)h.len;
        if (flen > size - off)
            break;
        size_t rest = (size_t)h.len + OCF_CRC_BYTES;
        got = pread_full(fd, buf + OCF_FRAME_HEAD_BYTES, rest, off + OCF_FRAME_HEAD_BYTES);
        if (got < 0)
            return (int)got;
        if ((size_t)got != rest || !ocf_frame_crc_ok(buf, h.len))
            break;
        off += flen;
        n++;
    }
    g->nrec = n;
    g->valid_end = off;
    return 0;
}

/* True if the bytes after the valid records are exactly a footer that
 * matches them. */
static int seg_footer_ok(int fd, off_t size, const struct seg *g, bool *ok)
{
    *ok = false;
    if (g->nrec == 0 || size - g->valid_end != (off_t)OCF_FOOTER_BYTES)
        return 0;
    unsigned char f[OCF_FOOTER_BYTES];
    ssize_t n = pread_full(fd, f, sizeof f, g->valid_end);
    if (n < 0)
        return (int)n;
    struct ocf_footer ft;
    *ok = (size_t)n == sizeof f && ocf_get_footer(f, &ft) && ft.count == g->nrec &&
          ft.last_seq == g->first_seq + g->nrec - 1;
    return 0;
}

static int file_size(int fd, off_t *size)
{
    struct stat sb;
    if (fstat(fd, &sb) != 0)
        return -errno;
    *size = sb.st_size;
    return 0;
}

/* Running totals while recovery walks a stream's segments in order. */
struct recov {
    struct oc_store_stream_report *rep;
    bool have_last;
    uint64_t last_kept;   /* highest seq kept so far, if have_last */
};

/* Counts the segment's valid records into the report, including any seqs
 * missing between the previous kept record and this segment's first. */
static void recov_account(struct recov *r, const struct seg *g)
{
    if (g->nrec == 0)
        return;
    if (r->have_last && g->first_seq > r->last_kept + 1)
        r->rep->seq_gaps += g->first_seq - r->last_kept - 1;
    r->rep->records_kept += g->nrec;
    r->last_kept = g->first_seq + g->nrec - 1;
    r->have_last = true;
}

/* True if the segment's first seq does not follow the records already kept,
 * which our writer never produces: its records would duplicate kept seqs. */
static bool recov_overlaps(const struct recov *r, const struct seg *g)
{
    return r->have_last && g->first_seq <= r->last_kept;
}

/* An older segment was sealed before its successor was created, so any
 * failed check means damage, not a torn write: it is kept unmodified (opened
 * read-only), marked damaged, and only its valid prefix is used. */
static int recover_sealed(const struct stream *s, struct seg *g, struct recov *r,
                          unsigned char *buf)
{
    int fd = seg_open(s, g->id, O_RDONLY);
    if (fd < 0)
        return fd;
    off_t size = 0;
    int rc = file_size(fd, &size);
    int valid = rc == 0 ? seg_read_hdr(s, fd, g) : rc;
    bool footer_ok = false;
    if (valid < 0) {
        rc = valid;
    } else if (valid == 0 || recov_overlaps(r, g)) {
        g->nrec = 0;
        g->valid_end = OCF_SEG_HDR_BYTES;
        g->damaged = true;
    } else {
        rc = seg_scan(fd, size, g, buf);
        if (rc == 0)
            rc = seg_footer_ok(fd, size, g, &footer_ok);
        g->damaged = !footer_ok;
    }
    int crc = close_checked(fd);
    if (rc == 0)
        rc = crc;
    if (rc != 0)
        return rc;
    if (g->damaged)
        r->rep->damaged_segments++;
    recov_account(r, g);
    return 0;
}

/* The newest segment may end in a torn write. Everything after its last
 * valid record is truncated: a matching footer (left by a clean close) is
 * dropped silently, anything else counts as torn bytes. The segment is then
 * synced, so every record recovery keeps is durable when open returns, and
 * it becomes the active segment. Its header was validated by the caller. */
static int recover_newest(struct stream *s, struct seg *g, struct recov *r)
{
    int fd = seg_open(s, g->id, O_RDWR);
    if (fd < 0)
        return fd;
    off_t size = 0;
    int rc = file_size(fd, &size);
    if (rc == 0)
        rc = seg_scan(fd, size, g, s->buf);
    bool footer_ok = false;
    if (rc == 0)
        rc = seg_footer_ok(fd, size, g, &footer_ok);
    if (rc == 0 && size > g->valid_end) {
        if (!footer_ok)
            r->rep->torn_bytes += (uint64_t)(size - g->valid_end);
        if (ftruncate(fd, g->valid_end) != 0)
            rc = -errno;
    }
    if (rc == 0 && fdatasync(fd) != 0)
        rc = -errno;
    if (rc != 0) {
        (void)close(fd);
        return rc;
    }
    s->fd = fd;
    recov_account(r, g);
    return 0;
}

/* Recovery for one stream (DESIGN.md §4 "Framing and recovery"). Never
 * fabricates a record: everything kept passed every check in seg_scan. */
static int stream_recover(struct stream *s, struct oc_store_stream_report *rep)
{
    struct recov r = {.rep = rep};
    bool dir_dirty = false;
    int rc = segs_list(s);
    if (rc != 0)
        return rc;

    /* A newest segment with an invalid header was never durably created
     * (its header is synced before any record is written), so remove it. */
    while (s->nseg > 0) {
        struct seg *g = active_seg(s);
        int fd = seg_open(s, g->id, O_RDONLY);
        if (fd < 0)
            return fd;
        int valid = seg_read_hdr(s, fd, g);
        rc = close_checked(fd);
        if (valid < 0)
            return valid;
        if (rc != 0)
            return rc;
        if (valid)
            break;
        char name[SEG_NAME_LEN + 1];
        rc = seg_name(g->id, name);
        if (rc == 0 && unlinkat(s->dirfd, name, 0) != 0)
            rc = -errno;
        if (rc != 0)
            return rc;
        s->nseg--;
        rep->removed_segments++;
        dir_dirty = true;
    }

    for (size_t i = 0; i + 1 < s->nseg; i++) {
        rc = recover_sealed(s, &s->segs[i], &r, s->buf);
        if (rc != 0)
            return rc;
    }

    if (s->nseg > 0) {
        struct seg *g = active_seg(s);
        if (recov_overlaps(&r, g)) {
            /* Cannot append after records that duplicate kept seqs: keep the
             * file untouched as damaged and start a new segment below. */
            g->damaged = true;
            g->valid_end = OCF_SEG_HDR_BYTES;
            rep->damaged_segments++;
        } else {
            rc = recover_newest(s, g, &r);
            if (rc != 0)
                return rc;
        }
    }

    if (s->fd < 0) {
        uint32_t id = s->nseg > 0 ? active_seg(s)->id + 1 : 0;
        rc = seg_create(s, id, r.have_last ? r.last_kept + 1 : 0);   /* syncs the dir */
        if (rc != 0)
            return rc;
    } else if (dir_dirty && fsync(s->dirfd) != 0) {
        return -errno;
    }

    struct seg *a = active_seg(s);
    s->next_seq = a->first_seq + a->nrec;
    s->has_durable = r.have_last;
    s->durable_seq = r.last_kept;
    rep->next_seq = s->next_seq;
    return 0;
}

/* ---------- open / close ---------- */

static bool cfg_valid(const struct oc_store_cfg *cfg)
{
    if (cfg->dir == NULL || cfg->dir[0] == '\0' || cfg->batch_records == 0 ||
        cfg->batch_ms == 0)
        return false;
    for (int i = 0; i < OC_STREAM_COUNT; i++) {
        const struct oc_store_stream_cfg *c = &cfg->stream[i];
        if (c->segment_bytes < SEG_MIN_BYTES || c->segment_bytes > SEG_MAX_BYTES ||
            c->max_segments == 0)
            return false;
    }
    return true;
}

/* Releases everything the store owns. Returns the first close() error. */
static int store_free(struct oc_store *st)
{
    int rc = 0;
    for (int i = 0; i < OC_STREAM_COUNT; i++) {
        struct stream *s = &st->st[i];
        if (s->fd >= 0) {
            int r = close_checked(s->fd);
            if (rc == 0)
                rc = r;
        }
        if (s->dirfd >= 0) {
            int r = close_checked(s->dirfd);
            if (rc == 0)
                rc = r;
        }
        free(s->segs);
        free(s->buf);
    }
    if (st->dirfd >= 0) {
        int r = close_checked(st->dirfd);
        if (rc == 0)
            rc = r;
    }
    free(st);
    return rc;
}

/* Creates the stream directories if needed; syncs the parent if it changed. */
static int store_make_dirs(struct oc_store *st)
{
    bool created = false;
    for (int i = 0; i < OC_STREAM_COUNT; i++) {
        if (mkdirat(st->dirfd, stream_dir_name[i], 0755) == 0)
            created = true;
        else if (errno != EEXIST)
            return -errno;
    }
    if (created && fsync(st->dirfd) != 0)
        return -errno;
    return 0;
}

int oc_store_open(const struct oc_store_cfg *cfg, struct oc_store **out,
                  struct oc_store_report *report)
{
    if (out != NULL)
        *out = NULL;
    if (cfg == NULL || out == NULL || !cfg_valid(cfg))
        return -EINVAL;

    struct oc_store *st = calloc(1, sizeof *st);
    if (st == NULL)
        return -ENOMEM;
    st->dirfd = -1;
    st->batch_records = cfg->batch_records;
    st->batch_ns = (uint64_t)cfg->batch_ms * 1000000u;
    st->state = OC_STORE_OK;
    for (int i = 0; i < OC_STREAM_COUNT; i++) {
        st->st[i].id = (enum oc_stream)i;
        st->st[i].cfg = cfg->stream[i];
        st->st[i].dirfd = -1;
        st->st[i].fd = -1;
    }

    struct oc_store_report rep = {0};
    int rc = 0;
    st->dirfd = open(cfg->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (st->dirfd < 0)
        rc = -errno;
    if (rc == 0)
        rc = store_make_dirs(st);
    for (int i = 0; rc == 0 && i < OC_STREAM_COUNT; i++) {
        struct stream *s = &st->st[i];
        s->dirfd = openat(st->dirfd, stream_dir_name[i], O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (s->dirfd < 0) {
            rc = -errno;
            break;
        }
        s->buf = malloc(WBUF_BYTES);
        if (s->buf == NULL) {
            rc = -ENOMEM;
            break;
        }
        rc = stream_recover(s, &rep.stream[i]);
    }
    if (rc != 0) {
        (void)store_free(st);   /* already failing; report the first error */
        return rc;
    }
    if (report != NULL)
        *report = rep;
    *out = st;
    return 0;
}

int oc_store_close(struct oc_store *st)
{
    if (st == NULL)
        return 0;
    int rc = 0;
    for (int i = 0; i < OC_STREAM_COUNT && st->state == OC_STORE_OK; i++)
        rc = stream_seal(st, &st->st[i]);
    if (st->state != OC_STORE_OK)
        rc = -st->last_errno;
    int frc = store_free(st);
    return rc != 0 ? rc : frc;
}

/* ---------- append / tick / sync / status ---------- */

int oc_store_append(struct oc_store *st, enum oc_stream stream,
                    enum oc_rec_type type, const struct oc_prov *prov,
                    const void *payload, size_t len, unsigned flags,
                    uint64_t *seq_out)
{
    if (st == NULL || (unsigned)stream >= OC_STREAM_COUNT || !ocf_type_valid((unsigned)type) ||
        prov == NULL || !ocf_prov_valid(prov) || (payload == NULL && len > 0) ||
        len > OC_STORE_MAX_PAYLOAD || (flags & ~OC_STORE_FORCE_SYNC) != 0)
        return -EINVAL;
    if (st->state != OC_STORE_OK)
        return -EBADFD;

    struct stream *s = &st->st[stream];
    size_t flen = OCF_FRAME_OVERHEAD + len;
    uint64_t end = (uint64_t)active_seg(s)->valid_end + s->buf_len + flen + OCF_FOOTER_BYTES;
    int rc = 0;
    if (end > s->cfg.segment_bytes)
        rc = stream_rollover(st, s);
    if (rc == 0 && s->buf_len + flen > WBUF_BYTES)
        rc = stream_write_buf(st, s);
    if (rc != 0)
        return rc;

    uint64_t seq = s->next_seq;
    ocf_put_frame(s->buf + s->buf_len, &(struct ocf_frame_head){
        .type = (uint8_t)type, .len = (uint32_t)len, .seq = seq, .prov = *prov,
    }, payload);
    s->buf_len += flen;
    s->buf_nrec++;
    s->n_buffered++;
    s->next_seq++;

    if ((flags & OC_STORE_FORCE_SYNC) != 0) {
        rc = stream_flush(st, s);
        if (rc != 0)
            return rc;
    }
    if (seq_out != NULL)
        *seq_out = seq;
    return 0;
}

int oc_store_tick(struct oc_store *st, uint64_t now_mono_ns)
{
    if (st == NULL)
        return -EINVAL;
    if (st->state != OC_STORE_OK)
        return -EBADFD;
    for (int i = 0; i < OC_STREAM_COUNT; i++) {
        struct stream *s = &st->st[i];
        uint64_t pending = s->n_buffered + s->n_written;
        if (pending == 0) {
            s->timer_running = false;
            continue;
        }
        if (!s->timer_running) {
            s->timer_running = true;
            s->pending_since = now_mono_ns;
        }
        bool due = pending >= st->batch_records ||
                   (now_mono_ns >= s->pending_since &&
                    now_mono_ns - s->pending_since >= st->batch_ns);
        if (due) {
            int rc = stream_flush(st, s);
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}

int oc_store_sync(struct oc_store *st, enum oc_stream stream,
                  uint64_t *durable_seq)
{
    if (st == NULL || (unsigned)stream >= OC_STREAM_COUNT)
        return -EINVAL;
    if (st->state != OC_STORE_OK)
        return -EBADFD;
    struct stream *s = &st->st[stream];
    if (!stream_has_records(s))
        return -ENODATA;
    int rc = stream_flush(st, s);
    if (rc != 0)
        return rc;
    if (durable_seq != NULL)
        *durable_seq = s->durable_seq;
    return 0;
}

int oc_store_status(const struct oc_store *st, struct oc_store_status *out)
{
    if (st == NULL || out == NULL)
        return -EINVAL;
    *out = (struct oc_store_status){.state = st->state, .last_errno = st->last_errno};
    for (int i = 0; i < OC_STREAM_COUNT; i++) {
        const struct stream *s = &st->st[i];
        out->stream[i] = (struct oc_store_stream_status){
            .buffered = s->n_buffered,
            .written = s->n_written,
            .durable = s->n_durable,
            .uncertain = s->n_uncertain,
            .has_durable = s->has_durable,
            .durable_seq = s->durable_seq,
        };
    }
    return 0;
}

/* ---------- read ---------- */

/* Visits the records of one segment with seq >= from_seq. Only the records
 * recovery validated or this process wrote are read, and each is checked
 * again: a file that changed underneath the store yields -EBADMSG rather
 * than a fabricated record. */
static int read_seg(struct stream *s, size_t i, uint64_t from_seq,
                    oc_store_visit_fn visit, void *ctx, unsigned char *buf)
{
    const struct seg *g = &s->segs[i];
    bool active = i + 1 == s->nseg && s->fd >= 0;
    int fd = active ? s->fd : seg_open(s, g->id, O_RDONLY);
    if (fd < 0)
        return fd;
    int rc = 0;
    off_t off = OCF_SEG_HDR_BYTES;
    for (uint64_t k = 0; k < g->nrec && rc == 0; k++) {
        uint64_t want = g->first_seq + k;
        struct ocf_frame_head h;
        ssize_t n = pread_full(fd, buf, OCF_FRAME_HEAD_BYTES, off);
        if (n < 0) {
            rc = (int)n;
            break;
        }
        if ((size_t)n != OCF_FRAME_HEAD_BYTES || !ocf_get_frame_head(buf, &h) ||
            h.seq != want) {
            rc = -EBADMSG;
            break;
        }
        size_t rest = (size_t)h.len + OCF_CRC_BYTES;
        n = pread_full(fd, buf + OCF_FRAME_HEAD_BYTES, rest, off + OCF_FRAME_HEAD_BYTES);
        if (n < 0) {
            rc = (int)n;
            break;
        }
        if ((size_t)n != rest || !ocf_frame_crc_ok(buf, h.len)) {
            rc = -EBADMSG;
            break;
        }
        off += (off_t)(OCF_FRAME_OVERHEAD + h.len);
        if (want < from_seq)
            continue;
        struct oc_record_view v = {
            .stream = s->id,
            .seq = want,
            .type = h.type,
            .prov = h.prov,
            .payload = buf + OCF_FRAME_HEAD_BYTES,
            .len = h.len,
        };
        rc = visit(&v, ctx);
    }
    if (!active) {
        int crc = close_checked(fd);
        if (rc == 0)
            rc = crc;
    }
    return rc;
}

int oc_store_read(struct oc_store *st, enum oc_stream stream, uint64_t from_seq,
                  oc_store_visit_fn visit, void *ctx)
{
    if (st == NULL || (unsigned)stream >= OC_STREAM_COUNT || visit == NULL)
        return -EINVAL;
    struct stream *s = &st->st[stream];
    unsigned char *buf = malloc(OCF_FRAME_MAX_BYTES);
    if (buf == NULL)
        return -ENOMEM;
    int rc = 0;
    for (size_t i = 0; i < s->nseg && rc == 0; i++) {
        const struct seg *g = &s->segs[i];
        if (g->nrec == 0 || g->first_seq + g->nrec <= from_seq)
            continue;
        rc = read_seg(s, i, from_seq, visit, ctx, buf);
    }
    free(buf);
    return rc;
}
