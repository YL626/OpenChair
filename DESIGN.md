# OpenChair Design Document (M2)

**Agreed by Alan and Kacper:** Mechanism **E** (multi-process isolation and supervision), Mechanism **D** (custom crash-consistent storage), the four-process architecture (supervisor, light reader, distance reader, hub), and the section 7 ownership. This is agreement on architecture and responsibilities, not a completed review of every low-level interface. **[TARGET]** = goal, not a result; **[MEASURE]** = needs hardware data; **[REVIEW]** = detail awaiting technical review. Nothing here is measured on hardware. Alan has hardware now; both partners Monday–Wednesday. Low-level detail is in [`docs/IMPLEMENTATION_NOTES.md`](docs/IMPLEMENTATION_NOTES.md) ("Notes"); this document stands alone.

## 1. Architecture

An ALS-PT19 via an ADS1115 (I2C `0x48`, A0) captures the controller's diagnostic LED; a VL6180X (I2C `0x29`) validates placement; our pipeline decodes the pattern and the PiTFT shows it (UP / DOWN / ENTER). A capture is accepted only while the distance stream shows the reader in range and stable. One executable, `openchair`, runs as four processes chosen by role **[REVIEW]**; the UI is a hub module, so a UI fault is a hub fault.

```
  ADS1115 (I2C)               VL6180X (I2C)             PiTFT + buttons (SPI/GPIO)
       |                           |                            |
  +----+-----+              +------+-----+                      |
  |  light   |              |  distance  |                      |
  +----+-----+              +------+-----+                      |
       | (A) oc_sample 28 B        | (B) oc_sample 28 B         |
       | 64-250/s [MEASURE]        | 10-20/s [TARGET]           |
       |     AF_UNIX SOCK_SEQPACKET|                            |
       v                           v                            |
  +------------------------------------------+                  |
  | hub: fusion, validation, decode, lookup, |<-----------------+
  | UI; links oc_store                       |  (C) human rate
  +--------------------+---------------------+
                       | (D) evidence <1 record/s; capture mode <=150 samples/s
                       v
               SD card: evidence + trace streams
  supervisor: fork/exec, waitpid, restart policy, fallback log
   (E) control socket (fd 3) to each child: 1 Hz heartbeat + events [TARGET]
```

**Interface [REVIEW].** `oc_sample` carries raw counts: `t_mono_ns` (`CLOCK_MONOTONIC` when the I2C read returns; system-wide, so comparable across processes), per-source `seq`, `source`, `origin` (LIVE, SIMULATED, REPLAYED), `status`, `value`, `aux`. A failed read carries a status and `value = 0`; nothing is fabricated. It is a fixed 24-byte little-endian record in a versioned envelope; receivers reject and count malformed messages (Notes §1).

**Session and restart.** The supervisor draws a random `session_id` and numbers each spawn with an `instance`; a sample is identified by `(session_id, source, instance, seq)`. A new `instance` means the reader restarted: the hub resets that source's filter state and abandons any in-progress capture. A `seq` gap means counted drops, and a capture spanning one is rejected (Notes §2).

**IPC.** The hub listens on a `SOCK_SEQPACKET` socket; readers connect with a handshake, send without blocking (dropping and counting rather than stalling their I2C loop), and reconnect with backoff if the hub restarts (Notes §3). Everything is `CLOEXEC`, and each child starts with only its control socket as fd 3 (Notes §4).

**Inputs and pipeline.** Readers have `live_i2c`, `sim` and `replay` backends behind one interface; the hub distinguishes them only through `origin`. The pipeline (stages in `CLAUDE.md`) yields exactly one of: a verified code, UNKNOWN, RETRY or MANUAL ENTRY. Optical rate, gain, shroud and rejection thresholds await hardware validation **[MEASURE]**.

## 2. OS mechanism mapping

