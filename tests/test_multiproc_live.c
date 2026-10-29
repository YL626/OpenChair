/* M3-B multiprocess input-mode scenarios: real exec'd `openchair` processes.
 *
 * NO LIVE HARDWARE IS READ HERE. These scenarios check what happens around the
 * LIVE input mode on a host without the sensors: the hub and a reader must
 * agree on the origin, and a LIVE reader whose I2C device cannot be opened
 * must refuse to run (and never fall back to simulation). Passing is not
 * evidence about the Raspberry Pi, the ADS1115 or the VL6180X.
 *
 *   L1  LIVE hub + SIMULATED reader: HELLO rejected BAD_ORIGIN (permanent, exit 3);
 *       the hub reports origin=LIVE and never accepts a sample
 *   L2  LIVE reader, I2C device missing: exit 3, no READY, nothing reaches the hub
 *   L3  LIVE reader, device that is not an I2C adapter (/dev/null): same
 *   L4  LIVE hub shuts down cleanly and labels START/FINAL/SUMMARY origin=LIVE
 *   L5  reader loop over a FAKE bus that claims origin LIVE (oc_role_reader_run_backend):
 *       the LIVE hub accepts it, a forced hub-connection loss reconnects with the SAME
 *       adapter and sequence, and a sensor that starts failing produces non-OK samples
 *       while the reader keeps running (no fallback to simulation)
 *   L6  the same fake-LIVE readers into a LIVE hub with capture: PASS, origin=LIVE
 *
 * L5/L6 are the only places a fake claims origin LIVE, and only to exercise the
 * enforcement and reconnect mechanics through the real reader loop and hub. The
 * samples are fake. Fault controls (--drop-conn-on-sigusr1) are set on the options
 * struct directly: the command line rejects them with --input live.
 *
 * A SIMULATED hub with SIMULATED readers is covered by test_multiproc. */
#include "mp_harness.h"

#include "oc_reader_backend.h"
#include "oc_role_reader.h"
#include "oc_sample.h"
#include "oc_store.h"
#include "fake_i2c.h"
#include "fake_vl6180x.h"

static bool recent_has(const struct child *c, const char *needle)
{
    unsigned n = c->recent_n < RECENT_N ? c->recent_n : RECENT_N;
    for (unsigned k = 0; k < n; k++)
        if (strstr(c->recent[k], needle) != nullptr)
            return true;
    return false;
}

static bool exited_with(const struct child *c, int code)
{
    return c->reaped && WIFEXITED(c->wstatus) && WEXITSTATUS(c->wstatus) == code;
}

static struct child *hub_live(void)
{
    g_input_a = "--input";
    g_input_b = "live";
    static const char *const none[] = {nullptr};
    struct child *h = spawn_hub(none);
    g_input_a = "--simulate";
    g_input_b = nullptr;
    return h;
}

static struct child *reader_with(const char *a, const char *b, const char *role, unsigned inst,
                                 const char *const *extra)
{
    g_input_a = a;
    g_input_b = b;
    struct child *r = spawn_reader(role, role, inst, "20", extra);
    g_input_a = "--simulate";
    g_input_b = nullptr;
    return r;
}

static struct child *g_hub;

static void l1_live_hub_rejects_simulated_reader(void)
{
    t_begin("L1 LIVE hub rejects a SIMULATED reader (BAD_ORIGIN)");
    g_hub = hub_live();
    CHECK(g_hub != nullptr);
    if (g_hub == nullptr)
        return;
    bool ok;
    WAIT_UNTIL(ok, g_hub->ready[0] != '\0', 10000);
    CHECK(ok);
    CHECK(kv_eq(g_hub->ready, "origin", "LIVE"));
    static const char *const none[] = {nullptr};
    struct child *r = reader_with("--simulate", nullptr, "light", LIGHT_INST, none);
    CHECK(r != nullptr);
    if (r == nullptr)
        return;
    CHECK(reap_within(r, 15000));
    CHECK_MSG(exited_with(r, 3), "reader exit status %d", r->reaped ? r->wstatus : -1);
    CHECK(recent_has(r, "reject=BAD_ORIGIN"));
    CHECK(r->ready[0] == '\0');                        /* never connected */
    CHECK(!recent_has(r, "ev=connected"));
    run_for(300);
    CHECK(child_alive(g_hub));
    CHECK(U(g_hub->src[SRC_LIGHT], "accepted") == 0);  /* no sample ever accepted */
    CHECK(!kv_eq(g_hub->src[SRC_LIGHT], "state", "ACTIVE"));
    t_end();
}

