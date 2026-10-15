# NDFS Cross-Implementation Validation Plan

**Goal:** prove — with exhaustive, adversarial unit tests — that four NDFS
implementations behave **identically to RetroCore's NDFS**, which is the
known-good reference that produces files SINTRAN can actually open.

**Implementations under test**

| ID | Language | Location | Role |
|----|----------|----------|------|
| **RC** | C# | `RetroCore/Emulated.Utilities/ND/FileSystem` | **Golden reference** (authoritative; known-correct) |
| RFS | C# | `RetroFS/src/RetroFS.NDFS` | Must align with RC |
| C | C99 | `norskdata-ndfs/ndfs-c` | Must align with RC |
| TS | TypeScript | `norskdata-ndfs/ndfs-ts` | Must align with RC |
| PY | Python | `norskdata-ndfs/ndfs-py` | Must align with RC |

**Secondary oracles** (independent third implementations, for cross-checking
listings/dates only): Tor Arntsen's `ndfs` CLI (WSL `/usr/local/sbin/ndfs`,
source `~/repos/ndfs/ndfs-tool`), and a live **SINTRAN** under the nd100x
emulator (ultimate acceptance oracle).

**Non-negotiable testing principles**
1. RC is the source of truth. When an implementation disagrees with RC, the
   implementation is wrong — **fix the code, never weaken the test**.
2. Every behaviour is pinned by a test that would **fail** if the behaviour
   regressed (byte-level where on-disk format is involved).
3. For every "X works" test there is a paired **negative** test proving the
   illegal/edge variant **fails the right way** (correct error, no crash, no
   silent corruption).
4. Golden byte-vectors are generated from RC and committed as fixtures; all
   other implementations must reproduce them byte-for-byte (or a divergence is
   explicitly documented and justified).

---

## Phase 0 — Characterise the RetroCore reference (the "golden spec")

Produce an authoritative, byte-level specification of every NDFS operation by
reading RC's `NDDiskBlock`, `ObjectEntry`, `UserEntry`/`UserFile`,
`MasterBlock`, `BitFile`, `BlockPointer`, `ObjectFile` and the XAT/property-bag
serializer. Deliverable: `docs/NDFS-GOLDEN-SPEC.md` capturing, with exact
offsets/encodings:

- **ObjectEntry** (64 B): header word; name@2; type@18; next/prev version@22/24;
  access@26; file-type flags@28; device@30; file-type code@32;
  object index word@34 (`user<<8 | slot`); current/total open@36/38;
  dates@40/44/48; pages@52; bytes-1@56; file pointer@60.
- **Version semantics**: a single-version file has `next = prev = ownIndex`;
  document how additional versions chain and how SINTRAN renders `;N`.
- **Object index scheme**: `slot = indexBlockPtr*32 + entry`. **ANSWERED
  2026-08-01** - the old question here ("what happens at the 256th file per
  user") assumed 256 was a hard cap. It is not: the user gets a second *object
  block*. Creating file 257 must allocate a new block and raise ACOBL, and must
  only error once `ACOBL == MXOBL`. The file number is `block*256 + slot`, not
  the raw physical position.
- **UserEntry** (64 B): name@2; password@18; dates@20/24; reserved@28; used@32;
  dir index@36; user index@37; **default access@40**; **friends@48** (8×2);
  **byte 47 = MXOBL/ACOBL, two ZERO-BASED nibbles** (high = max object blocks
  minus 1, low = allocated minus 1; a default user stores `0x00` = 1 block =
  256 files). Verified against a live SINTRAN III K pack.
- **Object blocks** (new test area): block *n* for user *U* is at pages
  `n*512 + U*8 .. +7`. Tests worth having: byte-47 round trip for a
  multi-block user; a directory walk that finds entries in blocks 2+; the file
  number for a second-block file (real vector: `F0500` on the BIGMAN pack is
  `FILE 307`, owner 8 - a position-derived implementation reports index 18483
  and owner 72); create-past-256 allocating a block rather than failing; and
  create-past-MXOBL failing cleanly.
  See `NDInsight SINTRAN/XMSG/DOC/NDFS-OBJECT-BLOCKS-DECODED-2026-08-01.md`.
- **MasterBlock**/extended info, **BitFile** (contiguous), **BlockPointer**
  (type in bits 31:30, id in 29:0), allocation (contiguous/indexed/subindexed,
  sparse holes = id 0).
- **XAT** JSON schema: exact keys, value encodings, which fields round-trip.
- **Parity**: which file types get even parity (text) vs not (binary).
- **Defaults**: new file access `0x3FF`; new user default access `0x4FF`.

Also record **known RC quirks** so we don't blindly copy bugs (e.g. RC's
extended-info has no write-back; confirm whether that matters).

