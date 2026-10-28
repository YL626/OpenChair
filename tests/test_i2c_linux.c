#define _GNU_SOURCE
/* Tests for the Linux I2C transport (src/oc_i2c_linux.c).
 *
 * There is no I2C adapter on the host. ioctl is wrapped (-Wl,--wrap=ioctl) so
 * the I2C_FUNCS and I2C_RDWR calls are scripted; the descriptor itself is a
 * real open of /dev/null, which lets descriptor cleanup be checked against
 * /proc/self/fd. This verifies the transport's request building and error
 * handling only. It says nothing about a real adapter. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "oc_i2c.h"
#include "oc_store.h"

static int failures;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

#define RUN(test)                          \
    do {                               \
        fprintf(stderr, "== %s\n", #test); \
        reset_script();                    \
        test();                            \
    } while (0)

int __real_ioctl(int fd, unsigned long req, ...);

enum funcs_mode { FUNCS_PASSTHROUGH, FUNCS_OK, FUNCS_NO_I2C, FUNCS_FAIL };

#define MAX_RESULTS 8
static struct {
    enum funcs_mode funcs;
    int funcs_errno;
    /* per-call RDWR results: ret >= 0 is returned as is, ret < 0 sets errno */
    int ret[MAX_RESULTS];
    int err[MAX_RESULTS];
    unsigned results;
    unsigned rdwr_calls;
    unsigned funcs_calls;
    unsigned nmsgs;
    struct i2c_msg msgs[OC_I2C_MAX_MSGS];
    uint8_t wr_copy[OC_I2C_MAX_MSGS][4];
    uint8_t rd_fill;   /* written into read buffers by the fake adapter */
} sc;

static void reset_script(void)
{
    memset(&sc, 0, sizeof sc);
    sc.funcs = FUNCS_OK;
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);

    if (req == I2C_FUNCS) {
        sc.funcs_calls++;
        switch (sc.funcs) {
        case FUNCS_PASSTHROUGH:
            return __real_ioctl(fd, req, arg);
        case FUNCS_OK:
            *(unsigned long *)arg = I2C_FUNC_I2C;
            return 0;
        case FUNCS_NO_I2C:
            *(unsigned long *)arg = I2C_FUNC_SMBUS_BYTE;
            return 0;
        case FUNCS_FAIL:
            errno = sc.funcs_errno;
            return -1;
        }
    }
    if (req == I2C_RDWR) {
        struct i2c_rdwr_ioctl_data *d = arg;
        unsigned idx = sc.rdwr_calls++;
        sc.nmsgs = d->nmsgs;
        for (unsigned i = 0; i < d->nmsgs && i < OC_I2C_MAX_MSGS; i++) {
            sc.msgs[i] = d->msgs[i];
            if (!(d->msgs[i].flags & I2C_M_RD) && d->msgs[i].len <= 4)
                memcpy(sc.wr_copy[i], d->msgs[i].buf, d->msgs[i].len);
            else if (d->msgs[i].flags & I2C_M_RD)
                memset(d->msgs[i].buf, sc.rd_fill, d->msgs[i].len);
        }
        unsigned k = idx < sc.results ? idx : sc.results - 1;
        if (sc.results == 0)
            return (int)d->nmsgs;
        if (sc.ret[k] < 0) {
            errno = sc.err[k];
            return -1;
        }
        return sc.ret[k];
    }
    return __real_ioctl(fd, req, arg);
}

static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (d == nullptr)
        return -1;
    int n = 0;
    while (readdir(d) != nullptr)
        n++;
    closedir(d);
    return n - 1;   /* the directory's own descriptor is counted once; fine as a delta */
}

static void script_result(int ret, int err)
{
    sc.ret[sc.results] = ret;
    sc.err[sc.results] = err;
    sc.results++;
}

static void test_open_missing_path(void)
{
    struct oc_i2c_linux dev = {.fd = 123};
    struct oc_i2c_bus bus;
    memset(&bus, 0, sizeof bus);
    int before = count_fds();
    CHECK(oc_i2c_linux_open(&dev, "/nonexistent/i2c-99", &bus) == -ENOENT);
    CHECK(dev.fd == -1);
    CHECK(bus.xfer == nullptr);
    CHECK(count_fds() == before);
    CHECK(sc.funcs_calls == 0);
}

static void test_open_args(void)
{
    struct oc_i2c_linux dev;
    struct oc_i2c_bus bus;
    CHECK(oc_i2c_linux_open(nullptr, "/dev/null", &bus) == -EINVAL);
    CHECK(oc_i2c_linux_open(&dev, nullptr, &bus) == -EINVAL);
    CHECK(oc_i2c_linux_open(&dev, "/dev/null", nullptr) == -EINVAL);
}

