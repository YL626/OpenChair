/* Write and sync failure tests for oc_store (Mechanism D).
 *
 * Linked with -Wl,--wrap for write, pwrite, writev, pwritev, fsync and
 * fdatasync (see Makefile), so calls made by oc_store go through the
 * __wrap_* functions below. Those pass straight through to the real call
 * unless a test arms an injection rule. Every test that arms a rule also
 * checks that the rule actually fired, so an implementation that writes by
 * some other route (e.g. mmap) fails loudly instead of passing vacuously.
 *
 * Two tests use real kernel errors with no mocking: RLIMIT_FSIZE (EFBIG)
 * and a read-only directory (EACCES).
 *
 * What this cannot show: real SD-card I/O errors, or what the kernel does
 * with dirty pages after a genuine failed fsync. It checks only that
 * oc_store reports failures honestly and never acknowledges durability it
 * did not get. Inputs are synthetic. */
#include "test_util.h"

#include <signal.h>
#include <sys/resource.h>
#include <sys/uio.h>

/* ---------- injection ---------- */

enum fi_class { FI_WRITE, FI_SYNC, FI_NCLASS };
enum fi_target { FI_ANY, FI_REG, FI_DIR };

/* One rule per class. Matching calls are let through `skip` times, then
 * fail `times` times (-1 = forever) with `err`, or, if err is 0, are cut to
 * at most short_len bytes. */
struct fi_rule {
    int active;
    enum fi_target target;
    int skip;
    int times;
    int err;
    size_t short_len;
    long injected;   /* how many calls were failed or shortened */
};

static struct fi_rule fi[FI_NCLASS];

enum { FI_PASS, FI_FAIL, FI_SHORT };

static void fi_reset(void)
{
    memset(fi, 0, sizeof fi);
}

static void fi_arm(enum fi_class c, enum fi_target t, int skip, int times, int err)
{
    fi[c] = (struct fi_rule){.active = 1, .target = t, .skip = skip,
                             .times = times, .err = err};
}

static void fi_arm_short(enum fi_target t, int times, size_t short_len)
{
    fi[FI_WRITE] = (struct fi_rule){.active = 1, .target = t, .times = times,
                                    .short_len = short_len};
}

static int fi_matches(enum fi_target t, int fd)
{
    if (t == FI_ANY)
        return 1;
    struct stat sb;
    if (fstat(fd, &sb) != 0)
        return 0;
    return t == FI_DIR ? S_ISDIR(sb.st_mode) : S_ISREG(sb.st_mode);
}

static int fi_check(enum fi_class c, int fd)
{
    struct fi_rule *r = &fi[c];
    if (!r->active || r->times == 0 || !fi_matches(r->target, fd))
        return FI_PASS;
    if (r->skip > 0) {
        r->skip--;
        return FI_PASS;
    }
    if (r->times > 0)
        r->times--;
    r->injected++;
    if (r->err == 0)
        return FI_SHORT;
    errno = r->err;
    return FI_FAIL;
}

ssize_t __real_write(int fd, const void *buf, size_t n);
ssize_t __real_pwrite(int fd, const void *buf, size_t n, off_t off);
ssize_t __real_writev(int fd, const struct iovec *iov, int cnt);
ssize_t __real_pwritev(int fd, const struct iovec *iov, int cnt, off_t off);
int __real_fsync(int fd);
int __real_fdatasync(int fd);

ssize_t __wrap_write(int fd, const void *buf, size_t n);
ssize_t __wrap_pwrite(int fd, const void *buf, size_t n, off_t off);
ssize_t __wrap_writev(int fd, const struct iovec *iov, int cnt);
ssize_t __wrap_pwritev(int fd, const struct iovec *iov, int cnt, off_t off);
int __wrap_fsync(int fd);
int __wrap_fdatasync(int fd);

static size_t fi_short(size_t n)
{
    return n < fi[FI_WRITE].short_len ? n : fi[FI_WRITE].short_len;
}

