/* Storage-failure behaviour of oc_capture over a REAL oc_store. Failures are
 * injected with link-time wrappers (same technique as tests/test_store_faults.c,
 * which is not modified): the write and sync calls oc_store makes are routed
 * through __wrap_* below and pass straight through unless a rule is armed.
 * All samples are SIMULATED.
 *
 * What is checked: the failure is classified correctly (append / tick / sync,
 * never mixed), every later accepted sample is counted as accepted_not_stored,
 * the capture makes NO further write/sync call after the failure (no silent
 * retry), nothing uncertain is reported durable, sensing (the callbacks)
 * keeps working, and the verdict is never PASS. */
#define _GNU_SOURCE   /* before any system header: nftw in capture_rig.h */
#include <sys/uio.h>

#include "capture_rig.h"

/* ---------- injection (counts every wrapped call so retries are visible) ---------- */

static struct {
    int armed_write, armed_sync;    /* 1 = fail from now on */
    long write_calls, sync_calls;
    long write_after_fail, sync_after_fail;
    int failed;                     /* a failure was injected */
} fi;

static void fi_reset(void)
{
    memset(&fi, 0, sizeof fi);
}

static int fi_write(void)
{
    fi.write_calls++;
    if (fi.failed)
        fi.write_after_fail++;
    if (fi.armed_write) {
        fi.failed = 1;
        errno = EIO;
        return 1;
    }
    return 0;
}

static int fi_sync(void)
{
    fi.sync_calls++;
    if (fi.failed)
        fi.sync_after_fail++;
    if (fi.armed_sync) {
        fi.failed = 1;
        errno = EIO;
        return 1;
    }
    return 0;
}

ssize_t __real_write(int fd, const void *buf, size_t n);
ssize_t __real_pwrite(int fd, const void *buf, size_t n, off_t off);
ssize_t __real_writev(int fd, const struct iovec *iov, int cnt);
ssize_t __real_pwritev(int fd, const struct iovec *iov, int cnt, off_t off);
int __real_fsync(int fd);
int __real_fdatasync(int fd);

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    return fi_write() ? -1 : __real_write(fd, buf, n);
}
ssize_t __wrap_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    return fi_write() ? -1 : __real_pwrite(fd, buf, n, off);
}
ssize_t __wrap_writev(int fd, const struct iovec *iov, int cnt)
{
    return fi_write() ? -1 : __real_writev(fd, iov, cnt);
}
ssize_t __wrap_pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
    return fi_write() ? -1 : __real_pwritev(fd, iov, cnt, off);
}
int __wrap_fsync(int fd)
{
    return fi_sync() ? -1 : __real_fsync(fd);
}
int __wrap_fdatasync(int fd)
{
    return fi_sync() ? -1 : __real_fdatasync(fd);
}

static void open_window(struct rig *r)
{
    rig_event(r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_LIGHT);
    rig_event(r, OC_HUB_EV_SOURCE_UP, OC_SOURCE_DISTANCE);
    rig_feed(r, OC_SOURCE_LIGHT, nullptr);
    r->now += MS;
    rig_feed(r, OC_SOURCE_DISTANCE, nullptr);
}

static void check_identity(const struct oc_capture_result *res)
{
    for (int s = OC_SOURCE_LIGHT; s <= OC_SOURCE_DISTANCE; s++) {
        const struct oc_capture_src *x = &res->src[s];
        CHECK(x->accepted_window == x->append_ok + x->append_fail + x->accepted_not_stored);
    }
}

/* ---- F1: a TICK sync fails; later samples are counted, not stored ---- */
static void test_tick_failure(void)
{
    fi_reset();
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    open_window(&r);
    for (unsigned i = 0; i < 40; i++) {
        r.now += MS;
        rig_feed(&r, i % 2 ? OC_SOURCE_LIGHT : OC_SOURCE_DISTANCE, nullptr);
    }
    const uint64_t stored_before = oc_capture_src(r.c, OC_SOURCE_LIGHT)->append_ok +
                                   oc_capture_src(r.c, OC_SOURCE_DISTANCE)->append_ok;
    CHECK(stored_before >= 32);                       /* enough pending for the batch policy */
    fi.armed_sync = 1;                                /* the next sync (inside tick) fails */
    r.now += MS;
    CHECK(oc_capture_tick(r.c, r.now) != 0);
    CHECK(oc_capture_flags(r.c) & OC_CAPF_TICK_FAILED);
    const long w = fi.write_calls, s = fi.sync_calls;
    for (unsigned i = 0; i < 60; i++) {               /* sensing continues; nothing is stored */
        r.now += MS;
        rig_feed(&r, i % 2 ? OC_SOURCE_LIGHT : OC_SOURCE_DISTANCE, nullptr);
        if (i % 10 == 0)
            CHECK(oc_capture_tick(r.c, r.now) != 0);  /* reports the failure, does no I/O */
    }
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.verdict == OC_CAP_FAIL);
    CHECK((res.flags & OC_CAPF_TICK_FAILED) && (res.flags & OC_CAPF_STORE_FAILED) &&
          (res.flags & OC_CAPF_NOT_STORED));
    CHECK(!(res.flags & OC_CAPF_APPEND_FAILED) && !(res.flags & OC_CAPF_SYNC_FAILED));   /* not misclassified */
    CHECK(res.tick_fail == 1 && res.sync_fail == 0);
    uint64_t not_stored = 0, fails = 0;
    for (int x = OC_SOURCE_LIGHT; x <= OC_SOURCE_DISTANCE; x++) {
        not_stored += res.src[x].accepted_not_stored;
        fails += res.src[x].append_fail;
    }
    CHECK(not_stored >= 60 && fails == 0);
    check_identity(&res);
    CHECK(res.store_failed && res.store_errno == EIO);
    /* no silent retry: not one write or sync call after the failure */
    CHECK(fi.write_after_fail == 0 && fi.sync_after_fail == 0);
    CHECK(fi.write_calls == w && fi.sync_calls == s);
    /* nothing uncertain is claimed durable */
    CHECK(res.status_valid && res.status.state == OC_STORE_FAILED);
    CHECK(res.status.stream[OC_STREAM_TRACE].uncertain > 0);
    CHECK(res.status.stream[OC_STREAM_TRACE].durable == 0 && !res.status.stream[OC_STREAM_TRACE].has_durable);
    CHECK(res.close_rc != 0 && !res.reopen_done);     /* no verification reopen on a failed store */
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