**E: multi-process architecture with a supervisor.** Implemented in C by the supervisor, readers, hub, and the IPC and control channels. *Why:* a caregiver away from a bench cannot have a hung ADC read or crashed reader freeze the screen or remove manual entry; the device must degrade honestly and recover. The supervisor detects **crashes** at once via `waitpid`/EOF, and **hangs** only via missed heartbeats (3 missed at 1 Hz **[TARGET]**). It logs PID, status and time, marks the system degraded, restarts (backoff 1 s to 30 s; after 5 failures in 10 minutes the child stays down **[REVIEW]**) and logs recovery. A process stuck in an uninterruptible I2C call cannot be killed until it returns; the system stays degraded and says so. *Comparison:* a single-process baseline (readers as threads), built only for the experiment. Crash: `SIGKILL` one reader vs. the whole baseline; measure detection latency, whether hub and UI keep running, and time to first valid sample. Hang: `SIGSTOP` a reader vs. an injected stall in a baseline thread; measure heartbeat detection latency and other components' responsiveness. Report RSS and CPU for both.

**D: custom crash-consistent storage** (`oc_store`, a C library in the hub). *Why:* the device runs unattended on an SD card and may lose power; fault history and results must survive, and logs must not fill the card. *Comparison:* per-record fsync vs. batched vs. none (throughput, latency, CPU, records recovered after interrupted writes).

## 3. Failure-mode table

Every row is exercised by fault injection; rows describe design, not observed behavior. "Fallback" is the supervisor's log (section 4).

| Component | Failure | Detection | Response | Log |
|---|---|---|---|---|
| ADS1115 / ALS-PT19 | NACK, timeout, saturated/stuck | Reader status; non-OK or stale `seq` | Optical failed; auto-diagnosis suspended; manual entry stays | `OPTICAL_FAIL` |
| VL6180X | NACK, timeout, range error | Same, distance stream | Positioning unavailable; no result called validated; retry | `DISTANCE_FAIL` |
| Shared I2C bus | Hang, contention | Both readers failing | Both failed; backoff retry; degraded | Correlated `*_FAIL` |
| Light/distance reader | Crash, `kill -9`, hang | `waitpid`/EOF; missed heartbeats | Degrade; restart; stop at bound | `CHILD_DEATH`, `RESTART n`, `RECOVERED` (store, else fallback) |
| Hub (incl. UI) | Crash, hang | Supervisor | Restart; readers reconnect; store recovery | `STORE_RECOVER`; fallback |
| Supervisor | Crash | OS restarter, e.g. systemd **[REVIEW]** | Children exit on control EOF | Next start marks unclean run |
| IPC channel | Peer closes, malformed message | `recv`, length/enum checks | Drop and count; close after repeats | `IPC_ERR` |
| Flash decode | Unrecognized/inconsistent | Rejection rules | UNKNOWN / RETRY / MANUAL ENTRY | `CAPTURE_REJECTED reason` |
| Positioning | Out of range/unstable | Distance gate | Reject; ask to reposition | `CAPTURE_REJECTED POSITION` |
| Storage write | `ENOSPC`, `EIO`, read-only, failed sync | Syscall returns | Store FAILED; sensing and UI continue; results shown unsaved | `STORE_FAIL` (fallback) |
| Storage corruption | Torn tail, damaged segment | CRC, record seq, footer | Truncate torn tail; quarantine damaged segment | `STORE_RECOVER` counts |
| Display/buttons | SPI/GPIO error | Error returns | Run headless; keep logging | `UI_FAIL` |
| Power | Brownout, loss | Undervoltage flags **[MEASURE]**; recovery scan | Recover at next boot | Unclean-run marker |
| Clock | Wall time unsynced | Startup flag | Monotonic only; wall time untrusted | `WALLCLOCK untrusted` |

## 4. Storage and data

**Streams.** *Evidence:* startup/shutdown, 60 s heartbeats (per-process RSS, liveness, cumulative counts, session input mode), sensor failures, process events, injected faults and detection, diagnostic attempts and results, recovery. Expected well under 2 MiB per 48 h **[MEASURE]**; cap 64 MiB with nothing deleted before the cap, so a 48 h soak keeps everything (reaching the cap is logged and fails the soak check). *Trace:* `oc_sample` batches around each diagnostic attempt and for live-capture mode; 32 MiB ring, oldest first. Soak logging is the evidence stream; there is no continuous raw sampling. Raw files are copied off with a checksum and never edited.