ssize_t __wrap_write(int fd, const void *buf, size_t n)
{
    switch (fi_check(FI_WRITE, fd)) {
    case FI_FAIL:
        return -1;
    case FI_SHORT:
        return __real_write(fd, buf, fi_short(n));
    default:
        return __real_write(fd, buf, n);
    }
}

ssize_t __wrap_pwrite(int fd, const void *buf, size_t n, off_t off)
{
    switch (fi_check(FI_WRITE, fd)) {
    case FI_FAIL:
        return -1;
    case FI_SHORT:
        return __real_pwrite(fd, buf, fi_short(n), off);
    default:
        return __real_pwrite(fd, buf, n, off);
    }
}

/* A shortened vector write stores only a prefix of the first buffer. */
static struct iovec fi_short_iov(const struct iovec *iov, int cnt)
{
    struct iovec one = {0};
    if (cnt > 0) {
        one = iov[0];
        one.iov_len = fi_short(one.iov_len);
    }
    return one;
}

ssize_t __wrap_writev(int fd, const struct iovec *iov, int cnt)
{
    switch (fi_check(FI_WRITE, fd)) {
    case FI_FAIL:
        return -1;
    case FI_SHORT: {
        struct iovec one = fi_short_iov(iov, cnt);
        return __real_writev(fd, &one, 1);
    }
    default:
        return __real_writev(fd, iov, cnt);
    }
}

ssize_t __wrap_pwritev(int fd, const struct iovec *iov, int cnt, off_t off)
{
    switch (fi_check(FI_WRITE, fd)) {
    case FI_FAIL:
        return -1;
    case FI_SHORT: {
        struct iovec one = fi_short_iov(iov, cnt);
        return __real_pwritev(fd, &one, 1, off);
    }
    default:
        return __real_pwritev(fd, iov, cnt, off);
    }
}

int __wrap_fsync(int fd)
{
    return fi_check(FI_SYNC, fd) == FI_FAIL ? -1 : __real_fsync(fd);
}

int __wrap_fdatasync(int fd)
{
    return fi_check(FI_SYNC, fd) == FI_FAIL ? -1 : __real_fdatasync(fd);
}

/* ---------- helpers ---------- */

/* After a failure, reopen with injection off: whatever recovery keeps must
 * be an intact, contiguous prefix that includes every acknowledged-durable
 * record (seqs < acked) and nothing beyond what was appended (< appended). */
static void check_recovered_prefix(const struct oc_store_cfg *cfg, enum oc_stream s,
                                   uint64_t acked, uint64_t appended)
{
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    struct tu_collect c = {0};

    fi_reset();
    REQUIRE(oc_store_open(cfg, &st, &rep) == 0);
    REQUIRE(tu_read_all(st, s, tu_len_marked, &c) == 0);
    CHECK(c.bad == 0);
    CHECK(c.n >= acked && c.n <= appended);
    CHECK(tu_is_exact_range(&c, 0, c.n));
    CHECK(rep.stream[s].records_kept == c.n);
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
}

/* Once FAILED, nothing may report success again on this handle, even after
 * the fault is gone: no append, no sync, no change in durable counts. */
static void check_stays_failed(struct oc_store *st, enum oc_stream s, int err)
{
    struct oc_store_status before, after;
    uint64_t seq = 777;

    fi_reset();
    REQUIRE(oc_store_status(st, &before) == 0);
    CHECK(before.state == OC_STORE_FAILED);
    CHECK(before.last_errno == err);
    CHECK(tu_append(st, s, 1000, tu_len_marked, 0) < 0);
    CHECK(tu_append(st, s, 1000, tu_len_marked, OC_STORE_FORCE_SYNC) < 0);
    CHECK(oc_store_sync(st, s, &seq) < 0);
    CHECK(seq == 777);
    CHECK(oc_store_tick(st, UINT64_MAX / 2) < 0);
    REQUIRE(oc_store_status(st, &after) == 0);
    CHECK(after.state == OC_STORE_FAILED);
    CHECK(after.stream[s].durable == before.stream[s].durable);
    CHECK(after.stream[s].has_durable == before.stream[s].has_durable);
    CHECK(after.stream[s].durable_seq == before.stream[s].durable_seq);
out:
    return;
}

