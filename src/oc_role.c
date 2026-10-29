#define _GNU_SOURCE   /* signalfd (strict -std=c23 hides it) */

#include "oc_role.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/un.h>
#include <unistd.h>

#include "oc_capture.h"
#include "oc_role_hub.h"
#include "oc_role_reader.h"

/* ---------- option table ---------- */

#define R_HUB 1u
#define R_LIGHT 2u
#define R_DIST 4u
#define R_READER (R_LIGHT | R_DIST)
#define R_ALL (R_HUB | R_READER)

enum opt_id {
    O_SIMULATE, O_UNSUP, O_DROPCONN, O_DIR, O_SESSION, O_INSTANCE, O_LIGHT_INST,
    O_DIST_INST, O_PERIOD, O_STAT, O_LIFETIME, O_FAULT, O_HELLO_TO, O_HEADROOM,
    O_CAP_DIR, O_CAP_MS, O_CAP_ARM, O_CAP_FRESH, O_CAP_SEG, O_CAP_TSEG, O_FSIZE,
    O_COUNT
};

struct optdef {
    const char *name;
    bool has_arg;
    unsigned roles;
    enum opt_id id;
};

static const struct optdef OPTS[] = {
    {"--simulate", false, R_ALL, O_SIMULATE},
    {"--unsupervised-test", false, R_HUB, O_UNSUP},
    {"--drop-conn-on-sigusr1", false, R_READER, O_DROPCONN},
    {"--runtime-dir", true, R_ALL, O_DIR},
    {"--session-id", true, R_ALL, O_SESSION},
    {"--instance", true, R_READER, O_INSTANCE},
    {"--light-instance", true, R_HUB, O_LIGHT_INST},
    {"--distance-instance", true, R_HUB, O_DIST_INST},
    {"--period-ms", true, R_READER, O_PERIOD},
    {"--stat-ms", true, R_ALL, O_STAT},
    {"--lifetime-ms", true, R_ALL, O_LIFETIME},
    {"--fault-every", true, R_READER, O_FAULT},
    {"--hello-timeout-ms", true, R_HUB, O_HELLO_TO},
    {"--test-fd-headroom", true, R_HUB, O_HEADROOM},
    {"--capture-dir", true, R_HUB, O_CAP_DIR},
    {"--capture-ms", true, R_HUB, O_CAP_MS},
    {"--capture-arm-timeout-ms", true, R_HUB, O_CAP_ARM},
    {"--capture-fresh-ms", true, R_HUB, O_CAP_FRESH},
    {"--capture-segment-bytes", true, R_HUB, O_CAP_SEG},
    {"--capture-trace-segments", true, R_HUB, O_CAP_TSEG},
    {"--test-fsize-limit", true, R_HUB, O_FSIZE},
};

static int fail(char *err, size_t cap, const char *what, const char *detail)
{
    if (err != nullptr && cap > 0)
        snprintf(err, cap, "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
    return -EINVAL;
}

/* Strict unsigned decimal: digits only (no sign, space or prefix), no
 * trailing junk, no overflow of `max`. */
static bool parse_u64(const char *s, uint64_t max, uint64_t *out)
{
    if (s == nullptr || s[0] < '0' || s[0] > '9')
        return false;
    errno = 0;
    char *end = nullptr;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v > max)
        return false;
    *out = v;
    return true;
}

/* 1..16 hex digits with an optional 0x/0X prefix; nonzero. */
static bool parse_session(const char *s, uint64_t *out)
{
    if (s == nullptr)
        return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    size_t n = strlen(s);
    if (n < 1 || n > 16)
        return false;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9')
            d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (unsigned)(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F')
            d = (unsigned)(c - 'A') + 10;
        else
            return false;
        v = (v << 4) | d;
    }
    if (v == 0)
        return false;
    *out = v;
    return true;
}

static unsigned role_bit(enum oc_role_kind k)
{
    return k == OC_ROLE_HUB ? R_HUB : k == OC_ROLE_LIGHT ? R_LIGHT : R_DIST;
}

