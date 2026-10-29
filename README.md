# OpenChair

OpenChair is a handheld Raspberry Pi-based diagnostic reader for powered wheelchairs. It uses non-contact optical sensing to capture the blink pattern of a wheelchair controller's existing diagnostic LED. A separate distance sensor validates that the reader is positioned correctly before a reading is accepted.

The software will decode supported, verified controller-specific patterns and show an understandable explanation. Unknown or unreliable readings must not produce an invented diagnosis; the device will instead offer retry or manual code entry. The product is designed to operate fully offline.

> **Status: early development.** No sensor support, diagnostic logic, supervisor or real application exists yet. The repository contains project documentation, a working native Linux/WSL2 C23 build and test system, host-tested IPC/hub/reader modules, a CLI whose `--role` testing processes run a SIMULATED three-process demonstration (see below), and the `oc_store` storage library (Mechanism D) with its host-side tests. Everything is tested with synthetic or SIMULATED input only; `oc_store` is not yet integrated into the hub. Nothing has been run on the Raspberry Pi or with live sensors.

## Target hardware

- Raspberry Pi 3 Model B+
- Adafruit ALS-PT19 analog light sensor (optical sensing)
- Adafruit ADS1115 16-bit I²C ADC, address `0x48`, receiving the ALS-PT19 output on channel A0
- Adafruit VL6180X distance/proximity sensor, I²C address `0x29`
- Adafruit PiTFT 2.2" 320x240 HAT Mini Kit (#2315), connected over SPI
- Physical tactile switches for navigation
- 32 GB microSD card
- Regulated 5 V / 2.5 A supply for development and testing
- Rechargeable battery system: planned, but not selected or implemented

## Operating system and development environment

- **Deployment target:** Raspberry Pi OS Lite 64-bit is the preferred deployment system, pending actual hardware verification. It has not been finalized or tested on hardware.
- **Current development environment:** WSL2 Ubuntu with GCC 15.2.0 and GNU Make 4.4.1.
- **Language and flags:** the systems core is written in C23 and compiled with `-std=c23 -Wall -Wextra -Werror -pedantic`.
- **Simulation:** software-first development will use simulated sensor inputs until hardware is available. Simulation does not constitute live-hardware verification.

## Milestone status

| Milestone | Status |
|-----------|--------|
| M0 | Repository setup verification is complete, subject to closing the remaining documentation and collaboration items. |
| M1 | Project/problem memo received professor approval (5/5). The local memo has since been updated to reflect the current design. |
| M2 | Detailed design and live hardware verification remain to be completed or verified. |

Mechanisms E (multiprocess fault isolation) and D (custom crash-consistent storage) are provisionally selected, pending Kacper's confirmation and final mapping in `DESIGN.md`.

## Repository structure

```
.
├── CLAUDE.md             # team and development rules
├── PROBLEM.md            # problem memo
├── DESIGN.md             # design document (currently empty)
├── Makefile              # native Linux/WSL2 build and test targets (see below)
├── README.md
├── .gitignore
├── src/                  # C23 systems implementation
│   ├── main.c            # scaffold CLI (--version, --help); not the application
│   ├── version.c         # scaffold version string
│   ├── crc32.c           # CRC-32/IEEE
│   ├── oc_store.c        # oc_store storage library (Mechanism D)
│   └── oc_store_format.{c,h}  # oc_store on-disk byte layout (private)
├── include/              # public/shared C headers
│   ├── openchair_version.h
│   ├── oc_crc32.h
│   └── oc_store.h        # oc_store API [REVIEW]
├── docs/
│   ├── IMPLEMENTATION_NOTES.md
│   ├── oc_store.md       # storage format, sync policy, recovery, open decisions
│   └── oc_store_tests.md # storage test map
├── tools/
│   └── distance_probe.c  # live VL6180X bring-up probe (make probe)
├── tests/                # automated tests
│   ├── test_smoke.c      # smoke test of the scaffold CLI
│   ├── test_store.c      # oc_store functional tests
│   ├── test_store_faults.c  # oc_store write/sync fault injection
│   ├── test_store_kill.c    # oc_store fork + SIGKILL crash trials
│   ├── test_util.h       # shared test helpers
│   └── fixtures/         # simulated/replayed sensor data (empty)
├── YL626/
│   ├── PROMPTLOG.md
│   └── REFLECTION.md
└── kacperm37/
    ├── PROMPTLOG.md
    └── REFLECTION.md
```

