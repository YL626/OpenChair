/* S3C stress / negative multiprocess scenarios. Kept apart from the core
 * suite (test_multiproc.c) because they are heavier and probe the edges of
 * S3B from outside. Same rules: real exec'd processes, SIMULATED samples,
 * loose scaled monotonic deadlines, no kernel-queue-size assertions.
 *
 *   T7  same-instance respawn with a RESET sequence (negative test)
 *   T8  real descriptor exhaustion in the hub: accept-error backoff,
 *       no busy loop, established readers served, recovery, and shutdown
 *       while the listener is suppressed
 *
 * T7 documents a limitation rather than hiding it: S3B alone cannot keep a
 * restarted process out for good. It only makes a reset sequence look like
 * regressions and cuts the connection off. S4 must give every spawn a fresh
 * instance. */
#include "mp_harness.h"

#define SNAP(dst, srcline) snprintf((dst), LINE_CAP, "%s", (srcline))

static int open_raw_clients(int *fds, int n)
{
    int ok = 0;
    for (int i = 0; i < n; i++) {
        fds[i] = -1;
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd < 0)
            continue;
        struct sockaddr_un a = {.sun_family = AF_UNIX};
        snprintf(a.sun_path, sizeof a.sun_path, "%s", g_sock);
        if (connect(fd, (struct sockaddr *)&a, sizeof a) == 0) {
            fds[i] = fd;
            ok++;
        } else {
            close(fd);
        }
    }
    return ok;
}

static void close_raw_clients(int *fds, int n)
{
    for (int i = 0; i < n; i++)
        if (fds[i] >= 0) {
            close(fds[i]);
            fds[i] = -1;
        }
}