static int apply_opt(struct oc_role_opts *o, const struct optdef *d, const char *val,
                     char *err, size_t errcap)
{
    uint64_t v = 0;
    switch (d->id) {
    case O_SIMULATE: o->simulate = true; return 0;
    case O_UNSUP: o->unsupervised_test = true; return 0;
    case O_DROPCONN: o->drop_conn_on_sigusr1 = true; return 0;
    case O_DIR:
        if (val[0] != '/')
            return fail(err, errcap, "--runtime-dir must be an absolute path", val);
        o->runtime_dir = val;
        return 0;
    case O_SESSION:
        if (!parse_session(val, &o->session_id))
            return fail(err, errcap, "--session-id must be 1..16 hex digits, nonzero", val);
        return 0;
    case O_INSTANCE:
        if (!parse_u64(val, UINT32_MAX, &v))
            return fail(err, errcap, "--instance must be a u32", val);
        o->instance = (uint32_t)v;
        return 0;
    case O_LIGHT_INST:
        if (!parse_u64(val, UINT32_MAX, &v))
            return fail(err, errcap, "--light-instance must be a u32", val);
        o->have_light_instance = true;
        o->light_instance = (uint32_t)v;
        return 0;
    case O_DIST_INST:
        if (!parse_u64(val, UINT32_MAX, &v))
            return fail(err, errcap, "--distance-instance must be a u32", val);
        o->have_distance_instance = true;
        o->distance_instance = (uint32_t)v;
        return 0;
    case O_PERIOD:
        if (!parse_u64(val, 10000, &v) || v < 1)
            return fail(err, errcap, "--period-ms must be 1..10000", val);
        o->period_ms = (uint32_t)v;
        return 0;
    case O_STAT:
        if (!parse_u64(val, 60000, &v))
            return fail(err, errcap, "--stat-ms must be 0..60000", val);
        o->stat_ms = (uint32_t)v;
        return 0;
    case O_LIFETIME:
        if (!parse_u64(val, 86400000ull, &v))
            return fail(err, errcap, "--lifetime-ms must be 0..86400000", val);
        o->lifetime_ms = v;
        return 0;
    case O_FAULT:
        if (!parse_u64(val, 1000000, &v))
            return fail(err, errcap, "--fault-every must be 0..1000000", val);
        o->fault_every = (uint32_t)v;
        return 0;
    case O_HELLO_TO:
        if (!parse_u64(val, 600000, &v) || v < 1)
            return fail(err, errcap, "--hello-timeout-ms must be 1..600000", val);
        o->hello_timeout_ms = (uint32_t)v;
        return 0;
    case O_HEADROOM:
        if (!parse_u64(val, 64, &v))
            return fail(err, errcap, "--test-fd-headroom must be 0..64", val);
        o->fd_headroom = (int)v;
        return 0;
    case O_CAP_DIR:
        if (val[0] != '/')
            return fail(err, errcap, "--capture-dir must be an absolute path", val);
        o->capture_dir = val;
        return 0;
    case O_CAP_MS:
        if (!parse_u64(val, OC_CAPTURE_MAX_MS, &v) || v < 1)
            return fail(err, errcap, "--capture-ms must be 1..600000 (10 minutes)", val);
        o->capture_ms = (uint32_t)v;
        return 0;
    case O_CAP_ARM:
        if (!parse_u64(val, 600000, &v) || v < 1)
            return fail(err, errcap, "--capture-arm-timeout-ms must be 1..600000", val);
        o->capture_arm_timeout_ms = (uint32_t)v;
        return 0;
    case O_CAP_FRESH:
        if (!parse_u64(val, 60000, &v) || v < 1)
            return fail(err, errcap, "--capture-fresh-ms must be 1..60000", val);
        o->capture_fresh_ms = (uint32_t)v;
        return 0;
    case O_CAP_SEG:
        if (!parse_u64(val, 1u << 30, &v) || v < 65624)
            return fail(err, errcap, "--capture-segment-bytes must be 65624..1073741824", val);
        o->capture_segment_bytes = (size_t)v;
        return 0;
    case O_CAP_TSEG:
        if (!parse_u64(val, 64, &v) || v < 1)
            return fail(err, errcap, "--capture-trace-segments must be 1..64", val);
        o->capture_trace_segments = (uint32_t)v;
        return 0;
    case O_FSIZE:
        if (!parse_u64(val, UINT64_MAX / 2, &v) || v < 1)
            return fail(err, errcap, "--test-fsize-limit must be >= 1 byte", val);
        o->test_fsize_limit = v;
        return 0;
    case O_COUNT:
        break;
    }
    return fail(err, errcap, "internal option error", nullptr);
}

