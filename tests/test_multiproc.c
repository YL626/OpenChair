/* S3C core multiprocess suite: three REAL Linux processes (hub, light reader,
 * distance reader) exec'd from `openchair --role ...`, talking over the real
 * S3A AF_UNIX SOCK_SEQPACKET transport into the real S3B hub.
 *
 * Every sample is SIMULATED. Passing shows IPC/hub behaviour on this kernel
 * only; it is not evidence about the Raspberry Pi or any sensor.
 *
 * Scenarios (a single set of processes lives through T1..T6, T9):
 *   T1  reader with no hub: bounded backoff, no busy loop
 *   T2  hub + both handshakes, distinct pids, authz is test config
 *   T3  two separately counted SIMULATED streams
 *   T4  backpressure: nonblocking send keeps its schedule, drops are counted
 *   T5  same-process reconnect keeps instance and sequence
 *   T6  SIGKILL one reader: hub and other reader unaffected, degraded reported
 *   T9  clean shutdown, exact accounting, socket removed
 *   T11 shutdown while a reader is in reconnect backoff
 * Descriptor exhaustion and same-instance respawn are in test_multiproc_stress.
 *
 * No assertion depends on a kernel queue capacity or on tight scheduling:
 * waits are predicates under scaled monotonic deadlines. CPU and descriptor
 * count assertions are skipped under Valgrind. */
#include "mp_harness.h"

#define SNAP(dst, srcline) snprintf((dst), LINE_CAP, "%s", (srcline))

static struct child *hub, *light, *dist;

static void t1_no_hub_backoff(void)
{
    t_begin("T1 light reader with no hub: bounded backoff, no busy loop");
    static const char *const ex[] = {"--drop-conn-on-sigusr1", nullptr};
    light = spawn_reader("light", "light", LIGHT_INST, "5", ex);
    CHECK(light != nullptr);
    if (light == nullptr)
        return;
    const uint64_t t0 = mono_ns();
    bool ok;
    WAIT_UNTIL(ok, light->stat_n >= 1, 5000);
    CHECK(ok);
    run_for(1500);
    char st[LINE_CAP];
    SNAP(st, light->stat);
    const uint64_t elapsed_ms = (mono_ns() - t0) / 1000000ull;
    const uint64_t att = U(st, "connect_attempts");
    printf("  T1 elapsed=%llu ms connect_attempts=%llu attempted=%llu unsent_disc=%llu\n",
           (unsigned long long)elapsed_ms, (unsigned long long)att,
           (unsigned long long)U(st, "attempted"), (unsigned long long)U(st, "unsent_disc"));
    CHECK(kv_eq(st, "up", "0"));
    CHECK_MSG(att >= 2, "reader must keep retrying (attempts=%llu)", (unsigned long long)att);
    /* The reader waits AT LEAST 100, 200, 400, 800, 1600, 2000... ms between attempts, so
     * the count cannot exceed what that ladder allows in the elapsed time (a constant
     * 100 ms retry would exceed it; a busy loop by orders of magnitude). */
    uint64_t max_att = 1, ladder_sum = 0, ladder_d = 100;
    while (ladder_sum + ladder_d <= elapsed_ms) {
        ladder_sum += ladder_d;
        max_att++;
        ladder_d = ladder_d * 2 > 2000 ? 2000 : ladder_d * 2;
    }
    CHECK_MSG(att <= max_att, "attempts=%llu in %llu ms, ladder allows %llu", (unsigned long long)att,
              (unsigned long long)elapsed_ms, (unsigned long long)max_att);
    CHECK(U(st, "sent") == 0 && U(st, "bp_drop") == 0 && U(st, "send_fail") == 0);
    CHECK(U(st, "attempted") > 0 && U(st, "unsent_disc") == U(st, "attempted"));
    CHECK(reader_line_consistent(st));
    CHECK(U(st, "connects") == 0);   /* overruns are timing-dependent under instrumentation: not asserted */
    if (!g_valgrind) {
        long ticks = cpu_ticks(light->pid);
        long limit = (long)(elapsed_ms * (uint64_t)sysconf(_SC_CLK_TCK) / 1000 / 2);
        printf("  T1 reader cpu=%ld ticks (limit %ld)\n", ticks, limit);
        CHECK(ticks >= 0 && ticks < limit);
    }
    t_end();
}

