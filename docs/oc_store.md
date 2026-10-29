# oc_store: storage specification (Mechanism D)

Owner: Kacper (DESIGN.md §7). `DESIGN.md` §4 is authoritative; this file records how `oc_store` implements it. Everything here is **[REVIEW]** until Alan, as the consumer, has reviewed it. The API is in `include/oc_store.h`, the implementation in `src/oc_store.c` and `src/oc_store_format.c`, and the tests in `docs/oc_store_tests.md`.

**Status:** implemented, and every test passes under plain, ASan/UBSan and Valgrind builds. All of this is **software-only evidence with synthetic input**, on a Fedora laptop (GCC 16.2.1). The suite was run on tmpfs, where `fdatasync` is a no-op, and once on btrfs with `TMPDIR` on disk, where it is real. It has not been run on ext4, the target filesystem. Nothing here has been verified on the Raspberry Pi, the SD card, or a real power cut.

## 1. On-disk format

All integers are little-endian and are encoded byte by byte, never by copying a struct.

- **Directory:** `<dir>/evidence/` and `<dir>/trace/`. Segment files are named `NNNNNNNN.seg` (8-digit decimal id). Recovery ignores any other name. A segment file is never preallocated.

**Segment header (32 B).** The CRC lets recovery tell a valid header from a torn one.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"OCSG"` |
| 4 | 2 | format version = 1 |
| 6 | 1 | stream (0 evidence, 1 trace) |
| 7 | 1 | reserved = 0 |
| 8 | 4 | segment id (must match the file name) |
| 12 | 8 | seq of the segment's first record |
| 20 | 8 | reserved = 0 |
| 28 | 4 | CRC32 of bytes 0..27 |

**Record frame (36 B + payload).** The CRC covers every byte before it.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | magic `0x5243` |
| 2 | 1 | type (EVENT 1, HEARTBEAT 2, RESULT 3, SAMPLES 4) |
| 3 | 1 | frame version = 1 |
| 4 | 4 | payload length (≤ `OC_STORE_MAX_PAYLOAD` = 64 KiB) |
| 8 | 8 | record seq |
| 16 | 1 | provenance: kind |
| 17 | 1 | provenance: origin |
| 18 | 1 | provenance: emitter |
| 19 | 1 | provenance: flags (only `OC_PROV_INJECTED` is defined) |
| 20 | 4 | provenance: instance |
| 24 | 8 | provenance: session_id |
| 32 | len | payload, raw bytes |
| 32+len | 4 | CRC32 |

**Footer (20 B).** Written when a segment is sealed, and only if it holds at least one record.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `"OCFT"` |
| 4 | 4 | record count |
| 8 | 8 | last record seq |
| 16 | 4 | CRC32 of bytes 0..15 |

The footer is what detects an older segment that lost whole records at a record boundary. Every remaining frame's CRC would still pass, but the count would not match.

**CRC:** CRC-32/IEEE, the zlib CRC: reflected polynomial `0xEDB88320`, init and xorout `0xFFFFFFFF`. Check value: `"123456789"` → `0xCBF43926`. It detects random damage; it is not a security check.

**Seq:** record seqs are per stream, start at 0, and are contiguous within a segment and across segments.

**Segment size:** `segment_bytes` must be between 65,624 B (header + one maximum frame + footer) and 1 GiB. A frame goes into the active segment only if the footer still fits after it.

## 2. Write path and sync policy

| Step | What happens |
|---|---|
| `append` | Validates every argument, then copies the frame into a 256 KiB per-stream buffer. The record is *buffered*. If the buffer would overflow, it is written out first, which bounds memory. |
| `FORCE_SYNC` | Writes the buffer, then `fdatasync`. The record and everything appended before it are *durable* when `append` returns 0. |
| `tick(now)` | Per stream: the batch timer starts at the first tick that sees pending records. Sync when pending ≥ `batch_records` (32) or `now − start ≥ batch_ms` (500 ms). |
| `sync` | Writes and `fdatasync`s; returns `durable_seq`. |
| Rollover | When the next frame does not fit, `append` itself does the following before buffering the record: (1) write pending records and the footer, then `fdatasync`. This seals the segment, and its records become durable. (2) Create the next segment with `O_EXCL`, write its header, `fdatasync`. (3) `fsync` the directory. |
| `close` | Writes, seals (footer + `fdatasync`) and closes. It writes no implicit record. |

Writes use `pwrite` at an explicit offset, continuing after short writes and retrying `EINTR`. `write()` returning is never treated as durable.

The ordering guarantees one property: **if a segment exists, every older segment is complete and sealed.** Only the newest segment can hold a torn write.

## 3. Bounded storage

- **Trace** is a ring of `max_segments` files. At rollover the oldest segments are unlinked until the new one fits. The directory `fsync` done for the new segment also covers the unlinks. Rotated-out records are not counted as gaps.
- **Evidence** is never deleted. A rollover past `max_segments` is refused with **`-EDQUOT`** before any I/O. The record is not stored, no seq is consumed, the store stays OK and trace keeps working. The hub must log this to the fallback log. DESIGN.md treats reaching the cap as a soak-check failure. The defaults in DESIGN.md (16 × 4 MiB evidence, 16 × 2 MiB trace) are cfg values the hub passes in.

## 4. Recovery (`oc_store_open`)

Per stream, in this order:

1. **List** the `NNNNNNNN.seg` files and sort them by id.
2. **Newest segment with an invalid header** (short, bad CRC, wrong stream or id): it was never durably created, because headers are synced before any record goes in. Unlink it and count it in `removed_segments`. Repeat, since the next one is now the newest.
3. **Older segments** are opened read-only and never modified.
   - Scan records from offset 32. A record is accepted only if all of these hold:
     - the magic and version are right;
     - type, provenance enums and flags are valid;
     - the length is ≤ max and the frame lies wholly inside the file;
     - the seq is exactly the next expected one;
     - the CRC matches.
   - The scan stops at the first failure.
   - The segment must then end in a footer whose count and last seq match.
   - Otherwise it is **damaged**: counted in `damaged_segments`, left byte-identical, and its valid prefix is kept.
   - A segment whose first seq does not follow the records already kept is damaged and contributes nothing.
4. **Newest segment:** the same scan.
   - Everything after the last valid record is truncated. If those bytes are exactly a matching footer (a clean close), they are not counted; otherwise they are `torn_bytes`.
   - The segment is then `fdatasync`ed, so every record recovery keeps is durable when `open` returns.
   - It becomes the active segment, and the next append continues it.
5. **`seq_gaps`** counts the seqs missing between consecutive kept records, including across segments.
6. **`next_seq`** = the newest segment's first seq + its record count. Seqs lost in a torn tail are reused.
7. If no appendable segment remains, a new one is created.

**Guarantee.** Every record that was durable before the failure is present and intact. Later records are either kept whole or lost whole; none is accepted corrupt. A byte-exact copy of a valid frame is rejected by the seq check, so recovery never fabricates a record. This assumes the SD card honours flush and ext4 behaves as documented (DESIGN.md §4).

## 5. Failure semantics

Any failed `pwrite`, `fdatasync`, `fsync`, `unlink` or `close` after `open` has returned does all of the following. During `open` itself, a failure makes `open` return the error with nothing allocated:

- the state becomes `OC_STORE_FAILED`, and `last_errno` keeps the first error;
- buffered and written records of both streams become *uncertain*;
- the pending buffer is discarded;
- `durable_seq` never advances again;
- `append`, `tick` and `sync` return `-EBADFD`.

`status` and `read` still work. `close` does no I/O and returns the failure. The failed descriptor is never retried, because after a failed `fsync` Linux may mark the dirty pages clean, and a retry could report success for data that was never written. The hub reopens with backoff, and recovery decides what survived.

**Known limit.** Recovery's `fdatasync` uses a new descriptor. If a previous run's failed write-back left pages marked clean but unwritten, the new descriptor does not see that error. Recovery can then keep records that are in the page cache but not on the card. Only the planned power-cut experiments (DESIGN.md §6) can probe this. It is not tested here.

## 6. Decisions for partner review

The tests depend on the first three (from `docs/oc_store_tests.md`). The rest were made during implementation and are proposed, not agreed.

| # | Decision | Proposal |
|---|---|---|
| 1 | First seq | 0, per stream |
| 2 | Errors | Negative errno; `-EINVAL` never changes state; `sync` on an empty stream returns `-ENODATA` |
| 3 | Batch timer | Starts at the first `tick` that sees pending records |
| 4 | Seq after a torn tail | **Reused.** Those records were never acknowledged, and reuse keeps a "gap" meaning real loss of kept data |
| 5 | `read` and buffered records | Not visible. Written records are, including uncertain ones after a failure |
| 6 | Damaged sealed segment | Its valid prefix is returned, and the rest of that segment is a reported gap |
| 7 | Shutdown record at `close` | None. The hub writes its own shutdown event |
| 8 | Fault-injection seam | Link-time `--wrap`, with no production API change. Still open |
| 9 | `INJECTED` | A provenance flag. Still open |
| 10 | Bounded storage | Trace ring; evidence refuses with `-EDQUOT` at the cap (chosen by Kacper; needs Alan's agreement and a DESIGN.md line) |
| 11 | After FAILED | `-EBADFD` from append/tick/sync |
| 12 | Rollover cost | An `append` that rolls over performs one seal sync plus a header sync and a directory sync. This is the "block no longer than one sync" item in Notes §6 and needs Alan's acceptance; it happens once per segment |
| 13 | Recovery sync | `open` `fdatasync`s the newest segment, so recovered records count as durable (`durable_seq` includes them) |
| 14 | Clean-shutdown flag | Not in the report. Recovery can tell (a newest segment ending in a matching footer), but exposing it would change the interface |