/* ===== 7. write failures ===== */

/* fdatasync fails on a batched sync: the records become uncertain, not
 * durable, and the store enters FAILED for good. */
static void test_fault_sync_eio_batched(void)
{
    enum { N = 5 };
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct oc_store_status ss;
    int fds = tu_fd_count();

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);
    REQUIRE(tu_append_n(st, OC_STREAM_EVIDENCE, 0, N, tu_len_marked, 0) == 0);

    fi_arm(FI_SYNC, FI_REG, 0, -1, EIO);
    uint64_t dseq = 777;
    CHECK(oc_store_sync(st, OC_STREAM_EVIDENCE, &dseq) == -EIO);
    CHECK(fi[FI_SYNC].injected > 0);
    CHECK(dseq == 777);
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.state == OC_STORE_FAILED);
    CHECK(ss.last_errno == EIO);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].durable == 0);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].has_durable == 0);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].uncertain == N);
    check_stays_failed(st, OC_STREAM_EVIDENCE, EIO);

    (void)oc_store_close(st);   /* may report the failure; must still free */
    st = NULL;
    check_recovered_prefix(&cfg, OC_STREAM_EVIDENCE, 0, N);
out:
    fi_reset();
    (void)oc_store_close(st);
    CHECK(tu_fd_count() == fds);
    tu_rmrf(dir);
}

/* A FORCE_SYNC append whose sync fails: earlier durable records stay
 * durable, durable_seq does not advance, and the new record is uncertain. */
static void test_fault_sync_eio_force_sync(void)
{
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct oc_store_status ss;
    int fds = tu_fd_count();

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);
    REQUIRE(tu_append_n(st, OC_STREAM_TRACE, 0, 3, tu_len_marked, OC_STORE_FORCE_SYNC) == 0);

    fi_arm(FI_SYNC, FI_REG, 0, -1, EIO);
    CHECK(tu_append(st, OC_STREAM_TRACE, 3, tu_len_marked, OC_STORE_FORCE_SYNC) == -EIO);
    CHECK(fi[FI_SYNC].injected > 0);
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.state == OC_STORE_FAILED);
    CHECK(ss.stream[OC_STREAM_TRACE].durable == 3);
    CHECK(ss.stream[OC_STREAM_TRACE].durable_seq == 2);
    CHECK(ss.stream[OC_STREAM_TRACE].uncertain == 1);
    check_stays_failed(st, OC_STREAM_TRACE, EIO);

    (void)oc_store_close(st);
    st = NULL;
    check_recovered_prefix(&cfg, OC_STREAM_TRACE, 3, 4);
out:
    fi_reset();
    (void)oc_store_close(st);
    CHECK(tu_fd_count() == fds);
    tu_rmrf(dir);
}

/* write fails with ENOSPC: same contract as a failed sync. */
static void test_fault_write_enospc(void)
{
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct oc_store_status ss;
    int fds = tu_fd_count();

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);
    REQUIRE(tu_append_n(st, OC_STREAM_EVIDENCE, 0, 4, tu_len_marked, OC_STORE_FORCE_SYNC) == 0);

    fi_arm(FI_WRITE, FI_REG, 0, -1, ENOSPC);
    CHECK(tu_append(st, OC_STREAM_EVIDENCE, 4, tu_len_marked, OC_STORE_FORCE_SYNC) == -ENOSPC);
    CHECK(fi[FI_WRITE].injected > 0);
    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.state == OC_STORE_FAILED);
    CHECK(ss.last_errno == ENOSPC);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].durable_seq == 3);
    check_stays_failed(st, OC_STREAM_EVIDENCE, ENOSPC);

    (void)oc_store_close(st);
    st = NULL;
    check_recovered_prefix(&cfg, OC_STREAM_EVIDENCE, 4, 5);