static void t7_same_instance_respawn(void)
{
    t_begin("T7 same-instance respawn with reset sequence: regressions, abuse close");
    struct child *hub = spawn_hub(nullptr);
    CHECK(hub != nullptr);
    if (hub == nullptr)
        return;
    bool ok;
    WAIT_UNTIL(ok, hub->ready[0] != '\0', 10000);
    CHECK(ok);
    struct child *a = spawn_reader("light-A", "light", LIGHT_INST, "5", nullptr);
    CHECK(a != nullptr);
    if (a == nullptr)
        return;
    /* run long enough that a fresh process (seq restarting at 0, same 5 ms
     * period) needs a few seconds to catch up with the hub's last_seq */
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "accepted") >= 600, 30000);
    CHECK(ok);
    CHECK(kill(a->pid, SIGKILL) == 0);
    CHECK(reap_within(a, 5000));
    CHECK(killed_by(a, SIGKILL));
    WAIT_UNTIL(ok, kv_eq(hub->src[SRC_LIGHT], "state", "DISCONNECTED"), 10000);
    CHECK(ok);
    char h0[LINE_CAP], hs0[LINE_CAP];
    SNAP(h0, hub->src[SRC_LIGHT]);
    SNAP(hs0, hub->stat);
    const uint64_t acc0 = U(h0, "accepted"), last0 = U(h0, "last_seq");
    CHECK(U(h0, "regr") == 0 && acc0 >= 600);

    const uint64_t t_spawn = mono_ns();
    struct child *b = spawn_reader("light-B", "light", LIGHT_INST, "5", nullptr);   /* SAME instance, seq 0 */
    CHECK(b != nullptr);
    if (b == nullptr)
        return;
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "regr") >= 8 &&
                       U(hub->stat, "abusive_closes") > U(hs0, "abusive_closes"), 20000);
    CHECK(ok);
    char h1[LINE_CAP], hs1[LINE_CAP], rb[LINE_CAP];
    SNAP(h1, hub->src[SRC_LIGHT]);
    SNAP(hs1, hub->stat);
    SNAP(rb, b->stat);                       /* may be older than h1: informational only */
    const uint64_t elapsed_ms = (mono_ns() - t_spawn) / 1000000ull;
    printf("  T7 old last_seq=%llu; %llu ms after respawn: regr=%llu abusive_closes=%llu accepted %llu -> %llu\n",
           (unsigned long long)last0, (unsigned long long)elapsed_ms, (unsigned long long)U(h1, "regr"),
           (unsigned long long)U(hs1, "abusive_closes"), (unsigned long long)acc0,
           (unsigned long long)U(h1, "accepted"));
    /* premise: B (one attempt per 5 ms from seq 0) cannot yet have reached the old last_seq */
    CHECK_MSG(elapsed_ms / 5 + 2 < last0, "premise failed: %llu ms is enough for B to pass seq %llu",
              (unsigned long long)elapsed_ms, (unsigned long long)last0);
    CHECK(U(h1, "regr") >= 8);
    CHECK(U(hs1, "abusive_closes") >= U(hs0, "abusive_closes") + 1);
    CHECK(U(h1, "accepted") == acc0);                       /* none of B's regressed samples was accepted */
    CHECK(U(h1, "gen") == U(h0, "gen") && U(h1, "resets") == 0);   /* hub history was NOT reset */
    CHECK(U(h1, "conns") > U(h0, "conns"));                 /* S3B admitted the HELLO; it is the sequence that failed */
    CHECK(child_alive(hub) && child_alive(b));
    /* B's backoff is not reset by connections that die within 1 s */
    CHECK(U(rb, "connect_attempts") <= 2 + elapsed_ms / 100);
    /* ...and not reset by connections that die within 1 s: delays keep growing 100, 200, 400,
     * 800 ms, so the 5th attempt cannot come sooner than 1500 ms after the spawn. (A reset after
     * every short-lived connection would give ~100 ms each.) */
    WAIT_UNTIL(ok, U(b->stat, "connect_attempts") >= 5, 30000);
    CHECK(ok);
    const uint64_t t5_ms = (mono_ns() - t_spawn) / 1000000ull;
    printf("  T7 B reached its 5th connect attempt %llu ms after spawn (growing backoff >= 1500 ms)\n",
           (unsigned long long)t5_ms);
    CHECK_MSG(t5_ms >= 1500, "backoff was reset by short-lived connections (%llu ms)",
              (unsigned long long)t5_ms);

    /* Informational: once B's own seq passes the old last_seq the hub sees an
     * ordinary GAP and accepts it again. This is the documented limitation. */
    bool again;
    WAIT_UNTIL(again, U(hub->src[SRC_LIGHT], "accepted") > acc0, 40000);
    printf("  T7 LIMITATION %s: S3B accepted the respawned process again once its seq passed %llu "
           "(accepted now %llu); S4 must allocate a fresh instance per spawn\n",
           again ? "OBSERVED" : "not observed in this window", (unsigned long long)last0,
           (unsigned long long)U(hub->src[SRC_LIGHT], "accepted"));
    CHECK(terminate(b, 5000) && exited_ok(b));
    CHECK(terminate(hub, 5000) && exited_ok(hub));
    t_end();
}

