# OpenChair

OpenChair is a handheld Raspberry Pi-based diagnostic reader for powered wheelchairs. It uses non-contact optical sensing to capture the blink pattern of a wheelchair controller's existing diagnostic LED. A separate distance sensor validates that the reader is positioned correctly before a reading is accepted.

The software will decode supported, verified controller-specific patterns and show an understandable explanation. Unknown or unreliable readings must not produce an invented diagnosis; the device will instead offer retry or manual code entry. The product is designed to operate fully offline.

> **Status: early development.** No sensor support, diagnostic logic, supervisor or real application exists yet. The repository contains project documentation, a working native Linux/WSL2 C23 build and test system, host-tested IPC/hub/reader modules, and a CLI whose `--role` testing processes run a SIMULATED three-process demonstration (see below). Nothing has been run on the Raspberry Pi or with live sensors.

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
│   └── version.c         # scaffold version string
├── include/              # public/shared C headers
│   └── openchair_version.h
├── tests/                # automated tests
│   ├── test_smoke.c      # smoke test of the scaffold CLI
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
| `make test` | Builds and runs the smoke test (`build/plain/`). It runs the real `openchair` binary and checks its output and exit status. A failing check makes `make test` exit nonzero. |
| `make asan` | Rebuilds in a separate tree (`build/asan/`) with AddressSanitizer + UBSan (UBSan findings are fatal) and runs the same smoke test. |
| `make memcheck` | Runs the plain (non-sanitized) smoke test and its `openchair` children under Valgrind, failing on any error or definite/possible leak. Logs go to `build/valgrind-*.log`. Requires `valgrind` to be installed. |
| `make test-store` | Builds and runs the `oc_store` storage tests (`tests/test_store*.c`). **Expected to fail** until the storage library is implemented (it is currently a stub); see `docs/oc_store_tests.md`. |
| `make asan-store` | The same storage tests under ASan/UBSan. Also expected to fail until implemented. |
| `make memcheck-store` | The same storage tests under Valgrind. It reports Valgrind errors separately from test failures. |
| `make test-multiproc` / `asan-multiproc` / `memcheck-multiproc` | S3C **core** multiprocess suite alone (real hub + two reader processes, SIMULATED samples): plain, under ASan/UBSan, and under Valgrind with every exec'd child traced (`build/valgrind-mp-*.log`). The core suite is also part of `make test` and `make asan`. |
| `make test-stress` / `make asan-stress` | S3C **stress/negative** scenarios, kept out of `make test`: real descriptor exhaustion in the hub (T8) and same-instance respawn with a reset sequence (T7). |
| `make test-roles` / `asan-roles` | Unit tests of the pure S3C modules (backoff, simulated source, option parsing). |
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

Plain and sanitizer builds use separate object trees and are never mixed; Valgrind runs only on the plain build because it is incompatible with ASan.

Verified on WSL2 Ubuntu (GCC 15.2.0, Make 4.4.1, Valgrind 3.26.0): `make test`, `make asan`, and `make memcheck` pass. This is software-only verification. Nothing here has been verified on a Raspberry Pi or with live hardware.
