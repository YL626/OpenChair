# OpenChair

OpenChair is a handheld Raspberry Pi-based diagnostic reader for powered wheelchairs. It uses non-contact optical sensing to capture the blink pattern of a wheelchair controller's existing diagnostic LED. A separate distance sensor validates that the reader is positioned correctly before a reading is accepted.

The software will decode supported, verified controller-specific patterns and show an understandable explanation. Unknown or unreliable readings must not produce an invented diagnosis; the device will instead offer retry or manual code entry. The product is designed to operate fully offline.

> **Status: early development.** No sensor support, diagnostic logic, or real application exists yet. The repository contains project documentation, a working native Linux/WSL2 C23 build and test system, a minimal scaffold CLI with smoke tests that exist only to exercise that toolchain, and the `oc_store` storage library (Mechanism D) with its host-side tests. The library is tested with synthetic input only and is not yet integrated into an application.

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
| `make test-distance` / `asan-distance` / `memcheck-distance` | Runs only the VL6180X driver and distance-adapter tests (`tests/test_distance.c`) against a **simulated** sensor. Also part of `make test` and `make asan`; see `docs/vl6180x.md`. |
| `make probe` | Builds `build/plain/distance_probe`, a live bring-up tool that reads the VL6180X over `/dev/i2c-N` and prints timestamped LIVE samples. It only reads a real sensor on the Pi; the procedure is in `docs/vl6180x.md` §4. |
| `make clean` | Removes `build/`. |
| `make run`, `make soakcheck` | **Not available.** Print a message and exit nonzero; no application or soak test exists. |
| `make deploy` | Does not exist. |

Plain and sanitizer builds use separate object trees and are never mixed; Valgrind runs only on the plain build because it is incompatible with ASan.

Verified on WSL2 Ubuntu (GCC 15.2.0, Make 4.4.1, Valgrind 3.26.0): `make test`, `make asan`, and `make memcheck` pass. With the storage tests added, `make test`, `make asan` and `make memcheck` also pass on Fedora (GCC 16.2.1). This is software-only verification. Nothing here has been verified on a Raspberry Pi or with live hardware.