static void test_open_close_ok(void)
{
    struct oc_i2c_linux dev;
    struct oc_i2c_bus bus;
    memset(&bus, 0, sizeof bus);
    int before = count_fds();
    CHECK(oc_i2c_linux_open(&dev, "/dev/null", &bus) == 0);
    CHECK(dev.fd >= 0 && count_fds() == before + 1);
    CHECK(bus.xfer != nullptr && bus.ctx == &dev && bus.origin == OC_ORIGIN_LIVE);
    int fl = fcntl(dev.fd, F_GETFD);
    CHECK(fl >= 0 && (fl & FD_CLOEXEC));
    CHECK(sc.funcs_calls == 1);
    oc_i2c_linux_close(&dev);
    CHECK(dev.fd == -1 && count_fds() == before);
    oc_i2c_linux_close(&dev);   /* second close is harmless */
    CHECK(dev.fd == -1 && count_fds() == before);
    oc_i2c_linux_close(nullptr);
}

static void test_open_cleans_up_when_adapter_unsuitable(void)
{
    struct oc_i2c_linux dev;
    struct oc_i2c_bus bus;
    memset(&bus, 0, sizeof bus);
    int before = count_fds();

    sc.funcs = FUNCS_NO_I2C;
    CHECK(oc_i2c_linux_open(&dev, "/dev/null", &bus) == -EOPNOTSUPP);
    CHECK(dev.fd == -1 && count_fds() == before && bus.xfer == nullptr);

    sc.funcs = FUNCS_FAIL;
    sc.funcs_errno = ENOTTY;
    CHECK(oc_i2c_linux_open(&dev, "/dev/null", &bus) == -ENOTTY);
    CHECK(dev.fd == -1 && count_fds() == before && bus.xfer == nullptr);

    /* real ioctl on /dev/null: not an I2C device */
    sc.funcs = FUNCS_PASSTHROUGH;
    CHECK(oc_i2c_linux_open(&dev, "/dev/null", &bus) == -ENOTTY);
    CHECK(dev.fd == -1 && count_fds() == before);
}

static void open_ok(struct oc_i2c_linux *dev, struct oc_i2c_bus *bus)
{
    CHECK(oc_i2c_linux_open(dev, "/dev/null", bus) == 0);
}

static void test_xfer_builds_rdwr_request(void)
{
    struct oc_i2c_linux dev; struct oc_i2c_bus bus; open_ok(&dev, &bus);
    uint8_t reg = 0x01, rd[2] = {0, 0};
    struct oc_i2c_msg m[2] = {
        {.addr = 0x48, .flags = 0, .len = 1, .buf = &reg},
        {.addr = 0x48, .flags = OC_I2C_M_RD, .len = 2, .buf = rd},
    };
    sc.rd_fill = 0xA5;
    script_result(2, 0);
    CHECK(bus.xfer(bus.ctx, m, 2) == 0);
    CHECK(sc.rdwr_calls == 1 && sc.nmsgs == 2);
    CHECK(sc.msgs[0].addr == 0x48 && sc.msgs[0].flags == 0 && sc.msgs[0].len == 1);
    CHECK(sc.msgs[1].addr == 0x48 && sc.msgs[1].flags == I2C_M_RD && sc.msgs[1].len == 2);
    CHECK(sc.wr_copy[0][0] == 0x01);
    CHECK(rd[0] == 0xA5 && rd[1] == 0xA5);   /* read buffer is the caller's */
    oc_i2c_linux_close(&dev);
}

static void test_xfer_single_write(void)
{
    struct oc_i2c_linux dev; struct oc_i2c_bus bus; open_ok(&dev, &bus);
    uint8_t w[3] = {1, 0xC3, 0x83};
    struct oc_i2c_msg m = {.addr = 0x48, .flags = 0, .len = 3, .buf = w};
    script_result(1, 0);
    CHECK(bus.xfer(bus.ctx, &m, 1) == 0);
    CHECK(sc.nmsgs == 1 && sc.wr_copy[0][1] == 0xC3 && sc.wr_copy[0][2] == 0x83);
    oc_i2c_linux_close(&dev);
}

