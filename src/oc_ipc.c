#define _GNU_SOURCE   /* accept4, SO_PEERCRED / struct ucred (strict -std=c23 hides them) */

#include "oc_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ---- time ---- */

static int mono_now(uint64_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -errno;
    *out = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return 0;
}

/* Waits for `events` on fd until the ABSOLUTE deadline (CLOCK_MONOTONIC ns).
 * EINTR and early wakeups re-evaluate against the same deadline, so signals
 * can neither restart nor extend it. 0 = ready (or HUP/ERR), -ETIMEDOUT. */
static int wait_fd(int fd, short events, uint64_t deadline_ns)
{
    for (;;) {
        uint64_t now;
        int rc = mono_now(&now);
        if (rc != 0)
            return rc;
        bool expired = now >= deadline_ns;
        int ms = 0;
        if (!expired) {
            uint64_t rem_ms = (deadline_ns - now + 999999ull) / 1000000ull;   /* round up */
            ms = rem_ms > (uint64_t)INT_MAX ? INT_MAX : (int)rem_ms;
        }
        struct pollfd p = {.fd = fd, .events = events};
        int r = poll(&p, 1, ms);
        if (r > 0)
            return 0;            /* checked even when expired: no lost last-instant data */
        if (r == 0 && expired)
            return -ETIMEDOUT;
        if (r < 0 && errno != EINTR)
            return -errno;
    }
}

static int deadline_after_ms(int timeout_ms, uint64_t *deadline)
{
    uint64_t now;
    int rc = mono_now(&now);
    if (rc != 0)
        return rc;
    *deadline = now + (uint64_t)timeout_ms * 1000000ull;
    return 0;
}

/* ---- init / close ---- */

void oc_ipc_conn_init(struct oc_ipc_conn *c)
{
    if (c != nullptr)
        *c = (struct oc_ipc_conn){.fd = -1};
}

void oc_ipc_server_init(struct oc_ipc_server *s)
{
    if (s != nullptr)
        *s = (struct oc_ipc_server){.fd = -1};
}

void oc_ipc_conn_close(struct oc_ipc_conn *c)
{
    if (c == nullptr)
        return;
    /* Only an object that says it owns its descriptor closes anything. A
     * zeroed object has fd == 0 but open == false, so descriptor 0 is safe.
     * close() is not retried on EINTR: on Linux the descriptor is gone. */
    if (c->open && c->fd >= 0)
        (void)close(c->fd);
    c->fd = -1;
    c->open = false;
    c->connecting = false;
}

/* ---- socket helpers ---- */

static int make_socket(void)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    return fd < 0 ? -errno : fd;
}

static int fill_addr(struct sockaddr_un *a, socklen_t *len, const char *path)
{
    size_t n = strlen(path);
    if (n == 0)
        return -EINVAL;
    if (n >= sizeof a->sun_path)
        return -ENAMETOOLONG;
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    memcpy(a->sun_path, path, n);
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n + 1);
    return 0;
}

/* The only buffering of unread datagrams is this per-socket kernel send
 * buffer; the kernel stops accepting sends when it is full. */
static int set_sndbuf(int fd, uint32_t *effective)
{
    int req = (int)OC_IPC_SNDBUF_REQUEST;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &req, sizeof req) != 0)
        return -errno;
    int got = 0;
    socklen_t l = sizeof got;
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &got, &l) != 0)
        return -errno;
    if (got <= 0)
        return -EIO;
    *effective = (uint32_t)got;
    return 0;
}

static int get_peer(int fd, struct oc_ipc_peer *out)
{
    struct ucred uc;
    socklen_t l = sizeof uc;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &l) != 0)
        return -errno;
    if (l != sizeof uc || uc.pid <= 0)
        return -EPROTO;
    out->pid = (uint32_t)uc.pid;
    out->uid = (uint32_t)uc.uid;
    out->gid = (uint32_t)uc.gid;
    return 0;
}

static void adopt(struct oc_ipc_conn *c, int fd, bool connecting, const struct oc_ipc_peer *peer,
                  uint32_t sndbuf)
{
    /* counters are kept: they belong to the object, not to one connection */
    c->fd = fd;
    c->open = true;
    c->connecting = connecting;
    c->sndbuf = sndbuf;
    c->peer = peer != nullptr ? *peer : (struct oc_ipc_peer){0};
}

/* ---- server ---- */

/* The socket path is only unlinked if it is still the very file we created. */
static void unlink_if_ours(const char *path, dev_t dev, ino_t ino)
{
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISSOCK(st.st_mode) && st.st_dev == dev && st.st_ino == ino)
        (void)unlink(path);
}

/* The parent must be a directory we own that nobody else can write to; that is
 * what makes replacing a stale socket in it safe from path swapping. */
