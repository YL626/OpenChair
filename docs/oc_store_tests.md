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