**Fallback log.** The supervisor keeps a plain-text `fallback.log`, independent of `oc_store` code, for whenever the hub or store is unavailable: supervisor events, reader events relayed over the control channel, and `STORE_FAIL`. It is appended, synced per line, rate-limited and size-bounded; if it fails, stderr (Notes §8).

**Provenance.** Each record carries `kind` (SENSOR, DERIVED, SYSTEM), `origin`, `session_id`, `instance` and emitter. Samples keep their origin. A DERIVED result is LIVE only if every contributing sample is LIVE, else it takes the non-LIVE value or MIXED. SYSTEM events are not sensor measurements, so their origin is NONE; startup and every heartbeat record the session input mode, so a simulated session cannot read as live. Injected faults record `injected=true` and the method.

**Interface and durability [REVIEW].** The hub uses `open` (runs recovery), `append`, `tick`, `sync`, `status`, `read` and `close` (Notes §6). Data is *buffered* (process memory; lost if the process dies), *written* (`write()` returned; lost on power loss), *durable* (a sync succeeded) or *uncertain* (a sync failed). `append` only buffers; results, faults and heartbeats force a sync, and other records are batched (sync after 32 records or 500 ms **[MEASURE]**). The hub must check every return, never treat `append` as durable, and keep sensing if the store fails. `oc_store` must never report durable early, bound its memory, and report errno and state precisely. **Kacper owns the implementation, file layout, recovery algorithm and tests**, subject to this interface.

**Failure semantics.** A failed `fsync`/`fdatasync` does not prove records were lost. The store marks every record not yet confirmed durable as **uncertain**, enters FAILED, stops acknowledging records as durable, and never retries that descriptor and assumes success. Recovery on the next `open` (the hub retries with backoff) determines which uncertain records are valid; nothing is resubmitted silently. Other components are unaffected.

**Framing and recovery.** Append-only segment files hold a header, CRC32-protected records with a contiguous record seq, and a footer once sealed; a new segment's header and directory entry are synced before use (layout in Notes §5). Recovery scans segments in order. Only the newest segment may have a torn tail, which is truncated at its first invalid record. A sealed older segment that fails validation was damaged, not torn: it is kept unmodified, marked damaged, and scanning continues (Notes §7). Recovery never fabricates records. *Guarantee:* every record durable before the failure is present and intact; later records may be lost whole or partly and are never accepted corrupt. This assumes the SD card honors flush and the filesystem (assumed ext4) behaves as documented; CRC32 detects random damage and is not a security check. Power-loss experiments probe the assumption but cannot prove it in general.

**M3 live-capture mode (REQUIRED).** A bounded mode (target 60 s, maximum 10 min) in which the hub persists every sample from **both** readers, including non-OK statuses, with timestamp, `seq`, `instance` and origin, through the real hub and `oc_store` path into the trace stream, then verifies them by read-back. It is separate from 48 h soak logging; SIMULATED or REPLAYED data cannot satisfy it. *Acceptance, per source, on live hardware:* stored count = received count; all `seq` gaps accounted for; timestamps non-decreasing; origin LIVE; data reads back after `kill -9` of the hub and restart, with `STORE_RECOVER` reported (Notes §9). This is a criterion, not a result.

## 5. Constraints and substitutions

| Ideal | Actual | Cost |
|---|---|---|
| Several real supported controllers | Families undecided; early work uses sim/replay and any available LED | No accuracy or compatibility claim until verified documentation and real captures exist |
| Live hardware throughout | Software-first on WSL2 with SIMULATED/REPLAYED inputs through the same interfaces | Proves logic only; never M2/M3 or soak evidence |
| Both partners on hardware | Alan now; both Monday–Wednesday | Distance-reader live bring-up waits for Kacper's access |
| Final battery; low-noise optical front end | Regulated 5 V / 2.5 A supply; ALS-PT19 → ADS1115 (PiTFT owns SPI) | No battery-life claim; optical rate and noise **[MEASURE]** |
| Pi OS; sensor driver | Pi OS Lite 64-bit preferred, unverified; userspace `/dev/i2c-*` | Native vs. cross-compile undecided; no `make deploy`; neither mechanism is a driver |

