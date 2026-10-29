/* Tests for the IPC transport (include/oc_ipc.h) over REAL Linux
 * AF_UNIX SOCK_SEQPACKET sockets in a private temporary directory.
 *
 * Only the sensor data is synthetic (every sample here is SIMULATED origin,
 * with made-up values). There is no hardware, no I2C and no storage. Passing
 * these tests shows IPC logic on this Linux kernel; it says nothing about the
 * Raspberry Pi hardware.
 *
 * A "hub" is played by this test: either in-process (calling the library's
 * accept/recv/send directly) or by a forked child that runs a scripted reply
 * mode. The real hub state machine is NOT implemented here (that is S3B).
 *
 * Fault seams: this binary is linked with -Wl,--wrap=connect,--wrap=send,
 * --wrap=recvmsg so a test can make the library see in-progress connects,
 * short sends and EINTR deterministically. The default mode of every wrapper
 * is to call the real function. */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "oc_ipc.h"

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        long long a_ = (long long)(a), b_ = (long long)(b);                   \
        if (a_ != b_) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s == %lld, want %s == %lld\n",      \
                    __FILE__, __LINE__, #a, a_, #b, b_);                      \
            failures++;                                                       \
        }                                                                     \
    } while (0)

#define RUN(test)                          \
    do {                                   \
        fprintf(stderr, "== %s\n", #test); \
        test();                            \
    } while (0)

#define TEST_SESSION 0xC0FFEE0012345678ull

/* ---------- fault seams ---------- */

enum connect_mode { CONNECT_REAL, CONNECT_REAL_THEN_INPROGRESS, CONNECT_REAL_THEN_EINTR,
                    CONNECT_FAKE_INPROGRESS };
enum send_mode { SEND_REAL, SEND_SHORT, SEND_EINTR_ONCE };
enum recv_mode { RECV_REAL, RECV_EINTR_ONCE };
static enum connect_mode connect_mode;
static enum send_mode send_mode;
static enum recv_mode recv_mode;
static int last_send_flags = -1;   /* flags of the most recent send() */

int __real_connect(int fd, const struct sockaddr *a, socklen_t l);
ssize_t __real_send(int fd, const void *b, size_t n, int flags);
ssize_t __real_recvmsg(int fd, struct msghdr *m, int flags);

int __wrap_connect(int fd, const struct sockaddr *a, socklen_t l)
{
    switch (connect_mode) {
    case CONNECT_REAL_THEN_INPROGRESS:
    case CONNECT_REAL_THEN_EINTR: {
        int rc = __real_connect(fd, a, l);   /* really connects ... */
        if (rc == 0) {                        /* ... but reports it as in flight */
            errno = connect_mode == CONNECT_REAL_THEN_EINTR ? EINTR : EINPROGRESS;
            return -1;
        }
        return rc;
    }
    case CONNECT_FAKE_INPROGRESS:             /* never connects at all */
        errno = EINPROGRESS;
        return -1;
    case CONNECT_REAL:
    default:
        return __real_connect(fd, a, l);
    }
}

ssize_t __wrap_send(int fd, const void *b, size_t n, int flags)
{
    last_send_flags = flags;
    if (send_mode == SEND_SHORT && n > 1)
        return (ssize_t)(n - 1);              /* kernel "accepted" a partial datagram */
    if (send_mode == SEND_EINTR_ONCE) {
        send_mode = SEND_REAL;
        errno = EINTR;
        return -1;
    }
    return __real_send(fd, b, n, flags);
}

ssize_t __wrap_recvmsg(int fd, struct msghdr *m, int flags)
{
    if (recv_mode == RECV_EINTR_ONCE) {
        recv_mode = RECV_REAL;
        errno = EINTR;
        return -1;
    }
    return __real_recvmsg(fd, m, flags);
}

/* ---------- helpers ---------- */

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void ms_sleep(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/* Open descriptors of this process, excluding the one used to list them. */
static int fd_count(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (d == nullptr)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] != '.')
            n++;
    }
    closedir(d);
    return n - 1;
}

static bool fd_is_open(int fd)
{
    return fcntl(fd, F_GETFD) != -1;
}

static int rm_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb;
    (void)flag;
    (void)ftw;
    (void)remove(path);
    return 0;
}

