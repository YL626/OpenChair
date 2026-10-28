# oc_store test suite and open storage decisions

Owner: Kacper (Mechanism D, DESIGN.md §7). Everything here is **[REVIEW]** until the `oc_store` interface is reviewed by Alan as its consumer. `DESIGN.md` is authoritative. This file maps the tests to the M2 storage task and records the decisions the tests depend on.

## Status

The tests were written **before** the implementation. `src/oc_store.c` and `src/crc32.c` are stubs: every store call returns `-ENOSYS` and the CRC returns 0. So `make test-store`, `make asan-store` and `make memcheck-store` are **expected to fail** until the implementation lands. `make test` (the smoke test) is unaffected. Once the store tests pass they move into `make test`.

All test input is synthetic and generated deterministically by `tests/test_util.h`. No test result here is evidence of hardware behaviour.

## Test map

| # | Task bullet | File | Test functions |
|---|---|---|---|
| 1 | Write and read back valid records | `tests/test_store.c` | `test_crc32_known_answers`, `test_open_fresh_dir`, `test_roundtrip_both_streams`, `test_force_sync_vs_buffered`, `test_batching_policy` |
| 2 | Detect truncated records | `tests/test_store.c` | `test_truncation_sweep_tail` (every byte offset across the last two records), `test_truncation_sweep_head` (every offset inside the header and first record), `test_truncation_then_append` |
| 3 | Detect corrupted record payloads | `tests/test_store.c` | `test_corrupt_payload_marker` (payload found by content), `test_corrupt_every_byte_of_record` (one bit flipped at every byte of a record: frame header, provenance, payload, CRC), `test_corrupt_first_and_last_record` |
| 4 | Recover the valid prefix without fabricating records | `tests/test_store.c` | `test_prefix_trailing_garbage` (random and zero-filled tails), `test_prefix_replayed_frame` (byte-exact valid frames with repeated seqs), `test_prefix_missing_middle_record` (valid frames after a seq gap in the newest segment), `test_prefix_append_after_recovery` |
| 5 | Recovery across multiple segments | `tests/test_store.c` | `test_multiseg_roundtrip` (≥3 segments, contiguous seq), `test_multiseg_torn_newest`, `test_multiseg_damaged_sealed` (older segment kept byte-identical and marked damaged), `test_multiseg_bad_newest_header` (newest segment removed), `test_multiseg_missing_middle` (gap reported, nothing invented) |
| 6 | Invalid inputs | `tests/test_store.c` | `test_invalid_open` (bad cfg → `-EINVAL` with nothing created; missing dir → `-ENOENT`; file as dir → `-ENOTDIR`), `test_invalid_append` (every bad argument → `-EINVAL`, no seq consumed, state unchanged; empty and maximum payloads accepted), `test_invalid_other_calls` (NULL handles, bad stream, NULL visitor, read past end, visitor stop value propagated) |
| 7 | Write failures | `tests/test_store_faults.c` | `test_fault_sync_eio_batched`, `test_fault_sync_eio_force_sync` (failed sync → FAILED, records uncertain, `durable_seq` not advanced, never success again on that handle, recovery keeps an intact prefix containing every acknowledged record), `test_fault_write_enospc`, `test_fault_short_write_and_eintr` (must be retried, data intact), `test_fault_dir_fsync` (at open and at segment rollover), `test_fault_real_efbig` and `test_fault_real_eacces` (real kernel errors, no mocking), `test_no_fd_leak`; every test also checks its descriptor count is unchanged |

**Fault-injection seam (test 7).**

- *How it works:* the faults binary is linked with `-Wl,--wrap=` for `write`, `pwrite`, `writev`, `pwritev`, `fsync` and `fdatasync`. `oc_store`'s calls go through `__wrap_*` functions, which pass through unless a test arms a rule. Rules can target regular files or directories, and each armed test checks that its rule actually fired, so an implementation that writes another way fails instead of passing vacuously.
- *Verified:* a probe confirmed the redirection works for calls from a separately compiled object, in both plain and ASan builds.
- *Caveats:*
  - On a 32-bit build with `_FILE_OFFSET_BITS=64`, glibc renames `pwrite`/`pwritev` to `*64`, so the wrap list would need those names.
  - Under `-std=c23`, the implementation needs `_POSIX_C_SOURCE 200809L` (or `_GNU_SOURCE`) to declare `fdatasync`.
