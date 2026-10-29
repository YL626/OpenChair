/* distance_probe: reads the VL6180X through the real driver and adapter
 * (oc_distance) and prints one CSV line per sample. A bring-up and evidence
 * tool for M3, not the distance reader process: no IPC, no storage.
 *
 *   distance_probe [device] [count] [interval_ms]
 *   defaults:       /dev/i2c-1  50     100
 *
 * Samples are labelled with the bus's origin. The Linux transport is LIVE,
 * so every line printed from a real /dev/i2c-N is a LIVE measurement.
 * stdout: a "#" header, a CSV header, then the samples. stderr: a summary.
 * Exit status: 0 if at least one OK sample, 1 if none, 2 for bad arguments
 * or a device that cannot be opened, 3 if the clock failed. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oc_clock.h"
#include "oc_distance.h"
#include "oc_i2c.h"
#include "oc_store.h"   /* enum oc_origin */

static const char *status_name(uint8_t st)
{
    static const char *const names[] = {
        "?", "OK", "SATURATED", "NACK", "TIMEOUT", "RANGE_ERR", "STALE", "IO_ERR",
    };
    return st < sizeof names / sizeof names[0] ? names[st] : "?";
}

static const char *origin_name(uint8_t o)
{
    switch (o) {
    case OC_ORIGIN_LIVE:
        return "LIVE";
    case OC_ORIGIN_SIMULATED:
        return "SIMULATED";
    case OC_ORIGIN_REPLAYED:
        return "REPLAYED";
    default:
        return "?";
    }
}

/* Parses a decimal in [lo, hi]; returns false on anything else. */
static bool parse_ulong(const char *s, unsigned long lo, unsigned long hi,
                        unsigned long *out)
{
    char *end = nullptr;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return false;
    *out = v;
    return true;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/dev/i2c-1";
    unsigned long count = 50, interval_ms = 100;
    if (argc > 4 || (argc > 2 && !parse_ulong(argv[2], 1, 1000000, &count)) ||
        (argc > 3 && !parse_ulong(argv[3], 10, 60000, &interval_ms))) {
        fprintf(stderr, "usage: %s [device] [count 1..1000000] [interval_ms 10..60000]\n",
                argv[0]);
        return 2;
    }
    if (setvbuf(stdout, nullptr, _IOLBF, 0) != 0)   /* lines appear as they are read */
        return 2;

    struct oc_i2c_linux dev = {.fd = -1};
    struct oc_i2c_bus bus;
    int rc = oc_i2c_linux_open(&dev, path, &bus);
    if (rc != 0) {
        fprintf(stderr, "distance_probe: cannot open %s: %s\n", path, strerror(-rc));
        return 2;
    }
    struct oc_clock clk;
    oc_clock_system(&clk);
    struct oc_vl6180x_cfg cfg = OC_VL6180X_CFG_DEFAULT;
    struct oc_distance dist;
    rc = oc_distance_init(&dist, &bus, &clk, &cfg);
    if (rc != 0) {
        fprintf(stderr, "distance_probe: init: %s\n", strerror(-rc));
        oc_i2c_linux_close(&dev);
        return 2;
    }

    printf("# distance_probe device=%s addr=0x%02X origin=%s count=%lu interval_ms=%lu\n",
           path, (unsigned)cfg.addr, origin_name(bus.origin), count, interval_ms);
    printf("t_mono_ns,seq,source,origin,status,value_mm,aux,attempt_us,errno\n");

    unsigned long by_status[8] = {0};
    long min_mm = -1, max_mm = -1;
    uint64_t max_attempt_ns = 0;
    uint64_t next = 0;
    int exit_code = 0;
    if (clk.now_ns(clk.ctx, &next) != 0) {
        fprintf(stderr, "distance_probe: clock failed\n");
        oc_i2c_linux_close(&dev);
        return 3;
    }
    for (unsigned long i = 0; i < count; i++) {
        uint64_t t0 = 0;
        struct oc_sample s;
        int io = 0;
        if (clk.now_ns(clk.ctx, &t0) != 0 || oc_distance_sample(&dist, &s, &io) != 0) {
            fprintf(stderr, "distance_probe: clock failed; no sample fabricated\n");
            exit_code = 3;
            break;
        }
        uint64_t attempt_ns = s.t_mono_ns - t0;
        if (attempt_ns > max_attempt_ns)
            max_attempt_ns = attempt_ns;
        by_status[s.status < 8 ? s.status : 0]++;
        if (s.status == OC_ST_OK) {
            if (min_mm < 0 || s.value < min_mm)
                min_mm = s.value;
            if (s.value > max_mm)
                max_mm = s.value;
        }
        printf("%" PRIu64 ",%" PRIu32 ",DISTANCE,%s,%s,%" PRId32 ",0x%02" PRIX32 ",%" PRIu64 ",%s\n",
               s.t_mono_ns, s.seq, origin_name(s.origin), status_name(s.status), s.value,
               s.aux, attempt_ns / 1000u, io == 0 ? "0" : strerror(-io));

        next += (uint64_t)interval_ms * 1000000u;   /* fixed schedule: no drift */
        if (clk.sleep_until_ns(clk.ctx, next) != 0) {
            fprintf(stderr, "distance_probe: sleep failed\n");
            exit_code = 3;
            break;
        }
    }
    oc_i2c_linux_close(&dev);

    fprintf(stderr, "summary:");
    for (uint8_t st = 1; st < 8; st++)
        if (by_status[st] != 0)
            fprintf(stderr, " %s=%lu", status_name(st), by_status[st]);
    if (min_mm >= 0)
        fprintf(stderr, "; OK range %ld..%ld mm", min_mm, max_mm);
    else
        fprintf(stderr, "; no OK sample");
    fprintf(stderr, "; max attempt %" PRIu64 " us\n", max_attempt_ns / 1000u);
    if (exit_code != 0)
        return exit_code;
    return by_status[OC_ST_OK] > 0 ? 0 : 1;
}