out:
    fi_reset();
    (void)oc_store_close(st);
    CHECK(tu_fd_count() == fds);
    tu_rmrf(dir);
}

/* Short writes and EINTR are normal results, not failures: the store must
 * finish the write and the data must read back intact. */
static void test_fault_short_write_and_eintr(void)
{
    enum { N = 20 };
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct oc_store_status ss;
    struct oc_store_report rep;
    struct tu_collect c = {0};
    int fds = tu_fd_count();

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);

    fi_arm_short(FI_REG, -1, 7);   /* every write stores at most 7 bytes */
    REQUIRE(tu_append_n(st, OC_STREAM_EVIDENCE, 0, N / 2, tu_len_marked, OC_STORE_FORCE_SYNC) == 0);
    CHECK(fi[FI_WRITE].injected > 0);

    fi_arm(FI_WRITE, FI_REG, 0, 1, EINTR);   /* one interrupted write */
    REQUIRE(tu_append_n(st, OC_STREAM_EVIDENCE, N / 2, N / 2, tu_len_marked, OC_STORE_FORCE_SYNC) == 0);
    CHECK(fi[FI_WRITE].injected == 1);
    fi_reset();

    REQUIRE(oc_store_status(st, &ss) == 0);
    CHECK(ss.state == OC_STORE_OK);
    CHECK(ss.stream[OC_STREAM_EVIDENCE].durable == N);
    REQUIRE(tu_reopen(&st, &cfg, &rep) == 0);
    CHECK(rep.stream[OC_STREAM_EVIDENCE].records_kept == N);
    CHECK(rep.stream[OC_STREAM_EVIDENCE].torn_bytes == 0);
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, tu_len_marked, &c) == 0);
    CHECK(tu_is_exact_range(&c, 0, N));
out:
    fi_reset();
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
    CHECK(tu_fd_count() == fds);
    tu_rmrf(dir);
}

/* The directory fsync that makes a new segment's name durable fails: at
 * open (creating the stream directories) and at segment rollover. */
static void test_fault_dir_fsync(void)
{
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    int fds = tu_fd_count();

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);

    /* At open of a fresh directory. */
    fi_arm(FI_SYNC, FI_DIR, 0, -1, EIO);
    CHECK(oc_store_open(&cfg, &st, NULL) == -EIO);
    CHECK(st == NULL);
    CHECK(fi[FI_SYNC].injected > 0);
    fi_reset();
    CHECK(tu_fd_count() == fds);

    /* At rollover: 80 KiB segments fill after ~20 records of ~4 KiB. */
    cfg.stream[OC_STREAM_EVIDENCE].segment_bytes = 80u * 1024u;
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);
    fi_arm(FI_SYNC, FI_DIR, 0, -1, EIO);
    uint64_t acked = 0;
    int rc = 0;
    while (acked < 100) {
        unsigned char body[4000];
        tu_payload(OC_STREAM_EVIDENCE, acked, body, sizeof body);
        struct oc_prov prov = tu_prov(acked);
        rc = oc_store_append(st, OC_STREAM_EVIDENCE, tu_type(acked), &prov, body,
                             sizeof body, OC_STORE_FORCE_SYNC, NULL);
        if (rc != 0)
            break;
        acked++;
    }
    CHECK(rc == -EIO);
    CHECK(acked > 0 && acked < 100);
    CHECK(fi[FI_SYNC].injected > 0);
    check_stays_failed(st, OC_STREAM_EVIDENCE, EIO);
out:
    fi_reset();
    (void)oc_store_close(st);
    CHECK(tu_fd_count() == fds);
    tu_rmrf(dir);
}

