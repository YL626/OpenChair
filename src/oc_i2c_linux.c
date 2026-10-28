#define _POSIX_C_SOURCE 200809L

#include "oc_i2c.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "oc_store.h"   /* enum oc_origin */

static_assert(OC_I2C_M_RD == I2C_M_RD, "flag values must match the kernel's");
static_assert(OC_I2C_MAX_MSGS <= I2C_RDWR_IOCTL_MAX_MSGS, "kernel message limit");

static int linux_xfer(void *ctx, struct oc_i2c_msg *msgs, size_t n)
{
    struct oc_i2c_linux *dev = ctx;
    if (dev == nullptr || msgs == nullptr || n == 0 || n > OC_I2C_MAX_MSGS)
        return -EINVAL;

    struct i2c_msg k[OC_I2C_MAX_MSGS];
    for (size_t i = 0; i < n; i++) {
        if (msgs[i].buf == nullptr || msgs[i].len == 0 ||
            msgs[i].addr > 0x7F || (msgs[i].flags & ~OC_I2C_M_RD) != 0)
            return -EINVAL;
        k[i] = (struct i2c_msg){.addr = msgs[i].addr, .flags = msgs[i].flags,
                                .len = msgs[i].len, .buf = msgs[i].buf};
    }
    if (dev->fd < 0)
        return -EBADF;

    struct i2c_rdwr_ioctl_data req = {.msgs = k, .nmsgs = (__u32)n};
    for (int tries = 0; tries <= OC_I2C_EINTR_RETRIES; tries++) {
        int rc = ioctl(dev->fd, I2C_RDWR, &req);
        if (rc >= 0)
            return (size_t)rc == n ? 0 : -EIO;   /* all messages or none */
        if (errno != EINTR)
            return -errno;
    }
    return -EINTR;
}

int oc_i2c_linux_open(struct oc_i2c_linux *dev, const char *path,
                      struct oc_i2c_bus *bus)
{
    if (dev == nullptr || path == nullptr || bus == nullptr)
        return -EINVAL;
    dev->fd = -1;

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -errno;

    unsigned long funcs = 0;
    if (ioctl(fd, I2C_FUNCS, &funcs) != 0) {
        int err = errno;
        close(fd);
        return -err;
    }
    if ((funcs & I2C_FUNC_I2C) == 0) {   /* needed for combined transactions */
        close(fd);
        return -EOPNOTSUPP;
    }

    dev->fd = fd;
    *bus = (struct oc_i2c_bus){.xfer = linux_xfer, .ctx = dev,
                               .origin = OC_ORIGIN_LIVE};
    return 0;
}

void oc_i2c_linux_close(struct oc_i2c_linux *dev)
{
    if (dev == nullptr || dev->fd < 0)
        return;
    close(dev->fd);   /* the descriptor is released even if close reports an error */
    dev->fd = -1;
}
