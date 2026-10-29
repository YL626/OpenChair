#ifndef OC_ROLE_HUB_H
#define OC_ROLE_HUB_H

/* oc_role_hub: the hub process (S3C).
 *
 * STATUS: S3C testing role; since M3-B the origin it authorizes, checks and labels
 * (both sources, capture included) is the --input mode (sim -> SIMULATED, live -> LIVE). Reuses oc_hub_init / oc_hub_poll_setup /
 * oc_hub_dispatch / oc_hub_source_status / oc_hub_shutdown unchanged; there is
 * no new state machine here. The only logic added is the process loop
 * (signals, periodic reports, accept-error backoff) and the callbacks.
 *
 * ---- Configuration is NOT supervisor authorization ----
 * --session-id, --light-instance and --distance-instance are explicit startup
 * flags (cfg.initial[]), accepted only together with --unsupervised-test, and
 * the first READY line says authz=TEST-CONFIG-NOT-SUPERVISOR. No
 * authorization message crosses a process boundary in S3C; S4 replaces this
 * with a supervisor that draws the session id and allocates a fresh instance
 * per spawned reader.
 *
 * ---- Loop ----
 * poll() waits on [signalfd | oc_hub_poll_setup fds]. Exit on SIGTERM/SIGINT or
 * --lifetime-ms.
 *
 * ---- Accept-error backoff ----
 * oc_hub_dispatch only calls accept when the listener's revents is set, and
 * an accept that keeps failing (EMFILE) leaves the connection in the kernel
 * backlog so poll would report the listener forever. When stats.accept_errors
 * grows, the runner stops polling the listener (its pollfd is set to -1, so
 * poll ignores it) for a delay of 50 ms doubling to 1 s (oc_backoff), while
 * established connections and the signalfd keep being served. The delay resets
 * after a successful accept. Wider recovery stays with S4.
 *
 * ---- Output (stdout, oc_runlog) ----
 *   READY    critical, after oc_hub_init
 *   STAT     routine, every --stat-ms: hub-wide counters
 *   SRC      routine, every --stat-ms, one line per source (state, counters)
 *   EVENT    routine, one line per hub event
 *   SRC + SUMMARY  critical, once at shutdown
 * Callbacks only update fixed-size counters and write one nonblocking line:
 * nothing is queued, so a slow stdout reader cannot grow hub memory (lines are
 * dropped and counted in log_dropped instead). */
#include "oc_role.h"

#define OC_HUB_ROLE_CRITICAL_MS 500
#define OC_HUB_ROLE_ACCEPT_BACKOFF_INITIAL_MS 50
#define OC_HUB_ROLE_ACCEPT_BACKOFF_CAP_MS 1000
/* A sample older than this (hub_rx - t_mono) fails the timestamp sanity check. */
#define OC_HUB_ROLE_MAX_SAMPLE_AGE_NS (10ull * 1000000000ull)

int oc_role_hub_run(const struct oc_role_opts *o);

#endif
