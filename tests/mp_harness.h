/* Shared harness for the S3C multiprocess integration tests
 * (test_multiproc.c = core suite, test_multiproc_stress.c = stress/negative).
 *
 * Everything the child processes report is SIMULATED: no hardware, no LIVE
 * samples. A pass shows IPC / hub logic on this Linux kernel only.
 *
 * Design rules
 *  - Children are real exec'd `openchair --role ...` processes (fork+execv).
 *  - Child stdout is a pipe the harness DRAINS CONTINUOUSLY (pump()). Each
 *    child has one fixed line buffer; an over-long line is discarded and
 *    counted. Only the latest line per tag (plus a short ring of recent lines
 *    for failure diagnostics) is kept, so memory is bounded however much the
 *    children print.
 *  - Waiting is by predicate against CLOCK_MONOTONIC deadlines multiplied by
 *    OC_TEST_TIME_SCALE (set by the Makefile for sanitizer / Valgrind runs).
 *    There are no fixed sleeps standing in for synchronization.
 *  - A global deadline (hard timeout) aborts all waits; teardown still runs
 *    (kill, reap, remove the runtime directory). SIGALRM is the last resort
 *    if the harness itself hangs: it SIGKILLs every child and _exit(124).
 *  - A child that is not reaped, or that had to be SIGKILLed by teardown
 *    without the test intending it, FAILS the run. */
#ifndef MP_HARNESS_H
#define MP_HARNESS_H

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef OPENCHAIR_BIN
#error "OPENCHAIR_BIN must be defined"
#endif

#define SESSION_HEX "5333c0de00000001"
#define LIGHT_INST 7u
#define DIST_INST 9u
#define LINE_CAP 512u
#define MAX_CHILD 8
#define RECENT_N 12u

enum { SRC_NONE = 0, SRC_LIGHT = 1, SRC_DIST = 2 };

struct child {
    const char *name;
    pid_t pid;
    int fd;                       /* read end of stdout pipe, -1 after EOF */
    bool spawned, reaped, harness_killed;
    int wstatus;
    char buf[LINE_CAP];
    size_t len;
    bool discarding;              /* inside an over-long line */
    uint64_t nlines, nlong;
    char ready[LINE_CAP];
    char stat[LINE_CAP];
    unsigned stat_n;
    char src[3][LINE_CAP];
    unsigned src_n[3];
    char fin[LINE_CAP];
    char summary[LINE_CAP];
    char recent[RECENT_N][LINE_CAP];
    unsigned recent_n;
};

static struct child g_ch[MAX_CHILD];
static volatile pid_t g_pids[MAX_CHILD];
static int g_nch;
static unsigned g_scale = 1;
static bool g_valgrind;
static bool g_abort;
static uint64_t g_hard_deadline_ns;
static int g_failures;
static int g_t_fail0;
static const char *g_t_name = "";
static int g_t_total, g_t_passed;
static char g_dir[64];
static char g_sock[108];   /* fits sockaddr_un.sun_path */

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_MSG(cond, ...)                                                 \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);\
            fprintf(stderr, __VA_ARGS__);                                    \
            fputc('\n', stderr);                                             \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---------- key=value line access ---------- */

static bool kv_str(const char *line, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = line; (p = strstr(p, key)) != nullptr; p++) {
        if ((p == line || p[-1] == ' ') && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = strcspn(v, " \n");
            if (n >= cap)
                return false;
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
    }
    return false;
}

static uint64_t U(const char *line, const char *key)
{
    char v[32];
    if (!kv_str(line, key, v, sizeof v))
        return 0;
    return strtoull(v, nullptr, 10);
}

static bool has_key(const char *line, const char *key)
{
    char v[64];
    return kv_str(line, key, v, sizeof v);
}

static bool kv_eq(const char *line, const char *key, const char *val)
{
    char v[64];
    return kv_str(line, key, v, sizeof v) && strcmp(v, val) == 0;
}

/* ---------- output draining ---------- */

