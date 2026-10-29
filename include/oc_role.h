#ifndef OC_ROLE_H
#define OC_ROLE_H

/* oc_role: role dispatch for the single openchair executable (S3C).
 *
 *   openchair --role hub      ...   the hub process
 *   openchair --role light    ...   a light reader (SIMULATED or LIVE)
 *   openchair --role distance ...   a distance reader (SIMULATED or LIVE)
 *   openchair --role help           role usage on stdout, exit 0
 *
 * --version and --help are handled by main() and are unchanged; role options
 * are documented by `--role help` and in README.md, not in --help.
 *
 * STATUS: S3C testing roles plus M3-B. The input mode is mandatory and explicit:
 *   --input sim | --input live     (--simulate is an alias for --input sim)
 * Giving --simulate together with --input live is a conflict and is rejected.
 * SIM readers take synthetic readings from oc_sim_source; LIVE readers read the
 * real ADS1115 / VL6180X through oc_i2c_linux (--i2c-dev, default /dev/i2c-1).
 * The hub is told the mode too and authorizes exactly that origin for both
 * sources, so a SIMULATED reader cannot feed a LIVE hub or the reverse. A LIVE
 * reader never falls back to simulation: a hardware error is reported as a
 * non-OK sample, and an I2C device that cannot be opened is exit 3.
 * Nothing in this repository's host tests is LIVE evidence; LIVE means "the
 * Linux I2C transport was used", which on a host without the sensors yields
 * NACK/TIMEOUT samples, never data.
 * The hub's authorized instances come from explicit command-line flags that
 * only exist for testing (--unsupervised-test); this is NOT supervisor
 * authorization, and no authorization message crosses a process boundary.
 *
 * Exit codes: 0 clean shutdown, 2 usage error, 3 configuration error or a
 * permanent HELLO rejection, 4 runtime failure (including a critical report
 * that could not be delivered).
 *
 * Testing controls (hub: only with --unsupervised-test). Fault injection is
 * restricted to --input sim: --fault-every, --drop-conn-on-sigusr1,
 * --test-fd-headroom, --test-fsize-limit are rejected with --input live.
 * Usable in LIVE demonstrations until the supervisor exists: --lifetime-ms,
 * --unsupervised-test, --hello-timeout-ms, --capture-segment-bytes,
 * --capture-trace-segments.
 *
 * Capture mode (hub only, --capture-dir): see oc_capture.h. The capture
 * directory holds persistent evidence and must be separate from the runtime
 * (socket) directory, which test code deletes. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define OC_ROLE_EXIT_OK 0
#define OC_ROLE_EXIT_USAGE 2
#define OC_ROLE_EXIT_CONFIG 3
#define OC_ROLE_EXIT_RUNTIME 4

enum oc_role_kind {
    OC_ROLE_NONE = 0,
    OC_ROLE_HUB,
    OC_ROLE_LIGHT,
    OC_ROLE_DISTANCE
};

enum oc_input_mode {
    OC_INPUT_NONE = 0,
    OC_INPUT_SIM,
    OC_INPUT_LIVE
};

#define OC_I2C_DEV_DEFAULT "/dev/i2c-1"
#define OC_PERIOD_LIVE_DISTANCE_MS 150   /* conservative starting value, unmeasured [MEASURE] */

struct oc_role_opts {
    enum oc_role_kind role;
    enum oc_input_mode input;        /* always SIM or LIVE after a successful parse */
    bool simulate;                   /* input == SIM (kept for existing callers) */
    const char *i2c_dev;             /* readers, LIVE only; OC_I2C_DEV_DEFAULT if not given */
    bool unsupervised_test;          /* hub: acknowledges test-only authorization */
    bool drop_conn_on_sigusr1;       /* readers: testing control */
    const char *runtime_dir;         /* points into argv; absolute */
    uint64_t session_id;             /* nonzero */
    uint32_t instance;               /* readers */
    bool have_light_instance;        /* hub */
    uint32_t light_instance;
    bool have_distance_instance;
    uint32_t distance_instance;
    uint32_t period_ms;              /* readers; role default if not given */
    uint32_t stat_ms;                /* 0 disables periodic STAT lines */
    uint64_t lifetime_ms;            /* 0 = unlimited; backstop against orphans */
    uint32_t fault_every;            /* readers: testing control */
    uint32_t hello_timeout_ms;       /* hub: 0 = hub default */
    int fd_headroom;                 /* hub: -1 = off, else descriptors allowed after init */
    /* hub capture mode (M3-A): persists accepted SIMULATED samples via oc_store */
    const char *capture_dir;         /* NULL = capture off; absolute, existing, NOT the runtime dir */
    uint32_t capture_ms;             /* 0 = OC_CAPTURE_DEFAULT_MS; max OC_CAPTURE_MAX_MS */
    uint32_t capture_arm_timeout_ms; /* 0 = default */
    uint32_t capture_fresh_ms;       /* 0 = default */
    size_t capture_segment_bytes;    /* testing: trace segment size, 0 = default */
    uint32_t capture_trace_segments; /* testing: trace ring length, 0 = default */
    uint64_t test_fsize_limit;       /* testing: RLIMIT_FSIZE in bytes after the store opens; 0 = off */
};

/* Parses argv[0..argc) where argv[1] is "--role" and argv[2] the role name.
 * Returns 0, or -EINVAL with a one-line message in err. Unknown options,
 * duplicates, missing or malformed values, options not valid for the role and
 * missing required options are all errors. */
int oc_role_parse(int argc, char **argv, struct oc_role_opts *o, char *err, size_t errcap);

/* True if either path equals, contains or lies inside the other, comparing
 * normalized components (repeated and trailing slashes ignored). Both must be
 * absolute; otherwise true (fail closed). Purely lexical: the runner also
 * compares realpath() results. */
bool oc_role_paths_overlap(const char *a, const char *b);

/* "<runtime_dir>/hub.sock" into out. 0, or -ENAMETOOLONG (does not fit
 * sockaddr_un) / -EINVAL. */
int oc_role_socket_path(const struct oc_role_opts *o, char *out, size_t cap);

/* "LIVE" / "SIMULATED" / "REPLAYED" / "?" for an enum oc_origin value, and the origin a mode
 * stands for (enum oc_origin; 0 for OC_INPUT_NONE). */
const char *oc_role_origin_name(uint8_t origin);
uint8_t oc_role_input_origin(enum oc_input_mode m);

void oc_role_usage(FILE *out);

/* Entry point called by main() when argv[1] is "--role". Returns the exit code. */
int oc_role_main(int argc, char **argv);

/* ---- signal helpers shared by the role runners ---- */

/* Blocks `sigs` (and SIGPIPE, so a closed log pipe gives EPIPE instead of a
 * signal) in the calling thread FIRST, then creates a CLOEXEC|NONBLOCK
 * signalfd for `sigs`. Returns the fd, or a negative errno (the mask change
 * is then undone). */
int oc_signalfd_open(const int *sigs, size_t n);

/* Reads every pending signal (nonblocking) into out[0..cap). Returns the
 * number stored (extra signals beyond cap are consumed and ignored), or a
 * negative errno. */
int oc_signalfd_drain(int sfd, int *out, size_t cap);

#endif
