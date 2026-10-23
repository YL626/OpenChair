/* Smoke test: runs the real openchair binary (path in OPENCHAIR_BIN) and
 * checks its output and exit status, plus a direct library call. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "openchair_version.h"

#ifndef OPENCHAIR_BIN
#error "OPENCHAIR_BIN must be defined"
#endif

static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

struct result {
    int status;      /* exit code, or -1 on abnormal termination/error */
    char out[512];
    char err[512];
};

static ssize_t drain(int fd, char *buf, size_t cap)
{
    size_t len = 0;
    buf[0] = '\0';
    while (len + 1 < cap) {
        ssize_t n = read(fd, buf + len, cap - 1 - len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            buf[len] = '\0';
            return -1;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    buf[len] = '\0';
    return (ssize_t)len;
}

static int run_child(const char *arg, struct result *r)
{
    int po[2], pe[2];
    r->status = -1;
    r->out[0] = r->err[0] = '\0';
    if (pipe(po) != 0)
        return -1;
    if (pipe(pe) != 0) {
        close(po[0]);
        close(po[1]);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
        return -1;
    }
    if (pid == 0) {
        if (dup2(po[1], STDOUT_FILENO) < 0 || dup2(pe[1], STDERR_FILENO) < 0)
            _exit(127);
        close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
        execl(OPENCHAIR_BIN, OPENCHAIR_BIN, arg, (char *)NULL);
        _exit(127);
    }
    close(po[1]);
    close(pe[1]);
    /* Output is far below pipe capacity, so sequential draining cannot deadlock. */
    int rc = (drain(po[0], r->out, sizeof r->out) < 0 ||
              drain(pe[0], r->err, sizeof r->err) < 0) ? -1 : 0;
    close(po[0]);
    close(pe[0]);
    int st;
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    if (WIFEXITED(st))
        r->status = WEXITSTATUS(st);
    return rc;
}

static void test_library_version(void)
{
    const char *v = oc_version();
    CHECK(v != NULL);
    CHECK(v != NULL && v[0] != '\0');
}

static void test_cli_version(void)
{
    struct result r;
    char expect[256];
    CHECK(run_child("--version", &r) == 0);
    CHECK(r.status == 0);
    snprintf(expect, sizeof expect, "%s\n", oc_version());
    CHECK(strcmp(r.out, expect) == 0);
    CHECK(r.err[0] == '\0');
}

static void test_cli_help(void)
{
    struct result r;
    CHECK(run_child("--help", &r) == 0);
    CHECK(r.status == 0);
    CHECK(strstr(r.out, "--version") != NULL);
}

static void test_cli_bad_arg(void)
{
    struct result r;
    CHECK(run_child("--bogus", &r) == 0);
    CHECK(r.status == 2);
    CHECK(r.out[0] == '\0');
    CHECK(r.err[0] != '\0');
}

int main(void)
{
    test_library_version();
    test_cli_version();
    test_cli_help();
    test_cli_bad_arg();
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    puts("smoke tests passed");
    return 0;
}
