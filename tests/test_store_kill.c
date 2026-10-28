/* Process-kill tests for oc_store (Mechanism D).
 *
 * A forked child writes synthetic records through oc_store and reports
 * every acknowledgement over a pipe: after a FORCE_SYNC append returns 0,
 * or after oc_store_sync returns a durable_seq. The parent SIGKILLs the
 * child at a seeded-random point (after K acknowledgements, plus a short
 * random delay so the kill can land inside a write, a sync, a segment
 * rollover, or open/recovery itself). It then opens the store and checks:
 *   - every acknowledged record is present, byte-exact;
 *   - the recovered stream is an intact run of seqs with no corrupt or
 *     duplicated record;
 *   - no sealed segment is reported damaged;
 *   - recovery is idempotent (a second open changes nothing).
 *
 * WHAT THESE TESTS PROVE
 *   oc_store recovers to a consistent state when its process dies at an
 *   arbitrary instruction: between records, part-way through building a
 *   batch, during a sync, while rolling over to a new segment, or while
 *   recovering from the previous kill. Recovery never accepts a partial
 *   record, never loses an acknowledged one, and the next run can append
 *   after it.
 *
 * WHAT THEY DO NOT PROVE
 *   - Power-loss durability. SIGKILL ends the process, not the kernel: every
 *     write() that returned is still in the page cache and reaches the file.
 *     A kill test therefore passes even if fsync is never called. Whether
 *     acknowledged records survive power loss is a separate question,
 *     covered only partly by the failed-sync tests (test_store_faults.c) and
 *     the planned controlled power cuts (DESIGN.md §6).
 *   - That the SD card or ext4 honour flush; DESIGN.md §4 states that as an
 *     assumption.
 *   - Torn or reordered sector writes. The kernel will not tear a completed
 *     write() on process death; torn and corrupted data is exercised
 *     instead by the truncation and bit-flip tests in test_store.c.
 *   - The DESIGN.md §6 target of >= 1,000 trials. These are a regression
 *     check; the evaluation runs the same scenario at scale.
 *
 * Inputs are synthetic; nothing here involves sensors or hardware. */
#include "test_util.h"

#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#define KILL_SEED 0x0C5701E5u   /* fixed: failures are reproducible */
#define SEG_BYTES (80u * 1024u) /* ~20 records per segment, so kills hit rollovers */

static size_t len_4k(uint64_t seq)
{
    return 4000 + (size_t)(seq % 7) * 13;
}

static uint32_t rng_state = KILL_SEED;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

enum kill_mode {
    KM_FORCE_SYNC,   /* every append is FORCE_SYNC; ack = that seq */
    KM_BATCHED       /* plain appends, sync every 5th; ack = durable_seq */
};

/* ---------- child ---------- */