int oc_role_parse(int argc, char **argv, struct oc_role_opts *o, char *err, size_t errcap)
{
    if (err != nullptr && errcap > 0)
        err[0] = '\0';
    if (o == nullptr || argv == nullptr || argc < 3 || strcmp(argv[1], "--role") != 0)
        return fail(err, errcap, "expected --role <hub|light|distance>", nullptr);

    *o = (struct oc_role_opts){.fd_headroom = -1, .stat_ms = 500};
    if (strcmp(argv[2], "hub") == 0)
        o->role = OC_ROLE_HUB;
    else if (strcmp(argv[2], "light") == 0)
        o->role = OC_ROLE_LIGHT;
    else if (strcmp(argv[2], "distance") == 0)
        o->role = OC_ROLE_DISTANCE;
    else {
        o->role = OC_ROLE_NONE;
        return fail(err, errcap, "unknown role", argv[2]);
    }
    o->period_ms = o->role == OC_ROLE_LIGHT ? 20 : 50;

    bool seen[O_COUNT] = {false};
    for (int i = 3; i < argc; i++) {
        const struct optdef *d = nullptr;
        for (size_t k = 0; k < sizeof OPTS / sizeof OPTS[0]; k++)
            if (strcmp(argv[i], OPTS[k].name) == 0)
                d = &OPTS[k];
        if (d == nullptr)
            return fail(err, errcap, "unknown option", argv[i]);
        if ((d->roles & role_bit(o->role)) == 0)
            return fail(err, errcap, "option not valid for this role", argv[i]);
        if (seen[d->id])
            return fail(err, errcap, "duplicate option", argv[i]);
        seen[d->id] = true;
        const char *val = nullptr;
        if (d->has_arg) {
            if (i + 1 >= argc)
                return fail(err, errcap, "missing value for", argv[i]);
            val = argv[++i];
        }
        int rc = apply_opt(o, d, val, err, errcap);
        if (rc != 0) {
            o->role = OC_ROLE_NONE;
            return rc;
        }
    }

    if (!seen[O_SIMULATE])
        return fail(err, errcap, "--simulate is required (S3C has no LIVE mode)", nullptr);
    if (!seen[O_DIR])
        return fail(err, errcap, "--runtime-dir is required", nullptr);
    if (!seen[O_SESSION])
        return fail(err, errcap, "--session-id is required", nullptr);
    if (o->role == OC_ROLE_HUB) {
        if (!seen[O_UNSUP])
            return fail(err, errcap,
                        "--unsupervised-test is required (test-only authorization, "
                        "not supervisor authorization)", nullptr);
    } else if (!seen[O_INSTANCE]) {
        return fail(err, errcap, "--instance is required", nullptr);
    }
    if (o->role == OC_ROLE_HUB) {
        const bool cap_opts = seen[O_CAP_MS] || seen[O_CAP_ARM] || seen[O_CAP_FRESH] ||
                              seen[O_CAP_SEG] || seen[O_CAP_TSEG] || seen[O_FSIZE];
        if (cap_opts && !seen[O_CAP_DIR])
            return fail(err, errcap, "capture options require --capture-dir", nullptr);
        if (seen[O_CAP_DIR] && oc_role_paths_overlap(o->capture_dir, o->runtime_dir))
            return fail(err, errcap,
                        "--capture-dir must be separate from --runtime-dir (the runtime "
                        "directory is deleted by test cleanup; captured evidence must not be)",
                        o->capture_dir);
    }
    return 0;
}

/* Splits an absolute path into components, ignoring empty ones. Returns the
 * count or -1 if the path is not absolute or has more than max components. */
static int split_path(const char *p, const char **start, size_t *len, int max)
{
    if (p == nullptr || p[0] != '/')
        return -1;
    int n = 0;
    while (*p != '\0') {
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        const char *s = p;
        while (*p != '\0' && *p != '/')
            p++;
        if (n >= max)
            return -1;
        start[n] = s;
        len[n] = (size_t)(p - s);
        n++;
    }
    return n;
}

