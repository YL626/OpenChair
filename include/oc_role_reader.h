#ifndef OC_ROLE_READER_H
#define OC_ROLE_READER_H

/* oc_role_reader: the SIMULATED light / distance reader process (S3C).
 *
 * STATUS: S3C testing role plus M3-B. With --input sim, samples come from
 * oc_sim_source and are labelled SIMULATED. With --input live they come from
 * the real ADS1115 / VL6180X through oc_light / oc_distance over oc_i2c_linux
 * (oc_reader_backend.h) and are labelled LIVE; that path is host-tested only
 * against fakes and has not run on the Pi. The backend, not the loop, owns the
 * sample sequence, the origin and the timestamp. Uses only the existing
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
 * A critical line that cannot be delivered makes the process exit 4.
 *
 * ---- Input backend (M3-B) ----
 * Opened once before the first connection, kept across every reconnect and
 * closed on every exit. A LIVE backend that cannot be opened (no such device,
 * not an I2C adapter) is exit 3 before anything connects. A LIVE hardware
 * error is a non-OK sample (NACK/TIMEOUT/IO_ERR, value 0), counted in
 * non_ok= and sent like any sample; the reader never switches to simulation.
 * HELLO, samples and every log line carry the backend's origin. */
#include "oc_role.h"

#define OC_READER_HANDSHAKE_MS 250
#define OC_READER_BYE_WAIT_MS 200
#define OC_READER_CRITICAL_MS 500
#define OC_READER_STABLE_NS 1000000000ull
#define OC_READER_BACKOFF_INITIAL_MS 100
#define OC_READER_BACKOFF_CAP_MS 2000

struct oc_reader_backend;

/* Runs a reader role (o->role LIGHT or DISTANCE). Returns the exit code. The backend (SIM, or
 * LIVE over o->i2c_dev) is opened once here, kept across every reconnect, and closed on every
 * exit; a LIVE backend that cannot be opened is exit 3 before anything connects or is sent. */
int oc_role_reader_run(const struct oc_role_opts *o);

/* Test seam: the same loop over an already-open backend (for example an adapter on a fake bus).
 * Takes ownership: the backend is closed before return. Exit 3 if it is not open. Not used by
 * the openchair binary. */
int oc_role_reader_run_backend(const struct oc_role_opts *o, struct oc_reader_backend *be);

#endif