**Exit criteria:** golden spec reviewed; every field has an offset, size,
encoding, and a "set on create to" value.

---

## Phase 1 — Golden corpus & fixtures

1. **Real images**: curate a representative set from the local collection of ND disk images, kept outside this repository (floppy, SMD,
   Winchester; populated SYSTEM + multi-user; files with versions; sparse
   files). Record provenance.
2. **Synthetic golden images**: drive **RC** to generate images for each
   scenario in Phase 2/3/4 and dump them. Commit the images (or compact
   hex/base64 vectors for small ones) under `tests/fixtures/golden/`.
3. **Golden outputs**: for each fixture capture `ndfs -t/-u/-i`, full entry
   dumps, and XAT exports as expected-output text fixtures.
4. **Harness**: a small generator that, given a scenario id, yields the input
   ops + the golden image/bytes, consumed by all four test suites.

**Exit criteria:** committed corpus + a documented fixture format each language
can load.

---

## Phase 2 — Core parity matrix (positive scenarios)

Each scenario runs in **C, TS, PY, RFS** and is asserted against the **RC**
golden (byte-compare on-disk where applicable, plus semantic asserts).

| # | Scenario | Key assertions |
|---|----------|----------------|
| A | Create image (every template) | master block, extended info, bit file, empty object/user file byte-match golden |
| B | Add user | name, quota, index, default access `0x4FF`, empty friends, byte layout |
| C | Copy in a text file | name/type normalisation, user assignment, access `0x3FF`, flags=Indexed, **version self-ref**, **object index `user<<8|slot`**, pages/bytes, file pointer, **even parity applied** |
| D | Copy in a binary file | **parity NOT applied**; correct type code |
| E | Copy out (extract) | parity stripped; XAT sidecar contains every field with correct values |
| F | Delete file | object slot cleared, bitmap pages freed, quota restored, **gone after reload** |
| G | Rename | name/type updated, everything else preserved |
| H | Overwrite existing | data replaced; access/dates/version preserved |
| I | Multi-version | `ls` shows `;1`,`;2`…; version chain pointers correct vs RC/SINTRAN |
| J | XAT round-trip | export then import reproduces all metadata |
| K | Quota add/remove, password set/clear | values + byte layout |
| L | SINTRAN initial commands | locate via segment table + INIBU; **self-consistency guard** (reject coincidental parse); encode/parse round-trip (textLen 54/74); in-place `patch_file_region`; repair a header-corrupted buffer. C `test_sintran.c` / py `test_sintran.py` / ts `sintran.test.ts` all green. End-to-end vs a real pack validated in RFS; see [SINTRAN-INITIAL-COMMANDS-SPEC.md](SINTRAN-INITIAL-COMMANDS-SPEC.md) |

**Exit criteria:** every cell green in all four implementations against RC.

---

## Phase 3 — Negative tests (must-fail matrix)

For each, assert the operation fails with the **correct** error and leaves the
image **uncorrupted** (no crash, no partial write):

- Write to a read-only image.
- Write exceeding the user's quota / exceeding free pages.
- Add a 257th user; add a duplicate user name.
- Create a 257th file for one user (object index overflow > 255).
- Create a duplicate file (without `--overwrite`).
- Delete / stat / chmod a non-existent file.
- Illegal filename (lowercase normalised; illegal chars rejected; >16 name,
  >4 type truncated/rejected exactly as RC does).
- `chmod` with a malformed spec / out-of-range raw value.
- Open a truncated, misaligned, or non-NDFS image (e.g. a Sun disk) → clean
  error, never a wild read.
- Corrupted master block / bad pointers / bad checksum → detected.

**Exit criteria:** each failure mode has a test; behaviour matches RC (or, where
RC is silent, a deliberate, documented policy).