static void store_line(struct child *c, const char *line)
{
    c->nlines++;
    snprintf(c->recent[c->recent_n % RECENT_N], LINE_CAP, "%s", line);
    c->recent_n++;
    if (strncmp(line, "READY ", 6) == 0) {
        snprintf(c->ready, LINE_CAP, "%s", line);
    } else if (strncmp(line, "STAT ", 5) == 0) {
        snprintf(c->stat, LINE_CAP, "%s", line);
        c->stat_n++;
    } else if (strncmp(line, "FINAL ", 6) == 0) {
        snprintf(c->fin, LINE_CAP, "%s", line);
    } else if (strncmp(line, "SUMMARY ", 8) == 0) {
        snprintf(c->summary, LINE_CAP, "%s", line);
    } else if (strncmp(line, "SRC ", 4) == 0) {
        int idx = kv_eq(line, "src", "light") ? SRC_LIGHT
                  : kv_eq(line, "src", "distance") ? SRC_DIST : SRC_NONE;
        if (idx != SRC_NONE) {
            snprintf(c->src[idx], LINE_CAP, "%s", line);
            c->src_n[idx]++;
        }
    }
}

static void feed(struct child *c, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char ch = data[i];
        if (ch == '\n') {
            if (!c->discarding) {
                c->buf[c->len] = '\0';
                store_line(c, c->buf);
            }
            c->len = 0;
            c->discarding = false;
        } else if (c->discarding) {
            continue;
        } else if (c->len + 1 >= sizeof c->buf) {
            c->discarding = true;           /* over-long line: drop it, count it */
            c->nlong++;
            c->len = 0;
        } else {
            c->buf[c->len++] = ch;
        }
    }
}

static void read_child(struct child *c)
{
    char tmp[2048];
    for (int guard = 0; guard < 64; guard++) {
        ssize_t n = read(c->fd, tmp, sizeof tmp);
        if (n > 0) {
            feed(c, tmp, (size_t)n);
            continue;
        }
        if (n == 0) {
            close(c->fd);
            c->fd = -1;
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            close(c->fd);
            c->fd = -1;
        }
        return;
    }
}

/* Reads every child's pipe for up to `ms`, returning early once any data came. */
static void pump(int ms)
{
    if (!g_abort && mono_ns() > g_hard_deadline_ns) {
        g_abort = true;
        fprintf(stderr, "FAIL: hard timeout reached; aborting waits\n");
        g_failures++;
    }
    struct pollfd pf[MAX_CHILD];
    int map[MAX_CHILD];
    int n = 0;
    for (int i = 0; i < g_nch; i++)
        if (g_ch[i].fd >= 0) {
            pf[n] = (struct pollfd){.fd = g_ch[i].fd, .events = POLLIN};
            map[n++] = i;
        }
    if (n == 0) {
        struct timespec ts = {.tv_nsec = (long)(ms > 0 ? ms : 1) * 1000000L};
        nanosleep(&ts, nullptr);
        return;
    }
    int rc = poll(pf, (nfds_t)n, ms);
    if (rc <= 0)
        return;
    for (int k = 0; k < n; k++)
        if (pf[k].revents != 0)
            read_child(&g_ch[map[k]]);
}

#define WAIT_UNTIL(okvar, cond, ms)                                              \
    do {                                                                         \
        uint64_t dl_ = mono_ns() + (uint64_t)(ms) * g_scale * 1000000ull;        \
        (okvar) = false;                                                         \
        for (;;) {                                                               \
            if (cond) {                                                          \
                (okvar) = true;                                                  \
                break;                                                           \
            }                                                                    \
            uint64_t n_ = mono_ns();                                             \
            if (g_abort || n_ >= dl_)                                            \
                break;                                                           \
            int w_ = (int)((dl_ - n_) / 1000000ull);                             \
            pump(w_ > 20 ? 20 : (w_ < 1 ? 1 : w_));                              \
        }                                                                        \
    } while (0)

/* Drain for `ms` of wall time regardless of data (a measurement window). */
static void run_for(int ms)
{
    uint64_t end = mono_ns() + (uint64_t)ms * 1000000ull;
    for (;;) {
        uint64_t n = mono_ns();
        if (g_abort || n >= end)
            return;
        int w = (int)((end - n) / 1000000ull);
        pump(w > 20 ? 20 : (w < 1 ? 1 : w));
    }
}