static int write_full(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Runs until killed. Exit codes report setup failures, which the parent
 * treats as test failures: 2 open, 3 append/sync, 4 pipe, 5 ran out. */
[[noreturn]] static void child_writer(const struct oc_store_cfg *cfg, enum kill_mode mode,
                                      int ack_fd)
{
    struct oc_store *st = NULL;
    struct oc_store_report rep;
    if (oc_store_open(cfg, &st, &rep) != 0)
        _exit(2);
    uint64_t seq = rep.stream[OC_STREAM_EVIDENCE].next_seq;
    for (uint64_t i = 0; i < 100000; i++, seq++) {
        if (mode == KM_FORCE_SYNC) {
            if (tu_append(st, OC_STREAM_EVIDENCE, seq, len_4k, OC_STORE_FORCE_SYNC) != 0)
                _exit(3);
            if (write_full(ack_fd, &seq, sizeof seq) != 0)
                _exit(4);
        } else {
            if (tu_append(st, OC_STREAM_EVIDENCE, seq, len_4k, 0) != 0)
                _exit(3);
            if (i % 5 == 4) {
                uint64_t dseq;
                if (oc_store_sync(st, OC_STREAM_EVIDENCE, &dseq) != 0)
                    _exit(3);
                if (write_full(ack_fd, &dseq, sizeof dseq) != 0)
                    _exit(4);
            }
        }
    }
    _exit(5);   /* the parent should have killed us long before */
}

/* ---------- parent ---------- */

/* Reads one 8-byte ack. Returns 1 on success, 0 on EOF, -1 on error. */
static int read_ack(int fd, uint64_t *ack)
{
    unsigned char *p = (unsigned char *)ack;
    size_t got = 0;
    while (got < sizeof *ack) {
        ssize_t n = read(fd, p + got, sizeof *ack - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return got == 0 ? 0 : -1;
        got += (size_t)n;
    }
    return 1;
}

/* Starts a writer, waits for k acks, sleeps delay_us, SIGKILLs it, then
 * drains the remaining acks. *acked receives one past the highest
 * acknowledged seq (0 if none). Returns 0 if the child died of SIGKILL. */
static int run_and_kill(const struct oc_store_cfg *cfg, enum kill_mode mode,
                        unsigned k, unsigned delay_us, uint64_t *acked)
{
    int fds[2];
    *acked = 0;
    if (pipe(fds) != 0)
        return -1;
    fflush(NULL);   /* do not duplicate buffered output into the child */
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        child_writer(cfg, mode, fds[1]);
    }
    close(fds[1]);

    uint64_t ack;
    unsigned seen = 0;
    int r = 1;
    while (seen < k && (r = read_ack(fds[0], &ack)) == 1) {
        *acked = ack + 1;
        seen++;
    }
    if (delay_us > 0) {
        struct timespec ts = {.tv_nsec = (long)delay_us * 1000L};
        (void)nanosleep(&ts, NULL);
    }
    (void)kill(pid, SIGKILL);
    int status = 0;
    pid_t w;
    while ((w = waitpid(pid, &status, 0)) < 0 && errno == EINTR)
        ;
    /* Acks written before death still count: the child was told "durable". */
    while (read_ack(fds[0], &ack) == 1)
        *acked = ack + 1;
    close(fds[0]);

    if (w != pid)
        return -1;
    if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
        fprintf(stderr, "  child did not die of SIGKILL (exit status %d, %u/%u acks)\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1, seen, k);
        return -1;
    }
    return 0;
}

/* Opens the store after a kill and checks the stream. On return *n holds
 * the number of recovered records. */
static void check_after_kill(const struct oc_store_cfg *cfg, uint64_t first_required,
                             uint64_t acked, int contiguous, uint64_t *n)
{
    struct oc_store *st = NULL;
    struct oc_store_report rep, rep2;
    struct tu_collect c = {0};
    *n = 0;

    REQUIRE(oc_store_open(cfg, &st, &rep) == 0);
    const struct oc_store_stream_report *r = &rep.stream[OC_STREAM_EVIDENCE];
    CHECK(r->damaged_segments == 0);
    REQUIRE(tu_read_all(st, OC_STREAM_EVIDENCE, len_4k, &c) == 0);
    CHECK(c.bad == 0);
    CHECK(r->records_kept == c.n);
    /* Starts at 0 and strictly increases (no duplicates). Within one writer
     * life the run is also contiguous; across several crash rounds a seq
     * lost in a torn tail may or may not be reused (open decision 4). */
    for (size_t i = 0; i < c.n; i++)
        CHECK(i == 0 ? c.seqs[0] == 0 : c.seqs[i] > c.seqs[i - 1]);
    if (contiguous)
        CHECK(tu_is_exact_range(&c, 0, c.n));
    /* Every acknowledged seq in [first_required, acked) is present. */
    size_t j = 0;
    for (uint64_t s = first_required; s < acked; s++) {
        while (j < c.n && c.seqs[j] < s)
            j++;
        if (!(j < c.n && c.seqs[j] == s)) {
            fprintf(stderr, "  acknowledged seq %llu missing after kill\n",
                    (unsigned long long)s);
            tu_failures++;
            break;
        }
    }
    *n = c.n;

    /* Recovery is idempotent. */
    REQUIRE(tu_reopen(&st, cfg, &rep2) == 0);
    CHECK(rep2.stream[OC_STREAM_EVIDENCE].records_kept == c.n);
    CHECK(rep2.stream[OC_STREAM_EVIDENCE].torn_bytes == 0);
    CHECK(rep2.stream[OC_STREAM_EVIDENCE].damaged_segments == 0);
out:
    tu_collect_free(&c);
    CHECK(oc_store_close(st) == 0);
}

/* Independent trials, each on a fresh store. */
static void kill_trials(enum kill_mode mode, int trials)
{
    for (int t = 0; t < trials; t++) {
        int before = tu_failures;
        char dir[PATH_MAX] = "";
        uint64_t acked = 0, n = 0;
        unsigned k = rng() % 61;            /* 0..60 acks: may kill during open */
        unsigned delay = rng() % 2000;      /* 0..2 ms past the k-th ack */

        REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
        struct oc_store_cfg cfg = tu_cfg(dir);
        cfg.stream[OC_STREAM_EVIDENCE].segment_bytes = SEG_BYTES;
        REQUIRE(run_and_kill(&cfg, mode, k, delay, &acked) == 0);
        CHECK(acked >= k);   /* k acks were received before the kill */
        check_after_kill(&cfg, 0, acked, 1, &n);
        CHECK(n >= acked);
    out:
        tu_rmrf(dir);
        if (tu_failures != before) {
            fprintf(stderr, "  (trial %d: mode %d, k %u, delay %u us, acked %llu)\n",
                    t, (int)mode, k, delay, (unsigned long long)acked);
            return;   /* one diagnosed trial is enough */
        }
    }
}

static void test_kill_force_sync(void)
{
    kill_trials(KM_FORCE_SYNC, 20);
}

static void test_kill_batched(void)
{
    kill_trials(KM_BATCHED, 10);
}

/* Repeated crash-and-restart on one store: each child continues from the
 * recovered next_seq, and every record acknowledged in any round survives
 * all later kills and recoveries. */
static void test_kill_repeated_same_store(void)
{
    enum { ROUNDS = 8 };
    char dir[PATH_MAX] = "";
    uint64_t acked_total = 0;

    REQUIRE(tu_mkdtemp(dir, sizeof dir) == 0);
    struct oc_store_cfg cfg = tu_cfg(dir);
    cfg.stream[OC_STREAM_EVIDENCE].segment_bytes = SEG_BYTES;
    for (int round = 0; round < ROUNDS; round++) {
        int before = tu_failures;
        uint64_t acked = 0, n = 0;
        unsigned k = 1 + rng() % 30;
        unsigned delay = rng() % 2000;
        REQUIRE(run_and_kill(&cfg, KM_FORCE_SYNC, k, delay, &acked) == 0);
        if (acked > acked_total)
            acked_total = acked;
        /* Acks from every earlier round must still be present. */
        check_after_kill(&cfg, 0, acked_total, 0, &n);
        CHECK(n >= acked_total);
        if (tu_failures != before) {
            fprintf(stderr, "  (round %d: k %u, delay %u us)\n", round, k, delay);
            break;
        }
    }
out:
    tu_rmrf(dir);
}

int main(void)
{
    fprintf(stderr, "kill-test seed 0x%08X\n", KILL_SEED);
    RUN(test_kill_force_sync);
    RUN(test_kill_batched);
    RUN(test_kill_repeated_same_store);
    return tu_finish("test_store_kill");
}