static void t8_fd_exhaustion(void)
{
    t_begin("T8 descriptor exhaustion: accept backoff, no busy loop, recovery, shutdown");
    static const char *const hx[] = {"--test-fd-headroom", "3", "--hello-timeout-ms", "6000", nullptr};
    struct child *hub = spawn_hub(hx);
    CHECK(hub != nullptr);
    if (hub == nullptr)
        return;
    bool ok;
    WAIT_UNTIL(ok, hub->ready[0] != '\0', 10000);
    CHECK(ok);
    static const char *const lx[] = {nullptr};
    struct child *light = spawn_reader("light", "light", LIGHT_INST, "200", lx);
    CHECK(light != nullptr);
    if (light == nullptr)
        return;
    WAIT_UNTIL(ok, kv_eq(hub->src[SRC_LIGHT], "state", "ACTIVE"), 15000);
    CHECK(ok);                                  /* headroom left for raw clients: 3 - 1 = 2 */

    int raw[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int connected = open_raw_clients(raw, 6);
    printf("  T8 raw clients connected: %d of 6\n", connected);
    CHECK(connected >= 3);                      /* 2 can be accepted, the rest wait in the backlog */
    WAIT_UNTIL(ok, U(hub->stat, "accept_errors") >= 1, 15000);
    CHECK(ok);

    /* measurement window while the hub is exhausted */
    char s0[LINE_CAP], s1[LINE_CAP];
    SNAP(s0, hub->stat);
    const uint64_t acc0 = U(hub->src[SRC_LIGHT], "accepted");
    const long cpu0 = cpu_ticks(hub->pid);
    const uint64_t w0 = mono_ns();
    run_for(1000);
    const uint64_t win_ms = (mono_ns() - w0) / 1000000ull;
    SNAP(s1, hub->stat);
    const long cpu1 = cpu_ticks(hub->pid);
    const uint64_t d_iter = U(s1, "iter") - U(s0, "iter");
    const uint64_t d_err = U(s1, "accept_errors") - U(s0, "accept_errors");
    printf("  T8 window %llu ms: hub iterations +%llu, accept_errors +%llu, backoffs +%llu, cpu +%ld ticks\n",
           (unsigned long long)win_ms, (unsigned long long)d_iter, (unsigned long long)d_err,
           (unsigned long long)(U(s1, "backoffs") - U(s0, "backoffs")), cpu1 - cpu0);
    /* a spin would be hundreds of thousands of iterations per second */
    CHECK_MSG(d_iter <= 200 * (win_ms / 1000 + 1), "hub spun: +%llu iterations",
              (unsigned long long)d_iter);
    CHECK_MSG(d_err <= 40, "accept retried too eagerly: +%llu", (unsigned long long)d_err);
    CHECK(U(s1, "backoffs") >= 1);
    if (!g_valgrind && cpu0 >= 0 && cpu1 >= 0) {
        long limit = (long)(win_ms * (uint64_t)sysconf(_SC_CLK_TCK) / 1000 / 2);
        CHECK_MSG(cpu1 - cpu0 < limit, "hub cpu +%ld ticks (limit %ld)", cpu1 - cpu0, limit);
    }
    /* the established reader is still served while the listener is suppressed */
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "accepted") >= acc0 + 3, 15000);
    CHECK(ok);
    CHECK(child_alive(hub) && kv_eq(hub->src[SRC_LIGHT], "state", "ACTIVE"));

    /* recovery: release the descriptors, a NEW reader must get through */
    close_raw_clients(raw, 8);
    static const char *const dx[] = {nullptr};
    struct child *dist = spawn_reader("distance", "distance", DIST_INST, "200", dx);
    CHECK(dist != nullptr);
    WAIT_UNTIL(ok, kv_eq(hub->src[SRC_DIST], "state", "ACTIVE"), 40000);
    CHECK(ok);
    CHECK(kv_eq(hub->src[SRC_LIGHT], "state", "ACTIVE"));
    const uint64_t ae1 = U(hub->stat, "accept_errors");
    run_for(600);
    CHECK_MSG(U(hub->stat, "accept_errors") == ae1, "accept errors continued after recovery (%llu -> %llu)",
              (unsigned long long)ae1, (unsigned long long)U(hub->stat, "accept_errors"));
    printf("  T8 recovered: distance ACTIVE, accept_errors stable at %llu\n", (unsigned long long)ae1);

    /* exhaust again (one descriptor left) and shut the hub down while the
     * listener is suppressed */
    int raw2[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int c2 = open_raw_clients(raw2, 5);
    CHECK(c2 >= 3);
    WAIT_UNTIL(ok, U(hub->stat, "accept_errors") > ae1, 15000);
    CHECK(ok);
    const uint64_t t0 = mono_ns();
    CHECK(kill(hub->pid, SIGTERM) == 0);
    CHECK(reap_within(hub, 3000));
    printf("  T8 hub exited %llu us after SIGTERM while in accept backoff\n",
           (unsigned long long)((mono_ns() - t0) / 1000ull));
    CHECK(exited_ok(hub));
    CHECK(hub->summary[0] != '\0' && kv_eq(hub->summary, "exit", "0"));
    CHECK(access(g_sock, F_OK) != 0 && !unix_socket_listed(g_sock));
    close_raw_clients(raw2, 8);
    CHECK(terminate(light, 5000) && exited_ok(light));
    CHECK(terminate(dist, 5000) && exited_ok(dist));
    t_end();
}

int main(void)
{
    harness_init(150);
    t7_same_instance_respawn();
    t8_fd_exhaustion();
    harness_teardown();
    return harness_finish("test_multiproc_stress");
}
