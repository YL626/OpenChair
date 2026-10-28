# OpenChair Implementation Notes

Supplement to `DESIGN.md`. `DESIGN.md` is authoritative; if the two disagree, `DESIGN.md` wins and this file is corrected. These notes hold low-level detail removed from the design document. Everything here is **[REVIEW]**: the architecture is team-agreed, but byte-level and API-level details await technical review by the owning and consuming partner. **Implementation status.** Host-tested on Linux with SIMULATED inputs: the sample codec (S1), the ADS1115 light reader (S2, against a fake I2C bus), the IPC control codec and transport (S3A), and the hub receiver and per-source state machine (S3B). Not implemented: the VL6180X reader, the supervisor and control channel, storage integration, the optical analysis, the UI, and the assembled application. Nothing here has been measured or verified on the Raspberry Pi hardware; passing host tests shows logic only. **[MEASURE]** marks values that need hardware data.

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

**Control messages (S3A, `include/oc_ipc_msg.h`).** Same envelope (version 1, type, payload length u16 LE) and the same explicit little-endian encoding. Each type has exactly one legal payload length and a datagram must be exactly `4 + payload_len` bytes. Every reserved byte must be 0. The largest defined datagram is 28 B (SAMPLE).

| Type | Payload | Layout (offset: field) |
|---|---|---|
| HELLO (1) | 20 B | 0: `source` u8 (1 LIGHT, 2 DISTANCE); 1: `origin` u8 (1 LIVE, 2 SIMULATED, 3 REPLAYED); 2-3: reserved; 4: `session_id` u64 (nonzero); 12: `instance` u32; 16: `pid` u32 (declared) |
| HELLO_ACK (2) | 16 B | 0: `session_id` u64 (echo); 8: `instance` u32 (echo); 12: `source` u8 (echo); 13-15: reserved |
| HELLO_REJECT (3) | 8 B | 0: `reason` u16; 2-3: reserved; 4: `detail` u32 (0 for now) |
| BYE (5) | 4 B | 0: `reason` u16 (1 NORMAL, 2 SHUTDOWN, 3 ERROR); 2-3: reserved |

REJECT reasons: 1 MALFORMED, 2 BAD_SOURCE, 3 BAD_ORIGIN, 4 SESSION_MISMATCH, 5 PEER_CRED, 6 DUPLICATE_SOURCE, 7 STALE_INSTANCE, 8 NOT_READY. Values 6 and 7 are defined on the wire but need hub state, so they are first produced by the S3B hub, not by the stateless check. The existing `oc_msg_header_decode` still accepts any length for these types; the exact lengths are enforced by `oc_ipc_msg_decode`, which is what the transport uses.

**Receive errors.** A datagram of the wrong length (`-EMSGSIZE`), unknown type, wrong payload length for its type, nonzero reserved byte or illegal enum (`-EBADMSG`) is consumed whole and counted; the connection stays synchronized and usable. An unsupported version (`-EPROTONOSUPPORT`, checked first and for any size) is connection-fatal: the receiver closes without a reply.

## 2. Session and instance handling

- The supervisor draws a random 64-bit `session_id` with `getrandom` at startup and gives each spawned child an `instance` number from a per-session counter, passed on the command line.
- The child sends both in HELLO, with its source, input mode (`origin`: LIVE, SIMULATED or REPLAYED) and its own pid; the protocol version is in the envelope. The hub rejects any sample whose `origin` differs from the declared mode (checked per connection in S3B).
- The stateless HELLO check (`oc_ipc_hello_check`) compares, in this order: peer uid and, when enabled, the declared pid against `SO_PEERCRED` (PEER_CRED); `session_id` (SESSION_MISMATCH); the source is known and permitted (BAD_SOURCE); the origin equals the one expected for that source (BAD_ORIGIN). The expected origin is configured **per source**, so a development session may mix origins (for example LIVE light with SIMULATED distance). A mixed-origin session cannot count as M3's two-LIVE-sensor evidence. `SO_PEERCRED` shows which local process connected; it does not show that the data is a genuine sensor measurement.
- `session_id` 0 is reserved and rejected. `instance` 0 is a legal value (the per-session counter may start at 0). Whether an instance is stale, a duplicate or a restart is decided by the hub from its per-source state (S3B).
- A sample is identified by `(session_id, source, instance, seq)`.
- A new `instance` for a source means the reader restarted. The hub resets that source's filter and baseline state, abandons any in-progress capture, and records the discontinuity.
- A `seq` gap within one instance means loss, including samples dropped while the hub was away. The hub counts the gap and rejects a capture that spans it.

## 3. IPC connection lifecycle