static void t2_handshakes(void)
{
    t_begin("T2 hub + both handshakes, distinct pids, test-config authorization");
    hub = spawn_hub(nullptr);
    CHECK(hub != nullptr);
    if (hub == nullptr || light == nullptr)
        return;
    bool ok;
    WAIT_UNTIL(ok, hub->ready[0] != '\0', 10000);
    CHECK(ok);
    CHECK(kv_eq(hub->ready, "authz", "TEST-CONFIG-NOT-SUPERVISOR"));
    CHECK(kv_eq(hub->ready, "origin", "SIMULATED"));
    CHECK(U(hub->ready, "pid") == (uint64_t)hub->pid);

    WAIT_UNTIL(ok, light->ready[0] != '\0', 10000);   /* waits out its backoff (<= 2 s) */
    CHECK(ok);
    static const char *const ex[] = {"--fault-every", "10", nullptr};
    dist = spawn_reader("distance", "distance", DIST_INST, "10", ex);
    CHECK(dist != nullptr);
    if (dist == nullptr)
        return;
    WAIT_UNTIL(ok, dist->ready[0] != '\0', 10000);
    CHECK(ok);
    WAIT_UNTIL(ok, kv_eq(hub->src[SRC_LIGHT], "state", "ACTIVE") &&
                       kv_eq(hub->src[SRC_DIST], "state", "ACTIVE"), 10000);
    CHECK(ok);

    CHECK(light->pid != hub->pid && dist->pid != hub->pid && light->pid != dist->pid);
    CHECK(light->pid != getpid() && dist->pid != getpid() && hub->pid != getpid());
    CHECK(U(light->ready, "pid") == (uint64_t)light->pid);
    CHECK(U(dist->ready, "pid") == (uint64_t)dist->pid);
    CHECK(kv_eq(light->ready, "origin", "SIMULATED") && kv_eq(dist->ready, "origin", "SIMULATED"));
    CHECK(U(hub->src[SRC_LIGHT], "conns") == 1 && U(hub->src[SRC_DIST], "conns") == 1);
    CHECK(access(g_sock, F_OK) == 0);
    CHECK(unix_socket_listed(g_sock));
    printf("  T2 pids hub=%d light=%d distance=%d (harness=%d)\n", (int)hub->pid, (int)light->pid,
           (int)dist->pid, (int)getpid());
    t_end();
}

static int base_fd_hub, base_fd_light, base_fd_dist;