## Build and test

The build is native to Linux/WSL2 (GCC, GNU Make). There is no cross-compilation or Raspberry Pi deployment yet. The only program is a scaffold CLI (`openchair --version` / `--help`) whose purpose is to give the toolchain something real to build and test. It reads no sensors and produces no diagnoses.

| Target | Behavior |
|--------|----------|
| `make` / `make all` | Builds `build/plain/openchair`. |
| `make test` | Builds and runs the `oc_store` storage tests and the smoke test (`build/plain/`). The smoke test runs the real `openchair` binary and checks its output and exit status. A failing check makes `make test` exit nonzero. |
| `make asan` | Rebuilds in a separate tree (`build/asan/`) with AddressSanitizer + UBSan (UBSan findings are fatal) and runs the same storage and smoke tests. |
| `make memcheck` | Runs the plain (non-sanitized) storage tests, the smoke test and its `openchair` children under Valgrind, failing on any error or definite/possible leak. Logs go to `build/valgrind-*.log`. Requires `valgrind` to be installed. |
| `make test-store` | Builds and runs only the `oc_store` storage tests (`tests/test_store*.c`) with synthetic input; see `docs/oc_store_tests.md`. Set `TMPDIR` to put the test stores on a disk-backed filesystem (by default `/tmp`, often tmpfs, where `fdatasync` is a no-op). |
| `make asan-store` | The same storage tests under ASan/UBSan. |
| `make memcheck-store` | The same storage tests under Valgrind. It reports Valgrind errors separately from test failures. |
| `make test-multiproc` / `asan-multiproc` / `memcheck-multiproc` | S3C **core** multiprocess suite alone (real hub + two reader processes, SIMULATED samples): plain, under ASan/UBSan, and under Valgrind with every exec'd child traced (`build/valgrind-mp-*.log`). The core suite is also part of `make test` and `make asan`. |
| `make test-stress` / `make asan-stress` | S3C **stress/negative** scenarios, kept out of `make test`: real descriptor exhaustion in the hub (T8) and same-instance respawn with a reset sequence (T7). |
| `make test-capture` / `asan-capture` / `memcheck-capture` | M3-A capture units: payload codec, `oc_capture` over a **real** `oc_store` (exact read-back, arming, tick schedule, reused directory), and injected storage failures. Part of `make test` / `make asan`. |
| `make test-m3a` / `asan-m3a` / `memcheck-m3a` | M3-A **multiprocess** suite (dedicated, not in `make test`): real hub with capture + two reader processes, real `EFBIG`, hub `SIGKILL` recovery. `memcheck-m3a` traces every child (`build/valgrind-m3a-*.log`). |
| `make test-roles` / `asan-roles` | Unit tests of the pure S3C modules (backoff, simulated source, option parsing). |
| `make test-distance` / `asan-distance` / `memcheck-distance` | Runs only the VL6180X driver and distance-adapter tests (`tests/test_distance.c`) against a **simulated** sensor. Also part of `make test` and `make asan`; see `docs/vl6180x.md`. |
| `make probe` | Builds `build/plain/distance_probe`, a live bring-up tool that reads the VL6180X over `/dev/i2c-N` and prints timestamped LIVE samples. It only reads a real sensor on the Pi; the procedure is in `docs/vl6180x.md` §4. |
| `make clean` | Removes `build/`. |
| `make run`, `make soakcheck` | **Not available.** Print a message and exit nonzero; no application or soak test exists. |
| `make deploy` | Does not exist. |

### S3C role processes (SIMULATED only)