## 6. Evaluation plan

All numbers are **[TARGET]**s, revised after first measurements; report distributions (p50/p95/p99/max).

| Area | Method | Target |
|---|---|---|
| Sample-to-hub latency, idle and loaded | Reader timestamp vs. hub receive | p99 ≤ 5 ms idle, ≤ 20 ms loaded |
| Sample drops | `seq` gaps per source/instance | 0 unexplained in 1 h idle; ≤ 0.1% loaded, all accounted |
| Crash detection and recovery | `kill -9` each reader ≥ 30× | Detect p99 ≤ 100 ms; first valid sample p95 ≤ 5 s |
| Hang detection | `SIGSTOP` a reader ≥ 30× | Detected within 3.5 s |
| Descriptor leak | `/proc/<pid>/fd` over 100 restarts per child | No growth; post-exec set exactly {0,1,2,3} |
| CPU and RSS per process | `/proc` sampling idle, loaded, whole soak | RSS: readers ≤ 8 MiB, hub ≤ 32 MiB, growth ≤ 1 MiB/24 h; CPU: reader ≤ 3%, hub ≤ 10% of a core |
| Storage throughput/latency | Per-record vs. batched vs. no fsync | `append` p99 ≤ 1 ms; ≥ 10× fewer syncs than per-record; recovery ≤ 5 s |
| Crash consistency | (a) `kill -9` mid-write, injected failed sync; (b) automated truncation, torn-write, bit-flip injection; (c) a few controlled real power cuts on a backed-up or disposable card | (a)+(b) ≥ 1,000 trials: 0 corrupt records accepted, all durable records recovered, no durable ack after a failed sync. (c) ≥ 3 cuts, each reported individually; supports but never proves crash consistency |
| Domain metric | Detection vs. ground-truth patterns we generate | ≥ 200 captures, ≥ 3 brightness/ambient/distance conditions: ≥ 95% correct, wrong-code ≤ 1%; UNKNOWN on bad input counts as correct rejection |
| Fault injection | One run per failure-table row, ≥ 3 repeats | All rows exercised, 100% detected |
| Soak | 1 h, 8 h, then ≥ 48 h on live hardware | 0 missed hourly heartbeats, injected fault detected and recovered, orderly end, evidence under cap |

## 7. Ownership map (agreed first authorship)

Ownership means first authorship and demo-day answerability, not exclusivity; each partner must answer across the boundary.

| Area | First author |
|---|---|
| ALS-PT19/ADS1115 light reader; sensor fusion; optical decoding and controller lookup; Mechanism E (supervisor, IPC and control channels, restart policy, fallback log, hub process loop) | Alan |
| VL6180X distance reader; Mechanism D (`oc_store` implementation, layout, recovery, associated tests) | Kacper |
| Hardware integration; UI; evaluation; design review; final integration; the `oc_sample` and storage-interface contracts (each partner reviews the side they consume) | Shared |

## 8. AI-use plan

- **Development:** Claude Code in each partner's own clone and session, per the `CLAUDE.md` workflow, with fresh-context and partner review. Each partner copies out their own raw transcripts at every milestone.
- **Not in the product:** no hosted model, LLM API, cloud inference or pretrained model at runtime. Detection, fusion, decoding and diagnosis are our code; diagnostic codes come only from verified documentation.
- **Network independence.** A text search cannot prove it, so we layer checks, none sufficient alone: (1) a source tripwire for network headers, `AF_INET`/`AF_INET6`, resolver calls, HTTP libraries and model files (a hit fails review; a clean result proves nothing); (2) `readelf -d`/`ldd` showing only libc (and libm); (3) test and soak runs under `strace -f -e trace=network` and `ss`, expecting only `AF_UNIX`; (4) a full diagnosis and a soak segment with all network interfaces down; (5) partner review of dependency changes. Pi services such as SSH are outside the claim.