1. The hub's runtime directory (mode 0700, created by the caller) must be owned by the effective uid with no group/other write bit. `oc_ipc_server_open` creates a `SOCK_SEQPACKET|SOCK_CLOEXEC|SOCK_NONBLOCK` listening socket there (chmod 0600) and records the socket file's device and inode. An existing path is only replaced if it is a socket owned by us with no listener (a connect probe returns `ECONNREFUSED`); a live server gives `-EADDRINUSE`; any other file type, including a symlink, is refused and never unlinked. On close the path is unlinked only if it is still that same socket file. The supervisor runs one hub at a time.
2. A reader connects (nonblocking; a full backlog gives `-EAGAIN`, an in-progress connect is completed with `poll` and `getsockopt(SO_ERROR)`, and the peer's uid must be ours) and sends HELLO. The hub checks the version, framing, `SO_PEERCRED`, session, source, origin and instance, then replies HELLO_ACK or HELLO_REJECT with a reason code. The reader's `oc_ipc_client_handshake` applies one `CLOCK_MONOTONIC` deadline to completing the connect, sending HELLO and receiving the reply, succeeds only for a HELLO_ACK that echoes its `session_id`, `instance` and `source`, reports a REJECT with its reason, and closes the connection on any failure.
3. The reader sends SAMPLE messages with `MSG_DONTWAIT|MSG_NOSIGNAL`. If the socket is full or disconnected, it drops the sample, increments `tx_dropped` exactly once, and keeps its I2C loop on schedule. There is no userspace queue. The bound on unread data is the sender's `SO_SNDBUF` (requested 16 KiB, so 32 KiB effective [MEASURE]): the kernel refuses a send once it is full, so at most `SO_SNDBUF / 28 + 1` samples are ever queued (the real count is far lower because of per-datagram kernel overhead); the library does not rely on `net.unix.max_dgram_qlen` for the bound, though a smaller sysctl could lower the capacity further. Control messages are never dropped silently: a full buffer returns `-EAGAIN` to the caller. On the development kernel (Linux 6.18, WSL2) 43 samples were accepted before `-EAGAIN`.
4. On EOF or `ECONNRESET` the reader reconnects with backoff (100 ms doubling to 2 s) and sends a new HELLO with the same instance. `seq` keeps counting, so the gap is visible to the hub.
5. When a reader exits or its connection closes, the hub marks that source disconnected and the system degraded. The supervisor restarts the reader with a new instance; the new connection resets the source (section 2).
6. When the hub restarts, readers see EOF and follow step 4.

### Hub rules (S3B, `include/oc_hub.h`, `include/oc_hub_seq.h`)

**Source states.** UNAUTHORIZED -> (authorize) -> AWAITING -> (valid HELLO, ACK sent) -> ACTIVE -> (EOF, reset, BYE, abuse, version error) -> DISCONNECTED -> (HELLO, same instance, ACK sent) -> ACTIVE. Authorizing a higher instance moves ACTIVE or DISCONNECTED to AWAITING and closes an active connection (SUPERSEDED). A source is **available** only in ACTIVE; the hub is **degraded** while any authorized source is not ACTIVE (a startup grace period is policy for S4/UI). A source becomes ACTIVE only after its HELLO_ACK was sent successfully; if that nonblocking send fails the connection is closed and the source, tracker and generation stay as they were.

**Authorization.** The supervisor runs in another process, so it will send the hub an authorization *command* over the control channel (not defined yet); the hub process then calls `oc_hub_authorize(source, instance)` locally. A HELLO is accepted only if its instance equals the authorized one (otherwise STALE_INSTANCE, whether lower or higher); an unauthorized source answers NOT_READY. Authorizing the same instance again is a no-op, a lower one than any authorized before is `-ESTALE`, instance 0 is valid. The tracker is reset only when the first HELLO of a new instance is ACKed (`generation` + 1); until then the old history stays readable.

**HELLO check order.** `oc_ipc_hello_check` (peer, session, source, origin), NOT_READY, STALE_INSTANCE, DUPLICATE_SOURCE, then ACK. Duplicate resolution is bounded: the incumbent connection is serviced only within its own remaining per-step budget; if it is then conclusively closed the newcomer is admitted, otherwise it is rejected with DUPLICATE_SOURCE and retries later. REJECT `detail` stays 0.

**Sequence accounting (per source and instance).** Sequence numbers count every sample *attempt*, so a failed acquisition arrives as a sample with a non-OK status and consumes its number; a sample the reader could not transmit leaves a gap. Comparisons use modular u32 distance `d = seq - (last + 1)`: `d = 0` in order; `0 < d < 2^31` GAP (accepted, `d` numbers added to `lost`); `seq == last` DUPLICATE; anything else behind REGRESSION. Rejected samples are counted and change nothing else. A first sample with a nonzero seq makes that many numbers `unobserved_prefix`, which is not confirmed loss (a restarted hub meets a running reader). A u64 `logical_last` extends the position across wraps so that `accepted + lost + unobserved_prefix == logical_last + 1` always holds.

**Timestamps.** For accepted samples only, a `t_mono_ns` smaller than the previous accepted sample's is flagged (`ts_backwards` in the callback, counted per instance); the sample is not altered or rejected and is not a strike. Equal values are not backward. The state resets with a new instance and survives a same-instance reconnect (`CLOCK_MONOTONIC` is system-wide). Analysis must reject any capture during which `lost`, `ts_backwards` or the generation changed.

**Abuse score.** Per connection: +1 for a malformed or oversized datagram, a sample whose source or origin differs from the HELLO, a duplicate or regressed seq, or a message type not allowed in the state; -1 (floor 0) per 64 valid datagrams; at 8 the hub sends BYE(ERROR) and closes. A gap, a non-OK status or a backward timestamp is never a strike. An unsupported version closes the connection at once with no reply.

**Scheduling.** Single-threaded `poll`. Each step reads at most `batch` datagrams per connection, starting at a rotating slot, so a noisy reader delays another by at most one batch; POLLHUP does not lift the budget, and for EOF/reset every queued datagram is read and delivered before SOURCE_DOWN (not for SUPERSEDED, SHUTDOWN, ABUSE, or data queued behind a BYE, which are discarded unread). ACTIVE means a valid connection, not that usable data is flowing: a reader restarted without a new instance has its samples rejected as regressions and is cut off after 8 strikes; `seq.regressions` and ABUSE closes are the evidence. A hub with no source authorized is not reported degraded. At most 4 connections wait for HELLO (each for `hello_timeout_ms`) and 2 are active; further connections are accepted and closed at once (shed). Callbacks run inside the hub's calls, may not call back into it (`-EDEADLK`), and are the only way samples leave: the hub keeps no queue and issues no diagnostic result.

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