static void t3_two_streams(void)
{
    t_begin("T3 two separately counted SIMULATED streams");
    bool ok;
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "accepted") >= 100 &&
                       U(hub->src[SRC_DIST], "accepted") >= 100, 15000);
    CHECK(ok);
    char hl[LINE_CAP], hd[LINE_CAP], rl[LINE_CAP], rd[LINE_CAP];
    SNAP(hl, hub->src[SRC_LIGHT]);
    SNAP(hd, hub->src[SRC_DIST]);
    const unsigned ls = light->stat_n, ds = dist->stat_n;
    WAIT_UNTIL(ok, light->stat_n > ls && dist->stat_n > ds, 5000);   /* reader snapshots AFTER the hub's */
    CHECK(ok);
    SNAP(rl, light->stat);
    SNAP(rd, dist->stat);

    CHECK(kv_eq(hl, "src", "light") && U(hl, "inst") == LIGHT_INST);
    CHECK(kv_eq(hd, "src", "distance") && U(hd, "inst") == DIST_INST);
    CHECK(kv_eq(hl, "state", "ACTIVE") && kv_eq(hd, "state", "ACTIVE"));
    CHECK(kv_eq(hl, "avail", "1") && kv_eq(hd, "avail", "1"));
    CHECK(U(hl, "gen") == 1 && U(hd, "gen") == 1);
    CHECK(U(hl, "origin_bad") == 0 && U(hd, "origin_bad") == 0);
    CHECK(U(hl, "check_fail") == 0 && U(hd, "check_fail") == 0);   /* origin + timestamps sane */
    CHECK(U(hl, "ts_back") == 0 && U(hd, "ts_back") == 0);
    CHECK(U(hl, "dup") == 0 && U(hd, "dup") == 0 && U(hl, "regr") == 0 && U(hd, "regr") == 0);
    CHECK(U(hl, "ident_mismatch") == 0 && U(hd, "ident_mismatch") == 0);
    /* the callback saw exactly what the tracker accepted, per source */
    CHECK(U(hl, "cb_samples") == U(hl, "accepted") && U(hd, "cb_samples") == U(hd, "accepted"));
    CHECK(hub_src_consistent(hl) && hub_src_consistent(hd));
    /* non-OK statuses are counted, never turned into readings */
    CHECK(U(hd, "st_other") > 0 && U(hd, "st_ok") > 0);
    CHECK(U(hl, "st_other") == 0);
    /* reader counters: every attempt has exactly one outcome */
    CHECK(reader_line_consistent(rl) && reader_line_consistent(rd));
    /* hub never counts more than the reader (later snapshot) says it produced */
    CHECK(U(hl, "accepted") <= U(rl, "sent") && U(hl, "last_seq") + 1 <= U(rl, "attempted"));
    CHECK(U(hd, "accepted") <= U(rd, "sent") && U(hd, "last_seq") + 1 <= U(rd, "attempted"));
    CHECK(kv_eq(hub->stat, "degraded", "0"));
    printf("  T3 light accepted=%llu lost=%llu unobs=%llu | distance accepted=%llu (st_other=%llu) lost=%llu\n",
           (unsigned long long)U(hl, "accepted"), (unsigned long long)U(hl, "lost"),
           (unsigned long long)U(hl, "unobs"), (unsigned long long)U(hd, "accepted"),
           (unsigned long long)U(hd, "st_other"), (unsigned long long)U(hd, "lost"));

    base_fd_hub = count_fds(hub->pid);
    base_fd_light = count_fds(light->pid);
    base_fd_dist = count_fds(dist->pid);
    printf("  T3 fd counts hub=%d light=%d distance=%d\n", base_fd_hub, base_fd_light, base_fd_dist);
    if (!g_valgrind) {
        CHECK_MSG(base_fd_hub == 7, "hub fds=%d (0,1,2,listener,signalfd,2 conns)", base_fd_hub);
        CHECK_MSG(base_fd_light == 5, "light fds=%d (0,1,2,signalfd,conn)", base_fd_light);
        CHECK_MSG(base_fd_dist == 5, "distance fds=%d", base_fd_dist);
        if (g_failures != g_t_fail0) {
            dump_fds(hub);
            dump_fds(light);
        }
    }
    t_end();
}

static void t4_backpressure(void)
{
    t_begin("T4 backpressure: nonblocking send, schedule kept, drops counted");
    bool ok;
    const uint64_t bp0 = U(light->stat, "bp_drop");
    char hl0[LINE_CAP];
    SNAP(hl0, hub->src[SRC_LIGHT]);
    CHECK(kill(hub->pid, SIGSTOP) == 0);       /* the hub stops reading; readers must not stall */
    WAIT_UNTIL(ok, U(light->stat, "bp_drop") > bp0, 20000);
    CHECK(ok);
    const uint64_t att1 = U(light->stat, "attempted");
    WAIT_UNTIL(ok, U(light->stat, "attempted") >= att1 + 40, 10000);   /* sampling continued */
    CHECK(ok);
    char rl[LINE_CAP];
    SNAP(rl, light->stat);
    CHECK(reader_line_consistent(rl));
    CHECK(U(rl, "bp_drop") > bp0);
    CHECK(U(rl, "send_fail") == 0 && kv_eq(rl, "up", "1"));
    printf("  T4 while hub stopped: attempted=%llu sent=%llu bp_drop=%llu overruns=%llu\n",
           (unsigned long long)U(rl, "attempted"), (unsigned long long)U(rl, "sent"),
           (unsigned long long)U(rl, "bp_drop"), (unsigned long long)U(rl, "overruns"));
    CHECK(kill(hub->pid, SIGCONT) == 0);
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "lost") > U(hl0, "lost"), 20000);   /* gap becomes visible */
    CHECK(ok);
    WAIT_UNTIL(ok, kv_eq(hub->src[SRC_LIGHT], "state", "ACTIVE") &&
                       kv_eq(hub->src[SRC_DIST], "state", "ACTIVE"), 10000);
    CHECK(ok);
    char hl[LINE_CAP];
    SNAP(hl, hub->src[SRC_LIGHT]);
    CHECK(U(hl, "dup") == 0 && U(hl, "regr") == 0 && hub_src_consistent(hl));
    CHECK(U(hl, "gen") == 1 && U(hl, "resets") == 0);
    printf("  T4 after SIGCONT: hub light lost %llu -> %llu\n", (unsigned long long)U(hl0, "lost"),
           (unsigned long long)U(hl, "lost"));
    t_end();
}

