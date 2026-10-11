# OpenChair

OpenChair is a handheld Raspberry Pi-based diagnostic reader for powered wheelchairs. It uses non-contact optical sensing to capture the blink pattern of a wheelchair controller's existing diagnostic LED. A separate distance sensor validates that the reader is positioned correctly before a reading is accepted.

The software will decode supported, verified controller-specific patterns and show an understandable explanation. Unknown or unreliable readings must not produce an invented diagnosis; the device will instead offer retry or manual code entry. The product is designed to operate fully offline.

> **Status: early development.** No application code, sensor support, or test suite exists yet. The repository currently contains project documentation and a placeholder Makefile only.

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

- **Deployment target:** Raspberry Pi OS / Linux. The exact OS release has not been finalized or verified on hardware.
- **Current development environment:** WSL2 Ubuntu with GCC 15.2.0 and GNU Make 4.4.1.
- **Language and flags:** the systems core is written in C23 and compiled with `-Wall -Wextra -Werror`.
- **Simulation:** software-first development will use simulated sensor inputs until hardware is available. Simulation does not constitute live-hardware verification.

## Milestone status

| Milestone | Status |
|-----------|--------|
| M0 | Repository skeleton exists; setup audit is underway. |
| M1 | Project/problem memo received professor approval (5/5). The local memo has since been updated to reflect the current design. |
| M2 | Detailed design and live hardware verification remain to be completed or verified. |

Mechanisms E (multiprocess fault isolation) and D (custom crash-consistent storage) are provisionally selected, pending Kacper's confirmation and final mapping in `DESIGN.md`.

## Repository structure

```
.
├── CLAUDE.md             # team and development rules
├── PROBLEM.md            # problem memo
├── DESIGN.md             # design document (currently empty)
├── Makefile              # placeholder targets (see below)
├── README.md
├── .gitignore
├── src/                  # C23 systems implementation
├── include/              # public/shared C headers
├── tests/                # automated tests
│   └── fixtures/         # simulated/replayed sensor data
├── YL626/
│   ├── PROMPTLOG.md
│   └── REFLECTION.md
└── kacperm37/
    ├── PROMPTLOG.md
    └── REFLECTION.md
```

## Build and test

There is currently nothing to build, run, deploy, or test. The software cannot yet read sensors or display diagnoses.

Current `Makefile` targets:

| Target | Behavior |
|--------|----------|
| `make`, `make test`, `make asan`, `make memcheck`, `make run`, `make soakcheck` | Placeholders that only print a "not implemented" message. An exit code of 0 is **not** evidence of a successful build or test. |
| `make clean` | Implemented; removes `build/`. |
| `make deploy` | Does not exist. |

No production application or automated test suite has been implemented yet. Build, test, and deployment instructions will be added once they exist.