/* ---- F2: an APPEND fails (segment rollover writes) ---- */
static void test_append_failure(void)
{
    fi_reset();
    struct rig r;
    struct oc_capture_cfg cfg = rig_cfg(&r);
    cfg.trace_segment_bytes = 65624;                   /* the smallest segment: rolls over after ~1000 records */
    cfg.trace_segments = 4;
    CHECK(rig_init(&r, &cfg) == 0);
    open_window(&r);
    fi.armed_write = 1;                                /* the first write (at rollover) fails */
    unsigned i = 0;
    for (; i < 1400; i++) {                            /* no ticks: the failure must come from append */
        r.now += MS / 4;
        rig_feed(&r, i % 2 ? OC_SOURCE_LIGHT : OC_SOURCE_DISTANCE, nullptr);
    }
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.verdict == OC_CAP_FAIL);
    CHECK((res.flags & OC_CAPF_APPEND_FAILED) && (res.flags & OC_CAPF_STORE_FAILED) &&
          (res.flags & OC_CAPF_NOT_STORED));
    CHECK(!(res.flags & OC_CAPF_TICK_FAILED) && res.tick_fail == 0);
    uint64_t fails = 0, not_stored = 0;
    for (int x = OC_SOURCE_LIGHT; x <= OC_SOURCE_DISTANCE; x++) {
        fails += res.src[x].append_fail;
        not_stored += res.src[x].accepted_not_stored;
    }
    CHECK(fails == 1);                                 /* exactly the append that hit the error */
    CHECK(not_stored > 0);
    check_identity(&res);
    CHECK(fi.write_after_fail == 0 && fi.sync_after_fail == 0);
    CHECK(res.status.stream[OC_STREAM_TRACE].durable <= res.records_appended);
    CHECK(res.status.stream[OC_STREAM_TRACE].uncertain > 0 || res.status.state == OC_STORE_FAILED);
    oc_capture_free(r.c);
    rig_rmdir(&r);
}

/* ---- F3: the FINAL sync fails ---- */
static void test_sync_failure_at_finish(void)
{
    fi_reset();
    struct rig r;
    CHECK(rig_init(&r, nullptr) == 0);
    open_window(&r);
    for (unsigned i = 0; i < 20; i++) {                /* < 32 pending: nothing syncs before finish */
        r.now += MS;
        rig_feed(&r, i % 2 ? OC_SOURCE_LIGHT : OC_SOURCE_DISTANCE, nullptr);
    }
    fi.armed_sync = 1;
    struct oc_capture_result res;
    CHECK(oc_capture_finish(r.c, &res) == 0);
    CHECK(res.verdict == OC_CAP_FAIL);
    CHECK(res.sync_rc != 0 && res.sync_fail == 1 && res.tick_fail == 0);
    CHECK((res.flags & OC_CAPF_SYNC_FAILED) && (res.flags & OC_CAPF_STORE_FAILED));
    CHECK(!(res.flags & OC_CAPF_APPEND_FAILED) && !(res.flags & OC_CAPF_TICK_FAILED));
    CHECK(res.status.stream[OC_STREAM_TRACE].durable == 0);          /* never claimed durable */
    CHECK(res.status.stream[OC_STREAM_TRACE].uncertain > 0);
    CHECK(res.rb[0].done && !res.reopen_done);
    CHECK(fi.sync_after_fail == 0 && fi.write_after_fail == 0);
    oc_capture_free(r.c);

    /* What survives is decided by recovery on the next open, not by this run:
     * whatever it keeps must be whole and decodable; nothing is invented. */
    fi_reset();
    struct oc_store_cfg sc = rig_store_cfg(&r);
    struct oc_store *st = nullptr;
    struct oc_store_report rep;
    CHECK(oc_store_open(&sc, &st, &rep) == 0);
    CHECK(rep.stream[OC_STREAM_TRACE].records_kept <= res.records_appended);
    CHECK(rep.stream[OC_STREAM_TRACE].damaged_segments == 0);
    if (st != nullptr)
        (void)oc_store_close(st);
    rig_rmdir(&r);
}

int main(void)
{
    RUN(test_tick_failure);
    RUN(test_append_failure);
    RUN(test_sync_failure_at_finish);
    if (g_failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    puts("test_capture_faults: all passed");
    return 0;
}