static void l2_live_reader_missing_device(void)
{
    t_begin("L2 LIVE reader with a missing I2C device refuses to run");
    static const char *const ex[] = {"--i2c-dev", "/nonexistent/i2c-99", nullptr};
    const char *roles[] = {"light", "distance"};
    for (int i = 0; i < 2; i++) {
        struct child *r = reader_with("--input", "live", roles[i], i == 0 ? LIGHT_INST : DIST_INST, ex);
        CHECK(r != nullptr);
        if (r == nullptr)
            continue;
        CHECK(reap_within(r, 15000));
        CHECK_MSG(exited_with(r, 3), "%s exit status %d", roles[i], r->reaped ? r->wstatus : -1);
        CHECK(r->ready[0] == '\0' && r->stat_n == 0);   /* nothing was ever reported as running */
        CHECK(!recent_has(r, "ev=connected"));
        CHECK(!killed_by(r, SIGKILL) && !r->harness_killed);
    }
    CHECK(U(g_hub->src[SRC_LIGHT], "accepted") == 0 && U(g_hub->src[SRC_DIST], "accepted") == 0);
    t_end();
}

static void l3_live_reader_not_an_i2c_adapter(void)
{
    t_begin("L3 LIVE reader on a file that is not an I2C adapter refuses to run");
    static const char *const ex[] = {"--i2c-dev", "/dev/null", nullptr};
    struct child *r = reader_with("--input", "live", "light", LIGHT_INST, ex);
    CHECK(r != nullptr);
    if (r == nullptr)
        return;
    CHECK(reap_within(r, 15000));
    CHECK_MSG(exited_with(r, 3), "exit status %d", r->reaped ? r->wstatus : -1);
    CHECK(r->ready[0] == '\0' && r->stat_n == 0);
    CHECK(U(g_hub->src[SRC_LIGHT], "accepted") == 0);
    t_end();
}

static void l4_live_hub_shutdown_labels(void)
{
    t_begin("L4 LIVE hub: orderly shutdown, origin=LIVE in every report");
    CHECK(g_hub != nullptr && child_alive(g_hub));
    if (g_hub == nullptr)
        return;
    CHECK(kv_eq(g_hub->stat, "origin", "LIVE"));
    CHECK(terminate(g_hub, 5000) && exited_ok(g_hub));
    CHECK(g_hub->summary[0] != '\0' && kv_eq(g_hub->summary, "origin", "LIVE") &&
          kv_eq(g_hub->summary, "exit", "0"));
    CHECK(g_hub->fin[0] != '\0' && kv_eq(g_hub->fin, "origin", "LIVE"));
    CHECK(access(g_sock, F_OK) != 0);
    t_end();
}

/* ---- in-process reader children over a fake bus ---- */

struct fake_reader_arg {
    enum oc_role_kind role;
    uint32_t instance;
    uint32_t period_ms;
    unsigned fail_from_call;     /* light only: every bus call from this one on NACKs; 0 = never */
};