/* Real kernel error, no mocking: a file-size limit makes write() fail with
 * EFBIG (SIGXFSZ is ignored so the process is not killed). */
static void test_fault_real_efbig(void)
{
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    struct rlimit old_lim;
    struct sigaction ign = {.sa_handler = SIG_IGN}, old_sa;
    int limited = 0, sa_saved = 0;
    uint64_t acked = 0;
    int rc = 0;
    int fds = tu_fd_count();

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);

    REQUIRE(getrlimit(RLIMIT_FSIZE, &old_lim) == 0);
    REQUIRE(sigaction(SIGXFSZ, &ign, &old_sa) == 0);
    sa_saved = 1;
    struct rlimit lim = {.rlim_cur = 8192, .rlim_max = old_lim.rlim_max};
    REQUIRE(setrlimit(RLIMIT_FSIZE, &lim) == 0);
    limited = 1;
    while (acked < 1000) {
        rc = tu_append(st, OC_STREAM_EVIDENCE, acked, tu_len_marked, OC_STORE_FORCE_SYNC);
        if (rc != 0)
            break;
        acked++;
    }
    REQUIRE(setrlimit(RLIMIT_FSIZE, &old_lim) == 0);
    limited = 0;
    CHECK(rc == -EFBIG);
    CHECK(acked > 0 && acked < 1000);
    check_stays_failed(st, OC_STREAM_EVIDENCE, EFBIG);

    (void)oc_store_close(st);
    st = NULL;
    check_recovered_prefix(&cfg, OC_STREAM_EVIDENCE, acked, acked + 1);
out:
    if (limited)
        (void)setrlimit(RLIMIT_FSIZE, &old_lim);
    if (sa_saved)
        (void)sigaction(SIGXFSZ, &old_sa, NULL);
    (void)oc_store_close(st);
    CHECK(tu_fd_count() == fds);
    tu_rmrf(dir);
}

/* Real kernel error, no mocking: a read-only store directory. Root ignores
 * permissions, so the case is skipped (and says so) when run as root. */
static void test_fault_real_eacces(void)
{
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    int fds = tu_fd_count();

    if (geteuid() == 0) {
        fprintf(stderr, "  SKIP: running as root, permissions not enforced\n");
        return;
    }
    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    REQUIRE(chmod(dir, 0500) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    CHECK(oc_store_open(&cfg, &st, NULL) == -EACCES);
    CHECK(st == NULL);
out:
    (void)oc_store_close(st);
    CHECK(tu_fd_count() == fds);
    tu_rmrf(dir);   /* restores write permission first */
}

/* A full clean cycle, repeated, leaves no descriptor behind. */
static void test_no_fd_leak(void)
{
    char dir[PATH_MAX] = "";
    struct oc_store *st = NULL;
    int fds = tu_fd_count();

    REQUIRE(fds > 0);
    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    for (int round = 0; round < 20; round++) {
        REQUIRE(oc_store_open(&cfg, &st, NULL) == 0);
        REQUIRE(tu_append_n(st, OC_STREAM_TRACE, (uint64_t)round * 5, 5, tu_len_marked,
                            OC_STORE_FORCE_SYNC) == 0);
        REQUIRE(oc_store_close(st) == 0);
        st = NULL;
        CHECK(tu_fd_count() == fds);
    }
out:
    (void)oc_store_close(st);
    tu_rmrf(dir);
}

int main(void)
{
    RUN(test_fault_sync_eio_batched);
    RUN(test_fault_sync_eio_force_sync);
    RUN(test_fault_write_enospc);
    RUN(test_fault_short_write_and_eintr);
    RUN(test_fault_dir_fsync);
    RUN(test_fault_real_efbig);
    RUN(test_fault_real_eacces);
    RUN(test_no_fd_leak);
    return tu_finish("test_store_faults");
}
