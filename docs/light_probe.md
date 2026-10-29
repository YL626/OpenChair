# light_probe: ADS1115 / ALS-PT19 bring-up

`tools/light_probe.c` is the light-sensor counterpart of `distance_probe` (`docs/vl6180x.md` §4). It reads the ADS1115 (A0, ALS-PT19 OUT) through the same driver and adapter the light reader uses (`oc_ads1115`, `oc_light`, `oc_i2c_linux`) and prints one CSV line per attempt. It has no IPC and no storage and is independent of the multiprocess pipeline.

**Status: host-built only.** The tool builds and its argument/open-failure paths were exercised on a machine without I2C. **It has never read a real ADS1115.** Its CSV is the first LIVE evidence once run on the Pi.

## Build and run (on the Pi)

```
make probe-light
./build/plain/light_probe [device] [count] [interval_ms]    # defaults: /dev/i2c-1 200 20
./build/plain/light_probe /dev/i2c-1 500 20 > light-$(date +%s).csv 2> light-summary.txt
```

Wiring (power off while connecting; 3.3 V only): ALS-PT19 OUT to ADS1115 A0, both at 3.3 V; ADS1115 SDA/SCL on the Pi's I2C bus, address 0x48 (ADDR to GND). I2C must be enabled and the user in the `i2c` group (`ls -l /dev/i2c-1`, `i2cdetect -y 1` should show `48`).

## Output

stdout: a `#` header (device, address, PGA, rate, **origin**), then `t_mono_ns,seq,source,origin,status,raw_code,aux,attempt_us,errno`.

- `origin` is LIVE for a real `/dev/i2c-N`. `raw_code` is the signed 16-bit ADC code (no volts, no thresholds). A failed attempt is printed with its status and `raw_code` 0; nothing is fabricated, retried or filtered.
- `status`: `OK`, `SATURATED` (code at INT16_MAX/MIN: digital clipping of the ADC only), `NACK` (no device / not responding), `TIMEOUT` (conversion did not finish in time), `IO_ERR` (other bus error, or the config readback differed from what was written). `errno` gives the cause.
- `attempt_us` is the time one attempt took (conversion latency at the default 128 SPS should be a little over 8 ms; this column is how to measure it).
- `aux` is the ADS1115 idle config word (0x4383 at the defaults).

stderr: a summary (counts per status, min/max/mean of OK codes, longest attempt).

Exit status: 0 at least one OK sample; 1 none (for example everything NACKs: check wiring, address, power); 2 bad arguments or the device cannot be opened; 3 the clock failed.

## What to record

Keep the raw CSV and the summary unedited with the date, wiring, and ambient-light conditions, and run it with the sensor covered, in room light and in front of the controller LED. Gain (±4.096 V) and rate (128 SPS) are unverified starting defaults [MEASURE].