/* ---------- processes ---------- */

static void alarm_handler(int sig)
{
    (void)sig;
    for (int i = 0; i < MAX_CHILD; i++)
        if (g_pids[i] > 0)
            kill(g_pids[i], SIGKILL);
    static const char msg[] = "FAIL: harness watchdog fired; children killed\n";
    ssize_t r = write(STDERR_FILENO, msg, sizeof msg - 1);
    (void)r;
    _exit(124);
}

static struct child *spawn(const char *name, char *const argv[])
{
    if (g_nch >= MAX_CHILD)
        return nullptr;
    struct child *c = &g_ch[g_nch];
    memset(c, 0, sizeof *c);
    c->name = name;
    c->fd = -1;
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0)
        return nullptr;
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return nullptr;
    }
    if (pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent)
            _exit(127);
        int dn = open("/dev/null", O_RDONLY);
        if (dn < 0 || dup2(dn, STDIN_FILENO) < 0 || dup2(p[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (dn > STDERR_FILENO)
            close(dn);
        execv(OPENCHAIR_BIN, argv);
        _exit(127);
    }
    close(p[1]);
    int fl = fcntl(p[0], F_GETFL);
    if (fl < 0 || fcntl(p[0], F_SETFL, fl | O_NONBLOCK) != 0) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        close(p[0]);
        return nullptr;
    }
    c->fd = p[0];
    c->pid = pid;
    c->spawned = true;
    g_pids[g_nch] = pid;
    g_nch++;
    return c;
}

static void mark_reaped(struct child *c, int st)
{
    c->reaped = true;
    c->wstatus = st;
    for (int i = 0; i < g_nch; i++)
        if (&g_ch[i] == c)
            g_pids[i] = 0;
}

static bool child_exited(struct child *c)
{
    if (c->reaped)
        return true;
    int st = 0;
    pid_t r = waitpid(c->pid, &st, WNOHANG);
    if (r == c->pid) {
        mark_reaped(c, st);
        return true;
    }
    return false;
}

static bool child_alive(struct child *c)
{
    return c->spawned && !child_exited(c);
}

static bool reap_within(struct child *c, int ms)
{
    bool ok;
    WAIT_UNTIL(ok, child_exited(c), ms);
    if (ok)
        for (int i = 0; i < 5 && c->fd >= 0; i++)      /* collect the tail of its output */
            pump(10);
    return ok;
}

/* SIGTERM, bounded wait, SIGKILL escalation. Escalation is a failure. */
static bool terminate(struct child *c, int ms)
{
    if (!c->spawned || c->reaped)
        return true;
    kill(c->pid, SIGCONT);                 /* a child left SIGSTOPped by a failed test must see SIGTERM */
    kill(c->pid, SIGTERM);
    if (reap_within(c, ms))
        return true;
    fprintf(stderr, "FAIL: %s (pid %d) ignored SIGTERM within %d ms; SIGKILL\n", c->name,
            (int)c->pid, ms);
    g_failures++;
    c->harness_killed = true;
    kill(c->pid, SIGKILL);
    (void)reap_within(c, 5000);
    return false;
}

static bool exited_ok(const struct child *c)
{
    return c->reaped && WIFEXITED(c->wstatus) && WEXITSTATUS(c->wstatus) == 0;
}

static bool killed_by(const struct child *c, int sig)
{
    return c->reaped && WIFSIGNALED(c->wstatus) && WTERMSIG(c->wstatus) == sig;
}

/* ---------- spawning the roles ---------- */

static char *dup_str(const char *s)
{
    char *d = strdup(s);
    if (d == nullptr) {
        fputs("FAIL: out of memory\n", stderr);
        exit(2);
    }
    return d;
}