static void rmrf(const char *path)
{
    if (path[0] == '\0')
        return;
    (void)chmod(path, 0700);
    (void)nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* Fresh private directory (mode 0700, owned by us). */
static bool make_dir(char *buf, size_t cap)
{
    const char *base = getenv("TMPDIR");
    if (base == nullptr || base[0] == '\0')
        base = "/tmp";
    int n = snprintf(buf, cap, "%s/oc_ipc_test.XXXXXX", base);
    if (n < 0 || (size_t)n >= cap)
        return false;
    return mkdtemp(buf) != nullptr;
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s/%s", dir, name);
}

static struct oc_hello make_hello(uint8_t source, uint8_t origin, uint32_t instance)
{
    return (struct oc_hello){.source = source, .origin = origin, .session_id = TEST_SESSION,
                             .instance = instance, .pid = (uint32_t)getpid()};
}

static struct oc_sample make_sample(uint32_t seq)
{
    return (struct oc_sample){.t_mono_ns = 1000u + seq, .seq = seq, .source = OC_SOURCE_LIGHT,
                              .origin = OC_ORIGIN_SIMULATED, .status = OC_ST_OK,
                              .value = (int32_t)seq, .aux = 0x4383};
}

/* Leaves a socket file with no listener behind (what a crashed hub leaves). */
static bool make_stale_socket(const char *path)
{
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    size_t n = strlen(path);
    if (n >= sizeof a.sun_path)
        return false;
    memcpy(a.sun_path, path, n);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    bool ok = bind(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}

static int raw_send(const struct oc_ipc_conn *c, const void *buf, size_t len)
{
    ssize_t n = send(c->fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
    return n == (ssize_t)len ? 0 : -1;
}

/* Receives with a short wait; returns oc_ipc_recv's result. */
static int recv_wait(struct oc_ipc_conn *c, struct oc_ipc_msg *m)
{
    int rc = oc_ipc_wait_readable(c, 2000);
    if (rc != 0)
        return rc;
    return oc_ipc_recv(c, m);
}

/* A server in a private dir with one connected client and its accepted peer. */
struct pair {
    char dir[PATH_MAX];
    char path[PATH_MAX + 16];
    struct oc_ipc_server srv;
    struct oc_ipc_conn cli;   /* client end */
    struct oc_ipc_conn hub;   /* accepted end: this test plays the hub */
    bool dir_made;
};

static bool pair_open(struct pair *p)
{
    *p = (struct pair){0};
    oc_ipc_server_init(&p->srv);
    oc_ipc_conn_init(&p->cli);
    oc_ipc_conn_init(&p->hub);
    p->dir_made = make_dir(p->dir, sizeof p->dir);
    CHECK(p->dir_made);
    if (!p->dir_made)
        return false;
    join(p->path, sizeof p->path, p->dir, "hub.sock");
    CHECK_EQ(oc_ipc_server_open(&p->srv, p->path), 0);
    CHECK_EQ(oc_ipc_client_connect(&p->cli, p->path), 0);
    int rc = -EAGAIN;
    for (int i = 0; i < 100 && rc == -EAGAIN; i++) {
        rc = oc_ipc_server_accept(&p->srv, &p->hub);
        if (rc == -EAGAIN)
            ms_sleep(5);
    }
    CHECK_EQ(rc, 0);
    return p->srv.open && p->cli.open && p->hub.open;
}

static void pair_close(struct pair *p)
{
    oc_ipc_conn_close(&p->cli);
    oc_ipc_conn_close(&p->hub);
    oc_ipc_server_close(&p->srv);
    if (p->dir_made)
        rmrf(p->dir);
}

/* ---------- signal storm: periodic SIGALRM without SA_RESTART ---------- */

static volatile sig_atomic_t alarms;
static struct sigaction old_alarm;

static void on_alarm(int sig)
{
    (void)sig;
    alarms++;
}

static void storm_start(long interval_us)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* deliberately no SA_RESTART: syscalls see EINTR */
    alarms = 0;
    sigaction(SIGALRM, &sa, &old_alarm);
    struct itimerval it = {.it_interval = {0, interval_us}, .it_value = {0, interval_us}};
    setitimer(ITIMER_REAL, &it, nullptr);
}

static void storm_stop(void)
{
    struct itimerval off = {0};
    setitimer(ITIMER_REAL, &off, nullptr);
    sigaction(SIGALRM, &old_alarm, nullptr);
}

/* ---------- scripted hub in a forked child ---------- */

enum hub_mode {
    H_POLICY,            /* hello_check, then ACK (echo) or REJECT(reason) */
    H_ACK_BAD_SESSION,   /* ACK echoing a different session_id */
    H_ACK_BAD_INSTANCE,
    H_ACK_BAD_SOURCE,
    H_BYE,
    H_SAMPLE,            /* a valid SAMPLE instead of ACK/REJECT */
    H_BADVER,            /* a version-2 datagram */
    H_SHORT,             /* 3-byte datagram */
    H_UNKNOWN_TYPE,
    H_CLOSE,             /* close without replying */
    H_SILENT             /* read HELLO, never answer */
};

[[noreturn]] static void hub_child(struct oc_ipc_server *srv, enum hub_mode mode, int delay_ms)
{
    struct oc_ipc_conn c;
    oc_ipc_conn_init(&c);
    uint64_t end = now_ns() + (mode == H_SILENT ? 800000000ull : 3000000000ull);
    int rc;
    while ((rc = oc_ipc_server_accept(srv, &c)) == -EAGAIN) {
        if (now_ns() > end)
            _exit(mode == H_SILENT ? 0 : 10);   /* H_SILENT: client may never connect */
        struct pollfd p = {.fd = srv->fd, .events = POLLIN};
        (void)poll(&p, 1, 50);
    }
    if (rc != 0)
        _exit(11);
    if (oc_ipc_wait_readable(&c, 3000) != 0)
        _exit(12);
    struct oc_ipc_msg m;
    if (oc_ipc_recv(&c, &m) != 0 || m.type != OC_MSG_HELLO)
        _exit(13);
    if (c.peer.pid != (uint32_t)getppid())   /* the test process connected */
        _exit(14);
    ms_sleep(delay_ms);

    uint8_t b[64];
    size_t n = 0;
    int send_rc = 0;
    struct oc_hello_ack a = {.session_id = m.u.hello.session_id,
                             .instance = m.u.hello.instance, .source = m.u.hello.source};
    switch (mode) {
    case H_POLICY: {
        struct oc_ipc_policy pol = {.session_id = TEST_SESSION, .uid = (uint32_t)geteuid(),
                                    .check_pid = true};
        pol.origin[OC_SOURCE_LIGHT] = OC_ORIGIN_LIVE;
        pol.origin[OC_SOURCE_DISTANCE] = OC_ORIGIN_SIMULATED;
        enum oc_reject_reason why = oc_ipc_hello_check(&pol, &m.u.hello, &c.peer);
        if (why == OC_REJECT_NONE) {
            if (oc_ipc_encode_hello_ack(&a, b, sizeof b, &n) != 0)
                _exit(15);
        } else {
            struct oc_hello_reject r = {.reason = (uint16_t)why, .detail = 0};
            if (oc_ipc_encode_hello_reject(&r, b, sizeof b, &n) != 0)
                _exit(16);
        }
        send_rc = oc_ipc_send_msg(&c, b, n);
        break;
    }
    case H_ACK_BAD_SESSION:
        a.session_id ^= 1;
        goto send_ack;
    case H_ACK_BAD_INSTANCE:
        a.instance ^= 1;
        goto send_ack;
    case H_ACK_BAD_SOURCE:
        a.source = a.source == OC_SOURCE_LIGHT ? OC_SOURCE_DISTANCE : OC_SOURCE_LIGHT;
    send_ack:
        if (oc_ipc_encode_hello_ack(&a, b, sizeof b, &n) != 0)
            _exit(17);
        send_rc = oc_ipc_send_msg(&c, b, n);
        break;
    case H_BYE: {
        struct oc_bye y = {.reason = OC_BYE_SHUTDOWN};
        if (oc_ipc_encode_bye(&y, b, sizeof b, &n) != 0)
            _exit(18);
        send_rc = oc_ipc_send_msg(&c, b, n);
        break;
    }
    case H_SAMPLE: {
        struct oc_sample s = make_sample(1);
        if (oc_msg_sample_encode(&s, b, sizeof b, &n) != 0)
            _exit(19);
        send_rc = oc_ipc_send_msg(&c, b, n);
        break;
    }
    case H_BADVER:
        memset(b, 0, sizeof b);
        memcpy(b, "\x02\x02\x10\x00", 4);
        send_rc = raw_send(&c, b, 20);
        break;
    case H_SHORT:
        send_rc = raw_send(&c, "\x01\x02\x10", 3);
        break;
    case H_UNKNOWN_TYPE:
        send_rc = raw_send(&c, "\x01\x63\x00\x00", 4);
        break;
    case H_CLOSE:
        oc_ipc_conn_close(&c);
        _exit(0);
    case H_SILENT:
        break;
    }
    /* a client that already gave up (deadline test) is not a hub failure */
    if (send_rc != 0 && send_rc != -EPIPE && send_rc != -ECONNRESET)
        _exit(20);
    /* stay until the client hangs up (or a bounded time), so the reply is
     * never raced by our own EOF */
    (void)oc_ipc_wait_readable(&c, 1500);
    oc_ipc_conn_close(&c);
    _exit(0);
}

struct hs_opts {
    enum hub_mode mode;
    int hub_delay_ms;
    int timeout_ms;
    enum connect_mode cmode;   /* applied only around the client connect */
    long storm_us;             /* >0: SIGALRM storm during the handshake */
};

struct hs_result {
    int rc;
    struct oc_hello_reject reject;
    bool conn_open_after;
    uint64_t elapsed_ms;
    int alarms_seen;
    int fds_before, fds_after;
    uint16_t peer_uid_ok;
};

#define REJECT_SENTINEL 0xFFFFu

static void run_handshake(const struct hs_opts *o, const struct oc_hello *hello,
                          struct hs_result *r)
{
    *r = (struct hs_result){.reject = {.reason = REJECT_SENTINEL, .detail = 0xABCD}};
    char dir[PATH_MAX], path[PATH_MAX + 16];
    struct oc_ipc_server srv;
    struct oc_ipc_conn cli;
    oc_ipc_server_init(&srv);
    oc_ipc_conn_init(&cli);
    CHECK(make_dir(dir, sizeof dir));
    join(path, sizeof path, dir, "hub.sock");
    r->fds_before = fd_count();
    CHECK_EQ(oc_ipc_server_open(&srv, path), 0);

    pid_t pid = fork();
    if (pid == 0)
        hub_child(&srv, o->mode, o->hub_delay_ms);
    CHECK(pid > 0);

    connect_mode = o->cmode;
    int crc = oc_ipc_client_connect(&cli, path);
    connect_mode = CONNECT_REAL;
    CHECK(crc == 0 || crc == -EINPROGRESS);

    if (o->storm_us > 0)
        storm_start(o->storm_us);
    uint64_t t0 = now_ns();
    r->rc = oc_ipc_client_handshake(&cli, hello, o->timeout_ms, &r->reject);
    r->elapsed_ms = (now_ns() - t0) / 1000000ull;
    if (o->storm_us > 0) {
        storm_stop();
        r->alarms_seen = alarms;
    }
    r->conn_open_after = cli.open;
    if (r->rc == 0)
        r->peer_uid_ok = cli.peer.uid == (uint32_t)geteuid() &&
                         cli.peer.pid == (uint32_t)getpid();

    oc_ipc_conn_close(&cli);
    int st = 0;
    if (pid > 0) {
        CHECK_EQ(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
            fprintf(stderr, "  hub child status 0x%x\n", st);
    }
    oc_ipc_server_close(&srv);
    rmrf(dir);
    r->fds_after = fd_count();
}

/* ============================ tests ============================ */

static void test_init_contract(void)
{
    /* make sure descriptor 0 is genuinely open, so closing it would be visible */
    bool had0 = fd_is_open(0);
    if (!had0) {
        int nul = open("/dev/null", O_RDONLY);
        if (nul != 0 && nul >= 0) {
            dup2(nul, 0);
            close(nul);
        }
    }
    CHECK(fd_is_open(0));

    struct oc_ipc_conn c = OC_IPC_CONN_INIT;
    struct oc_ipc_server s = OC_IPC_SERVER_INIT;
    CHECK(c.fd == -1 && !c.open && s.fd == -1 && !s.open);

    /* zeroed objects (fd == 0, open == false): closing must not touch fd 0 */
    struct oc_ipc_conn zc = {0};
    struct oc_ipc_server zs = {0};
    oc_ipc_conn_close(&zc);
    oc_ipc_conn_close(&zc);
    oc_ipc_server_close(&zs);
    oc_ipc_server_close(&zs);
    CHECK(fd_is_open(0));
    CHECK(zc.fd == -1 && !zc.open && zs.fd == -1 && !zs.open);   /* re-initialized */

    /* partially initialized: fd set but never marked open -> not ours to close */
    int spare = dup(0);
    CHECK(spare >= 0);
    struct oc_ipc_conn pc = {.fd = spare, .open = false};
    oc_ipc_conn_close(&pc);
    CHECK(fd_is_open(spare));
    close(spare);

    /* use of a closed/zeroed object is an error, never I/O on fd 0 */
    struct oc_ipc_conn z2 = {0};
    struct oc_ipc_msg m;
    uint8_t byte[8] = {1, 5, 4, 0, 1, 0, 0, 0};
    CHECK_EQ(oc_ipc_recv(&z2, &m), -ENOTCONN);
    CHECK_EQ(oc_ipc_send_msg(&z2, byte, 8), -ENOTCONN);
    CHECK_EQ(oc_ipc_wait_readable(&z2, 0), -ENOTCONN);
    struct oc_sample smp = make_sample(0);
    CHECK_EQ(oc_ipc_send_sample(&z2, &smp), -ENOTCONN);
    CHECK_EQ(z2.st.tx_dropped, 1);   /* a sample that cannot be sent is dropped, once */
    struct oc_ipc_server z3 = {0};
    struct oc_ipc_conn out = OC_IPC_CONN_INIT;
    CHECK_EQ(oc_ipc_server_accept(&z3, &out), -ENOTCONN);
    CHECK(fd_is_open(0));

    /* argument errors */
    CHECK_EQ(oc_ipc_server_open(nullptr, "x"), -EINVAL);
    CHECK_EQ(oc_ipc_server_open(&s, nullptr), -EINVAL);
    CHECK_EQ(oc_ipc_client_connect(nullptr, "x"), -EINVAL);
    CHECK_EQ(oc_ipc_client_connect(&c, nullptr), -EINVAL);
    CHECK_EQ(oc_ipc_server_accept(nullptr, &out), -EINVAL);
    CHECK_EQ(oc_ipc_recv(nullptr, &m), -EINVAL);
    CHECK_EQ(oc_ipc_send_msg(nullptr, byte, 8), -EINVAL);
    oc_ipc_conn_close(nullptr);
    oc_ipc_server_close(nullptr);

    /* failed initialization leaves the object closed and leaks nothing */
    int before = fd_count();
    CHECK_EQ(oc_ipc_client_connect(&c, "/nonexistent-dir-oc-ipc/none.sock"), -ENOENT);
    CHECK(!c.open && c.fd == -1);
    CHECK_EQ(fd_count(), before);

    char dir[PATH_MAX], path[PATH_MAX + 16];
    CHECK(make_dir(dir, sizeof dir));
    join(path, sizeof path, dir, "hub.sock");

    /* partial acquisition: the socket is created, then bind fails */
    if (geteuid() != 0) {
        CHECK_EQ(chmod(dir, 0500), 0);   /* secure, but not writable by us */
        before = fd_count();
        int rc = oc_ipc_server_open(&s, path);
        CHECK(rc == -EACCES || rc == -EROFS);
        CHECK(!s.open && s.fd == -1);
        CHECK_EQ(fd_count(), before);
        CHECK_EQ(chmod(dir, 0700), 0);
    }

    /* filling an open object is refused without disturbing it */
    CHECK_EQ(oc_ipc_server_open(&s, path), 0);
    int sfd = s.fd;
    CHECK_EQ(oc_ipc_server_open(&s, path), -EBUSY);
    CHECK(s.open && s.fd == sfd);
    CHECK_EQ(oc_ipc_client_connect(&c, path), 0);
    int cfd = c.fd;
    CHECK_EQ(oc_ipc_client_connect(&c, path), -EISCONN);
    CHECK(c.open && c.fd == cfd);
    CHECK_EQ(oc_ipc_server_accept(&s, &c), -EISCONN);
    CHECK(c.open && c.fd == cfd);
    struct oc_ipc_conn acc = OC_IPC_CONN_INIT;
    CHECK_EQ(oc_ipc_server_accept(&s, &acc), 0);

    /* repeated close */
    oc_ipc_conn_close(&acc);
    oc_ipc_conn_close(&acc);
    oc_ipc_conn_close(&c);
    oc_ipc_conn_close(&c);
    oc_ipc_server_close(&s);
    oc_ipc_server_close(&s);
    CHECK(!acc.open && !c.open && !s.open);
    rmrf(dir);

    /* accept failing for lack of descriptors leaves the connection pending
     * and the out object closed; nothing leaks */
    struct pair p;
    /* Valgrind emulates RLIMIT_NOFILE itself, after the kernel has already
     * accepted: the connection is consumed before it reports EMFILE, which says
     * nothing about the library. Run this subtest natively and under ASan only. */
    const char *preload = getenv("LD_PRELOAD");
    bool under_valgrind = preload != nullptr && strstr(preload, "vgpreload") != nullptr;
    if (under_valgrind)
        fprintf(stderr, "  (EMFILE accept subtest skipped under Valgrind)\n");
    if (!under_valgrind && pair_open(&p)) {
        struct oc_ipc_conn late = OC_IPC_CONN_INIT, late_hub = OC_IPC_CONN_INIT;
        CHECK_EQ(oc_ipc_client_connect(&late, p.path), 0);
        int base = fd_count();

        struct rlimit old, lim;
        CHECK_EQ(getrlimit(RLIMIT_NOFILE, &old), 0);
        int top = 0;
        DIR *d = opendir("/proc/self/fd");
        struct dirent *e;
        while (d != nullptr && (e = readdir(d)) != nullptr)
            if (e->d_name[0] != '.' && atoi(e->d_name) > top)
                top = atoi(e->d_name);
        if (d != nullptr)
            closedir(d);
        lim = old;
        lim.rlim_cur = (rlim_t)top + 1;
        int filler[256], nfill = 0;
        if (setrlimit(RLIMIT_NOFILE, &lim) == 0) {
            int f;
            while (nfill < 256 && (f = dup(0)) >= 0)
                filler[nfill++] = f;       /* until EMFILE: no free fd below the limit */
            int rc = oc_ipc_server_accept(&p.srv, &late_hub);
            CHECK(rc == -EMFILE);
            CHECK(!late_hub.open && late_hub.fd == -1);
            for (int i = 0; i < nfill; i++)
                close(filler[i]);
            setrlimit(RLIMIT_NOFILE, &old);
            CHECK_EQ(fd_count(), base);
            /* the pending connection was not lost */
            rc = -EAGAIN;
            for (int i = 0; i < 100 && rc == -EAGAIN; i++) {
                rc = oc_ipc_server_accept(&p.srv, &late_hub);
                if (rc == -EAGAIN)
                    ms_sleep(5);
            }
            CHECK_EQ(rc, 0);
        }
        oc_ipc_conn_close(&late);
        oc_ipc_conn_close(&late_hub);
        pair_close(&p);
    }

    if (!had0)
        close(0);
}

static void test_server_lifecycle(void)
{
    char dir[PATH_MAX], path[PATH_MAX + 16];
    CHECK(make_dir(dir, sizeof dir));
    join(path, sizeof path, dir, "hub.sock");
    int before = fd_count();

    struct oc_ipc_server s;
    oc_ipc_server_init(&s);
    CHECK_EQ(oc_ipc_server_open(&s, path), 0);
    CHECK(s.open && s.fd >= 0);
    struct stat sb;
    CHECK_EQ(lstat(path, &sb), 0);
    CHECK(S_ISSOCK(sb.st_mode));
    CHECK_EQ(sb.st_mode & 0777, 0600);
    CHECK_EQ(sb.st_uid, geteuid());
    CHECK(sb.st_dev == s.dev && sb.st_ino == s.ino);
    int fl = fcntl(s.fd, F_GETFD);
    CHECK(fl != -1 && (fl & FD_CLOEXEC));
    fl = fcntl(s.fd, F_GETFL);
    CHECK(fl != -1 && (fl & O_NONBLOCK));
    int type = 0;
    socklen_t tl = sizeof type;
    CHECK_EQ(getsockopt(s.fd, SOL_SOCKET, SO_TYPE, &type, &tl), 0);
    CHECK_EQ(type, SOCK_SEQPACKET);

    /* nothing pending: accept does not block */
    struct oc_ipc_conn c = OC_IPC_CONN_INIT;
    uint64_t t0 = now_ns();
    CHECK_EQ(oc_ipc_server_accept(&s, &c), -EAGAIN);
    CHECK((now_ns() - t0) / 1000000ull < 500);
    CHECK(!c.open);

    oc_ipc_server_close(&s);
    CHECK(!s.open && s.fd == -1);
    CHECK_EQ(lstat(path, &sb), -1);
    CHECK_EQ(errno, ENOENT);
    CHECK_EQ(fd_count(), before);

    /* can be reopened on the same path */
    CHECK_EQ(oc_ipc_server_open(&s, path), 0);
    oc_ipc_server_close(&s);
    CHECK_EQ(fd_count(), before);
    rmrf(dir);
}

static void test_path_safety(void)
{
    char dir[PATH_MAX], path[PATH_MAX + 16], other[PATH_MAX + 16];
    CHECK(make_dir(dir, sizeof dir));
    join(path, sizeof path, dir, "hub.sock");
    int before = fd_count();
    struct oc_ipc_server s, s2;
    oc_ipc_server_init(&s);
    oc_ipc_server_init(&s2);
    struct stat sb;

    /* regular file at the path: refused, left alone */
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    CHECK_EQ(write(fd, "precious", 8), 8);
    close(fd);
    CHECK_EQ(oc_ipc_server_open(&s, path), -ENOTSOCK);
    CHECK(!s.open);
    char buf[16] = {0};
    fd = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0 && read(fd, buf, 15) == 8 && memcmp(buf, "precious", 8) == 0);
    close(fd);
    unlink(path);

    /* symlink at the path (even to a nonexistent target): refused, untouched */
    join(other, sizeof other, dir, "target");
    CHECK_EQ(symlink(other, path), 0);
    CHECK_EQ(oc_ipc_server_open(&s, path), -ENOTSOCK);
    CHECK_EQ(lstat(path, &sb), 0);
    CHECK(S_ISLNK(sb.st_mode));
    CHECK_EQ(lstat(other, &sb), -1);   /* no socket was created through the link */
    unlink(path);

    /* directory at the path */
    CHECK_EQ(mkdir(path, 0700), 0);
    CHECK_EQ(oc_ipc_server_open(&s, path), -ENOTSOCK);
    CHECK_EQ(lstat(path, &sb), 0);
    CHECK(S_ISDIR(sb.st_mode));
    rmdir(path);

    /* stale socket from a dead server: replaced */
    {
        CHECK(make_stale_socket(path));       /* file remains; nobody listens */
        CHECK_EQ(lstat(path, &sb), 0);
        CHECK_EQ(oc_ipc_server_open(&s, path), 0);
        struct oc_ipc_conn c = OC_IPC_CONN_INIT;
        CHECK_EQ(oc_ipc_client_connect(&c, path), 0);   /* the new server is reachable */
        oc_ipc_conn_close(&c);
        oc_ipc_server_close(&s);
    }

    /* live server on the path: second open fails, the live one is untouched */
    CHECK_EQ(oc_ipc_server_open(&s, path), 0);
    CHECK_EQ(oc_ipc_server_open(&s2, path), -EADDRINUSE);
    CHECK(!s2.open && s2.fd == -1);
    CHECK_EQ(lstat(path, &sb), 0);
    CHECK(sb.st_dev == s.dev && sb.st_ino == s.ino);
    {
        struct oc_ipc_conn c = OC_IPC_CONN_INIT, a = OC_IPC_CONN_INIT;
        CHECK_EQ(oc_ipc_client_connect(&c, path), 0);
        int rc = -EAGAIN;
        /* the failed probe left its own connection in the queue; skip it */
        for (int i = 0; i < 4 && rc != 0; i++) {
            rc = oc_ipc_server_accept(&s, &a);
            if (rc == 0 && oc_ipc_wait_readable(&a, 0) == 0) {
                struct oc_ipc_msg m;
                if (oc_ipc_recv(&a, &m) == -EPIPE) {   /* the probe: EOF, no data */
                    oc_ipc_conn_close(&a);
                    rc = -EAGAIN;
                }
            }
        }
        CHECK_EQ(rc, 0);
        oc_ipc_conn_close(&c);
        oc_ipc_conn_close(&a);
    }
    oc_ipc_server_close(&s);
    CHECK_EQ(lstat(path, &sb), -1);

    /* close must not unlink a file that replaced our socket */
    CHECK_EQ(oc_ipc_server_open(&s, path), 0);
    CHECK_EQ(unlink(path), 0);
    fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    close(fd);
    oc_ipc_server_close(&s);
    CHECK_EQ(lstat(path, &sb), 0);
    CHECK(S_ISREG(sb.st_mode));
    unlink(path);

    /* ... nor a socket that another server created at the same path */
    CHECK_EQ(oc_ipc_server_open(&s, path), 0);
    CHECK_EQ(unlink(path), 0);
    CHECK_EQ(oc_ipc_server_open(&s2, path), 0);
    oc_ipc_server_close(&s);
    CHECK_EQ(lstat(path, &sb), 0);
    CHECK(sb.st_dev == s2.dev && sb.st_ino == s2.ino);
    {
        struct oc_ipc_conn c = OC_IPC_CONN_INIT;
        CHECK_EQ(oc_ipc_client_connect(&c, path), 0);   /* s2 is still served */
        oc_ipc_conn_close(&c);
    }
    oc_ipc_server_close(&s2);
    CHECK_EQ(lstat(path, &sb), -1);

    /* unsafe or unusable parent directories */
    CHECK_EQ(chmod(dir, 0770), 0);
    CHECK_EQ(oc_ipc_server_open(&s, path), -EACCES);
    CHECK_EQ(chmod(dir, 0702), 0);
    CHECK_EQ(oc_ipc_server_open(&s, path), -EACCES);
    CHECK_EQ(chmod(dir, 0777), 0);
    CHECK_EQ(oc_ipc_server_open(&s, path), -EACCES);
    CHECK_EQ(chmod(dir, 0700), 0);
    CHECK(!s.open);
    {
        char nodir[PATH_MAX + 32];
        snprintf(nodir, sizeof nodir, "%s/missing/hub.sock", dir);
        CHECK_EQ(oc_ipc_server_open(&s, nodir), -ENOENT);
        join(other, sizeof other, dir, "afile");
        fd = open(other, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
        close(fd);
        snprintf(nodir, sizeof nodir, "%s/afile/hub.sock", dir);
        CHECK_EQ(oc_ipc_server_open(&s, nodir), -ENOTDIR);
    }

    /* malformed paths */
    CHECK_EQ(oc_ipc_server_open(&s, ""), -EINVAL);
    {
        char slash[PATH_MAX + 4];
        snprintf(slash, sizeof slash, "%s/", dir);
        CHECK_EQ(oc_ipc_server_open(&s, slash), -EINVAL);
        char longp[200];
        memset(longp, 'a', sizeof longp - 1);
        longp[0] = '/';
        longp[sizeof longp - 1] = '\0';
        CHECK_EQ(oc_ipc_server_open(&s, longp), -ENAMETOOLONG);
        CHECK_EQ(oc_ipc_client_connect(&(struct oc_ipc_conn){.fd = -1}, longp), -ENAMETOOLONG);
    }
    CHECK(!s.open);

    /* connect failures acquire nothing */
    struct oc_ipc_conn c = OC_IPC_CONN_INIT;
    CHECK_EQ(oc_ipc_client_connect(&c, path), -ENOENT);
    CHECK(!c.open && c.fd == -1);
    {
        CHECK(make_stale_socket(path));
        CHECK_EQ(oc_ipc_client_connect(&c, path), -ECONNREFUSED);   /* stale file */
        CHECK(!c.open && c.fd == -1);
        unlink(path);
    }

    CHECK_EQ(fd_count(), before);
    rmrf(dir);
}

static void test_connect_states(void)
{
    char dir[PATH_MAX], path[PATH_MAX + 16];
    struct oc_ipc_server srv;
    oc_ipc_server_init(&srv);
    CHECK(make_dir(dir, sizeof dir));
    join(path, sizeof path, dir, "hub.sock");
    CHECK_EQ(oc_ipc_server_open(&srv, path), 0);
    int before = fd_count();

    /* connect that the kernel reports as in progress, then completes */
    struct oc_ipc_conn c = OC_IPC_CONN_INIT;
    connect_mode = CONNECT_REAL_THEN_INPROGRESS;
    CHECK_EQ(oc_ipc_client_connect(&c, path), -EINPROGRESS);
    connect_mode = CONNECT_REAL;
    CHECK(c.open && c.connecting);
    struct oc_ipc_msg m;
    CHECK_EQ(oc_ipc_recv(&c, &m), -ENOTCONN);       /* not usable until finished */
    uint8_t bye[8] = {1, 5, 4, 0, 1, 0, 0, 0};
    CHECK_EQ(oc_ipc_send_msg(&c, bye, 8), -ENOTCONN);
    CHECK_EQ(oc_ipc_client_connect_finish(&c), 0);
    CHECK(c.open && !c.connecting);
    CHECK(c.peer.pid == (uint32_t)getpid() && c.peer.uid == (uint32_t)geteuid());
    CHECK_EQ(oc_ipc_client_connect_finish(&c), 0);   /* idempotent */
    oc_ipc_conn_close(&c);

    /* EINTR from connect is the same in-progress state */
    connect_mode = CONNECT_REAL_THEN_EINTR;
    CHECK_EQ(oc_ipc_client_connect(&c, path), -EINPROGRESS);
    connect_mode = CONNECT_REAL;
    CHECK(c.open && c.connecting);
    CHECK_EQ(oc_ipc_client_connect_finish(&c), 0);
    oc_ipc_conn_close(&c);

    /* "in progress" but never actually connecting: finish must fail and clean up */
    connect_mode = CONNECT_FAKE_INPROGRESS;
    CHECK_EQ(oc_ipc_client_connect(&c, path), -EINPROGRESS);
    connect_mode = CONNECT_REAL;
    CHECK(c.open && c.connecting);
    int rc = oc_ipc_client_connect_finish(&c);
    CHECK(rc < 0 && rc != -EINPROGRESS);
    CHECK(!c.open && c.fd == -1);

    /* finish on a never-connected object */
    struct oc_ipc_conn idle = OC_IPC_CONN_INIT;
    CHECK_EQ(oc_ipc_client_connect_finish(&idle), -ENOTCONN);

    /* drain whatever the pending connections left in the server */
    struct oc_ipc_conn a = OC_IPC_CONN_INIT;
    while (oc_ipc_server_accept(&srv, &a) == 0)
        oc_ipc_conn_close(&a);
    CHECK_EQ(fd_count(), before);

    /* full backlog: -EAGAIN, nothing acquired; earlier connections intact */
    struct oc_ipc_conn many[16];
    int ok = 0, eagain = 0;
    for (int i = 0; i < 16; i++) {
        oc_ipc_conn_init(&many[i]);
        int r = oc_ipc_client_connect(&many[i], path);
        if (r == 0) {
            ok++;
        } else {
            CHECK_EQ(r, -EAGAIN);
            CHECK(!many[i].open && many[i].fd == -1);
            eagain++;
        }
    }
    fprintf(stderr, "  backlog: %d connected, %d refused with EAGAIN (listen backlog %d)\n",
            ok, eagain, OC_IPC_BACKLOG);
    CHECK(ok >= 1 && eagain >= 1);
    CHECK_EQ(fd_count(), before + ok);
    for (int i = 0; i < 16; i++)
        oc_ipc_conn_close(&many[i]);
    while (oc_ipc_server_accept(&srv, &a) == 0)
        oc_ipc_conn_close(&a);
    CHECK_EQ(fd_count(), before);

    oc_ipc_server_close(&srv);
    rmrf(dir);
}

static void test_handshake_ack(void)
{
    struct hs_opts o = {.mode = H_POLICY, .timeout_ms = 3000};
    struct hs_result r;

    /* LIGHT/LIVE and DISTANCE/SIMULATED are both acceptable: a mixed session */
    struct oc_hello h = make_hello(OC_SOURCE_LIGHT, OC_ORIGIN_LIVE, 0);   /* instance 0 */
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, 0);
    CHECK(r.conn_open_after && r.peer_uid_ok);
    CHECK_EQ(r.reject.reason, REJECT_SENTINEL);       /* reject untouched on success */
    CHECK_EQ(r.fds_after, r.fds_before);

    h = make_hello(OC_SOURCE_DISTANCE, OC_ORIGIN_SIMULATED, 7);
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, 0);
    CHECK(r.conn_open_after);

    /* a reply that arrives later, within the deadline */
    o.hub_delay_ms = 80;
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, 0);
    CHECK(r.elapsed_ms >= 70);
}

