/* distance_probe: reads the VL6180X through the real driver and adapter
 * (oc_distance) and prints one CSV line per sample. A bring-up, calibration
 * and evidence tool, not the distance reader process: no IPC, no storage.
 *
 *   distance_probe [--offset MM] [--calibrate KNOWN_MM] [device] [count] [interval_ms]
 *   defaults:                                            /dev/i2c-1  50     100
 *
 *   --offset MM           write part-to-part offset MM (-128..127) at setup
 *                         and verify it; without it the factory value is kept
 *                         and nothing is written to that register
 *   --calibrate KNOWN_MM  hold a flat target at KNOWN_MM (1..255) during the
 *                         run; prints the current and the suggested offset.
 *                         Writes nothing beyond the normal setup.
 *
 * Samples are labelled with the bus's origin. The Linux transport is LIVE,
 * so every line printed from a real /dev/i2c-N is a LIVE attempt.
 * stdout: "#" comment lines, a CSV header, the samples, and (with
 * --calibrate) a "# calibrate" line. stderr: the summary: counts per status,
 * per range error and per position verdict, adapter stats, attempt-time
 * distribution (p50/p95/p99/max) and OK-range statistics.
 * Exit status: 0 if at least one OK sample (and, with --calibrate, a usable
 * suggestion), 1 otherwise, 2 for bad arguments, a device that cannot be
 * opened or no memory, 3 if the clock failed. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oc_clock.h"
#include "oc_distance.h"
#include "oc_i2c.h"
#include "oc_position.h"
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
    case OC_ORIGIN_MIXED:
        return "MIXED";
    default:
        return "?";
    }
}

/* Parses a decimal in [lo, hi]; returns false on anything else. */
static bool parse_long(const char *s, long lo, long hi, long *out)
{
    char *end = nullptr;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < lo || v > hi)
        return false;
    *out = v;
    return true;
}

struct opts {
    const char *path;
    unsigned long count;
    unsigned long interval_ms;
    bool set_offset;
    int8_t offset_mm;
    bool calibrate;
    uint8_t known_mm;
};

static bool parse_args(int argc, char **argv, struct opts *o)
{
    *o = (struct opts){.path = "/dev/i2c-1", .count = 50, .interval_ms = 100};
    int i = 1;
    long v;
    for (; i < argc && strncmp(argv[i], "--", 2) == 0; i++) {
        if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc &&
            parse_long(argv[i + 1], INT8_MIN, INT8_MAX, &v)) {
            o->set_offset = true;
            o->offset_mm = (int8_t)v;
        } else if (strcmp(argv[i], "--calibrate") == 0 && i + 1 < argc &&
                   parse_long(argv[i + 1], 1, 255, &v)) {
            o->calibrate = true;
            o->known_mm = (uint8_t)v;
        } else {
            return false;
        }
        i++;
    }
    const int npos = argc - i;
    if (npos > 3)
        return false;
    if (npos > 0)
        o->path = argv[i];
    if (npos > 1) {
        if (!parse_long(argv[i + 1], 1, 1000000, &v))
            return false;
        o->count = (unsigned long)v;
    }
    if (npos > 2) {
        if (!parse_long(argv[i + 2], 10, 60000, &v))
            return false;
        o->interval_ms = (unsigned long)v;
    }
    return true;
}

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile of a sorted array, pct in 1..100. */
static uint64_t percentile(const uint64_t *sorted, size_t n, unsigned pct)
{
    size_t rank = (n * pct + 99) / 100;   /* ceil(n * pct / 100) */
    return sorted[rank == 0 ? 0 : rank - 1];
}

static uint64_t isqrt_u64(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > v)
        bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

struct tally {
    unsigned long by_status[8];
    unsigned long by_position[8];
    uint64_t *attempt_ns;   /* one per sample, owned; freed by main */
    size_t n_attempts;
    uint8_t *ok_mm;         /* OK values for --calibrate, owned; freed by main */
    size_t n_ok;
    uint64_t ok_sum, ok_sumsq;
    long ok_min, ok_max;
};