static int check_parent_dir(const char *path)
{
    char dir[sizeof((struct sockaddr_un){0}.sun_path)];
    const char *slash = strrchr(path, '/');
    if (slash == nullptr) {
        strcpy(dir, ".");
    } else if (slash == path) {
        strcpy(dir, "/");
    } else {
        memcpy(dir, path, (size_t)(slash - path));
        dir[slash - path] = '\0';
    }
    struct stat st;
    if (stat(dir, &st) != 0)
        return -errno;
    if (!S_ISDIR(st.st_mode))
        return -ENOTDIR;
    if (st.st_uid != geteuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0)
        return -EACCES;
    return 0;
}

/* Existing socket file: 0 = stale (no listener), 1 = vanished meanwhile,
 * -EADDRINUSE = a server answers (or its backlog is full). */
static int probe_existing(const char *path)
{
    struct sockaddr_un a;
    socklen_t al;
    int rc = fill_addr(&a, &al, path);
    if (rc != 0)
        return rc;
    int fd = make_socket();
    if (fd < 0)
        return fd;
    int r = connect(fd, (struct sockaddr *)&a, al);
    int e = errno;
    (void)close(fd);
    if (r == 0)
        return -EADDRINUSE;
    switch (e) {
    case ECONNREFUSED: return 0;
    case ENOENT: return 1;
    case EAGAIN: return -EADDRINUSE;
    default: return -e;
    }
}

int oc_ipc_server_open(struct oc_ipc_server *s, const char *path)
{
    if (s == nullptr || path == nullptr)
        return -EINVAL;
    if (s->open)
        return -EBUSY;
    size_t n = strlen(path);
    if (n == 0 || path[n - 1] == '/')
        return -EINVAL;
    if (n >= sizeof s->path)
        return -ENAMETOOLONG;

    int rc = check_parent_dir(path);
    if (rc != 0)
        return rc;

    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode))
            return -ENOTSOCK;                  /* file, symlink, directory: never touched */
        if (st.st_uid != geteuid())
            return -EACCES;
        rc = probe_existing(path);
        if (rc < 0)
            return rc;
        if (rc == 0) {                         /* stale: remove exactly what we examined */
            struct stat again;
            if (lstat(path, &again) != 0 || !S_ISSOCK(again.st_mode) ||
                again.st_dev != st.st_dev || again.st_ino != st.st_ino)
                return -EEXIST;                /* changed under us: do not guess */
            if (unlink(path) != 0 && errno != ENOENT)
                return -errno;
        }
    } else if (errno != ENOENT) {
        return -errno;
    }

    struct sockaddr_un a;
    socklen_t al;
    rc = fill_addr(&a, &al, path);
    if (rc != 0)
        return rc;
    int fd = make_socket();
    if (fd < 0)
        return fd;
    if (bind(fd, (struct sockaddr *)&a, al) != 0) {
        rc = -errno;
        (void)close(fd);
        return rc;
    }

    struct stat mine;
    if (lstat(path, &mine) != 0) {
        rc = -errno;
        (void)close(fd);
        return rc;
    }
    if (!S_ISSOCK(mine.st_mode)) {         /* replaced between bind and lstat */
        (void)close(fd);
        return -EEXIST;
    }
    if (chmod(path, 0600) != 0 || listen(fd, OC_IPC_BACKLOG) != 0) {
        rc = -errno;
        unlink_if_ours(path, mine.st_dev, mine.st_ino);
        (void)close(fd);
        return rc;
    }

    s->fd = fd;
    s->open = true;
    memcpy(s->path, path, n + 1);
    s->dev = mine.st_dev;
    s->ino = mine.st_ino;
    return 0;
}

