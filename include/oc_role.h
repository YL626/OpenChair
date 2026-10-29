#ifndef OC_ROLE_H
#define OC_ROLE_H

/* oc_role: role dispatch for the single openchair executable (S3C).
 *
 *   openchair --role hub      ...   the hub process
 *   openchair --role light    ...   a SIMULATED light reader
 *   openchair --role distance ...   a SIMULATED distance reader
 *   openchair --role help           role usage on stdout, exit 0
 *
 * --version and --help are handled by main() and are unchanged; role options
 * are documented by `--role help` and in README.md, not in --help.
 *
 * STATUS: S3C testing roles. Every sample is SIMULATED. There is no LIVE mode.
 * The hub's authorized instances come from explicit command-line flags that
 * only exist for testing (--unsupervised-test); this is NOT supervisor
 * authorization, and no authorization message crosses a process boundary.
 *
 * Exit codes: 0 clean shutdown, 2 usage error, 3 configuration error or a
 * permanent HELLO rejection, 4 runtime failure (including a critical report
 * that could not be delivered).
 *
 * Testing controls (readers, only with --simulate; hub, only with
 * --unsupervised-test): --fault-every, --drop-conn-on-sigusr1,
 * --hello-timeout-ms, --test-fd-headroom, --lifetime-ms. */
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

struct oc_role_opts {
    enum oc_role_kind role;
    bool simulate;
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
};

/* Parses argv[0..argc) where argv[1] is "--role" and argv[2] the role name.
 * Returns 0, or -EINVAL with a one-line message in err. Unknown options,
 * duplicates, missing or malformed values, options not valid for the role and
 * missing required options are all errors. */
int oc_role_parse(int argc, char **argv, struct oc_role_opts *o, char *err, size_t errcap);

/* "<runtime_dir>/hub.sock" into out. 0, or -ENAMETOOLONG (does not fit
 * sockaddr_un) / -EINVAL. */
int oc_role_socket_path(const struct oc_role_opts *o, char *out, size_t cap);

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