static void test_handshake_reject(void)
{
    struct hs_opts o = {.mode = H_POLICY, .timeout_ms = 3000};
    struct hs_result r;
    struct oc_hello h;

    h = make_hello(OC_SOURCE_LIGHT, OC_ORIGIN_LIVE, 1);
    h.session_id = TEST_SESSION + 1;
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, -EACCES);
    CHECK_EQ(r.reject.reason, OC_REJECT_SESSION_MISMATCH);
    CHECK_EQ(r.reject.detail, 0);
    CHECK(!r.conn_open_after);
    CHECK_EQ(r.fds_after, r.fds_before);

    h = make_hello(OC_SOURCE_LIGHT, OC_ORIGIN_SIMULATED, 1);   /* LIGHT must be LIVE */
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, -EACCES);
    CHECK_EQ(r.reject.reason, OC_REJECT_BAD_ORIGIN);
    CHECK(!r.conn_open_after);

    h = make_hello(OC_SOURCE_DISTANCE, OC_ORIGIN_LIVE, 1);     /* DISTANCE must be SIMULATED */
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, -EACCES);
    CHECK_EQ(r.reject.reason, OC_REJECT_BAD_ORIGIN);

    h = make_hello(OC_SOURCE_LIGHT, OC_ORIGIN_LIVE, 1);
    h.pid += 1;                                                  /* forged pid */
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, -EACCES);
    CHECK_EQ(r.reject.reason, OC_REJECT_PEER_CRED);
    CHECK_EQ(r.fds_after, r.fds_before);

    /* invalid hello: no I/O at all, the connection is left alone */
    struct pair p;
    if (pair_open(&p)) {
        struct oc_hello bad = make_hello(OC_SOURCE_LIGHT, OC_ORIGIN_MIXED, 1);
        struct oc_hello_reject rj = {.reason = REJECT_SENTINEL};
        CHECK_EQ(oc_ipc_client_handshake(&p.cli, &bad, 100, &rj), -EINVAL);
        CHECK(p.cli.open);
        CHECK_EQ(oc_ipc_client_handshake(&p.cli, nullptr, 100, &rj), -EINVAL);
        struct oc_hello ok = make_hello(OC_SOURCE_LIGHT, OC_ORIGIN_LIVE, 1);
        CHECK_EQ(oc_ipc_client_handshake(&p.cli, &ok, 0, &rj), -EINVAL);
        CHECK_EQ(oc_ipc_client_handshake(&p.cli, &ok, -5, &rj), -EINVAL);
        CHECK(p.cli.open);
        CHECK_EQ(oc_ipc_wait_readable(&p.hub, 0), -ETIMEDOUT);   /* nothing was sent */
        struct oc_ipc_conn closed = OC_IPC_CONN_INIT;
        CHECK_EQ(oc_ipc_client_handshake(&closed, &ok, 100, &rj), -ENOTCONN);
        CHECK_EQ(rj.reason, REJECT_SENTINEL);
        pair_close(&p);
    }
}