`openchair --version` and `--help` are unchanged. The same executable can also run as one of three cooperating processes. Everything these roles produce is **SIMULATED**: there is no hardware, no LIVE mode, no supervisor and no storage. They exist to exercise the IPC transport and the hub state machine across real Linux processes.

```
openchair --role hub --simulate --unsupervised-test --runtime-dir DIR --session-id HEX
          [--light-instance N] [--distance-instance N] [--stat-ms N] [--lifetime-ms N]
openchair --role light|distance --simulate --runtime-dir DIR --session-id HEX --instance N
          [--period-ms N] [--stat-ms N] [--lifetime-ms N]
openchair --role help        # full option list, including the testing controls
```

- `DIR` is a caller-created directory (mode 0700, owned by you); the hub listens on `DIR/hub.sock`.
- **The hub's session id and authorized instances are explicit command-line test configuration** (`--unsupervised-test`). This is not supervisor authorization; no authorization message crosses a process boundary in S3C. The supervisor (S4) will draw the session id and allocate a fresh instance per spawned reader.
- Testing controls, available only with `--simulate` / `--unsupervised-test`: `--fault-every N` (every Nth attempt is a TIMEOUT), `--drop-conn-on-sigusr1` (SIGUSR1 makes a reader drop its socket and reconnect in the same process, keeping instance and sequence), `--hello-timeout-ms`, `--test-fd-headroom N` (hub lowers `RLIMIT_NOFILE` after start-up so only N more descriptors can be opened).
- Each process prints `key=value` lines on stdout: `READY`, `STAT`/`SRC`, `EVENT`, `SUMMARY`. `READY` and `SUMMARY` are delivered with a bounded retry and a failure to deliver them makes the process exit 4; routine lines are dropped (and counted) rather than queued if the reader of the pipe is slow. A process killed with SIGKILL writes no `SUMMARY`.
- Exit codes: 0 clean, 2 usage, 3 configuration or permanent HELLO rejection, 4 runtime failure.

### M3-A capture mode (SIMULATED, software-only)

`openchair --role hub ... --capture-dir DIR [--capture-ms N]` persists the samples the hub accepts from both readers (a source that is silent for longer than `--capture-fresh-ms` inside the window fails the capture) into `oc_store`'s trace stream during a bounded window (default 60 s, hard maximum 600 s), then syncs, reads them back, closes, reopens and reads back again, and prints `CAPRESULT`/`CAPSRC`/`CAPRB`/`CAPSTORE`/`CAPLAT` lines (`PASS` or `FAIL` with named flags). `STORE_RECOVER` reports what recovery found when the store was opened.

- `DIR` holds **persistent evidence** and must be an existing directory *separate from* `--runtime-dir` (the socket directory, which test cleanup deletes). The hub refuses overlapping paths, including via symlinks.
- Everything here is **SIMULATED**. It exercises the same hub-to-`oc_store` path a LIVE capture will use, but it is **not M3 evidence**: M3 requires both real sensors on the Raspberry Pi. The VL6180X reader is not yet merged and the ADS1115 path is unverified on hardware.
- A storage failure does not stop sensing; every accepted-but-unstored sample is counted and the capture FAILS. The supervisor's fallback log does not exist yet; failures are reported on the structured stdout lines and stderr.
- Testing controls (hub, only with `--unsupervised-test`): `--capture-segment-bytes`, `--capture-trace-segments`, `--test-fsize-limit` (real `RLIMIT_FSIZE`).

Plain and sanitizer builds use separate object trees and are never mixed; Valgrind runs only on the plain build because it is incompatible with ASan.

Verified on WSL2 Ubuntu (GCC 15.2.0, Make 4.4.1, Valgrind 3.26.0): `make test`, `make asan`, and `make memcheck` pass. With the storage tests added, `make test`, `make asan` and `make memcheck` also pass on Fedora (GCC 16.2.1). This is software-only verification. Nothing here has been verified on a Raspberry Pi or with live hardware.