static void print_summary(struct tally *t, const struct oc_distance_stats *st)
{
    fprintf(stderr, "summary: status");
    for (uint8_t s = 1; s < 8; s++)
        if (t->by_status[s] != 0)
            fprintf(stderr, " %s=%lu", status_name(s), t->by_status[s]);
    fprintf(stderr, "\nsummary: range_err");
    for (uint8_t c = 0; c < 16; c++)
        if (st->range_errors[c] != 0)
            fprintf(stderr, " %s=%" PRIu32, oc_vl6180x_range_error_name(c), st->range_errors[c]);
    fprintf(stderr, "\nsummary: position");
    for (uint8_t p = 1; p < 8; p++)
        if (t->by_position[p] != 0)
            fprintf(stderr, " %s=%lu", oc_position_state_name(p), t->by_position[p]);
    fprintf(stderr,
            "\nsummary: adapter attempts=%" PRIu32 " setups=%" PRIu32 " setup_failures=%" PRIu32
            " silent_resets=%" PRIu32 " system_errors=%" PRIu32 " io_failures=%" PRIu32 "\n",
            st->attempts, st->setups, st->setup_failures, st->silent_resets, st->system_errors,
            st->io_failures);
    if (t->n_attempts > 0) {
        qsort(t->attempt_ns, t->n_attempts, sizeof t->attempt_ns[0], cmp_u64);
        fprintf(stderr,
                "summary: attempt_us n=%zu p50=%" PRIu64 " p95=%" PRIu64 " p99=%" PRIu64
                " max=%" PRIu64 "\n",
                t->n_attempts, percentile(t->attempt_ns, t->n_attempts, 50) / 1000u,
                percentile(t->attempt_ns, t->n_attempts, 95) / 1000u,
                percentile(t->attempt_ns, t->n_attempts, 99) / 1000u,
                t->attempt_ns[t->n_attempts - 1] / 1000u);
    }
    const uint64_t n = t->by_status[OC_ST_OK];
    if (n == 0) {
        fprintf(stderr, "summary: ok_mm none\n");
        return;
    }
    /* integer mean and population stddev, in tenths of a mm; values <= 255
     * and n <= 1e6 keep every product below 2^63 */
    const uint64_t mean_x10 = (t->ok_sum * 10u + n / 2u) / n;
    const uint64_t var_x100 = (n * t->ok_sumsq - t->ok_sum * t->ok_sum) * 100u / (n * n);
    const uint64_t sd_x10 = isqrt_u64(var_x100);
    fprintf(stderr,
            "summary: ok_mm n=%" PRIu64 " min=%ld max=%ld mean=%" PRIu64 ".%" PRIu64
            " stddev=%" PRIu64 ".%" PRIu64 "\n",
            n, t->ok_min, t->ok_max, mean_x10 / 10u, mean_x10 % 10u, sd_x10 / 10u, sd_x10 % 10u);
}

/* Prints the calibration line on stdout (so it lands in the CSV file) and
 * stderr. Returns true if the suggestion is usable. */
static bool report_calibration(struct oc_distance *dist, const struct opts *o,
                               const struct tally *t)
{
    int8_t current = 0;
    int rc = oc_vl6180x_read_offset(&dist->dev, &current);
    if (rc != 0) {
        printf("# calibrate known_mm=%u result=offset_read_failed errno=%s\n",
               (unsigned)o->known_mm, strerror(-rc));
        fprintf(stderr, "calibrate: cannot read the current offset: %s\n", strerror(-rc));
        return false;
    }
    struct oc_vl6180x_offset_fit fit = {};
    rc = oc_vl6180x_offset_suggest(o->known_mm, t->ok_mm, t->n_ok, current, &fit);
    const char *result = rc == 0             ? "OK"
                         : rc == -ENODATA    ? "too_few_ok_samples"
                         : rc == -EDOM       ? "too_noisy_target_moved"
                         : rc == -ERANGE     ? "offset_out_of_range"
                                             : "error";
    char line[256];
    if (rc == 0 || rc == -EDOM || rc == -ERANGE)
        snprintf(line, sizeof line,
                 "calibrate known_mm=%u n=%zu mean_mm=%" PRId32 ".%" PRId32
                 " spread_mm=%u current_offset=%d suggested_offset=%d result=%s",
                 (unsigned)o->known_mm, fit.n, fit.mean_x10_mm / 10, fit.mean_x10_mm % 10,
                 (unsigned)fit.spread_mm, (int)current, rc == 0 ? (int)fit.suggested_mm : 0,
                 result);
    else
        snprintf(line, sizeof line, "calibrate known_mm=%u n=%zu current_offset=%d result=%s",
                 (unsigned)o->known_mm, t->n_ok, (int)current, result);
    printf("# %s\n", line);
    fprintf(stderr, "%s\n", line);
    if (rc == 0)
        fprintf(stderr, "calibrate: apply with --offset %d (or the reader's offset config) and "
                        "re-run at the same distance to check\n", (int)fit.suggested_mm);
    return rc == 0;
}