static void test_xfer_errors_and_short_completion(void)
{
    struct oc_i2c_linux dev; struct oc_i2c_bus bus; open_ok(&dev, &bus);
    uint8_t reg = 0, rd[2];
    struct oc_i2c_msg m[2] = {
        {.addr = 0x48, .flags = 0, .len = 1, .buf = &reg},
        {.addr = 0x48, .flags = OC_I2C_M_RD, .len = 2, .buf = rd},
    };
    static const int errs[] = {ENXIO, EREMOTEIO, ETIMEDOUT, EIO, EBUSY, EAGAIN};
    for (size_t i = 0; i < sizeof errs / sizeof errs[0]; i++) {
        reset_script();
        script_result(-1, errs[i]);
        CHECK(bus.xfer(bus.ctx, m, 2) == -errs[i]);
        CHECK(sc.rdwr_calls == 1);
    }
    /* fewer messages completed than requested: an error, never success */
    reset_script(); script_result(1, 0);
    CHECK(bus.xfer(bus.ctx, m, 2) == -EIO);
    reset_script(); script_result(0, 0);
    CHECK(bus.xfer(bus.ctx, m, 2) == -EIO);
    reset_script(); script_result(3, 0);        /* more than requested is also wrong */
    CHECK(bus.xfer(bus.ctx, m, 2) == -EIO);
    oc_i2c_linux_close(&dev);
}

static void test_xfer_eintr_is_retried_but_bounded(void)
{
    struct oc_i2c_linux dev; struct oc_i2c_bus bus; open_ok(&dev, &bus);
    uint8_t w[3] = {1, 0, 0};
    struct oc_i2c_msg m = {.addr = 0x48, .flags = 0, .len = 3, .buf = w};

    script_result(-1, EINTR); script_result(-1, EINTR); script_result(1, 0);
    CHECK(bus.xfer(bus.ctx, &m, 1) == 0 && sc.rdwr_calls == 3);

    reset_script();
    script_result(-1, EINTR);                   /* last result repeats forever */
    CHECK(bus.xfer(bus.ctx, &m, 1) == -EINTR);
    CHECK(sc.rdwr_calls == 1 + OC_I2C_EINTR_RETRIES);
    oc_i2c_linux_close(&dev);
}

static void test_xfer_rejects_bad_requests_without_ioctl(void)
{
    struct oc_i2c_linux dev; struct oc_i2c_bus bus; open_ok(&dev, &bus);
    uint8_t b[4] = {0};
    struct oc_i2c_msg m[3] = {
        {.addr = 0x48, .flags = 0, .len = 1, .buf = b},
        {.addr = 0x48, .flags = OC_I2C_M_RD, .len = 2, .buf = b},
        {.addr = 0x48, .flags = 0, .len = 1, .buf = b},
    };
    CHECK(bus.xfer(bus.ctx, nullptr, 1) == -EINVAL);
    CHECK(bus.xfer(bus.ctx, m, 0) == -EINVAL);
    CHECK(bus.xfer(bus.ctx, m, 3) == -EINVAL);
    struct oc_i2c_msg z = {.addr = 0x48, .flags = 0, .len = 0, .buf = b};
    CHECK(bus.xfer(bus.ctx, &z, 1) == -EINVAL);
    struct oc_i2c_msg nb = {.addr = 0x48, .flags = 0, .len = 1, .buf = nullptr};
    CHECK(bus.xfer(bus.ctx, &nb, 1) == -EINVAL);
    struct oc_i2c_msg bf = {.addr = 0x48, .flags = 0x8000, .len = 1, .buf = b};
    CHECK(bus.xfer(bus.ctx, &bf, 1) == -EINVAL);   /* only RD is a defined flag */
    struct oc_i2c_msg wide = {.addr = 0x200, .flags = 0, .len = 1, .buf = b};
    CHECK(bus.xfer(bus.ctx, &wide, 1) == -EINVAL); /* 7-bit addresses only */
    CHECK(bus.xfer(nullptr, m, 1) == -EINVAL);
    CHECK(sc.rdwr_calls == 0);
    oc_i2c_linux_close(&dev);
}

static void test_xfer_after_close(void)
{
    struct oc_i2c_linux dev; struct oc_i2c_bus bus; open_ok(&dev, &bus);
    oc_i2c_linux_close(&dev);
    uint8_t w = 0;
    struct oc_i2c_msg m = {.addr = 0x48, .flags = 0, .len = 1, .buf = &w};
    CHECK(bus.xfer(bus.ctx, &m, 1) == -EBADF);
    CHECK(sc.rdwr_calls == 0);
}

int main(void)
{
    RUN(test_open_missing_path);
    RUN(test_open_args);
    RUN(test_open_close_ok);
    RUN(test_open_cleans_up_when_adapter_unsuitable);
    RUN(test_xfer_builds_rdwr_request);
    RUN(test_xfer_single_write);
    RUN(test_xfer_errors_and_short_completion);
    RUN(test_xfer_eintr_is_retried_but_bounded);
    RUN(test_xfer_rejects_bad_requests_without_ioctl);
    RUN(test_xfer_after_close);

    if (failures != 0) {
        fprintf(stderr, "test_i2c_linux: %d FAILURE(S)\n", failures);
        return 1;
    }
    fprintf(stderr, "test_i2c_linux: all passed\n");
    return 0;
}