static void t5_reconnect(void)
{
    t_begin("T5 same-process reconnect keeps instance and sequence");
    bool ok;
    char hl0[LINE_CAP], rl0[LINE_CAP];
    SNAP(hl0, hub->src[SRC_LIGHT]);
    SNAP(rl0, light->stat);
    const pid_t pid0 = light->pid;
    const uint64_t t_drop = mono_ns();
    CHECK(kill(light->pid, SIGUSR1) == 0);       /* testing control: drop the socket, reconnect */
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "reconn") > U(hl0, "reconn") &&
                       kv_eq(hub->src[SRC_LIGHT], "state", "ACTIVE") &&
                       U(light->stat, "connects") == U(rl0, "connects") + 1 &&
                       kv_eq(light->stat, "up", "1"), 15000);
    CHECK(ok);
    const uint64_t reconnect_ms = (mono_ns() - t_drop) / 1000000ull;
    /* The connection had been up far longer than 1 s, so the backoff was reset: the first retry
     * waits 100 ms. Had it not been reset, the delay left over from T1 would be 2000 ms. */
    printf("  T5 reconnect took %llu ms (stable connection => 100 ms first delay)\n",
           (unsigned long long)reconnect_ms);
    if (!g_valgrind)
        CHECK_MSG(reconnect_ms < 1200, "reconnect took %llu ms", (unsigned long long)reconnect_ms);
    const uint64_t acc_now = U(hub->src[SRC_LIGHT], "accepted");
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "accepted") >= acc_now + 20, 10000);
    CHECK(ok);
    char hl[LINE_CAP], rl[LINE_CAP];
    SNAP(hl, hub->src[SRC_LIGHT]);
    SNAP(rl, light->stat);
    CHECK(light->pid == pid0 && child_alive(light));
    CHECK(U(rl, "inst") == LIGHT_INST && U(hl, "inst") == LIGHT_INST);
    CHECK(U(hl, "gen") == U(hl0, "gen") && U(hl, "resets") == U(hl0, "resets"));   /* no reset */
    CHECK(U(hl, "regr") == 0 && U(hl, "dup") == 0);
    CHECK(U(hl, "conns") == U(hl0, "conns") + 1);
    CHECK(U(hl, "last_seq") > U(hl0, "last_seq"));                /* sequence continued */
    CHECK(U(hl, "lost") > U(hl0, "lost"));                        /* samples made while disconnected leave a gap */
    CHECK(hub_src_consistent(hl));
    CHECK(reader_line_consistent(rl));                            /* totals survived the reconnect */
    CHECK(U(rl, "attempted") > U(rl0, "attempted") && U(rl, "unsent_disc") > U(rl0, "unsent_disc"));
    CHECK(U(rl, "disconnects") == U(rl0, "disconnects") + 1);
    CHECK(U(rl, "connects") == U(rl0, "connects") + 1);
    CHECK(U(rl, "sent") >= U(rl0, "sent"));
    printf("  T5 reconnect: same pid %d, inst %u; reader attempted %llu->%llu unsent_disc %llu->%llu; "
           "hub last_seq %llu->%llu lost %llu->%llu gen=%llu resets=%llu\n",
           (int)pid0, LIGHT_INST, (unsigned long long)U(rl0, "attempted"),
           (unsigned long long)U(rl, "attempted"), (unsigned long long)U(rl0, "unsent_disc"),
           (unsigned long long)U(rl, "unsent_disc"), (unsigned long long)U(hl0, "last_seq"),
           (unsigned long long)U(hl, "last_seq"), (unsigned long long)U(hl0, "lost"),
           (unsigned long long)U(hl, "lost"), (unsigned long long)U(hl, "gen"),
           (unsigned long long)U(hl, "resets"));
    if (!g_valgrind) {
        CHECK_MSG(count_fds(light->pid) == base_fd_light, "light fds %d != %d", count_fds(light->pid),
                  base_fd_light);
        CHECK_MSG(count_fds(hub->pid) == base_fd_hub, "hub fds %d != %d", count_fds(hub->pid),
                  base_fd_hub);
    }
    t_end();
}