static void test_handshake_bad_replies(void)
{
    static const struct { enum hub_mode mode; int want; const char *name; } cases[] = {
        {H_ACK_BAD_SESSION, -EPROTO, "ACK echoing a different session_id"},
        {H_ACK_BAD_INSTANCE, -EPROTO, "ACK echoing a different instance"},
        {H_ACK_BAD_SOURCE, -EPROTO, "ACK echoing a different source"},
        {H_BYE, -ECONNABORTED, "BYE instead of ACK"},
        {H_SAMPLE, -EPROTO, "SAMPLE instead of ACK"},
        {H_BADVER, -EPROTONOSUPPORT, "version-2 datagram"},
        {H_SHORT, -EMSGSIZE, "3-byte datagram"},
        {H_UNKNOWN_TYPE, -EBADMSG, "unknown message type"},
        {H_CLOSE, -EPIPE, "hub closes without replying"},
    };
    struct oc_hello h = make_hello(OC_SOURCE_LIGHT, OC_ORIGIN_LIVE, 3);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        struct hs_opts o = {.mode = cases[i].mode, .timeout_ms = 3000};
        struct hs_result r;
        run_handshake(&o, &h, &r);
        if (r.rc != cases[i].want)
            fprintf(stderr, "  case '%s': rc %d, want %d\n", cases[i].name, r.rc, cases[i].want);
        CHECK_EQ(r.rc, cases[i].want);
        CHECK(!r.conn_open_after);                 /* a failed handshake closes */
        CHECK_EQ(r.reject.reason, REJECT_SENTINEL);   /* success/reject only */
        CHECK_EQ(r.fds_after, r.fds_before);
    }
}

