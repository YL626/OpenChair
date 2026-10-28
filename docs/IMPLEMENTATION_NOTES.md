# OpenChair Implementation Notes

Supplement to `DESIGN.md`. `DESIGN.md` is authoritative; if the two disagree, `DESIGN.md` wins and this file is corrected. These notes hold low-level detail removed from the design document. Everything here is **[REVIEW]**: the architecture is team-agreed, but byte-level and API-level details await technical review by the owning and consuming partner. Nothing here has been implemented or measured on hardware. **[MEASURE]** marks values that need hardware data.

## 1. oc_sample wire format

Raw counts only; derived values are never sent. A sample is 24 bytes, little-endian, produced and parsed by explicit encode/decode functions (never a struct `memcpy`) with a compile-time size assertion:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 8 | `t_mono_ns` | `CLOCK_MONOTONIC` when the I2C read returns. The ADS1115 conversion finished earlier; the offset is bounded by the data rate **[MEASURE]** |
| 8 | 4 | `seq` | Per source and instance, from 0, +1 per sample attempt |
| 12 | 1 | `source` | 1 LIGHT, 2 DISTANCE |
| 13 | 1 | `origin` | 1 LIVE, 2 SIMULATED, 3 REPLAYED |
| 14 | 1 | `status` | 1 OK, 2 SATURATED, 3 NACK, 4 TIMEOUT, 5 RANGE_ERR, 6 STALE, 7 IO_ERR (**pending: awaiting Kacper's approval**) |
| 15 | 1 | reserved | Must be 0 |
| 16 | 4 | `value` | i32: signed ADC counts, or millimetres. 0 when `status` is not OK |
| 20 | 4 | `aux` | u32: ADS1115 config word, or raw VL6180X status code **[MEASURE]** |

Every IPC message has a 4-byte envelope: version u8, type u8, payload length u16. Types: HELLO, HELLO_ACK, HELLO_REJECT, SAMPLE, BYE. A SAMPLE message is 28 bytes. **Status 7, IO_ERR (S2, issue #14; shared change awaiting Kacper's approval).** A read failed for a reason that is neither a NACK nor a timeout (EIO, EBUSY, arbitration loss, config readback mismatch). Values 1-6 are unchanged. `OC_PROTO_VERSION` stays 1 during this development phase, so a decoder built without IO_ERR support rejects status 7 as malformed (`-EBADMSG`) and is incompatible with this sender; all builds must be updated together. A version bump is a team decision to revisit before any release or soak. As with every non-OK status, `value` must be 0.

The receiver rejects and counts unknown enum values, a nonzero reserved byte, a wrong length, and an unsupported version (closing the connection on the last).

## 2. Session and instance handling

- The supervisor draws a random 64-bit `session_id` with `getrandom` at startup and gives each spawned child an `instance` number from a per-session counter, passed on the command line.
- The child sends both in HELLO, with its role, protocol version, and input mode (LIVE, SIMULATED or REPLAYED). The hub rejects any sample whose `origin` differs from the declared mode.
- A sample is identified by `(session_id, source, instance, seq)`.
- A new `instance` for a source means the reader restarted. The hub resets that source's filter and baseline state, abandons any in-progress capture, and records the discontinuity.
- A `seq` gap within one instance means loss, including samples dropped while the hub was away. The hub counts the gap and rejects a capture that spans it.

## 3. IPC connection lifecycle

1. The hub creates a runtime directory (mode 0700) and a `SOCK_SEQPACKET|SOCK_CLOEXEC` listening socket in it, removing any stale socket path first. The supervisor runs one hub at a time.
2. A reader connects and sends HELLO. The hub checks `SO_PEERCRED`, version, role, mode and instance, then replies HELLO_ACK or HELLO_REJECT with a reason code.
3. The reader sends SAMPLE messages with `MSG_DONTWAIT|MSG_NOSIGNAL`. If the socket is full or disconnected, it drops the sample, increments a counter, and keeps its I2C loop on schedule. It never buffers without bound.
4. On EOF or `ECONNRESET` the reader reconnects with backoff (100 ms doubling to 2 s) and sends a new HELLO with the same instance. `seq` keeps counting, so the gap is visible to the hub.
5. When a reader exits or its connection closes, the hub marks that source disconnected and the system degraded. The supervisor restarts the reader with a new instance; the new connection resets the source (section 2).
6. When the hub restarts, readers see EOF and follow step 4.

## 4. Descriptor handling across fork and exec

- The supervisor opens every descriptor with `CLOEXEC`/`O_CLOEXEC`.
- For each child it creates a control `socketpair(AF_UNIX, SOCK_SEQPACKET|SOCK_CLOEXEC)`. After `fork`, the child `dup2`s its end onto fd 3 (which clears `CLOEXEC` on fd 3), closes all other descriptors above 3 (`close_range` where available, otherwise a `/proc/self/fd` sweep), then calls `exec`.
- A child therefore starts with exactly fds 0, 1, 2 and 3, and opens its own sensor, socket and file descriptors afterwards. The supervisor closes its end of the pair when it reaps the child.
- The control channel carries the 1 Hz heartbeat (RSS, counters, state) and events from child to supervisor.
- Test: after `exec`, a child lists `/proc/self/fd` and the set must equal {0,1,2,3}; across 100 restarts the supervisor's own descriptor count must not grow.

## 5. Storage layout

Initial layout, owned by Kacper and changeable under the `DESIGN.md` interface.

- **Streams:** evidence (4 MiB segments, at most 16 = 64 MiB cap) and trace (2 MiB segments, at most 16 = 32 MiB ring). Names `NNNNNNNN.seg` in a directory per stream.
- **Segment:** header (magic, format version, segment id, first record seq); records; footer once sealed (record count, last record seq).
- **Record:** magic, type, version, payload length, record seq (contiguous across segments), payload, CRC32 over header and payload. All little-endian. Types: EVENT, HEARTBEAT, RESULT, SAMPLES (a batch from one source and instance).
- **Provenance block** at the start of each payload: `kind` (SENSOR, DERIVED, SYSTEM), `origin` (LIVE, SIMULATED, REPLAYED, MIXED, NONE), `session_id`, `instance`, emitter.
- **New segment protocol:** write header, `fdatasync`, `fsync` the directory, then append. **Sealing:** write footer, sync, then open the next segment. Deleting a segment is followed by a directory sync.

## 6. oc_store API

Single-writer, not thread-safe, used by the hub only. Data states: *buffered* (process memory), *written* (`write()` returned), *durable* (sync succeeded), *uncertain* (sync failed).

| Call | Behavior |
|---|---|
| `open(cfg, &report)` | Opens or creates streams and runs recovery (section 7). Report: records kept, torn bytes dropped, damaged segments, last durable seq. |
| `append(stream, type, prov, payload, len, flags, &seq)` | Copies the payload, assigns the record seq; the record is buffered. `FORCE_SYNC` writes and syncs before returning. Returns an error once FAILED. |
| `tick(now)` | Called by the hub at least every 100 ms; writes and `fdatasync`s when 32 records or 500 ms have accumulated, advancing records to written, then durable. |
| `sync(&durable_seq)` | Writes and syncs everything pending; on success every record ≤ `durable_seq` is durable. |
| `status(&st)` | State (OK or FAILED), counts of buffered, written, durable and uncertain records, last errno. |
| `read(stream, from_seq, visit)` | Calls `visit` for each record in order; the record is valid only during the call. |
| `close()` | Syncs, seals the active segment, records the final state. |

*Hub responsibilities:* check every return; treat `append` success as buffered only; pass `FORCE_SYNC` for results, faults, heartbeats and startup records; call `tick` regularly; on FAILED, report via the fallback log, tell the UI results are unsaved, and keep sensing. *`oc_store` responsibilities:* never report durable early; own its copies and bound its memory; block no longer than one sync; report errno and state precisely.

## 7. Recovery procedure

1. List segments per stream in id order; validate each header.
2. For each segment, read records in order, checking magic, length, CRC32 and contiguous record seq.
3. **Newest segment:** truncate at the first invalid record; if its header is not intact, delete it (it was never durably created).
4. **Older sealed segment:** if a record or the footer fails validation, it was damaged, not torn. Keep it unmodified, mark it damaged, and continue with the next segment.
5. Report records kept, bytes dropped, damaged segments, and sequence gaps. Recovery never fabricates records.
6. **After a failed sync:** mark every record not confirmed durable as uncertain, enter FAILED, acknowledge nothing further as durable, and do not retry that descriptor and treat the retry as success. The hub retries `open` with backoff; recovery decides which uncertain records are valid. Nothing is resubmitted silently.

## 8. Batching and fallback log

- **Batching:** `fdatasync` after 32 records or 500 ms, whichever first **[MEASURE]**; `FORCE_SYNC` records sync immediately.
- **Fallback log:** supervisor-owned plain text, independent of `oc_store` code. One line per event, appended with `O_APPEND`, `fdatasync`ed per line, rate-limited to 10 lines/s with a suppressed-count line, rotated at 4 MiB across at most 4 files. If writing fails the line goes to stderr. Lines carry the same provenance fields as store records, in text.

## 9. M3 live-capture procedure

1. Start with both readers in LIVE mode; start live-capture mode for 60 s (target; maximum 10 min).
2. The hub persists every received sample from both readers, including non-OK statuses, into the trace stream via `oc_store`, then flushes and syncs.
3. Read back through `read`. For each source check: stored count equals received count; all `seq` gaps are accounted for by drop counters; timestamps are non-decreasing; origin is LIVE.
4. `kill -9` the hub, restart it, confirm `STORE_RECOVER` is reported and repeat the read-back check.
5. Copy the raw files off with a checksum; never edit them. SIMULATED or REPLAYED runs may exercise the path but are labeled as such and cannot satisfy M3.

## 10. Open low-level review items

`oc_sample` byte layout, enum values and envelope; `SOCK_SEQPACKET` and single-executable launch; reconnect backoff values; restart bounds and the supervisor's own restarter; fallback-log rotation; API signatures and `tick` cadence; batching parameters; on-disk layout and segment sizes; evidence and trace caps; build integration (test registration, POSIX feature macros).