static int run_fake_live_reader(void *p)
{
    const struct fake_reader_arg *a = p;
    struct oc_role_opts o = {.role = a->role, .input = OC_INPUT_LIVE, .i2c_dev = "FAKE-BUS",
                             .runtime_dir = g_dir, .instance = a->instance,
                             .period_ms = a->period_ms, .stat_ms = 100, .lifetime_ms = 300000,
                             .drop_conn_on_sigusr1 = true};
    char *end = nullptr;
    o.session_id = strtoull(SESSION_HEX, &end, 16);
    static struct fake fl;
    static struct fv fd;
    struct oc_i2c_bus bus;
    struct oc_clock fake_clk, real_clk;
    oc_clock_system(&real_clk);               /* real timestamps: the hub checks sample age */
    struct oc_reader_backend be;
    if (a->role == OC_ROLE_LIGHT) {
        fake_init(&fl);
        for (int i = 0; i < FAKE_MAX_CONV; i++)
            fake_push(&fl, (int16_t)(1000 + i));
        fake_bus(&fl, &bus, &fake_clk);
        if (a->fail_from_call != 0) {
            fl.fail_from_call = a->fail_from_call;
            fl.fail_errno = ENXIO;
        }
    } else {
        fv_init(&fd);
        for (int i = 0; i < FV_MAX_MEAS; i++)
            fv_push(&fd, (uint8_t)(40 + i % 3), 0);
        fv_bus(&fd, &bus, &fake_clk);
    }
    bus.origin = OC_ORIGIN_LIVE;              /* MECHANICS ONLY */
    uint8_t src = a->role == OC_ROLE_LIGHT ? OC_SOURCE_LIGHT : OC_SOURCE_DISTANCE;
    if (oc_reader_backend_open_bus(&be, src, &bus, &real_clk) != 0)
        return 99;
    return oc_role_reader_run_backend(&o, &be);
}

static struct child *spawn_fake_reader(const char *name, struct fake_reader_arg *a)
{
    return spawn_impl(name, nullptr, run_fake_live_reader, a);
}

#define SNAP(dst, srcline) snprintf((dst), LINE_CAP, "%s", (srcline))

static void l5_fake_live_readers_reconnect_and_fail_honestly(void)
{
    t_begin("L5 fake-LIVE readers: accepted, reconnect keeps adapter and seq, failures stay honest");
    struct child *hub = hub_live();
    CHECK(hub != nullptr);
    if (hub == nullptr)
        return;
    bool ok;
    WAIT_UNTIL(ok, hub->ready[0] != '\0', 10000);
    CHECK(ok);
    static struct fake_reader_arg la = {OC_ROLE_LIGHT, LIGHT_INST, 5, 1200};
    static struct fake_reader_arg da = {OC_ROLE_DISTANCE, DIST_INST, 10, 0};
    struct child *light = spawn_fake_reader("light", &la);
    struct child *dist = spawn_fake_reader("distance", &da);
    CHECK(light != nullptr && dist != nullptr);
    if (light == nullptr || dist == nullptr)
        return;
    WAIT_UNTIL(ok, kv_eq(hub->src[SRC_LIGHT], "state", "ACTIVE") &&
                       kv_eq(hub->src[SRC_DIST], "state", "ACTIVE"), 20000);
    CHECK(ok);
    CHECK(kv_eq(light->ready, "origin", "LIVE") && kv_eq(dist->ready, "origin", "LIVE"));
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "accepted") >= 20 && U(hub->src[SRC_DIST], "accepted") >= 10, 20000);
    CHECK(ok);

    /* Forced loss of the hub connection: same process, same backend. */
    char before[LINE_CAP];
    SNAP(before, light->stat);
    const uint64_t seq_before = U(before, "next_seq");
    const uint64_t acc_before = U(hub->src[SRC_LIGHT], "accepted");
    CHECK(kill(light->pid, SIGUSR1) == 0);
    WAIT_UNTIL(ok, U(light->stat, "connects") >= 2 && kv_eq(light->stat, "up", "1"), 20000);
    CHECK(ok);
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "accepted") >= acc_before + 20 &&
                       kv_eq(hub->src[SRC_LIGHT], "state", "ACTIVE"), 20000);
    CHECK(ok);
    printf("  L5 light: next_seq %llu -> %llu, hub accepted %llu -> %llu, reconn=%llu lost=%llu\n",
           (unsigned long long)seq_before, (unsigned long long)U(light->stat, "next_seq"),
           (unsigned long long)acc_before, (unsigned long long)U(hub->src[SRC_LIGHT], "accepted"),
           (unsigned long long)U(hub->src[SRC_LIGHT], "reconn"),
           (unsigned long long)U(hub->src[SRC_LIGHT], "lost"));
    CHECK(U(light->stat, "next_seq") > seq_before);                 /* the sequence was never restarted */
    CHECK(U(hub->src[SRC_LIGHT], "regr") == 0 && U(hub->src[SRC_LIGHT], "dup") == 0 &&
          U(hub->src[SRC_LIGHT], "resets") == 0);
    CHECK(U(hub->src[SRC_LIGHT], "reconn") >= 1);

    /* The scripted sensor starts NACKing (call 1200): honest non-OK samples, reader keeps going. */
    WAIT_UNTIL(ok, U(hub->src[SRC_LIGHT], "st_other") >= 5 && U(light->stat, "non_ok") >= 5, 30000);
    CHECK(ok);
    const uint64_t att = U(light->stat, "attempted");
    run_for(300);
    CHECK(child_alive(light) && U(light->stat, "attempted") > att);
    CHECK(kv_eq(light->stat, "origin", "LIVE"));                    /* never silently simulated */
    CHECK(U(hub->src[SRC_LIGHT], "origin_bad") == 0 && U(hub->src[SRC_DIST], "origin_bad") == 0);
    CHECK(U(hub->src[SRC_DIST], "st_other") == 0);                  /* the healthy sensor is unaffected */
    for (const struct child *r = light; r != nullptr; r = (r == light) ? dist : nullptr) {
        const char *st = r->stat;
        CHECK(U(st, "attempted") == U(st, "sent") + U(st, "unsent_disc") + U(st, "bp_drop") + U(st, "send_fail"));
        CHECK(U(st, "attempted") == U(st, "next_seq"));             /* every attempt consumed one seq */
    }
    CHECK(terminate(light, 5000) && exited_ok(light));
    CHECK(terminate(dist, 5000) && exited_ok(dist));
    CHECK(kv_eq(light->summary, "origin", "LIVE") && kv_eq(light->summary, "exit", "0"));
    CHECK(terminate(hub, 5000) && exited_ok(hub));
    t_end();
}