---

## Phase 4 — Edge cases

- 0-byte file (→ 1 page); 1-byte; exactly 2047/2048/2049 bytes.
- Page-boundary file sizes around index/subindex limits (511/512/513 pages →
  indexed↔subindexed transition).
- Sparse files (interior zero pages → block id 0, no allocation; reads back
  zeros).
- Names using the full 16 chars / types using the full 4 (terminator handling).
- User index 255; object slot 255.
- ND dates at the 1950 epoch (value 0) and the 2013 ceiling.
- Every access-rights combination per tier (R/W/A/C/D × OWN/FRIEND/PUBLIC).
- Parity round-trip stability (strip∘set == identity for valid even-parity).

---

## Phase 5 — Cross-framework interop matrix (N×N)

For every ordered pair (writer, reader) over {RC, RFS, C, TS, PY}:
1. **Image interop**: writer creates/modifies an image; reader opens it and
   verifies files, users, metadata, parity.
2. **Byte-identity**: the same scenario performed by each writer yields a
   byte-identical image (diff against RC; any difference must be explained and
   justified, not tolerated by default).
3. **XAT interop**: XAT written by writer is consumed by reader with full
   fidelity.

**Exit criteria:** the full 5×5 grid passes for a core scenario set; deviations
documented.

---

## Phase 6 — Golden byte-vector tests

Embed small RC-generated reference images as hex/base64 in each suite. Each
implementation must **reproduce the exact bytes** for the documented op
sequence. This catches silent encoding drift that semantic tests miss (it is
how we found the user `DefaultFileAccess`@40 and version-chain bugs).

---

## Phase 7 — Listing & version rendering

- `ls`/`-t` renders the version suffix (`;1`, `;2`) consistently with RC and
  SINTRAN, including multi-version files.
- `-u` user listing matches RC (quotas, default access).
- `-i` filesystem info matches (pages, pointers, boot format).
- Validate against real images that contain versioned files.

---

## Phase 8 — SINTRAN acceptance (end-to-end oracle)

The ultimate proof: under the nd100x emulator running SINTRAN, copy a file in
with **each** tool (C ndtool, RFS, and via TS/PY-produced images) and confirm
SINTRAN can `@LIST-FILES`, open, read, and shows the correct version. Automate
where feasible; otherwise a documented manual checklist per release.

---

## Phase 9 — CI integration

- Run C/TS/PY suites (and RFS `dotnet test` for the NDFS project) on every push
  via GitHub Actions; publish results.
- Gate merges on the parity + negative matrices.
- Nightly job runs the cross-framework byte-identity grid against the golden
  corpus.

---

## Phase 10 — Triage & fix loop

For each divergence the matrix surfaces:
1. Confirm RC's behaviour is correct (cross-check Tor `ndfs` / SINTRAN if
   ambiguous — as we did for `DefaultFileAccess`@40).
2. Fix the offending implementation(s) to match RC.
3. Add/keep the failing test as a permanent regression guard.
4. Never adjust a test to make broken code pass.

---

## Test infrastructure to build

- `tests/fixtures/golden/` — RC-generated images + expected dumps (shared).
- A **scenario catalogue** (one machine-readable list of ops + expectations)
  referenced by all four suites so coverage stays in lockstep.
- A **byte-diff helper** per language that pretty-prints the first differing
  offset against a golden vector.
- RC and RFS: xUnit/NUnit projects producing the golden vectors and asserting
  RFS parity. C: extend the in-repo framework. TS: vitest. PY: pytest.

## Status of issues found

### Fixed (with regression tests), verified against RetroCore / a real disk
- ✅ ObjectEntry fields 22–51 (versioning/access/flags/device/open/dates) —
  were dropped in C/TS/PY/RFS; fixed + parser + golden byte-vector tests.
- ✅ User `DefaultFileAccess`@40 / friends@48 — ports read @38/@40 (wrong);
  fixed (verified vs real image + `ndfs`).
- ✅ **Name field encoding** — wrote a field full of `0x27`; SINTRAN expects one
  terminator + NULs, so copied-in files were invisible in SINTRAN. Fixed in
  C/TS/PY (+ golden round-trip). Proven via `ndfs` on a real SMD image.