static void test_handshake_deadline(void)
{
    struct oc_hello h = make_hello(OC_SOURCE_LIGHT, OC_ORIGIN_LIVE, 4);
    struct hs_result r;

    /* a silent hub: ETIMEDOUT close to the deadline, never before it */
    struct hs_opts o = {.mode = H_SILENT, .timeout_ms = 150};
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, -ETIMEDOUT);
    CHECK(r.elapsed_ms >= 145 && r.elapsed_ms < 600);
    CHECK(!r.conn_open_after);
    CHECK_EQ(r.fds_after, r.fds_before);
    fprintf(stderr, "  silent hub, 150 ms deadline: returned after %llu ms\n",
            (unsigned long long)r.elapsed_ms);

    /* EINTR storm (SIGALRM every 3 ms): the one deadline is neither restarted
     * (which would never time out) nor shortened */
    o.storm_us = 3000;
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, -ETIMEDOUT);
    CHECK(r.elapsed_ms >= 145 && r.elapsed_ms < 600);
    CHECK(r.alarms_seen >= 5);                    /* the storm really interrupted us */
    fprintf(stderr, "  under storm (%d signals): returned after %llu ms\n", r.alarms_seen,
            (unsigned long long)r.elapsed_ms);

    /* a reply that comes after some EINTRs but inside the deadline succeeds */
    o = (struct hs_opts){.mode = H_POLICY, .hub_delay_ms = 100, .timeout_ms = 2000,
                         .storm_us = 3000};
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, 0);
    CHECK(r.alarms_seen >= 5);

    /* ... and the deadline also bounds a reply that is too late */
    o = (struct hs_opts){.mode = H_POLICY, .hub_delay_ms = 600, .timeout_ms = 150,
                         .storm_us = 3000};
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, -ETIMEDOUT);
    CHECK(r.elapsed_ms >= 145 && r.elapsed_ms < 500);

    /* the same single deadline covers completing an in-progress connect */
    o = (struct hs_opts){.mode = H_POLICY, .timeout_ms = 2000,
                         .cmode = CONNECT_REAL_THEN_INPROGRESS};
    run_handshake(&o, &h, &r);
    CHECK_EQ(r.rc, 0);
    CHECK(r.conn_open_after);

    /* in-progress connect that never completes: fails, closed, no leak */
    o = (struct hs_opts){.mode = H_SILENT, .timeout_ms = 300,
                         .cmode = CONNECT_FAKE_INPROGRESS};
    run_handshake(&o, &h, &r);
    CHECK(r.rc < 0 && r.rc != -EPROTO);
    CHECK(!r.conn_open_after);
    CHECK_EQ(r.fds_after, r.fds_before);
}

