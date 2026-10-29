/* Unit tests for role option parsing (no processes, no sockets). */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "oc_role.h"

static int failures;
#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define PARSE(rc, o, ...)                                                    \
    do {                                                                     \
        char *argv_[] = {"openchair", "--role", __VA_ARGS__, nullptr};       \
        int argc_ = (int)(sizeof argv_ / sizeof argv_[0]) - 1;               \
        memset(&(o), 0, sizeof(o));                                          \
        (rc) = oc_role_parse(argc_, argv_, &(o), err, sizeof err);           \
    } while (0)

static void test_reader_ok(void)
{
    struct oc_role_opts o;
    char err[128] = "";
    int rc;
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id",
          "0xA11CE5E551014", "--instance", "7", "--period-ms", "5", "--stat-ms", "100",
          "--fault-every", "10", "--drop-conn-on-sigusr1");
    CHECK(rc == 0);
    CHECK(o.role == OC_ROLE_LIGHT && o.simulate && o.instance == 7);
    CHECK(o.session_id == 0xA11CE5E551014ull);
    CHECK(o.period_ms == 5 && o.stat_ms == 100 && o.fault_every == 10);
    CHECK(o.drop_conn_on_sigusr1);
    CHECK(strcmp(o.runtime_dir, "/tmp/x") == 0);
    char path[108];
    CHECK(oc_role_socket_path(&o, path, sizeof path) == 0);
    CHECK(strcmp(path, "/tmp/x/hub.sock") == 0);
}

static void test_role_defaults(void)
{
    struct oc_role_opts o;
    char err[128] = "";
    int rc;
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance", "0");
    CHECK(rc == 0 && o.period_ms == 20 && o.stat_ms == 500 && o.instance == 0);
    PARSE(rc, o, "distance", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance", "0");
    CHECK(rc == 0 && o.role == OC_ROLE_DISTANCE && o.period_ms == 50);
}

static void test_hub_ok(void)
{
    struct oc_role_opts o;
    char err[128] = "";
    int rc;
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/x",
          "--session-id", "ff", "--light-instance", "7", "--distance-instance", "9",
          "--hello-timeout-ms", "4000", "--test-fd-headroom", "3");
    CHECK(rc == 0);
    CHECK(o.role == OC_ROLE_HUB && o.unsupervised_test);
    CHECK(o.have_light_instance && o.light_instance == 7);
    CHECK(o.have_distance_instance && o.distance_instance == 9);
    CHECK(o.hello_timeout_ms == 4000 && o.fd_headroom == 3);
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/x",
          "--session-id", "ff");
    CHECK(rc == 0 && !o.have_light_instance && o.fd_headroom == -1);
}

static void test_rejections(void)
{
    struct oc_role_opts o;
    char err[128];
    int rc;
    const char *const base_ok = "x";
    (void)base_ok;

    /* readers must say --simulate; there is no LIVE mode */
    PARSE(rc, o, "light", "--runtime-dir", "/tmp/x", "--session-id", "1", "--instance", "1");
    CHECK(rc == -EINVAL && err[0] != '\0');
    /* hub must acknowledge test-only authorization */
    PARSE(rc, o, "hub", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1");
    CHECK(rc == -EINVAL);
    /* missing required */
    PARSE(rc, o, "light", "--simulate", "--session-id", "1", "--instance", "1");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--instance", "1");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1");
    CHECK(rc == -EINVAL);
    /* session id: zero, non-hex, too long, negative */
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "0",
          "--instance", "1");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "xyz",
          "--instance", "1");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id",
          "11112222333344445", "--instance", "1");
    CHECK(rc == -EINVAL);
    /* numbers: negative, trailing junk, overflow, empty */
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance", "-1");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance", "7x");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance", "4294967296");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance", "");
    CHECK(rc == -EINVAL);
    /* zero period, relative dir, unknown option, duplicate, missing value */
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance", "1", "--period-ms", "0");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "rel", "--session-id", "1",
          "--instance", "1");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance", "1", "--bogus");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance", "1", "--instance", "2");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance");
    CHECK(rc == -EINVAL);
    /* options of the other role */
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/x", "--session-id", "1",
          "--instance", "1", "--light-instance", "1");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/x",
          "--session-id", "1", "--fault-every", "3");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/x",
          "--session-id", "1", "--instance", "3");
    CHECK(rc == -EINVAL);
    /* unknown role */
    PARSE(rc, o, "bogus", "--simulate");
    CHECK(rc == -EINVAL);
    /* a failed parse reports into err and never claims a role */
    PARSE(rc, o, "light", "--simulate");
    CHECK(rc == -EINVAL && strlen(err) > 0);
}