- ✅ **Per-user object-file partition** — files for non-SYSTEM users landed in
  SYSTEM's region (flat slot). Now `findFreeUserSlot` + `ensureObjectDirPage`
  place them at `user<<8|fileEntry` and grow the user's directory page on
  demand, in C/TS/PY. Proven: a file put to BUILD is listed under BUILD by
  `ndfs`. (Supersedes the old "object-file slot allocation on create" item.)
- ✅ New-file **version chain** + object index — were 0 (SINTRAN `;2`); now
  self-referential `user<<8|slot` in C/TS/PY.
- ✅ `--rm`/delete: dispatch (`-f`), slot clearing, and **whole-emptied page
  zeroing** — deleted files no longer reappear, in C/TS/PY.
- ✅ `--dest` honored with a bare name (C).
- ✅ **>512-page files** now use a real sub-indexed layout in C/PY (previously
  rejected outright — a genuine ~1MB write-size cap, not a stub). Ported from
  TS's already-working `allocateAndWriteData` (sub-index block → one group
  index block per 512-page group → up to 512 data pages each). Also fixed a
  related quota-accounting bug found while porting: `update_existing_file`/
  `_update_existing_file` used a flat "1 structural page" cost, which
  undercounts (and leaks) free space when overwriting a file that is itself
  sub-indexed — now scales with the number of group index blocks. New tests
  in C (`test_write_comprehensive.c`) and PY (`test_subindexed_files.py`)
  cover the 512/513-page boundary, multi-group round-trips, sparse holes
  inside a sub-indexed file, delete freeing every structural block, and
  overwrite not leaking the old structure. Supersedes the old "Sub-indexed
  file creation in C/PY" open item below.
- ✅ Object header word preserved on rewrite (used/modified bits).
- ✅ **Friend management** — list/add/remove a user's friends (the 8 x 2-byte
  RWACD entries at user-entry offset 48). Added `ndfs_list_friends` /
  `ndfs_add_friend` / `ndfs_remove_friend` (C) and the equivalents in py/ts,
  plus C `UserEntry` friend helpers + a permission-letters parser. ndtool
  gains `--friends` / `--friendadd OWNER:FRIEND[:RWACD]` / `--frienddel`
  and shell `friends`/`friendadd`/`frienddel`; `stat USER` now shows a user's
  details + friend list. Persisted surgically (owner's user page only).
  Owner/friend accept a name or index. Tests added in all three.
- ✅ **ndtool command rename** — quota commands are now `--quotaadd` /
  `--quotadel` (were `--addquota` / `--remquota`); friend commands use the
  same `<noun><add|del>` style. Hard rename (old quota names removed).
- ✅ **Empty file type defaulted to "DATA" on read** — `from_bytes`/`fromBytes`
  rewrote an intentionally-empty type field (e.g. `TERMINAL`: `27 00 00 00`)
  to `44 41 54 41` on the next write-back. Now the empty type is preserved
  verbatim, matching RetroCore (`ObjectEntry.FromBytes`). Fixed in C/TS/PY +
  golden empty-type round-trip tests.
- ✅ **`persist_all` rewrote the entire filesystem on every mutation** — each
  add/delete/rename/chmod/quota re-serialized *all* metadata, giving every
  write a filesystem-wide blast radius (and re-applying the empty-type bug to
  unrelated files). Replaced with **immediate, surgical per-block writes**
  matching RetroCore's `NDDiskBlock.WriteBlock` (write only the touched
  object/user/bitmap page). C/TS/PY + a combined-regression test (empty-type
  file survives an unrelated add-user).

### Verified NOT a bug (audit claim debunked against a real disk)
- ✓ **Bit-file bit ordering** — the flat byte layout (`block/8`, `bit%8`) is
  correct: every reserved/structural block reads `used` on a real SINTRAN
  disk. An audit suggested a 16-bit-word byte-swap; that would corrupt real
  disks. **No change** to `bit_file`.
- ✓ **User-entry slot mapping** — `page*32+slot` is correct: on a real 57-user
  disk, user index 32 sits at page 1/slot 0. RetroCore's `user-(user/32)*8`
  formula is the latent bug (impossible slot ≥32). **Ports unchanged.**