/* extra: NULL-terminated list of additional arguments (literals). */
static struct child *spawn_role(const char *name, const char *role, const char *const *fixed,
                                const char *const *extra)
{
    char *argv[48];
    int n = 0;
    argv[n++] = dup_str("openchair");
    argv[n++] = dup_str("--role");
    argv[n++] = dup_str(role);
    argv[n++] = dup_str("--simulate");
    argv[n++] = dup_str("--runtime-dir");
    argv[n++] = dup_str(g_dir);
    argv[n++] = dup_str("--session-id");
    argv[n++] = dup_str(SESSION_HEX);
    for (; fixed != nullptr && *fixed != nullptr && n < 44; fixed++)
        argv[n++] = dup_str(*fixed);
    for (; extra != nullptr && *extra != nullptr && n < 46; extra++)
        argv[n++] = dup_str(*extra);
    argv[n] = nullptr;
    struct child *c = spawn(name, argv);
    for (int i = 0; i < n; i++)
        free(argv[i]);
    return c;
}

static struct child *spawn_hub(const char *const *extra)
{
    static const char *const fixed[] = {"--unsupervised-test", "--light-instance", "7",
                                        "--distance-instance", "9", "--stat-ms", "100",
                                        "--lifetime-ms", "300000", nullptr};
    return spawn_role("hub", "hub", fixed, extra);
}

static struct child *spawn_reader(const char *name, const char *role, unsigned inst, const char *period_ms,
                                  const char *const *extra)
{
    char inst_s[16];
    snprintf(inst_s, sizeof inst_s, "%u", inst);
    const char *fixed[] = {"--instance", inst_s, "--period-ms", period_ms, "--stat-ms", "100",
                           "--lifetime-ms", "300000", nullptr};
    return spawn_role(name, role, fixed, extra);
}

/* ---------- environment, directories, /proc ---------- */

static int rm_cb(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void rmtree(const char *path)
{
    nftw(path, rm_cb, 8, FTW_DEPTH | FTW_PHYS);
}

static void harness_init(unsigned hard_timeout_s)
{
    const char *s = getenv("OC_TEST_TIME_SCALE");
    if (s != nullptr && atoi(s) > 0)
        g_scale = (unsigned)atoi(s);
    g_valgrind = getenv("OC_TEST_UNDER_VALGRIND") != nullptr;
    const char *tmp = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/oc-s3c-XXXXXX", tmp != nullptr && strlen(tmp) < 24 ? tmp : "/tmp");
    if (mkdtemp(g_dir) == nullptr) {
        perror("mkdtemp");
        exit(2);
    }
    if (snprintf(g_sock, sizeof g_sock, "%s/hub.sock", g_dir) >= (int)sizeof g_sock) {
        fputs("runtime dir path too long for a socket\n", stderr);
        exit(2);
    }
    g_hard_deadline_ns = mono_ns() + (uint64_t)hard_timeout_s * g_scale * 1000000000ull;
    struct sigaction sa = {.sa_handler = alarm_handler};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, nullptr);
    alarm((hard_timeout_s * g_scale) + 30);
    signal(SIGPIPE, SIG_IGN);
    printf("[S3C SIMULATED -- no hardware, no LIVE samples] scale=%u valgrind=%d dir=%s\n",
           g_scale, g_valgrind, g_dir);
}

[[maybe_unused]] static int count_fds(pid_t pid)
{
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/fd", (int)pid);
    DIR *d = opendir(p);
    if (d == nullptr)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return pid == getpid() ? n - 1 : n;   /* our own listing includes opendir's descriptor */
}

[[maybe_unused]] static void dump_fds(const struct child *c)
{
    char p[64], l[384], t[128];
    snprintf(p, sizeof p, "/proc/%d/fd", (int)c->pid);
    DIR *d = opendir(p);
    if (d == nullptr)
        return;
    struct dirent *e;
    fprintf(stderr, "  fds of %s (pid %d):", c->name, (int)c->pid);
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(l, sizeof l, "%s/%s", p, e->d_name);
        ssize_t n = readlink(l, t, sizeof t - 1);
        if (n > 0) {
            t[n] = '\0';
            fprintf(stderr, " %s->%s", e->d_name, t);
        }
    }
    fputc('\n', stderr);
    closedir(d);
}

