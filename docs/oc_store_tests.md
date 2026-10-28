# oc_store test suite and open storage decisions

Owner: Kacper (Mechanism D, DESIGN.md §7). Everything here is **[REVIEW]** until the `oc_store` interface is reviewed by Alan as its consumer. `DESIGN.md` is authoritative. This file maps the tests to the M2 storage task. The on-disk format, sync policy, recovery rules and the open decisions are in [`docs/oc_store.md`](oc_store.md).

## Status

Tests 1–9 were written **before** the implementation (fail-first). Against the original `-ENOSYS` stubs every group failed. With `src/oc_store.c`, `src/oc_store_format.c` and `src/crc32.c` implemented, all three binaries pass. They run as part of `make test`, `make asan` and `make memcheck`, and on their own with `make test-store`, `make asan-store` and `make memcheck-store`.

Two tests in group 7, `test_fault_sync_at_recovery` and `test_fault_sync_at_seal`, were added **after** the implementation. A mutation check found them missing: deleting either sync left every other test green. With the sync deleted they fail; with the code as committed they pass.

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
| 7 | Write failures | `tests/test_store_faults.c` | `test_fault_sync_eio_batched`, `test_fault_sync_eio_force_sync` (failed sync → FAILED, records uncertain, `durable_seq` not advanced, never success again on that handle, recovery keeps an intact prefix containing every acknowledged record), `test_fault_write_enospc`, `test_fault_short_write_and_eintr` (must be retried, data intact), `test_fault_dir_fsync` (at open and at segment rollover), `test_fault_real_efbig` and `test_fault_real_eacces` (real kernel errors, no mocking), `test_fault_sync_at_recovery` (open fails if recovery's sync fails), `test_fault_sync_at_seal` (a failed footer sync at close is reported), `test_no_fd_leak`; every test also checks its descriptor count is unchanged |
| 8 | Process-kill tests (and what they do and do not prove) | `tests/test_store_kill.c` | `test_kill_force_sync` (20 trials), `test_kill_batched` (10 trials), `test_kill_repeated_same_store` (8 crash/restart rounds on one store) |
| 9 | Bounded storage (open decision 10) | `tests/test_store.c` | `test_trace_ring_rotation` (trace never exceeds `max_segments` files, keeps a contiguous suffix, reports no gap for rotated-out records, rotates across a reopen), `test_evidence_cap` (past the cap: `-EDQUOT`, no seq consumed, store stays OK, trace unaffected, nothing deleted, cap holds after reopen) |

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

**Process-kill tests (test 8).**

- *How they work:*
  - A forked child writes records and reports each acknowledgement over a pipe: a FORCE_SYNC append that returned 0, or a `durable_seq` from `oc_store_sync`.
  - The parent sends `SIGKILL` after a seeded-random number of acks (0–60) plus a 0–2 ms delay. The kill can therefore land in open/recovery, mid-batch, mid-sync or at a segment rollover; 80 KiB segments make rollovers frequent.
  - The parent then reopens the store and checks that every acknowledged record is present and byte-exact, there are no corrupt or duplicate records, no segment is reported damaged, and recovery is idempotent.
  - The seed is fixed (`0x0C5701E5`) and printed, so failures reproduce.
- *They prove:* recovery is consistent when the writer process dies at an arbitrary point, and acknowledged records survive process death.
- *They do not prove:*
  - power-loss durability: `SIGKILL` ends the process, not the kernel, so every completed `write()` still reaches the file through the page cache, and these tests would pass even with no fsync at all;
  - that the SD card or ext4 honour flush;
  - torn or reordered sector writes (covered instead by the truncation and bit-flip tests);
  - the DESIGN.md §6 target of ≥ 1,000 trials, which belongs to the evaluation.
- *Evidence cap:* children write until the kill lands, so over 8 rounds they would reach the default 16-segment evidence cap and exit 3 ("append failed"). The trials therefore set `max_segments = 100000`. The cap itself is tested in group 9. A child that still hits it fails the trial loudly.
- *Stability:* 20 of 20 repeated runs of `test_store_kill` passed (Fedora, GCC 16.2.1, tmpfs `/tmp`). The whole suite also passes with `TMPDIR` on a btrfs disk, where `fdatasync` is real (about 33 s instead of 3 s). Not yet run on ext4 or the Pi's SD card.

## Format, sync policy and open decisions

These moved to [`docs/oc_store.md`](oc_store.md) when the implementation landed. The layout facts listed above, which the corruption tests rely on, still hold.