int main(int argc, char **argv)
{
    struct opts o;
    if (!parse_args(argc, argv, &o)) {
        fprintf(stderr,
                "usage: %s [--offset -128..127] [--calibrate 1..255] [device] "
                "[count 1..1000000] [interval_ms 10..60000]\n",
                argv[0]);
        return 2;
    }
    if (setvbuf(stdout, nullptr, _IOLBF, 0) != 0)   /* lines appear as they are read */
        return 2;

    struct tally t = {.ok_min = -1, .ok_max = -1};
    t.attempt_ns = malloc(o.count * sizeof t.attempt_ns[0]);
    t.ok_mm = malloc(o.count);
    if (t.attempt_ns == nullptr || t.ok_mm == nullptr) {
        fprintf(stderr, "distance_probe: out of memory for %lu samples\n", o.count);
        free(t.attempt_ns);
        free(t.ok_mm);
        return 2;
    }

    int exit_code = 0;
    struct oc_i2c_linux dev = {.fd = -1};
    struct oc_i2c_bus bus;
    int rc = oc_i2c_linux_open(&dev, o.path, &bus);
    if (rc != 0) {
        fprintf(stderr, "distance_probe: cannot open %s: %s\n", o.path, strerror(-rc));
        exit_code = 2;
        goto out_free;
    }
    struct oc_clock clk;
    oc_clock_system(&clk);
    struct oc_vl6180x_cfg cfg = OC_VL6180X_CFG_DEFAULT;
    if (o.set_offset) {
        cfg.offset_mode = OC_VL6180X_OFFSET_SET;
        cfg.offset_mm = o.offset_mm;
    }
    struct oc_distance dist;
    struct oc_position pos;
    const struct oc_position_cfg pcfg = OC_POSITION_CFG_DEFAULT;
    rc = oc_distance_init(&dist, &bus, &clk, &cfg);
    if (rc == 0)
        rc = oc_position_init(&pos, &pcfg);
    if (rc != 0) {
        fprintf(stderr, "distance_probe: init: %s\n", strerror(-rc));
        exit_code = 2;
        goto out_close;
    }

    printf("# distance_probe device=%s addr=0x%02X origin=%s count=%lu interval_ms=%lu "
           "offset=%s",
           o.path, (unsigned)cfg.addr, origin_name(bus.origin), o.count, o.interval_ms,
           o.set_offset ? "set" : "factory");
    if (o.set_offset)
        printf(" offset_mm=%d", (int)o.offset_mm);
    if (o.calibrate)
        printf(" calibrate_known_mm=%u", (unsigned)o.known_mm);
    printf("\n# position cfg min_mm=%" PRId32 " max_mm=%" PRId32 " max_spread_mm=%" PRId32
           " window_n=%" PRIu32 " max_age_ms=%" PRIu64 " [MEASURE placeholders]\n",
           pcfg.min_mm, pcfg.max_mm, pcfg.max_spread_mm, pcfg.window_n,
           pcfg.max_age_ns / 1000000u);
    printf("t_mono_ns,seq,source,origin,status,value_mm,aux,range_err,position,attempt_us,errno\n");

    uint64_t next = 0;
    if (clk.now_ns(clk.ctx, &next) != 0) {
        fprintf(stderr, "distance_probe: clock failed\n");
        exit_code = 3;
        goto out_close;
    }
    for (unsigned long i = 0; i < o.count; i++) {
        uint64_t t0 = 0;
        struct oc_sample s;
        int io = 0;
        if (clk.now_ns(clk.ctx, &t0) != 0 || oc_distance_sample(&dist, &s, &io) != 0) {
            fprintf(stderr, "distance_probe: clock failed; no sample fabricated\n");
            exit_code = 3;
            break;
        }
        const uint64_t attempt_ns = s.t_mono_ns - t0;
        t.attempt_ns[t.n_attempts++] = attempt_ns;
        t.by_status[s.status < 8 ? s.status : 0]++;
        if (s.status == OC_ST_OK) {
            if (t.ok_min < 0 || s.value < t.ok_min)
                t.ok_min = s.value;
            if (s.value > t.ok_max)
                t.ok_max = s.value;
            t.ok_sum += (uint64_t)s.value;
            t.ok_sumsq += (uint64_t)s.value * (uint64_t)s.value;
            t.ok_mm[t.n_ok++] = (uint8_t)s.value;
        }
        struct oc_position_verdict v = {};
        if (oc_position_push(&pos, &s) != 0 || oc_position_eval(&pos, s.t_mono_ns, &v) != 0)
            v.state = 0;   /* cannot happen for an adapter sample; shows as "?" */
        t.by_position[v.state < 8 ? v.state : 0]++;
        const bool has_code = s.status == OC_ST_OK || s.status == OC_ST_RANGE_ERR;
        printf("%" PRIu64 ",%" PRIu32 ",DISTANCE,%s,%s,%" PRId32 ",0x%02" PRIX32 ",%s,%s,%" PRIu64
               ",%s\n",
               s.t_mono_ns, s.seq, origin_name(s.origin), status_name(s.status), s.value, s.aux,
               has_code ? oc_vl6180x_range_error_name(oc_vl6180x_range_error((uint8_t)s.aux))
                        : "-",
               oc_position_state_name(v.state), attempt_ns / 1000u,
               io == 0 ? "0" : strerror(-io));

        next += (uint64_t)o.interval_ms * 1000000u;   /* fixed schedule: no drift */
        if (clk.sleep_until_ns(clk.ctx, next) != 0) {
            fprintf(stderr, "distance_probe: sleep failed\n");
            exit_code = 3;
            break;
        }
    }

    bool cal_ok = true;
    if (o.calibrate)
        cal_ok = report_calibration(&dist, &o, &t);   /* before the device is closed */
    struct oc_distance_stats st;
    if (oc_distance_get_stats(&dist, &st) == 0)
        print_summary(&t, &st);
    if (exit_code == 0)
        exit_code = t.by_status[OC_ST_OK] > 0 && cal_ok ? 0 : 1;

out_close:
    oc_i2c_linux_close(&dev);
out_free:
    free(t.attempt_ns);
    free(t.ok_mm);
    return exit_code;
}