### Fixed (with regression tests), verified against RetroCore / a real disk (cont'd)
- ✅ **Master-block free count** (`unreserved_pages`@0x1C) now rewritten after
  every mutation that changes the bit file (create/update/delete file; RFS
  also covers `AddUser`'s on-demand user-file-page allocation), in C/TS/PY/RFS.
  Recomputed from the live bit file (`getFreePages()`/`get_free_pages()`/
  `ndfs_bf_count_free()`/`BitFile.GetFreePages()`) rather than tracked
  incrementally, so it can't drift out of sync with the bitmap. Confirmed the
  Extended Info Block checksum (0x07D0) is never touched by this write, in all
  four. Tests added in each port's write-persistence suite. Along the way,
  found a separate, still-open, lower-priority quirk: `image-creator`
  (TS)/`image_creator.c` (C) seed `unreserved_pages` with a template
  placeholder rather than the true free count at *creation* time — this fix
  only targets keeping it correct after mutations, not the initial value.
- ✅ **On-demand user-file page growth** — adding a user whose user-file page
  wasn't yet allocated silently no-op'd in `write_user_page`/`_write_user_page`/
  `writeUserPage` (page-not-linked-yet guard returned OK without allocating),
  so the user existed only in memory and vanished on next mount. RetroFS.NDFS
  already had this fixed (`EnsureUserFilePageAllocated`, predates this fix).
  Added `ensure_user_dir_page`/`_ensure_user_dir_page`/`ensureUserDirPage` to
  C/TS/PY, mirroring `ensureObjectDirPage`'s plain-Indexed case (the user file
  never needs Sub-Indexed — max 256 users fits in one 512-pointer index
  block). Called from `add_user`/`addUser`/`ndfs_add_user` before the write.
  Tests add 40 users (forcing a second page) and round-trip through a real
  close+reopen, not just in-memory state. c: 194/194 (+1). py: 351/5 (+1).
  ts: 307/0 (+1).