bool oc_role_paths_overlap(const char *a, const char *b)
{
    enum { MAXC = 64 };
    const char *as[MAXC], *bs[MAXC];
    size_t al[MAXC], bl[MAXC];
    int na = split_path(a, as, al, MAXC), nb = split_path(b, bs, bl, MAXC);
    if (na < 0 || nb < 0)
        return true;                    /* cannot prove they are apart: fail closed */
    int n = na < nb ? na : nb;
    for (int i = 0; i < n; i++)
        if (al[i] != bl[i] || memcmp(as[i], bs[i], al[i]) != 0)
            return false;               /* diverge: neither contains the other */
    return true;                        /* one is a prefix of (or equal to) the other */
}

int oc_role_socket_path(const struct oc_role_opts *o, char *out, size_t cap)
{
    if (o == nullptr || o->runtime_dir == nullptr || out == nullptr || cap == 0)
        return -EINVAL;
    static const char NAME[] = "/hub.sock";
    size_t dl = strlen(o->runtime_dir);
    while (dl > 1 && o->runtime_dir[dl - 1] == '/')
        dl--;
    if (dl == 0)
        return -EINVAL;
    size_t need = dl + sizeof NAME;                       /* incl. NUL */
    if (need > cap || need > sizeof(((struct sockaddr_un *)nullptr)->sun_path))
        return -ENAMETOOLONG;
    memcpy(out, o->runtime_dir, dl);
    memcpy(out + dl, NAME, sizeof NAME);
    return 0;
}

void oc_role_usage(FILE *out)
{
    fputs("usage: openchair --role hub --simulate --unsupervised-test --runtime-dir DIR\n"
          "                 --session-id HEX [--light-instance N] [--distance-instance N]\n"
          "                 [--stat-ms N] [--lifetime-ms N]\n"
          "                 [--capture-dir DIR [--capture-ms N] [--capture-arm-timeout-ms N]\n"
          "                  [--capture-fresh-ms N]]   (M3-A: store SIMULATED samples via oc_store)\n"
          "                 [--hello-timeout-ms N] [--test-fd-headroom N]   (testing)\n"
          "                 [--capture-segment-bytes N] [--capture-trace-segments N]\n"
          "                 [--test-fsize-limit BYTES]                      (testing)\n"
          "       openchair --role light|distance --simulate --runtime-dir DIR\n"
          "                 --session-id HEX --instance N [--period-ms N] [--stat-ms N]\n"
          "                 [--lifetime-ms N] [--fault-every N]\n"
          "                 [--drop-conn-on-sigusr1]                        (testing)\n"
          "       openchair --role help\n"
          "S3C roles: all samples are SIMULATED; the hub's authorized instances come\n"
          "from the flags above (test configuration, not supervisor authorization).\n",
          out);
}

int oc_role_main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[2], "help") == 0) {
        oc_role_usage(stdout);
        return OC_ROLE_EXIT_OK;
    }
    struct oc_role_opts o;
    char err[160];
    if (oc_role_parse(argc, argv, &o, err, sizeof err) != 0) {
        fprintf(stderr, "openchair: --role: %s\n", err);
        oc_role_usage(stderr);
        return OC_ROLE_EXIT_USAGE;
    }
    return o.role == OC_ROLE_HUB ? oc_role_hub_run(&o) : oc_role_reader_run(&o);
}

/* ---------- signals ---------- */

int oc_signalfd_open(const int *sigs, size_t n)
{
    sigset_t set, old;
    sigemptyset(&set);
    for (size_t i = 0; i < n; i++)
        sigaddset(&set, sigs[i]);
    sigset_t block = set;
    sigaddset(&block, SIGPIPE);
    if (sigprocmask(SIG_BLOCK, &block, &old) != 0)       /* single-threaded process */
        return -errno;
    int fd = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
    if (fd < 0) {
        int e = errno;
        sigprocmask(SIG_SETMASK, &old, nullptr);
        return -e;
    }
    return fd;
}

int oc_signalfd_drain(int sfd, int *out, size_t cap)
{
    size_t stored = 0;
    for (;;) {
        struct signalfd_siginfo si;
        ssize_t r = read(sfd, &si, sizeof si);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return (int)stored;
            return -errno;
        }
        if (r != (ssize_t)sizeof si)
            return -EIO;
        if (stored < cap)
            out[stored++] = (int)si.ssi_signo;
    }
}