static void l6_live_capture_pass(void)
{
    t_begin("L6 LIVE-labelled hub capture of fake-LIVE readers: PASS, origin=LIVE");
    capdir_make();
    g_input_a = "--input";
    g_input_b = "live";
    static const char *const hx[] = {"--capture-dir", g_capdir, "--capture-ms", "2000",
                                     "--capture-fresh-ms", "1000", nullptr};
    struct child *hub = spawn_hub(hx);
    g_input_a = "--simulate";
    g_input_b = nullptr;
    CHECK(hub != nullptr);
    if (hub == nullptr)
        return;
    bool ok;
    WAIT_UNTIL(ok, hub->ready[0] != '\0' && hub->store_recover[0] != '\0', 10000);
    CHECK(ok);
    static struct fake_reader_arg la = {OC_ROLE_LIGHT, LIGHT_INST, 5, 0};
    static struct fake_reader_arg da = {OC_ROLE_DISTANCE, DIST_INST, 10, 0};
    struct child *light = spawn_fake_reader("light", &la);
    struct child *dist = spawn_fake_reader("distance", &da);
    CHECK(light != nullptr && dist != nullptr);
    WAIT_UNTIL(ok, hub->cap_result[0] != '\0' && hub->cap_lat[3][0] != '\0', 60000);
    CHECK(ok);
    printf("  L6 %s\n", hub->cap_result);
    const char *R = hub->cap_result;
    CHECK(kv_eq(R, "verdict", "PASS") && kv_eq(R, "flags", "-"));
    CHECK(kv_eq(R, "origin", "LIVE"));
    CHECK(U(R, "appended") > 0 && U(R, "store_failed") == 0);
    if (light != nullptr)
        CHECK(terminate(light, 5000) && exited_ok(light));
    if (dist != nullptr)
        CHECK(terminate(dist, 5000) && exited_ok(dist));
    CHECK(terminate(hub, 5000) && exited_ok(hub));
    t_end();
}

int main(void)
{
    harness_init(90);
    l1_live_hub_rejects_simulated_reader();
    l2_live_reader_missing_device();
    l3_live_reader_not_an_i2c_adapter();
    l4_live_hub_shutdown_labels();
    l5_fake_live_readers_reconnect_and_fail_honestly();
    l6_live_capture_pass();
    harness_teardown();
    return harness_finish("test_multiproc_live");
}
