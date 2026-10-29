/* light_probe: reads the ALS-PT19 through the ADS1115 using the real driver
 * and adapter (oc_light) and prints one CSV line per sample. A bring-up and
 * evidence tool for M3, not the light reader process: no IPC, no storage.
 *
 *   light_probe [device] [count] [interval_ms]
 *   defaults:    /dev/i2c-1  200    20
 *
 * Samples are labelled with the bus's origin. The Linux transport is LIVE, so
 * every line printed from a real /dev/i2c-N is a LIVE attempt. A failed
 * attempt is printed with its status (NACK, TIMEOUT, IO_ERR, SATURATED) and
 * value 0; nothing is fabricated or retried.
 * stdout: a "#" header, a CSV header, then the samples. stderr: a summary.
 * Exit status: 0 if at least one OK sample, 1 if none, 2 for bad arguments
 * or a device that cannot be opened, 3 if the clock failed. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oc_ads1115.h"
#include "oc_clock.h"
#include "oc_i2c.h"
#include "oc_light.h"
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
    unsigned long count = 200, interval_ms = 20;
    if (argc > 4 || (argc > 2 && !parse_ulong(argv[2], 1, 1000000, &count)) ||
        (argc > 3 && !parse_ulong(argv[3], 1, 60000, &interval_ms))) {
        fprintf(stderr, "usage: %s [device] [count 1..1000000] [interval_ms 1..60000]\n",
                argv[0]);
        return 2;
    }
    if (setvbuf(stdout, nullptr, _IOLBF, 0) != 0)   /* lines appear as they are read */
        return 2;

    struct oc_i2c_linux dev = {.fd = -1};
    struct oc_i2c_bus bus;
    int rc = oc_i2c_linux_open(&dev, path, &bus);
    if (rc != 0) {
        fprintf(stderr, "light_probe: cannot open %s: %s\n", path, strerror(-rc));
        return 2;
    }
    struct oc_clock clk;
    oc_clock_system(&clk);
    struct oc_ads1115_cfg cfg = OC_ADS1115_CFG_DEFAULT;
    struct oc_light light;
    rc = oc_light_init(&light, &bus, &clk, &cfg);
    if (rc != 0) {
        fprintf(stderr, "light_probe: init: %s\n", strerror(-rc));
        oc_i2c_linux_close(&dev);
        return 2;
    }

    printf("# light_probe device=%s addr=0x%02X pga=%u rate=%u (%u SPS) origin=%s count=%lu "
           "interval_ms=%lu\n",
           path, (unsigned)cfg.addr, (unsigned)cfg.pga, (unsigned)cfg.rate,
           (unsigned)oc_ads1115_rate_sps(cfg.rate), origin_name(bus.origin), count, interval_ms);
    printf("t_mono_ns,seq,source,origin,status,raw_code,aux,attempt_us,errno\n");

    unsigned long by_status[8] = {0};
    long min_raw = 0, max_raw = 0;
    double sum_raw = 0;
    bool have_ok = false;
    uint64_t max_attempt_ns = 0;
    uint64_t next = 0;
    int exit_code = 0;
    if (clk.now_ns(clk.ctx, &next) != 0) {
        fprintf(stderr, "light_probe: clock failed\n");
        oc_i2c_linux_close(&dev);
        return 3;
    }
    for (unsigned long i = 0; i < count; i++) {
        uint64_t t0 = 0;
        struct oc_sample s;
        int io = 0;
        if (clk.now_ns(clk.ctx, &t0) != 0 || oc_light_sample(&light, &s, &io) != 0) {
            fprintf(stderr, "light_probe: clock failed; no sample fabricated\n");
            exit_code = 3;
            break;
        }
        uint64_t attempt_ns = s.t_mono_ns - t0;
        if (attempt_ns > max_attempt_ns)
            max_attempt_ns = attempt_ns;
        by_status[s.status < 8 ? s.status : 0]++;
        if (s.status == OC_ST_OK) {
            if (!have_ok || s.value < min_raw)
                min_raw = s.value;
            if (!have_ok || s.value > max_raw)
                max_raw = s.value;
            sum_raw += s.value;
            have_ok = true;
        }
        printf("%" PRIu64 ",%" PRIu32 ",LIGHT,%s,%s,%" PRId32 ",0x%04" PRIX32 ",%" PRIu64 ",%s\n",
               s.t_mono_ns, s.seq, origin_name(s.origin), status_name(s.status), s.value,
               s.aux, attempt_ns / 1000u, io == 0 ? "0" : strerror(-io));

        next += (uint64_t)interval_ms * 1000000u;   /* fixed schedule: no drift */
        if (clk.sleep_until_ns(clk.ctx, next) != 0) {
            fprintf(stderr, "light_probe: sleep failed\n");
            exit_code = 3;
            break;
        }
    }
    oc_i2c_linux_close(&dev);

    fprintf(stderr, "summary:");
    for (uint8_t st = 1; st < 8; st++)
        if (by_status[st] != 0)
            fprintf(stderr, " %s=%lu", status_name(st), by_status[st]);
    if (have_ok)
        fprintf(stderr, "; OK raw %ld..%ld mean %.1f", min_raw, max_raw,
                sum_raw / (double)by_status[OC_ST_OK]);
    else
        fprintf(stderr, "; no OK sample");
    fprintf(stderr, "; max attempt %" PRIu64 " us\n", max_attempt_ns / 1000u);
    if (exit_code != 0)
        return exit_code;
    return by_status[OC_ST_OK] > 0 ? 0 : 1;
}