int oc_ipc_server_accept(struct oc_ipc_server *s, struct oc_ipc_conn *out)
{
    if (s == nullptr || out == nullptr)
        return -EINVAL;
    if (!s->open)
        return -ENOTCONN;
    if (out->open)
        return -EISCONN;

    int fd;
    do {
        fd = accept4(s->fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return -EAGAIN;
        return -errno;                         /* ECONNABORTED, EMFILE, ... */
    }

    uint32_t sndbuf = 0;
    struct oc_ipc_peer peer;
    int rc = set_sndbuf(fd, &sndbuf);
    if (rc == 0)
        rc = get_peer(fd, &peer);
    if (rc != 0) {
        (void)close(fd);
        return rc;
    }
    adopt(out, fd, false, &peer, sndbuf);
    return 0;
}

void oc_ipc_server_close(struct oc_ipc_server *s)
{
    if (s == nullptr)
        return;
    if (s->open) {
        if (s->fd >= 0)
            (void)close(s->fd);
        unlink_if_ours(s->path, s->dev, s->ino);
    }
    *s = (struct oc_ipc_server){.fd = -1};
}

/* ---- client ---- */

/* Confirms the connect really produced a peer, and that the peer is us. */
static int connected_checks(int fd, struct oc_ipc_peer *peer)
{
    struct sockaddr_un pa;
    socklen_t pl = sizeof pa;
    if (getpeername(fd, (struct sockaddr *)&pa, &pl) != 0)
        return -errno;                         /* ENOTCONN: nothing is there */
    int rc = get_peer(fd, peer);
    if (rc != 0)
        return rc;
    if (peer->uid != (uint32_t)geteuid())
        return -EPERM;                         /* a server of another user (not -EACCES: that
                                                * is reserved for a HELLO_REJECT) */
    return 0;
}

int oc_ipc_client_connect(struct oc_ipc_conn *c, const char *path)
{
    if (c == nullptr || path == nullptr)
        return -EINVAL;
    if (c->open)
        return -EISCONN;
    struct sockaddr_un a;
    socklen_t al;
    int rc = fill_addr(&a, &al, path);
    if (rc != 0)
        return rc;
    int fd = make_socket();
    if (fd < 0)
        return fd;
    uint32_t sndbuf = 0;
    rc = set_sndbuf(fd, &sndbuf);
    if (rc != 0) {
        (void)close(fd);
        return rc;
    }

    /* One connect() only; EINTR is reported as in progress without a retry.
     * For AF_UNIX the attempt may in fact have been abandoned, which
     * oc_ipc_client_connect_finish detects (getpeername -> ENOTCONN) and
     * reports as a failure, so a connect that never completes is never
     * mistaken for a connected socket. */
    if (connect(fd, (struct sockaddr *)&a, al) != 0) {
        if (errno == EINPROGRESS || errno == EINTR) {
            adopt(c, fd, true, nullptr, sndbuf);
            return -EINPROGRESS;
        }
        rc = errno == EWOULDBLOCK ? -EAGAIN : -errno;   /* EAGAIN: backlog full */
        (void)close(fd);
        return rc;
    }
    struct oc_ipc_peer peer;
    rc = connected_checks(fd, &peer);
    if (rc != 0) {
        (void)close(fd);
        return rc;
    }
    adopt(c, fd, false, &peer, sndbuf);
    return 0;
}

int oc_ipc_client_connect_finish(struct oc_ipc_conn *c)
{
    if (c == nullptr)
        return -EINVAL;
    if (!c->open)
        return -ENOTCONN;
    if (!c->connecting)
        return 0;

    struct pollfd p = {.fd = c->fd, .events = POLLOUT};
    int r;
    do {
        r = poll(&p, 1, 0);
    } while (r < 0 && errno == EINTR);
    int rc;
    if (r < 0) {
        rc = -errno;
    } else if (r == 0) {
        return -EINPROGRESS;
    } else {
        int err = 0;
        socklen_t l = sizeof err;
        if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &l) != 0)
            rc = -errno;
        else if (err != 0)
            rc = -err;
        else
            rc = connected_checks(c->fd, &c->peer);
        if (rc == 0) {
            c->connecting = false;
            return 0;
        }
    }
    oc_ipc_conn_close(c);
    return rc;
}

/* ---- send / receive ---- */

