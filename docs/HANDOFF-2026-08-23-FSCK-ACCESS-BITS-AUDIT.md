# Handoff: fsck fix, access_bits port, cross-library API audit

**Status:** Done and verified locally. **NOT pushed to origin/main** — 4 commits ahead.
**Session:** 2026-08-21 through 2026-08-23.
**Scope:** `ndfs-c`, `ndfs-py`, `ndfs-ts` (all three ports touched).

All line/commit references below were accurate as of commit `f32b9c7`. Re-verify with
`git log`/`git status` before acting on this document — do not assume it is still current.

---

## 1. What happened, in order

1. Verified `git pull` was current and that all three ports + `ndtool` agreed on version
   `0.0.6`. `ndfs-c/CMakeLists.txt` was still stuck at `0.0.5` — fixed.
2. Analyzed and committed three pre-existing **uncommitted** fixes that were already sitting in
   the working tree at session start: sparse holes must be declared (never inferred from an
   all-zero page), a new file inherits the owning user's default file access, and
   `find_object`/`findObject` must match by TYPE, not just NAME (py/ts only — `ndfs-c` already
   did this correctly). Plus a `ndfs-py`-only `bit_file.py` performance fix (scan-cursor +
   cached used-page count) that the sparse-holes fix made necessary.
3. Updated `CHANGELOG.md`, tagged and pushed `v0.0.6` (GitHub Actions release build).
   **Note:** this tag predates steps 4-8 below — the published `v0.0.6` release does NOT include
   the fsck fix or the `access_bits` work. A future release will need a new tag.
4. Found, analyzed, and wrote regression tests for an already-fixed-but-untested `ndfs_fsck` bug
   (three distinct bugs: a contiguous file double-counting its own block as a phantom
   cross-link; whole classes of legitimate blocks — object/user-file data pages, bitmap pages
   past the first, anything behind a SubIndexed file — never counted, so they read as phantom
   orphans; and an Indexed file's real-page scan bounded by the wrong count, undercounting
   sparse files with real pages at high slots). New `ndfs-c/tests/test_fsck.c`, 13 tests,
   verified by temporarily stashing the fix and confirming the relevant tests fail against the
   old code. Commits `8ffa2d1`, `76233ea`.
5. Added an `access_bits` override parameter for distribution-media writes (measured on a real
   ND product floppy, `210580B01-XX-01D`: every file carries `0x07FF`, PUBLIC READ, not the
   `0x03FF` a live-machine user's inherited default would otherwise stamp). Landed in `ndfs-py`
   first (`c7b6100`), then ported to `ndfs-c` (new `ndfs_write_file_full`, `NDFS_ACCESS_PUBLIC_READ`)
   and `ndfs-ts` (`writeFile`'s new `accessBits` param, `ACCESS_PUBLIC_READ`) — `765e141`.
6. **Found a real bug while porting**: `ndfs-ts`'s image creator wrote `defaultFileAccess` to
   user-entry offset **+38** instead of **+40** (38-39 is an unused gap; the real field is at
   40, per `UserEntry.fromBytes`/`toBytes`). Every freshly-created TS image's SYSTEM user
   therefore had `defaultFileAccess` read back as `0`, so every new file silently fell through
   to the restrictive `0x03FF` default — the exact invisible-to-SYSTEM shape the `access_bits`
   feature above exists to work around. Fixed + tested. Commit `87c66da`.
7. Ran a full cross-library API audit: three parallel research agents exhaustively cataloged
   every public function in `ndfs-c`, `ndfs-py`, `ndfs-ts` with exact signatures and test
   coverage. Findings, ranked:
   - `ndfs_fsck` exists only in `ndfs-c`. **Ruled out of scope by Ronny — this is intentional,
     not a gap.** See memory note `fsck-c-only-by-design.md`; do not re-flag this.
   - Access-control/access-permissions logic (`access_control.py`/`.ts`,
     `access_permissions.py`/`.ts`, with dedicated tests) exists only in `ndfs-py`/`ndfs-ts` —
     **no equivalent in `ndfs-c`** at all (only raw bit constants). Still open.
   - `ndfs_set_file_access` (cheap in-place access-bits patch, no rewrite) exists only in C.
     Still open.
   - `write_file_with_properties`/`writeFileWithProperties` and
     `read_file_with_properties`/`readFileWithProperties` (write/read + XAT metadata in one
     call) exist only in `ndfs-py`/`ndfs-ts`, not `ndfs-c`. Still open.
   - Test-coverage gaps for functions that exist in all three: `get_object_entry` (zero tests in
     Python), `get_used_pages` (zero tests in Python), `ndfs_bf_load` (no direct test in C —
     notable since the bit file is this project's historically most dangerous subsystem, see
     the 0.0.5 byte-swap regression), `ndfs_get_file_blocks` (no test in C), `generate_report`
     (tested only in C). **Closed this session**, see step 8.
   - Uniform, not a cross-port gap: the whole SINTRAN initial-commands read/write/repair/diagnose
     facade is untested in all three languages at the filesystem-object level (the lower-level
     pure buffer functions in `sintran.c`/`.py`/`.ts` ARE tested). Still open.
8. Backfilled the test-coverage gaps from step 7's fourth bullet. Commit `f32b9c7`.

## 2. Current state

- All three suites green: `ndfs_tests` 280/280, `pytest` 485 passed / 7 skipped, `vitest`
  407/407.
- **4 commits ahead of `origin/main`, not pushed**: `c7b6100`, `765e141`, `87c66da`, `f32b9c7`.
  Confirm with the user before pushing — this session's norm was to ask before every push/tag.
- `v0.0.6` tag was pushed earlier (step 3) and does not include steps 4-8.

## 3. Known open items

1. **Push the 4 local commits** (or fold them into a new version bump / CHANGELOG entry and tag
   a new release — ask which).
2. **O(n²) allocation-scan performance**: `ndfs-c`'s `ndfs_bf_find_free` and `ndfs-ts`'s
   equivalent bit-file scan still do a full top-down rescan on every page allocation (unlike
   `ndfs-py`, which got a scan-cursor + cached used-count in `0bf9c8b`). Not a correctness bug —
   the 30,000-file stress test passes in all three — but it costs `ndfs-c` ~9 minutes and
   `ndfs-ts` ~3 minutes for that one test, versus seconds in `ndfs-py`. Flagged to Ronny twice
   this session; both times the call was "ship first, fix perf later." Still unfixed in C and TS.
3. **Cross-library API gaps still open** (Ronny's prioritization, not yet actioned):
   access-control/access-permissions missing from `ndfs-c`; `ndfs_set_file_access` missing from
   `ndfs-py`/`ndfs-ts`; `write_file_with_properties`/`read_file_with_properties` missing from
   `ndfs-c`. **`ndfs_fsck` is explicitly NOT one of these — do not port it, do not flag it.**
4. **SINTRAN initial-commands facade** (all three languages, filesystem-object level) has zero
   test coverage, including the write/repair path. Genuine blind spot, not yet actioned.

## 4. Caveat: concurrent work

Ronny mentioned mid-session that another agent/session was concurrently making fixes to this
same library. No conflicting changes were actually observed (`git status` was clean immediately
before each commit above), but a session picking this up should re-run `git status` and
`git log --oneline -10` before trusting anything in this document as still current.