static void t6_kill_isolation(void)
{
    t_begin("T6 SIGKILL distance reader: hub and light continue, degraded reported");
    bool ok;
    CHECK(kv_eq(hub->stat, "degraded", "0"));
    CHECK(kill(dist->pid, SIGKILL) == 0);
    CHECK(reap_within(dist, 5000));
    CHECK(killed_by(dist, SIGKILL));
    WAIT_UNTIL(ok, kv_eq(hub->src[SRC_DIST], "state", "DISCONNECTED") &&
                       kv_eq(hub->src[SRC_DIST], "avail", "0") &&
                       kv_eq(hub->stat, "degraded", "1"), 15000);
    CHECK(ok);
    CHECK(child_alive(hub) && child_alive(light));
    char hd0[LINE_CAP];
    SNAP(hd0, hub->src[SRC_DIST]);
    const uint64_t l0 = U(hub->src[SRC_LIGHT], "accepted");
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "accepted") >= l0 + 30 && hub->src_n[SRC_DIST] > 0 &&
                       hub->stat_n > 0, 15000);
    CHECK(ok);
    const unsigned dn = hub->src_n[SRC_DIST];
    WAIT_UNTIL(ok, hub->src_n[SRC_DIST] >= dn + 3, 5000);        /* a few more reports after the death */
    CHECK(ok);
    char hd[LINE_CAP], hl[LINE_CAP];
    SNAP(hd, hub->src[SRC_DIST]);
    SNAP(hl, hub->src[SRC_LIGHT]);
    /* honest: the dead source's counters are frozen, nothing is invented for it */
    CHECK(has_key(hd, "accepted") && has_key(hd0, "accepted"));
    CHECK(U(hd, "accepted") == U(hd0, "accepted"));
    CHECK(U(hd, "cb_samples") == U(hd0, "cb_samples"));
    CHECK(kv_eq(hd, "state", "DISCONNECTED") && kv_eq(hd, "avail", "0"));
    /* the other source is unaffected */
    CHECK(kv_eq(hl, "state", "ACTIVE") && kv_eq(hl, "avail", "1"));
    CHECK(U(hl, "accepted") >= l0 + 30 && U(hl, "regr") == 0 && U(hl, "dup") == 0);
    CHECK(kv_eq(hub->stat, "degraded", "1"));
    CHECK(child_alive(hub) && child_alive(light));
    CHECK(dist->summary[0] == '\0');             /* SIGKILL: no SUMMARY was (or could be) written */
    if (!g_valgrind)
        CHECK_MSG(count_fds(hub->pid) == base_fd_hub - 1, "hub fds %d, want %d", count_fds(hub->pid),
                  base_fd_hub - 1);
    printf("  T6 distance: state=%s accepted=%llu frozen; light accepted %llu -> %llu; hub degraded=1\n",
           "DISCONNECTED", (unsigned long long)U(hd, "accepted"), (unsigned long long)l0,
           (unsigned long long)U(hl, "accepted"));
    t_end();
}