static int send_raw(struct oc_ipc_conn *c, const uint8_t *buf, size_t len)
{
    if (!c->open || c->connecting)
        return -ENOTCONN;
    ssize_t n;
    do {
        n = send(c->fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -EAGAIN : -errno;
    if ((size_t)n != len)
        return -EIO;               /* SEQPACKET is all-or-nothing: this is a bug */
    c->st.tx_msgs++;
    return 0;
}

int oc_ipc_send_msg(struct oc_ipc_conn *c, const uint8_t *buf, size_t len)
{
    if (c == nullptr || buf == nullptr || len < OC_MSG_HEADER_SIZE || len > OC_IPC_MAX_MSG)
        return -EINVAL;
    return send_raw(c, buf, len);
}

int oc_ipc_send_sample(struct oc_ipc_conn *c, const struct oc_sample *s)
{
    if (c == nullptr || s == nullptr)
        return -EINVAL;
    uint8_t buf[OC_IPC_MAX_MSG];
    size_t n = 0;
    if (oc_msg_sample_encode(s, buf, sizeof buf, &n) != 0)
        return -EINVAL;            /* invalid sample: caller bug, not a drop */
    int rc = send_raw(c, buf, n);
    if (rc != 0)
        c->st.tx_dropped++;        /* the single place a sample is counted as dropped */
    return rc;
}

int oc_ipc_recv(struct oc_ipc_conn *c, struct oc_ipc_msg *out)
{
    if (c == nullptr || out == nullptr)
        return -EINVAL;
    if (!c->open || c->connecting)
        return -ENOTCONN;

    uint8_t buf[OC_IPC_RECV_CAP];
    struct iovec iov = {.iov_base = buf, .iov_len = sizeof buf};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
    ssize_t n;
    do {
        mh.msg_flags = 0;
        /* MSG_TRUNC: the return value is the datagram's real length even when
         * it is larger than the buffer; the excess is discarded. */
        n = recvmsg(c->fd, &mh, MSG_DONTWAIT | MSG_TRUNC);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? -EAGAIN : -errno;
    if (n == 0) {
        /* EOF, or a zero-length datagram from a misbehaving peer: on SEQPACKET
         * both read as 0. Only a hung-up socket is EOF. */
        struct pollfd hp = {.fd = c->fd, .events = POLLIN};
        int pr;
        do {
            pr = poll(&hp, 1, 0);
        } while (pr < 0 && errno == EINTR);
        if (pr > 0 && (hp.revents & (POLLHUP | POLLERR)) != 0)
            return -EPIPE;
        c->st.rx_malformed++;      /* consumed; the connection is still up */
        return -EBADMSG;
    }

    size_t len = (size_t)n;
    if (buf[0] != OC_PROTO_VERSION) {
        oc_ipc_conn_close(c);      /* fatal, whatever the size: no reply is possible */
        return -EPROTONOSUPPORT;
    }
    if (len > OC_IPC_MAX_MSG) {
        c->st.rx_oversized++;      /* consumed whole, stream stays aligned */
        return -EMSGSIZE;
    }
    struct oc_ipc_msg m;
    int rc = oc_ipc_msg_decode(buf, len, &m);
    if (rc == -EPROTONOSUPPORT) {
        oc_ipc_conn_close(c);
        return rc;
    }
    if (rc != 0) {
        c->st.rx_malformed++;
        return rc;
    }
    *out = m;
    c->st.rx_msgs++;
    return 0;
}

int oc_ipc_wait_readable(const struct oc_ipc_conn *c, int timeout_ms)
{
    if (c == nullptr)
        return -EINVAL;
    if (!c->open || c->connecting)
        return -ENOTCONN;
    if (timeout_ms < 0)
        return -EINVAL;
    uint64_t deadline;
    int rc = deadline_after_ms(timeout_ms, &deadline);
    if (rc != 0)
        return rc;
    return wait_fd(c->fd, POLLIN, deadline);
}

/* ---- handshake ---- */

static int handshake_io(struct oc_ipc_conn *c, const uint8_t *hello_bytes, size_t hello_len,
                        const struct oc_hello *hello, uint64_t deadline,
                        struct oc_hello_reject *reject)
{
    int rc;
    while ((rc = oc_ipc_client_connect_finish(c)) == -EINPROGRESS) {
        rc = wait_fd(c->fd, POLLOUT, deadline);
        if (rc != 0)
            return rc;
    }
    if (rc != 0)
        return rc;

    while ((rc = oc_ipc_send_msg(c, hello_bytes, hello_len)) == -EAGAIN) {
        rc = wait_fd(c->fd, POLLOUT, deadline);
        if (rc != 0)
            return rc;
    }
    if (rc != 0)
        return rc;

    for (;;) {
        rc = wait_fd(c->fd, POLLIN, deadline);
        if (rc != 0)
            return rc;
        struct oc_ipc_msg m;
        rc = oc_ipc_recv(c, &m);
        if (rc == -EAGAIN)
            continue;              /* spurious wakeup */
        if (rc != 0)
            return rc;
        switch (m.type) {
        case OC_MSG_HELLO_ACK:
            if (m.u.ack.session_id != hello->session_id ||
                m.u.ack.instance != hello->instance || m.u.ack.source != hello->source)
                return -EPROTO;    /* an ACK for someone else is not our ACK */
            return 0;
        case OC_MSG_HELLO_REJECT:
            if (reject != nullptr)
                *reject = m.u.reject;
            return -EACCES;
        case OC_MSG_BYE:
            return -ECONNABORTED;
        default:
            return -EPROTO;
        }
    }
}

int oc_ipc_client_handshake(struct oc_ipc_conn *c, const struct oc_hello *hello, int timeout_ms,
                            struct oc_hello_reject *reject)
{
    if (c == nullptr || hello == nullptr || timeout_ms <= 0)
        return -EINVAL;
    uint8_t buf[OC_IPC_MAX_MSG];
    size_t n = 0;
    int rc = oc_ipc_encode_hello(hello, buf, sizeof buf, &n);
    if (rc != 0)
        return rc;
    if (!c->open)
        return -ENOTCONN;

    uint64_t deadline;
    rc = deadline_after_ms(timeout_ms, &deadline);
    if (rc == 0)
        rc = handshake_io(c, buf, n, hello, deadline, reject);
    if (rc != 0)
        oc_ipc_conn_close(c);      /* a failed handshake is never reusable */
    return rc;
}