static void test_capture_options(void)
{
    struct oc_role_opts o;
    char err[160];
    int rc;
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt", "--session-id", "1",
          "--capture-dir", "/tmp/cap", "--capture-ms", "600000", "--capture-arm-timeout-ms", "1500",
          "--capture-fresh-ms", "250", "--capture-segment-bytes", "65624", "--capture-trace-segments", "4",
          "--test-fsize-limit", "100000");
    CHECK(rc == 0);
    CHECK(strcmp(o.capture_dir, "/tmp/cap") == 0 && o.capture_ms == 600000 && o.capture_arm_timeout_ms == 1500 &&
          o.capture_fresh_ms == 250 && o.capture_segment_bytes == 65624 && o.capture_trace_segments == 4 &&
          o.test_fsize_limit == 100000);
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt", "--session-id", "1");
    CHECK(rc == 0 && o.capture_dir == nullptr && o.capture_ms == 0);            /* capture off by default */

    /* the 10-minute cap is enforced at the CLI */
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt", "--session-id", "1",
          "--capture-dir", "/tmp/cap", "--capture-ms", "600001");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt", "--session-id", "1",
          "--capture-dir", "/tmp/cap", "--capture-ms", "0");
    CHECK(rc == -EINVAL);
    /* capture sub-options need --capture-dir */
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt", "--session-id", "1",
          "--capture-ms", "1000");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt", "--session-id", "1",
          "--test-fsize-limit", "1000");
    CHECK(rc == -EINVAL);
    /* absolute, and never the runtime dir or inside/around it */
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt", "--session-id", "1",
          "--capture-dir", "cap");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt", "--session-id", "1",
          "--capture-dir", "/tmp/rt");
    CHECK(rc == -EINVAL && strstr(err, "separate") != nullptr);
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt", "--session-id", "1",
          "--capture-dir", "/tmp/rt/captures");
    CHECK(rc == -EINVAL);
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt/sub", "--session-id", "1",
          "--capture-dir", "/tmp");
    CHECK(rc == -EINVAL);
    /* segment below the store's minimum */
    PARSE(rc, o, "hub", "--simulate", "--unsupervised-test", "--runtime-dir", "/tmp/rt", "--session-id", "1",
          "--capture-dir", "/tmp/cap", "--capture-segment-bytes", "65623");
    CHECK(rc == -EINVAL);
    /* readers do not take capture options */
    PARSE(rc, o, "light", "--simulate", "--runtime-dir", "/tmp/rt", "--session-id", "1", "--instance", "1",
          "--capture-dir", "/tmp/cap");
    CHECK(rc == -EINVAL);
}

static void test_paths_overlap(void)
{
    CHECK(oc_role_paths_overlap("/tmp/a", "/tmp/a"));
    CHECK(oc_role_paths_overlap("/tmp/a/", "/tmp/a"));
    CHECK(oc_role_paths_overlap("/tmp//a", "/tmp/a/b"));
    CHECK(oc_role_paths_overlap("/tmp/a/b", "/tmp/a"));
    CHECK(oc_role_paths_overlap("/", "/tmp/a"));
    CHECK(!oc_role_paths_overlap("/tmp/a", "/tmp/ab"));               /* a prefix of the NAME is not a parent */
    CHECK(!oc_role_paths_overlap("/tmp/a", "/tmp/b"));
    CHECK(!oc_role_paths_overlap("/var/cap", "/tmp/oc-s3c-x"));
    CHECK(oc_role_paths_overlap("rel", "/tmp/a"));                    /* not absolute: fail closed */
    CHECK(oc_role_paths_overlap("/tmp/a", nullptr));
}

static void test_socket_path_limits(void)
{
    struct oc_role_opts o = {.runtime_dir = "/tmp/x"};
    char path[15];   /* "/tmp/x/hub.sock" needs 16 with the NUL */
    CHECK(oc_role_socket_path(&o, path, sizeof path) == -ENAMETOOLONG);
    char big[200];
    memset(big, 'a', sizeof big);
    big[0] = '/';
    big[sizeof big - 1] = '\0';
    o.runtime_dir = big;
    char out[256];
    CHECK(oc_role_socket_path(&o, out, sizeof out) == -ENAMETOOLONG);   /* > sun_path */
    o.runtime_dir = nullptr;
    CHECK(oc_role_socket_path(&o, out, sizeof out) == -EINVAL);
}

int main(void)
{
    test_reader_ok();
    test_role_defaults();
    test_hub_ok();
    test_rejections();
    test_socket_path_limits();
    test_capture_options();
    test_paths_overlap();
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("test_role_args: all passed");
    return 0;
}
