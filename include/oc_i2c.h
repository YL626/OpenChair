#ifndef OC_I2C_H
#define OC_I2C_H

/* oc_i2c: minimal I2C transaction transport.
 *
 * A bus is a transfer function plus the trusted provenance of what it talks
 * to. xfer runs 1..OC_I2C_MAX_MSGS messages as ONE bus transaction (a read of
 * a register is a pointer write followed by a read with a repeated start, with
 * no other master in between).
 *
 * xfer returns 0 only if every message completed. Otherwise a negative errno;
 * on Linux an address NACK is -ENXIO and a data NACK is -EREMOTEIO. A partial
 * completion is an error (-EIO); the caller never sees "some messages ran".
 * For read messages, buf contents are unspecified after an error.
 *
 * origin is set by the transport, not by its user: the Linux transport is
 * OC_ORIGIN_LIVE; a fake used in tests is OC_ORIGIN_SIMULATED. Samples copy it,
 * so simulated bytes are not labelled LIVE by routine use.
 *
 * LIMITATION: this is a convention, not enforcement. struct oc_i2c_bus is a
 * plain struct: code that builds a bus can set any origin, and code that
 * later writes the field can change it. oc_light copies the origin at init,
 * so later edits to the caller's struct do not reach samples, but the
 * oc_light/oc_ads1115 structs are also plain and writable. The real guard is
 * the hub, which rejects any sample whose origin differs from the mode the
 * reader declared in HELLO (Notes section 2).
 *
 * Timing: a userspace deadline cannot interrupt a transfer that is blocked
 * inside the kernel. A hung bus can hold xfer past any deadline; detecting
 * that is the supervisor's job (missed heartbeats), not this layer's. */

#include <stddef.h>
#include <stdint.h>

#define OC_I2C_M_RD 0x0001u    /* same value as Linux I2C_M_RD */
#define OC_I2C_MAX_MSGS 2u

struct oc_i2c_msg {
    uint16_t addr;    /* 7-bit address */
    uint16_t flags;   /* 0 = write, OC_I2C_M_RD = read */
    uint16_t len;     /* > 0 */
    uint8_t *buf;     /* caller-owned; read into or written from */
};

struct oc_i2c_bus {
    int (*xfer)(void *ctx, struct oc_i2c_msg *msgs, size_t n);
    void *ctx;
    uint8_t origin;   /* enum oc_origin */
};

/* Linux /dev/i2c-N transport. Caller owns the struct; the struct owns fd. */
struct oc_i2c_linux {
    int fd;           /* -1 when closed */
};

/* Opens path (O_RDWR|O_CLOEXEC), verifies the adapter supports plain I2C
 * transactions, and fills *bus (origin LIVE, ctx = dev). *dev must be closed or
 * unused: an fd it already holds is overwritten, not closed. Returns 0 or a
 * negative errno; on error nothing stays open and *dev is fd = -1.
 * *bus is valid only while *dev stays open and at the same address. */
int oc_i2c_linux_open(struct oc_i2c_linux *dev, const char *path,
                      struct oc_i2c_bus *bus);

/* Closes the descriptor. Safe on a never-opened (fd = -1) or closed struct. */
void oc_i2c_linux_close(struct oc_i2c_linux *dev);

/* Retries of an interrupted (EINTR) I2C_RDWR before giving up with -EINTR. */
#define OC_I2C_EINTR_RETRIES 3

#endif