- *Limits:* it can't reproduce real SD-card I/O errors or the kernel's handling of dirty pages after a genuine failed fsync. It checks only that `oc_store` reports failures honestly and never acknowledges durability it didn't get.

**Layout facts the corruption tests assume:**

- segments are named `<dir>/{evidence,trace}/NNNNNNNN.seg`;
- payloads are stored as raw bytes;
- a segment file is not preallocated, so its size after a sync is the end offset of the last record. This is how the tests find record boundaries without knowing the frame format.

## Why three test files

The tests are split by **what they need from the harness**, not one file per case:

- `test_store.c` holds the functional tests, which link normally.
- `test_store_faults.c` injects write/sync failures with `-Wl,--wrap=...`. That link flag applies to the whole binary, so this suite must be its own binary.
- `test_store_kill.c` runs fork + `SIGKILL` crash trials. They're slow and timing-dependent, and they come with their own statement of what they prove.

## Proposed on-disk format [REVIEW]

All integers are little-endian. Encoding uses explicit byte functions, never struct `memcpy`.

- **Directory:** `<dir>/evidence/` and `<dir>/trace/`. Segment files are named `NNNNNNNN.seg` (8-digit decimal segment id).
- **Segment header, 32 B:** magic `"OCSG"`, format version u16, stream u8, reserved u8, segment id u32, first record seq u64, reserved (zero) u32×2, header CRC32 u32.
- **Record frame:**
  - a 16 B frame header: magic u16 `0x5243` ("CR"), type u8, version u8, payload length u32, record seq u64;
  - a 16 B provenance block: kind u8, origin u8, emitter u8, flags u8, instance u32, session_id u64;
  - the payload body;
  - CRC32 u32 over the frame header + provenance + payload.

  The payload is stored raw, which the tests rely on.
- **Footer** (when sealed): magic `"OCFT"`, record count u32, last seq u64, CRC32 u32.
- **CRC:** CRC-32/IEEE (reflected `0xEDB88320`, init and xorout `0xFFFFFFFF`), the zlib CRC. Check value: `"123456789"` → `0xCBF43926`.
- **Max payload:** `OC_STORE_MAX_PAYLOAD` = 64 KiB.

## Sync policy (from DESIGN.md §4, Notes §5/§8)

- `append` only buffers the record. `OC_STORE_FORCE_SYNC` writes and `fdatasync`s it before returning.
- `tick` syncs a stream once 32 records are pending, or 500 ms after it first saw pending records.
- To open a new segment: write the header, `fdatasync` it, `fsync` the directory, then append.
- To seal a segment: write the footer and sync it.
- Deleting a segment is followed by a directory `fsync`.

## Open decisions for partner review

The tests are written so that they don't silently settle these. Where a test does depend on one, this list says so.

1. **First seq is 0** for each stream, and seqs are **per stream**. Tests 1+ depend on this.
2. **Errors are negative errno.** Invalid arguments return `-EINVAL` and never change state. `oc_store_sync` on an empty stream returns `-ENODATA`.
3. **Batch timer** starts at the first `tick` that sees pending records (tick time is supplied by the hub). `test_batching_policy` depends on this.
4. **Seq after torn-tail truncation**: should lost seqs be reused or skipped? Tests only assert that the next seq is greater than the last kept seq.
5. **Does `read` see buffered/unsynced records?** Tests only read after a sync.
6. **`read` inside a damaged sealed segment**: should it return the valid records before the damage? Tests only assert that no corrupt record is returned and that other segments are complete.
7. **`close` writing a shutdown record**: tests tolerate extra records after the last user record.
8. **Fault-injection seam**: link-time `--wrap` (no production API change) vs an I/O hook table in `oc_store_cfg`.
9. **INJECTED as a provenance flag** (`OC_PROV_INJECTED`) rather than a field only in EVENT payloads.
10. **Evidence cap / trace ring rotation**: not yet tested; planned for a follow-up commit.
