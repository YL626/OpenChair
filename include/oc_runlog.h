#ifndef OC_RUNLOG_H
#define OC_RUNLOG_H

/* oc_runlog: one-line "key=value" report channel of the S3C role processes
 * (stdout). Two delivery classes:
 *
 *   oc_runlog_line      ROUTINE. One nonblocking write(). If the pipe is full
 *                       the line is DROPPED and counted in `dropped`; nothing
 *                       is queued, so a slow or absent reader can never stall
 *                       a role or grow its memory. Returns -EAGAIN for a drop.
 *   oc_runlog_critical  CRITICAL (READY, SUMMARY, fatal lifecycle). Retries
 *                       under one CLOCK_MONOTONIC deadline (poll for POLLOUT)
 *                       and returns 0 only when the whole line was written.
 *                       On failure it returns -ETIMEDOUT / -errno and the
 *                       caller must surface that (stderr + nonzero exit); a
 *                       report that was not delivered is never evidence.
 *
 * A line is formatted into a fixed OC_RUNLOG_MAX buffer (including the
 * newline). A line that does not fit is not written truncated; it fails with
 * -EMSGSIZE (routine: counted as dropped). Lines up to PIPE_BUF are written
 * atomically, so concurrent writers to one pipe do not interleave them.
 *
 * Ownership: the struct holds no resources; fd belongs to the caller. The
 * init call sets O_NONBLOCK on the open file description of fd. */
#include <stdint.h>

#define OC_RUNLOG_MAX 480u

struct oc_runlog {
    int fd;
    uint64_t dropped;     /* routine lines dropped (full pipe, too long) */
    uint64_t written;     /* lines fully written */
    int orig_flags;       /* fd flags before init, for oc_runlog_restore */
};

/* Returns 0, or -errno if fd cannot be made nonblocking (the log is then
 * unusable and the caller treats it as a runtime failure). */
int oc_runlog_init(struct oc_runlog *l, int fd);

/* Puts back the descriptor flags saved by init (undoes O_NONBLOCK on a shared
 * file description such as an inherited terminal). Best effort; call at exit. */
void oc_runlog_restore(struct oc_runlog *l);

int oc_runlog_line(struct oc_runlog *l, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
int oc_runlog_critical(struct oc_runlog *l, int timeout_ms, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#endif