- ✅ **Sparse-file quota accounting** — `pages_used` (per-user quota) was
  charged the file's LOGICAL page count (counting sparse holes as if they
  consumed real disk space — they don't) plus the index/sub-index structural
  block(s) (filesystem overhead, not user data), in C/PY/TS. Also found a
  separate bug in the same area: the update-existing-file path never adjusted
  `pages_used` at all on overwrite, in all three plus RetroFS.NDFS. Fixed
  RetroFS.NDFS first (`CountRealDataPages`, commit `ff40eed`), then ported the
  same design to C/PY/TS: count only real (non-zero) data pages, scanning the
  write buffer directly for creates or walking the existing file's resolved
  block pointers for deletes/overwrites — never add structural-block cost.
  Along the way, ndfs-c's `ndfs_fsck` Phase 4 (quota verification) diagnostic
  needed the same real-page basis to avoid false warnings, and gained
  SubIndexed support it was missing (Phase 3, the bitmap-orphan check,
  still doesn't walk SubIndexed files — tracked separately below).
  New tests in each port cover fully-sparse (charges zero), mixed sparse
  (charges only real pages), fully-real (exact count, no index overhead),
  delete-refunds-exactly, grow-then-shrink overwrite, and a SubIndexed sparse
  file (charges only its few real pages despite needing several structural
  blocks). RFS: 148/152 (+6). c: 201/201 (+7). py: 357/5 (+6). ts: 313/0 (+6).
- ✅ **RFS C# audit** — checked whether RetroFS.NDFS needed the per-user
  partition, version chain, name-terminator, delete-clear, and >512 guard
  fixes already applied to c/py/ts. Verified against current code (2026-07-06,
  not assumed): all 5 are already correctly implemented there — per-user
  partitioning via `FindFreeUserSlot`/`EnsureObjectDirPage`, self-referential
  version chain on create, a single `0x27` terminator + NULs (not the old
  all-`0x27`-padding bug) in both `ObjectEntry`/`UserEntry`, whole-page
  zero-rebuild on delete (`ObjectFile.WritePageForEntry`), and the >512 guard
  via this session's own SubIndexed fix. Some landed in earlier sessions,
  the last in this one — this open item was stale, not actually outstanding.
  Added `NdfsHistoricalPortFixesAuditTests.cs` (5 tests) as a real regression
  guard proving all 5 together, rather than relying on the code read alone.
  RFS: 153/157 (+5).
- ✅ **Object-directory growth past 16,384 files** — the object file (the
  volume-wide directory listing every file across every user, distinct from
  a single file's own >512-page SubIndexed growth fixed earlier) was a plain
  Indexed structure capped at 512 directory pages: 16,384 files, or just 64
  users at 8 reserved index-pointer slots each, whichever bound first (the
  64-user ceiling binds first in practice, independent of files-per-user).
  Past that, `ensure_object_dir_page`/`_ensure_object_dir_page`/
  `ensureObjectDirPage`/`EnsureObjectDirPage` either returned failure (c),
  had no bounds check at all (py, ts — a latent out-of-bounds read/crash
  risk), or threw `NotSupportedException` (RFS). Fixed in all four with a
  one-time conversion mirroring how a single file's own data already grows
  past 512 pages: wrap the existing Indexed block as group 0 of a new
  SubIndexed structure (sub-index block -> up to 512 group index blocks ->
  up to 512 directory pages each). No data migration — the old block's
  pointers for pages 0-511 stay exactly where they are; only a new level of
  indirection is added above it. Fixed RFS first (`EnsureObjectDirPage`,
  commit `0c0089b`), then ported the same design to C/PY/TS (commit
  `f869e8a`). Two additional real bugs were found and fixed purely as a side
  effect of building the 30,000-file regression test: (1) RFS's
  `ReadIndexedObjectFileBlock` ignored its own `subindex` parameter, which
  would have produced colliding `ObjectIndex`/ownership values for every
  group beyond the first; (2) RFS's `ObjectFile.WritePageForEntry` had zero
  SubIndexed awareness at all, throwing `ArgumentOutOfRangeException` for
  any write past the first group; (3) ndfs-c's custom-image layout placed
  the object/user file index blocks at a fixed offset sized for only a
  1-page BitFile bitmap — large custom images (needed to provision a
  30,000-file stress image) have a multi-page bitmap whose later pages
  silently overlapped and corrupted the object directory, fixed by computing
  the real bitmap page span. Each of C/PY/TS uses `PointerType.Contiguous`
  (not `Indexed`) for the embedded group-pointer slots inside index/
  sub-index pages — verified against each read side directly (not assumed):
  this is the pre-existing convention for all embedded slot pointers in
  those ports (they gate on `.isValid()`/blockId, not stored type), so it is
  consistent, not a divergence. Verified with a real 30,000-file/120-user
  regression test (spread across enough users to force 2 SubIndexed groups
  and cross the old 64-user ceiling), round-tripped through a genuine
  close+reopen, in all four. RFS: 156/160 (+3, commit `0c0089b`). c: 204/204
  (+3, commit `f869e8a`). py: 360 passed/5 skipped (+3, commit `f869e8a`).
  ts: 316/316 (+3, commit `f869e8a`).

### Open (tracked, lower priority)
- ☐ **Image-creation-time `unreserved_pages` placeholder** — `image-creator.ts`
  and `image_creator.c` seed the master block's free-page count with a
  template constant (e.g. `1` for floppy templates) rather than computing the
  true free-page count at creation time. First mutation self-corrects it (see
  the fix above), so this only matters for a freshly-created, never-mutated
  image read by something that trusts the field verbatim. C/TS (PY/RFS not
  yet checked).
- ☐ **ndfs-c `ndfs_fsck` Phase 3 (bitmap-orphan check) doesn't walk SubIndexed
  files** — found while fixing the sparse-file quota item below; Phase 4
  (quota verification) was fixed to handle SubIndexed, Phase 3 wasn't (has its
  own pre-existing `/* SubIndexed: would need deeper walk, skip for now */`
  comment). Diagnostic-only gap, not a correctness bug in read/write/allocate.
- ☐ **RetroCore** `UserFile.cs:240` `*8` user-slot formula is latently buggy
  for ≥32 users (report upstream; ports already correct).
- ☐ Real ND creation **date** on new files (currently 0 → `1950-01-01`); needs
  an ND-timestamp encoder in libndfs.
- ☐ Multi-version create/read — **VERY LOW PRIORITY** (single-version `;1` is
  exact; chained-version ordinal best-effort/unvalidated).