static void t9_clean_shutdown(void)
{
    t_begin("T9 clean shutdown: exact accounting, socket removed, nothing orphaned");
    bool ok;
    CHECK(terminate(light, 5000));
    CHECK(exited_ok(light));
    CHECK(light->summary[0] != '\0' && kv_eq(light->summary, "exit", "0"));
    WAIT_UNTIL(ok, kv_eq(hub->src[SRC_LIGHT], "state", "DISCONNECTED"), 10000);
    CHECK(ok);
    char sm[LINE_CAP], hl[LINE_CAP];
    SNAP(sm, light->summary);
    CHECK(reader_line_consistent(sm));
    /* the reader's BYE follows every datagram it sent, so the hub read them all */
    SNAP(hl, hub->src[SRC_LIGHT]);
    CHECK_MSG(U(hl, "accepted") == U(sm, "sent"), "hub accepted %llu != reader sent %llu",
              (unsigned long long)U(hl, "accepted"), (unsigned long long)U(sm, "sent"));
    CHECK(hub_src_consistent(hl));
    CHECK(U(hl, "last_seq") + 1 <= U(sm, "attempted"));
    CHECK(U(hl, "lost") + U(hl, "unobs") <= U(sm, "attempted") - U(sm, "sent"));
    printf("  T9 light: reader sent=%llu attempted=%llu (bp_drop=%llu unsent_disc=%llu) | hub accepted=%llu "
           "lost=%llu unobs=%llu\n",
           (unsigned long long)U(sm, "sent"), (unsigned long long)U(sm, "attempted"),
           (unsigned long long)U(sm, "bp_drop"), (unsigned long long)U(sm, "unsent_disc"),
           (unsigned long long)U(hl, "accepted"), (unsigned long long)U(hl, "lost"),
           (unsigned long long)U(hl, "unobs"));

    CHECK(terminate(hub, 5000));
    CHECK(exited_ok(hub));
    CHECK(hub->summary[0] != '\0' && kv_eq(hub->summary, "exit", "0"));
    CHECK(hub->src[SRC_LIGHT][0] != '\0' && hub->src[SRC_DIST][0] != '\0');
    CHECK(access(g_sock, F_OK) != 0);                /* socket file unlinked */
    CHECK(!unix_socket_listed(g_sock));              /* and no listener left in the kernel table */
    CHECK(!child_alive(hub) && !child_alive(light) && !child_alive(dist));
    t_end();
}

static void t11_shutdown_in_backoff(void)
{
    t_begin("T11 shutdown while a reader is in reconnect backoff");
    static const char *const ex[] = {nullptr};
    struct child *r = spawn_reader("light-nohub", "light", LIGHT_INST, "20", ex);
    CHECK(r != nullptr);
    if (r == nullptr)
        return;
    bool ok;
    WAIT_UNTIL(ok, U(r->stat, "connect_attempts") >= 6, 20000);   /* next delay is then the 2 s cap */
    CHECK(ok);
    const uint64_t t0 = mono_ns();
    CHECK(kill(r->pid, SIGTERM) == 0);
    /* UNSCALED bound (the scaled wait is divided back): the pending delay is 2 s, so a reader
     * that slept through it would miss this on every build flavour. */
    CHECK(reap_within(r, (int)(800 / g_scale)));
    const uint64_t us = (mono_ns() - t0) / 1000ull;
    printf("  T11 exited %llu us after SIGTERM (pending backoff delay was 2000 ms)\n",
           (unsigned long long)us);
    CHECK_MSG(us < 1500000, "shutdown took %llu us", (unsigned long long)us);
    CHECK(exited_ok(r));
    CHECK(r->summary[0] != '\0' && kv_eq(r->summary, "exit", "0") && reader_line_consistent(r->summary));
    t_end();
}

int main(void)
{
    harness_init(120);
    t1_no_hub_backoff();
    if (light != nullptr) {
        t2_handshakes();
        if (hub != nullptr && dist != nullptr && hub->ready[0] != '\0' && dist->ready[0] != '\0') {
            t3_two_streams();
            t4_backpressure();
            t5_reconnect();
            t6_kill_isolation();
            t9_clean_shutdown();
        }
    }
    t11_shutdown_in_backoff();
    harness_teardown();
    return harness_finish("test_multiproc");
}