static void test_wait_readable(void)
{
    struct pair p;
    if (!pair_open(&p))
        return;
    struct oc_ipc_msg m;
    CHECK_EQ(oc_ipc_wait_readable(&p.hub, 0), -ETIMEDOUT);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), -EAGAIN);

    /* under a signal storm the wait still ends at ONE deadline */
    storm_start(3000);
    uint64_t t0 = now_ns();
    int rc = oc_ipc_wait_readable(&p.hub, 120);
    uint64_t ms = (now_ns() - t0) / 1000000ull;
    storm_stop();
    CHECK_EQ(rc, -ETIMEDOUT);
    CHECK(ms >= 115 && ms < 500);
    CHECK(alarms >= 5);

    /* data pending */
    uint8_t bye[8] = {1, 5, 4, 0, 1, 0, 0, 0};
    CHECK_EQ(oc_ipc_send_msg(&p.cli, bye, 8), 0);
    CHECK_EQ(oc_ipc_wait_readable(&p.hub, 1000), 0);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), 0);
    CHECK_EQ(m.type, OC_MSG_BYE);

    /* EOF counts as readable, and recv then reports it */
    oc_ipc_conn_close(&p.cli);
    CHECK_EQ(oc_ipc_wait_readable(&p.hub, 1000), 0);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), -EPIPE);
    CHECK_EQ(oc_ipc_wait_readable(&p.cli, 0), -ENOTCONN);
    pair_close(&p);
}

static void test_message_boundaries(void)
{
    struct pair p;
    if (!pair_open(&p))
        return;
    struct oc_hello h = make_hello(OC_SOURCE_LIGHT, OC_ORIGIN_SIMULATED, 9);
    struct oc_sample s1 = make_sample(1), s2 = make_sample(2);
    struct oc_bye y = {.reason = OC_BYE_NORMAL};
    uint8_t b[64];
    size_t n;

    CHECK_EQ(oc_ipc_encode_hello(&h, b, sizeof b, &n), 0);
    CHECK_EQ(oc_ipc_send_msg(&p.cli, b, n), 0);
    CHECK_EQ(oc_ipc_send_sample(&p.cli, &s1), 0);
    CHECK_EQ(oc_ipc_encode_bye(&y, b, sizeof b, &n), 0);
    CHECK_EQ(oc_ipc_send_msg(&p.cli, b, n), 0);
    CHECK_EQ(oc_ipc_send_sample(&p.cli, &s2), 0);
    CHECK_EQ(p.cli.st.tx_msgs, 4);
    CHECK_EQ(p.cli.st.tx_dropped, 0);

    struct oc_ipc_msg m;
    CHECK_EQ(recv_wait(&p.hub, &m), 0);
    CHECK(m.type == OC_MSG_HELLO && m.u.hello.instance == 9 &&
          m.u.hello.session_id == TEST_SESSION);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), 0);
    CHECK(m.type == OC_MSG_SAMPLE && memcmp(&m.u.sample, &s1, sizeof s1) == 0);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), 0);
    CHECK(m.type == OC_MSG_BYE && m.u.bye.reason == OC_BYE_NORMAL);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), 0);
    CHECK(m.type == OC_MSG_SAMPLE && memcmp(&m.u.sample, &s2, sizeof s2) == 0);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), -EAGAIN);
    CHECK_EQ(p.hub.st.rx_msgs, 4);
    CHECK(p.hub.st.rx_malformed == 0 && p.hub.st.rx_oversized == 0);

    /* every library send is nonblocking and signal-free */
    last_send_flags = -1;
    CHECK_EQ(oc_ipc_send_sample(&p.cli, &s1), 0);
    CHECK(last_send_flags != -1 && (last_send_flags & MSG_NOSIGNAL) &&
          (last_send_flags & MSG_DONTWAIT));
    last_send_flags = -1;
    CHECK_EQ(oc_ipc_send_msg(&p.cli, b, n), 0);
    CHECK(last_send_flags != -1 && (last_send_flags & MSG_NOSIGNAL) &&
          (last_send_flags & MSG_DONTWAIT));
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), 0);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), 0);

    /* a zero-length datagram is not EOF: consumed, counted, connection usable */
    {
        uint64_t mal = p.hub.st.rx_malformed;
        CHECK_EQ(raw_send(&p.cli, "", 0), 0);
        CHECK_EQ(oc_ipc_wait_readable(&p.hub, 1000), 0);
        CHECK_EQ(oc_ipc_recv(&p.hub, &m), -EBADMSG);
        CHECK_EQ(p.hub.st.rx_malformed, mal + 1);
        CHECK(p.hub.open);
        CHECK_EQ(oc_ipc_recv(&p.hub, &m), -EAGAIN);
    }

    /* both directions */
    struct oc_hello_ack a = {.session_id = TEST_SESSION, .instance = 9, .source = OC_SOURCE_LIGHT};
    CHECK_EQ(oc_ipc_encode_hello_ack(&a, b, sizeof b, &n), 0);
    CHECK_EQ(oc_ipc_send_msg(&p.hub, b, n), 0);
    CHECK_EQ(recv_wait(&p.cli, &m), 0);
    CHECK(m.type == OC_MSG_HELLO_ACK && m.u.ack.instance == 9);

    /* send argument checks */
    CHECK_EQ(oc_ipc_send_msg(&p.cli, nullptr, 8), -EINVAL);
    CHECK_EQ(oc_ipc_send_msg(&p.cli, b, 0), -EINVAL);   /* never a zero-length datagram */
    CHECK_EQ(oc_ipc_send_msg(&p.cli, b, 3), -EINVAL);
    CHECK_EQ(oc_ipc_send_msg(&p.cli, b, OC_IPC_MAX_MSG + 1), -EINVAL);
    struct oc_sample bad = make_sample(3);
    bad.status = 99;
    uint64_t dropped = p.cli.st.tx_dropped;
    CHECK_EQ(oc_ipc_send_sample(&p.cli, &bad), -EINVAL);
    CHECK_EQ(p.cli.st.tx_dropped, dropped);              /* a bug is not a drop */
    CHECK_EQ(oc_ipc_send_sample(&p.cli, nullptr), -EINVAL);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), -EAGAIN);          /* nothing leaked through */
    pair_close(&p);
}

static void expect_recv(struct oc_ipc_conn *c, int want, const char *what, int line)
{
    struct oc_ipc_msg m, ref;
    memset(&m, 0xA5, sizeof m);
    memset(&ref, 0xA5, sizeof ref);
    int rc = recv_wait(c, &m);
    if (rc != want) {
        fprintf(stderr, "FAIL %s:%d: %s: recv returned %d, want %d\n", __FILE__, line, what,
                rc, want);
        failures++;
    }
    if (rc != 0 && memcmp(&m, &ref, sizeof m) != 0) {
        fprintf(stderr, "FAIL %s:%d: %s: output modified on error\n", __FILE__, line, what);
        failures++;
    }
}
#define EXPECT_RECV(c, want, what) expect_recv((c), (want), (what), __LINE__)

/* A valid message right after a bad one proves the stream stayed in sync. */
static void expect_still_synced(struct pair *p, uint32_t seq, int line)
{
    struct oc_sample s = make_sample(seq);
    struct oc_ipc_msg m;
    int rc = oc_ipc_send_sample(&p->cli, &s);
    int r2 = rc == 0 ? recv_wait(&p->hub, &m) : rc;
    if (rc != 0 || r2 != 0 || m.type != OC_MSG_SAMPLE || m.u.sample.seq != seq) {
        fprintf(stderr, "FAIL %s:%d: stream not in sync after bad datagram (%d, %d)\n",
                __FILE__, line, rc, r2);
        failures++;
    }
}
#define SYNCED(p, seq) expect_still_synced((p), (seq), __LINE__)

