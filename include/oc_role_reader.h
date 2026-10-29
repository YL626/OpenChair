#ifndef OC_ROLE_READER_H
#define OC_ROLE_READER_H

/* oc_role_reader: the SIMULATED light / distance reader process (S3C).
 *
 * STATUS: S3C testing role. Samples come from oc_sim_source and are labelled
 * SIMULATED; there is no hardware and no LIVE mode. Uses only the existing
 * IPC client API (oc_ipc_client_connect / oc_ipc_client_handshake /
 * oc_ipc_send_sample / oc_ipc_recv) and the shared oc_sample codec.
 *
 * ---- Loop ----
 * One poll() waits on a signalfd (SIGTERM, SIGINT, and SIGUSR1 when
 * --drop-conn-on-sigusr1), the connection (hub BYE / EOF) and the nearest of:
 * next sampling tick, next reconnect attempt, next STAT line, lifetime end.
 * Shutdown therefore works in every state, including reconnect backoff.
 * Bounded non-interruptible waits: oc_ipc_client_handshake (at most
 * OC_READER_HANDSHAKE_MS, 250 ms; a shutdown request that arrives during it
 * waits that long), the BYE retry at exit (at most OC_READER_BYE_WAIT_MS,
 * 200 ms), and each critical report (at most OC_READER_CRITICAL_MS, 500 ms).
 *
 * ---- Counters (process lifetime, never reset by a reconnect) ----
 *   attempted      sample attempts; each consumes one seq (seq == attempted
 *                  before the attempt; the sequence is never restarted)
 *   Every attempt has EXACTLY ONE outcome:
 *   sent           handed to the kernel by oc_ipc_send_sample
 *   unsent_disc    not sent because no connection was open (not handed to the
 *                  IPC layer at all)
 *   bp_drop        send refused with -EAGAIN (kernel buffer full: backpressure)
 *   send_fail      any other send failure (-EPIPE, -ECONNRESET, ...)
 *   invariant: attempted == sent + unsent_disc + bp_drop + send_fail
 *   overruns       scheduled ticks that were NOT attempted because the loop
 *                  was late (e.g. after a handshake). They consume no seq and
 *                  are not samples; they are counted separately, never as drops.
 *   connect_attempts / connects / disconnects / rejects
 *
 * ---- Reconnect ----
 * After a lost connection or failed attempt: wait 100 ms, doubling to 2 s
 * (oc_backoff). The delay resets only after a connection stayed up for 1 s.
 * Reconnects reuse the SAME session and instance and never restart seq.
 * Permanent HELLO rejections (MALFORMED, BAD_SOURCE, BAD_ORIGIN,
 * SESSION_MISMATCH, PEER_CRED) end the process with exit 3; NOT_READY,
 * STALE_INSTANCE and DUPLICATE_SOURCE are retried with backoff.
 *
 * ---- Output (stdout, oc_runlog) ----
 *   READY   critical, after the first HELLO_ACK
 *   STAT    routine, every --stat-ms
 *   EVENT   routine lifecycle lines (connected, disconnected, ...)
 *   SUMMARY critical, once at exit (not written after SIGKILL: honest absence)
 * A critical line that cannot be delivered makes the process exit 4. */
#include "oc_role.h"

#define OC_READER_HANDSHAKE_MS 250
#define OC_READER_BYE_WAIT_MS 200
#define OC_READER_CRITICAL_MS 500
#define OC_READER_STABLE_NS 1000000000ull
#define OC_READER_BACKOFF_INITIAL_MS 100
#define OC_READER_BACKOFF_CAP_MS 2000

/* Runs a reader role (o->role LIGHT or DISTANCE). Returns the exit code. */
int oc_role_reader_run(const struct oc_role_opts *o);

#endif
