#define _GNU_SOURCE

#include "oc_runlog.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t mono_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int oc_runlog_init(struct oc_runlog *l, int fd)
{
    l->fd = fd;
    l->dropped = 0;
    l->written = 0;
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0)
        return -errno;
    l->orig_flags = fl;
    if ((fl & O_NONBLOCK) == 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0)
        return -errno;
    return 0;
}

/* Formats into buf; returns the length including the newline or -EMSGSIZE. */
static int format_line(char *buf, size_t cap, const char *fmt, va_list ap)
{
    int n = vsnprintf(buf, cap - 1, fmt, ap);
    if (n < 0 || (size_t)n >= cap - 1)
        return -EMSGSIZE;
    buf[n++] = '\n';
    return n;
}

/* Writes buf[0..len) without blocking. Returns bytes written (>= 0; 0 means
 * EAGAIN) or -errno. Short writes are possible for non-pipe descriptors. */
static ssize_t write_some(int fd, const char *buf, size_t len)
{
    for (;;) {
        ssize_t n = write(fd, buf, len);
        if (n >= 0)
            return n;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -errno;
    }
}

void oc_runlog_restore(struct oc_runlog *l)
{
    if (l->fd >= 0)
        (void)fcntl(l->fd, F_SETFL, l->orig_flags);
}

int oc_runlog_line(struct oc_runlog *l, const char *fmt, ...)
{
    char buf[OC_RUNLOG_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = format_line(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) {
        l->dropped++;
        return n;
    }
    ssize_t w = write_some(l->fd, buf, (size_t)n);
    if (w == (ssize_t)n) {
        l->written++;
        return 0;
    }
    /* Nothing written (EAGAIN) or an error: drop. A partial write of a short
     * line cannot happen on a pipe; on other files the remainder is abandoned,
     * which is acceptable for routine output and still counted. */
    l->dropped++;
    return w < 0 ? (int)w : -EAGAIN;
}

int oc_runlog_critical(struct oc_runlog *l, int timeout_ms, const char *fmt, ...)
{
    char buf[OC_RUNLOG_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = format_line(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return n;

    const uint64_t t0 = mono_ns();
    const uint64_t deadline = t0 + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0) * 1000000ull;
    size_t off = 0;
    for (;;) {
        ssize_t w = write_some(l->fd, buf + off, (size_t)n - off);
        if (w < 0)
            return (int)w;
        off += (size_t)w;
        if (off == (size_t)n) {
            l->written++;
            return 0;
        }
        uint64_t now = mono_ns();
        if (now >= deadline)
            return -ETIMEDOUT;
        int ms = (int)((deadline - now + 999999ull) / 1000000ull);
        struct pollfd pf = {.fd = l->fd, .events = POLLOUT};
        int rc = poll(&pf, 1, ms > 100 ? 100 : ms);
        if (rc < 0 && errno != EINTR)
            return -errno;
    }
}