static void test_hostile_datagrams(void)
{
    struct pair p;
    if (!pair_open(&p))
        return;
    uint8_t big[4096];
    memset(big, 0x5A, sizeof big);
    big[0] = OC_PROTO_VERSION;
    big[1] = OC_MSG_SAMPLE;

    /* oversized: consumed whole (nothing left over to misparse), counted */
    static const size_t sizes[] = {29, 32, 63, 64, 65, 200, 4096};
    uint64_t oversized = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        CHECK_EQ(raw_send(&p.cli, big, sizes[i]), 0);
        EXPECT_RECV(&p.hub, -EMSGSIZE, "oversized datagram");
        CHECK_EQ(p.hub.st.rx_oversized, ++oversized);
        CHECK_EQ(oc_ipc_recv(&p.hub, &(struct oc_ipc_msg){0}), -EAGAIN);   /* no remainder */
        SYNCED(&p, (uint32_t)(100 + i));
    }
    CHECK_EQ(p.hub.st.rx_malformed, 0);

    /* truncated (shorter than the defined length) */
    uint8_t good_hello[24] = {1, 1, 20, 0, 1, 2, 0, 0, 0x88, 0x77, 0x66, 0x55,
                              0x44, 0x33, 0x22, 0x11, 0, 0, 0, 0, 1, 0, 0, 0};
    uint64_t malformed = 0;
    for (size_t len = 1; len < 24; len += 7) {
        CHECK_EQ(raw_send(&p.cli, good_hello, len), 0);
        EXPECT_RECV(&p.hub, -EMSGSIZE, "truncated HELLO");
        CHECK_EQ(p.hub.st.rx_malformed, ++malformed);
        SYNCED(&p, (uint32_t)(200 + len));
    }
    /* trailing byte on a valid message */
    {
        uint8_t t[25];
        memcpy(t, good_hello, 24);
        t[24] = 0;
        CHECK_EQ(raw_send(&p.cli, t, 25), 0);
        EXPECT_RECV(&p.hub, -EMSGSIZE, "HELLO with a trailing byte");
        CHECK_EQ(p.hub.st.rx_malformed, ++malformed);
        SYNCED(&p, 250);
    }
    /* wrong exact length for the type, unknown type, nonzero reserved */
    {
        uint8_t t[24];
        memcpy(t, good_hello, 24);
        t[2] = 19;
        CHECK_EQ(raw_send(&p.cli, t, 23), 0);
        EXPECT_RECV(&p.hub, -EBADMSG, "HELLO payload_len 19");
        CHECK_EQ(p.hub.st.rx_malformed, ++malformed);
        SYNCED(&p, 251);

        memcpy(t, good_hello, 24);
        t[1] = 99;
        CHECK_EQ(raw_send(&p.cli, t, 24), 0);
        EXPECT_RECV(&p.hub, -EBADMSG, "unknown type 99");
        CHECK_EQ(p.hub.st.rx_malformed, ++malformed);
        SYNCED(&p, 252);

        memcpy(t, good_hello, 24);
        t[6] = 1;
        CHECK_EQ(raw_send(&p.cli, t, 24), 0);
        EXPECT_RECV(&p.hub, -EBADMSG, "HELLO with reserved byte set");
        CHECK_EQ(p.hub.st.rx_malformed, ++malformed);
        SYNCED(&p, 253);

        memcpy(t, good_hello, 24);
        t[4] = 3;                                  /* unknown source */
        CHECK_EQ(raw_send(&p.cli, t, 24), 0);
        EXPECT_RECV(&p.hub, -EBADMSG, "HELLO with unknown source");
        CHECK_EQ(p.hub.st.rx_malformed, ++malformed);
        SYNCED(&p, 254);
    }
    /* a SAMPLE of the wrong length, and a corrupt one */
    {
        struct oc_sample s = make_sample(5);
        uint8_t t[32];
        size_t n;
        CHECK_EQ(oc_msg_sample_encode(&s, t, sizeof t, &n), 0);
        CHECK_EQ(raw_send(&p.cli, t, 27), 0);
        EXPECT_RECV(&p.hub, -EMSGSIZE, "27-byte SAMPLE");
        CHECK_EQ(p.hub.st.rx_malformed, ++malformed);
        t[4 + 14] = 99;                            /* status out of range */
        CHECK_EQ(raw_send(&p.cli, t, 28), 0);
        EXPECT_RECV(&p.hub, -EBADMSG, "SAMPLE with illegal status");
        CHECK_EQ(p.hub.st.rx_malformed, ++malformed);
        SYNCED(&p, 255);
    }
    CHECK_EQ(p.hub.st.rx_oversized, oversized);
    CHECK(p.hub.open);                             /* ordinary garbage never closes */

    /* an unsupported version is connection-fatal, not "malformed": closed, no reply */
    uint8_t v2[20];
    memcpy(v2, "\x02\x02\x10\x00", 4);
    memset(v2 + 4, 0, 16);
    int hub_fd = p.hub.fd;
    CHECK_EQ(raw_send(&p.cli, v2, 20), 0);
    EXPECT_RECV(&p.hub, -EPROTONOSUPPORT, "version 2");
    CHECK(!p.hub.open && p.hub.fd == -1);
    CHECK(!fd_is_open(hub_fd));
    struct oc_ipc_msg m;
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), -ENOTCONN);
    uint8_t bye[8] = {1, 5, 4, 0, 1, 0, 0, 0};
    CHECK_EQ(oc_ipc_send_msg(&p.hub, bye, 8), -ENOTCONN);
    /* the client saw no reply at all, only EOF */
    CHECK_EQ(oc_ipc_wait_readable(&p.cli, 1000), 0);
    CHECK_EQ(oc_ipc_recv(&p.cli, &m), -EPIPE);
    pair_close(&p);

    /* the same for an oversized datagram of an unsupported version, and for a
     * one-byte datagram */
    static const size_t fatal_sizes[] = {1, 3, 29, 200};
    for (size_t i = 0; i < sizeof fatal_sizes / sizeof fatal_sizes[0]; i++) {
        if (!pair_open(&p))
            return;
        memset(big, 0x5A, sizeof big);
        big[0] = 7;
        CHECK_EQ(raw_send(&p.cli, big, fatal_sizes[i]), 0);
        EXPECT_RECV(&p.hub, -EPROTONOSUPPORT, "unsupported version, odd size");
        CHECK(!p.hub.open);
        pair_close(&p);
    }
}

static void test_eintr_on_send_recv(void)
{
    struct pair p;
    if (!pair_open(&p))
        return;
    struct oc_ipc_msg m;
    uint8_t bye[8] = {1, 5, 4, 0, 1, 0, 0, 0};

    send_mode = SEND_EINTR_ONCE;
    CHECK_EQ(oc_ipc_send_msg(&p.cli, bye, 8), 0);     /* retried internally */
    CHECK_EQ(send_mode, SEND_REAL);
    CHECK_EQ(p.cli.st.tx_msgs, 1);
    CHECK_EQ(recv_wait(&p.hub, &m), 0);
    CHECK_EQ(m.type, OC_MSG_BYE);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), -EAGAIN);       /* delivered exactly once */

    CHECK_EQ(oc_ipc_send_msg(&p.cli, bye, 8), 0);
    CHECK_EQ(oc_ipc_wait_readable(&p.hub, 1000), 0);
    recv_mode = RECV_EINTR_ONCE;
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), 0);
    CHECK_EQ(recv_mode, RECV_REAL);
    CHECK_EQ(m.type, OC_MSG_BYE);

    /* a kernel that reports a partial datagram is an error, not a success */
    send_mode = SEND_SHORT;
    CHECK_EQ(oc_ipc_send_msg(&p.cli, bye, 8), -EIO);
    uint64_t tx = p.cli.st.tx_msgs;
    struct oc_sample s = make_sample(1);
    CHECK_EQ(oc_ipc_send_sample(&p.cli, &s), -EIO);
    send_mode = SEND_REAL;
    CHECK_EQ(p.cli.st.tx_msgs, tx);                   /* neither counted as sent ... */
    CHECK_EQ(p.cli.st.tx_dropped, 1);                 /* ... the sample dropped once */
    pair_close(&p);
}

static volatile sig_atomic_t sigpipes;
static void on_sigpipe(int sig)
{
    (void)sig;
    sigpipes++;
}

static void test_peer_disconnect_and_sigpipe(void)
{
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigpipe;
    sigemptyset(&sa.sa_mask);
    CHECK_EQ(sigaction(SIGPIPE, &sa, &old), 0);

    struct pair p;
    if (!pair_open(&p)) {
        sigaction(SIGPIPE, &old, nullptr);
        return;
    }
    uint8_t bye[8] = {1, 5, 4, 0, 1, 0, 0, 0};
    struct oc_ipc_msg m;

    /* client hangs up; the hub sees EOF, then fails to send, without a signal */
    oc_ipc_conn_close(&p.cli);
    CHECK_EQ(recv_wait(&p.hub, &m), -EPIPE);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), -EPIPE);        /* EOF is sticky */
    sigpipes = 0;
    CHECK_EQ(oc_ipc_send_msg(&p.hub, bye, 8), -EPIPE);
    struct oc_sample s = make_sample(1);
    CHECK_EQ(oc_ipc_send_sample(&p.hub, &s), -EPIPE);
    CHECK_EQ(p.hub.st.tx_dropped, 1);                 /* exactly once */
    CHECK_EQ(oc_ipc_send_sample(&p.hub, &s), -EPIPE);
    CHECK_EQ(p.hub.st.tx_dropped, 2);
    CHECK_EQ(p.hub.st.tx_msgs, 0);
    CHECK_EQ(sigpipes, 0);

    /* Control: prove the handler works with a SOCK_STREAM pair, where Linux does
     * raise SIGPIPE for a send() without MSG_NOSIGNAL. (On this kernel a
     * SEQPACKET send without the flag raised none; the library sets MSG_NOSIGNAL
     * regardless, because POSIX allows SIGPIPE on connection-mode sockets.) */
    {
        int sv[2];
        CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv), 0);
        close(sv[1]);
        ssize_t n = send(sv[0], "x", 1, MSG_DONTWAIT);
        CHECK(n == -1 && errno == EPIPE);
        CHECK_EQ(sigpipes, 1);
        close(sv[0]);
        sigpipes = 0;
    }
    {
        ssize_t n = send(p.hub.fd, bye, 8, MSG_DONTWAIT);   /* observation only */
        fprintf(stderr, "  SEQPACKET send without MSG_NOSIGNAL to a closed peer: errno %d, "
                "%d SIGPIPE(s)\n", n == -1 ? errno : 0, (int)sigpipes);
        sigpipes = 0;
    }

    /* closed objects fail cleanly */
    oc_ipc_conn_close(&p.hub);
    CHECK_EQ(oc_ipc_send_msg(&p.hub, bye, 8), -ENOTCONN);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), -ENOTCONN);
    pair_close(&p);

    /* the other direction: hub hangs up, client reader sees it */
    if (pair_open(&p)) {
        oc_ipc_conn_close(&p.hub);
        CHECK_EQ(recv_wait(&p.cli, &m), -EPIPE);
        CHECK_EQ(oc_ipc_send_sample(&p.cli, &s), -EPIPE);
        CHECK_EQ(p.cli.st.tx_dropped, 1);
        CHECK_EQ(sigpipes, 0);
        pair_close(&p);
    }

    /* closing with unread data is reported as a reset or EOF, never a hang */
    if (pair_open(&p)) {
        CHECK_EQ(oc_ipc_send_msg(&p.cli, bye, 8), 0);   /* unread by the hub */
        oc_ipc_conn_close(&p.hub);
        int rc = recv_wait(&p.cli, &m);
        CHECK(rc == -EPIPE || rc == -ECONNRESET);
        rc = oc_ipc_send_msg(&p.cli, bye, 8);
        CHECK(rc == -EPIPE || rc == -ECONNRESET);
        CHECK_EQ(sigpipes, 0);
        pair_close(&p);
    }
    CHECK_EQ(sigaction(SIGPIPE, &old, nullptr), 0);
}