/* utime+stime in clock ticks, or -1. */
static long cpu_ticks(pid_t pid)
{
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(p, "r");
    if (f == nullptr)
        return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *rp = strrchr(buf, ')');
    if (rp == nullptr)
        return -1;
    unsigned long ut = 0, st = 0;
    /* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
    if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2)
        return -1;
    return (long)(ut + st);
}

/* True if `needle` appears in /proc/net/unix (i.e. a socket with that path exists). */
static bool unix_socket_listed(const char *needle)
{
    FILE *f = fopen("/proc/net/unix", "r");
    if (f == nullptr)
        return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof line, f) != nullptr)
        if (strstr(line, needle) != nullptr)
            found = true;
    fclose(f);
    return found;
}

/* ---------- reader / hub line helpers ---------- */

/* Counter invariants of a reader STAT or SUMMARY line. */
[[maybe_unused]] static bool reader_line_consistent(const char *line)
{
    uint64_t a = U(line, "attempted");
    return has_key(line, "attempted") &&
           a == U(line, "sent") + U(line, "unsent_disc") + U(line, "bp_drop") + U(line, "send_fail") &&
           a == U(line, "next_seq");
}

/* Hub SRC line: accepted + lost + unobserved == last_seq + 1 (or all zero). */
[[maybe_unused]] static bool hub_src_consistent(const char *line)
{
    if (!has_key(line, "accepted"))
        return false;                      /* a missing report is not consistent */
    uint64_t acc = U(line, "accepted");
    if (acc == 0)
        return true;
    return acc + U(line, "lost") + U(line, "unobs") == U(line, "last_seq") + 1;
}

/* ---------- test bookkeeping ---------- */

static void t_begin(const char *name)
{
    g_t_name = name;
    g_t_fail0 = g_failures;
    fprintf(stderr, "== %s\n", name);
}

static void t_end(void)
{
    g_t_total++;
    bool ok = g_failures == g_t_fail0;
    if (ok)
        g_t_passed++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", g_t_name);
    fflush(stdout);
}

static void dump_recent(void)
{
    for (int i = 0; i < g_nch; i++) {
        struct child *c = &g_ch[i];
        fprintf(stderr, "---- last output of %s (pid %d, %llu lines, %llu over-long):\n", c->name,
                (int)c->pid, (unsigned long long)c->nlines, (unsigned long long)c->nlong);
        unsigned start = c->recent_n > RECENT_N ? c->recent_n - RECENT_N : 0;
        for (unsigned k = start; k < c->recent_n; k++)
            fprintf(stderr, "  %s\n", c->recent[k % RECENT_N]);
    }
}

/* Terminates and reaps everything still running, closes pipes, verifies the
 * socket path and listing are gone, removes the runtime directory. */
static void harness_teardown(void)
{
    for (int i = 0; i < g_nch; i++)
        if (g_ch[i].spawned && !g_ch[i].reaped)
            (void)terminate(&g_ch[i], 5000);
    for (int i = 0; i < g_nch; i++) {
        struct child *c = &g_ch[i];
        CHECK_MSG(!c->spawned || c->reaped, "%s (pid %d) was never reaped", c->name, (int)c->pid);
        if (c->spawned && c->reaped)
            CHECK_MSG(kill(c->pid, 0) != 0 && errno == ESRCH, "%s pid %d still exists", c->name,
                      (int)c->pid);
        if (c->fd >= 0) {
            close(c->fd);
            c->fd = -1;
        }
    }
    CHECK(access(g_sock, F_OK) != 0);
    CHECK(!unix_socket_listed(g_sock));
    rmtree(g_dir);
    CHECK(access(g_dir, F_OK) != 0);
}

static int harness_finish(const char *suite)
{
    if (g_failures != 0)
        dump_recent();
    printf("%s: %d/%d scenarios passed, %d failed check(s) [SIMULATED]\n", suite, g_t_passed,
           g_t_total, g_failures);
    return g_failures == 0 ? 0 : 1;
}

#endif