static void test_send_queue_bounded(void)
{
    struct pair p;
    if (!pair_open(&p))
        return;
    uint32_t sndbuf = p.cli.sndbuf;
    CHECK(sndbuf >= OC_IPC_SNDBUF_REQUEST);
    int actual = 0;
    socklen_t al = sizeof actual;
    CHECK_EQ(getsockopt(p.cli.fd, SOL_SOCKET, SO_SNDBUF, &actual, &al), 0);
    CHECK_EQ(actual, sndbuf);

    /* The kernel refuses a send once the sender's buffer is full, and every
     * queued datagram costs at least its length, so at most this many are
     * ever accepted. This bound comes from SO_SNDBUF, not net.unix.max_dgram_qlen. */
    const uint32_t hard_bound = sndbuf / OC_MSG_SAMPLE_SIZE + 1;
    uint32_t accepted = 0;
    int rc = 0;
    uint64_t t0 = now_ns();
    while (accepted < hard_bound + 100) {
        struct oc_sample s = make_sample(accepted);
        rc = oc_ipc_send_sample(&p.cli, &s);
        if (rc != 0)
            break;
        accepted++;
    }
    uint64_t ms = (now_ns() - t0) / 1000000ull;
    fprintf(stderr, "  SO_SNDBUF %u (requested %u): %u samples accepted before EAGAIN "
            "(hard bound %u), %llu ms\n", sndbuf, OC_IPC_SNDBUF_REQUEST, accepted,
            hard_bound, (unsigned long long)ms);
    CHECK_EQ(rc, -EAGAIN);
    CHECK(accepted >= 1 && accepted <= hard_bound);
    CHECK(ms < 1000);                                  /* never blocked */
    CHECK_EQ(p.cli.st.tx_dropped, 1);                  /* the one that failed */
    CHECK_EQ(p.cli.st.tx_msgs, accepted);

    /* more attempts: each is dropped once and counted once */
    for (uint32_t i = 0; i < 10; i++) {
        struct oc_sample s = make_sample(1000 + i);
        CHECK_EQ(oc_ipc_send_sample(&p.cli, &s), -EAGAIN);
    }
    CHECK_EQ(p.cli.st.tx_dropped, 11);
    CHECK_EQ(p.cli.st.tx_msgs, accepted);

    /* control messages are NOT silently dropped: the caller is told */
    uint8_t bye[8] = {1, 5, 4, 0, 1, 0, 0, 0};
    CHECK_EQ(oc_ipc_send_msg(&p.cli, bye, 8), -EAGAIN);
    CHECK_EQ(p.cli.st.tx_dropped, 11);                 /* not a sample drop */
    CHECK_EQ(p.cli.st.tx_msgs, accepted);

    /* the hub drains exactly what was accepted, in order; dropped seqs never appear */
    struct oc_ipc_msg m;
    for (uint32_t i = 0; i < accepted; i++) {
        CHECK_EQ(oc_ipc_recv(&p.hub, &m), 0);
        CHECK(m.type == OC_MSG_SAMPLE && m.u.sample.seq == i);
    }
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), -EAGAIN);
    CHECK_EQ(p.hub.st.rx_msgs, accepted);

    /* and sending works again once there is room */
    struct oc_sample s = make_sample(5000);
    CHECK_EQ(oc_ipc_send_sample(&p.cli, &s), 0);
    CHECK_EQ(oc_ipc_send_msg(&p.cli, bye, 8), 0);
    CHECK_EQ(recv_wait(&p.hub, &m), 0);
    CHECK(m.type == OC_MSG_SAMPLE && m.u.sample.seq == 5000);
    CHECK_EQ(oc_ipc_recv(&p.hub, &m), 0);
    CHECK_EQ(m.type, OC_MSG_BYE);
    pair_close(&p);
}

static void test_fd_hygiene(void)
{
    struct pair p;
    if (!pair_open(&p))
        return;
    int fds[3] = {p.srv.fd, p.cli.fd, p.hub.fd};
    static const char *names[3] = {"server", "client", "accepted"};
    for (int i = 0; i < 3; i++) {
        int fd_flags = fcntl(fds[i], F_GETFD);
        int fl = fcntl(fds[i], F_GETFL);
        if (fd_flags == -1 || !(fd_flags & FD_CLOEXEC) || fl == -1 || !(fl & O_NONBLOCK)) {
            fprintf(stderr, "FAIL %s:%d: %s descriptor lacks CLOEXEC/NONBLOCK\n", __FILE__,
                    __LINE__, names[i]);
            failures++;
        }
        int type = 0;
        socklen_t tl = sizeof type;
        CHECK_EQ(getsockopt(fds[i], SOL_SOCKET, SO_TYPE, &type, &tl), 0);
        CHECK_EQ(type, SOCK_SEQPACKET);
    }
    CHECK(p.hub.peer.pid == (uint32_t)getpid() && p.hub.peer.uid == (uint32_t)geteuid());
    CHECK(p.cli.peer.pid == (uint32_t)getpid() && p.cli.peer.uid == (uint32_t)geteuid());
    pair_close(&p);

    /* 100 full cycles leave no descriptor and no socket file behind */
    char dir[PATH_MAX], path[PATH_MAX + 16];
    CHECK(make_dir(dir, sizeof dir));
    join(path, sizeof path, dir, "hub.sock");
    int before = fd_count();
    for (int i = 0; i < 100; i++) {
        struct oc_ipc_server s;
        struct oc_ipc_conn c, a;
        oc_ipc_server_init(&s);
        oc_ipc_conn_init(&c);
        oc_ipc_conn_init(&a);
        CHECK_EQ(oc_ipc_server_open(&s, path), 0);
        CHECK_EQ(oc_ipc_client_connect(&c, path), 0);
        int rc = -EAGAIN;
        for (int k = 0; k < 100 && rc == -EAGAIN; k++) {
            rc = oc_ipc_server_accept(&s, &a);
            if (rc == -EAGAIN)
                ms_sleep(1);
        }
        CHECK_EQ(rc, 0);
        struct oc_sample smp = make_sample((uint32_t)i);
        struct oc_ipc_msg m;
        CHECK_EQ(oc_ipc_send_sample(&c, &smp), 0);
        CHECK_EQ(recv_wait(&a, &m), 0);
        CHECK(m.type == OC_MSG_SAMPLE && m.u.sample.seq == (uint32_t)i);
        oc_ipc_conn_close(&c);
        oc_ipc_conn_close(&a);
        oc_ipc_server_close(&s);
        struct stat sb;
        CHECK_EQ(lstat(path, &sb), -1);
    }
    CHECK_EQ(fd_count(), before);
    rmrf(dir);
}

int main(void)
{
    RUN(test_init_contract);
    RUN(test_server_lifecycle);
    RUN(test_path_safety);
    RUN(test_connect_states);
    RUN(test_handshake_ack);
    RUN(test_handshake_reject);
    RUN(test_handshake_bad_replies);
    RUN(test_handshake_deadline);
    RUN(test_wait_readable);
    RUN(test_message_boundaries);
    RUN(test_hostile_datagrams);
    RUN(test_eintr_on_send_recv);
    RUN(test_peer_disconnect_and_sigpipe);
    RUN(test_send_queue_bounded);
    RUN(test_fd_hygiene);

    if (failures != 0) {
        fprintf(stderr, "test_ipc: %d FAILURE(S)\n", failures);
        return 1;
    }
    fprintf(stderr, "test_ipc: all passed\n");
    return 0;
}
