# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Three standalone libraries — C99 (`ndfs-c`), Python (`ndfs-py`), TypeScript (`ndfs-ts`) — that read and write NDFS (Nord Disk File System) disk images from Norsk Data minicomputers running Sintran-III. Plus `ndtool`, a CLI built on the C library. The three libraries are **deliberate parallel ports of the same API and on-disk logic** — they are kept in lock-step.

## The most important fact about this repo

**The behavioral authority is RetroCore's C# NDFS** (`RetroCore/Emulated.Utilities/ND/FileSystem`, a.k.a. RetroCommander) — the golden reference these three libraries must match (see `docs/NDFS-VALIDATION-PLAN.md`). Among the three ports here, **`ndfs-c` is the lead**: Python and TypeScript mirror it, and almost every fix lands as `c/py`, `py/ts`, or `ts/py` — the same bug corrected across implementations in one or adjacent commits. When you change on-disk behavior in one language, the equivalent change is expected in the other two, plus a regression test in each. When the ports disagree, verify against RetroCore's behavior and `docs/NDFS-FORMAT.md` — do not assume. Note RetroCore's write model: **writes are immediate and surgical** — a mutation rewrites only the block(s) it touched, never the whole filesystem.

The file/module layout is intentionally parallel across all three (`master_block`, `block_pointer`, `bit_file`, `user_entry`, `object_entry`, `object_file`/`user_file`, `filesystem`, `image_creator`, `boot_loader`, `parity`, `xat`, `wildmatch`, `ndfs_name`). Test files mirror each other too (`test_golden`, `test_write_*`, `sparse-files`, `stress`, etc.). When porting a fix, the corresponding file and test almost always already exist under the same name.

`docs/NDFS-FORMAT.md` is the authoritative binary-format spec. Treat it as the contract all three implementations satisfy. `docs/NDFS-VALIDATION-PLAN.md` tracks cross-implementation validation status.

## Build & test

From the repo root (`make help` lists all targets):

```bash
make            # build ndtool CLI (Release) via CMake
make test       # build + run the C unit tests (alias for test-c)
make test-ts    # ndfs-ts: npm install && npm test
make test-py    # ndfs-py: PYTHONPATH=src python -m pytest tests/ -v
make format     # clang-format over C sources
```

Per-implementation:

```bash
# C library + ndtool + tests
cd ndfs-c && mkdir build && cd build && cmake .. && make && ./ndfs_tests
ctest --test-dir build --output-on-failure          # run via ctest

# Python (note: PYTHONPATH=src is required)
cd ndfs-py && PYTHONPATH=src python -m pytest tests/ -v
cd ndfs-py && PYTHONPATH=src python -m pytest tests/test_golden.py -v       # single file
cd ndfs-py && PYTHONPATH=src python -m pytest tests/test_golden.py::test_name   # single test

# TypeScript (vitest)
cd ndfs-ts && npm install && npm test
cd ndfs-ts && npx vitest run tests/golden.test.ts                          # single file
```

To release a new version, follow "Releasing a new version" in `README.md`: the version is written by hand in four files, then a `vX.Y.Z` tag starts the GitHub release build.

There are **no external dependencies** in any of the three libraries — keep it that way.

## Core on-disk model (shared by all three)

- **Page** = 2048 bytes (1024 × 16-bit words). Everything is **big-endian**. Strings are terminated by a single `0x27` (`'`) byte followed by NULs — *not* a field padded with terminators (this distinction has caused real SINTRAN-visibility bugs).
- **Page 0** holds the boot sector, an Extended Info Block (hard disks only, with a checksum), and the 32-byte **Master Block** at offset `0x07E0`. The master block has three `BlockPointer`s: Object File, User File, Bit File.
- **BlockPointer** (4 bytes): top 2 bits = type (`00` contiguous, `01` indexed, `10` sub-indexed, `11` reserved/invalid), low 30 bits = block ID (page LBA). Indexed = index block of up to 512 data-page pointers (≤512 data pages/file). Sub-indexed adds one level. `blockId == 0` inside an index block = sparse hole (reads as zeros).
- **User entries** (64 bytes, 32/page, max 256 users): the owning user is identified by the **User Index byte at offset 37**, not physical position. A user lives at slot `userIndex % 32` in page `userIndex / 32`.
- **Object (file) entries** (64 bytes): `Object Index` (offset 34) = `(userIndex << 8) | slot` — the **high byte is the owning user**. `Bytes-in-file` (offset 56) stores `actual_size − 1`. A single-version file is **self-referential** (Next = Prev = its own object index); zeroed version pointers make SINTRAN report a broken chain.
- **Per-user object-file partitioning (critical, source of several past bugs):** the object file is partitioned by user. User *U* owns object slots `U*256 .. U*256+255`, i.e. index-block pointer slots `U*8 .. U*8+7`. SINTRAN derives ownership from physical position, so **a new file must be written into its owner's region**, never the first globally-free slot.
- **Object blocks — 256 files/user is the DEFAULT, not the limit (corrected 2026-08-01):** a user holds 256 files per *object block*, 1 block by default and up to **16 (4096 files)** on version K (`@GIVE-OBJECT-BLOCKS` raises it, `@USER-STATISTICS` reports `MAXIMUM NUMBER OF FILES`). The counts are **user-entry byte 47, two ZERO-BASED nibbles**: high = `MXOBL-1`, low = `ACOBL-1`, so a default user stores `0x00`. Block *n* for user *U* lives at pages `n*512 + U*8 .. +7` — a stride of 512 pages. The file number is `block*256 + slot`, **not** the raw physical position; deriving it from position is right only for a user's first block. Verified against a live K pack (user BIGMAN: byte 47 = `0x31`, entries at pages 64-71 and 576-583, `F0500` = `FILE 307`). See `NDInsight SINTRAN/XMSG/DOC/NDFS-OBJECT-BLOCKS-DECODED-2026-08-01.md` and, for the API, `docs/NDFS-OBJECT-BLOCKS-SPEC.md`.
  - **API (all ports):** `give_object_blocks` / `take_object_blocks` / `get_object_block_info`. Granting **ADDS** to MXOBL, does **not** allocate blocks (that happens on demand) and does **not** reserve disk pages — object blocks cap the NUMBER of files, quota caps their size. A grant past 16 is refused, never clamped. `take_*` has **no SINTRAN equivalent**; it exists only to undo a grant.
  - **Two limits, kept distinguishable:** full-but-grantable (MXOBL < 16) versus the hard 4096-file ceiling. Python/TS/C# carry it as `is_hard_limit`/`isHardLimit`/`IsHardLimit` on the raised error; C returns `NDFS_ERR_OBJ_BLOCKS_FULL` versus `NDFS_ERR_OBJ_BLOCKS_MAX`. Never tell an operator to run `@GIVE-OBJECT-BLOCKS` when they already have 16.
  - **`ndtool`:** `--objblocks`, `--giveobjblocks NAME N`, `--takeobjblocks NAME N`; `--stat` prints the SINTRAN file number with its block and slot.
  - **Growth needs a SubIndexed object file.** Block 1 starts at page 512, which a plain Indexed object file cannot address. No port converts it; creation fails when a second block is genuinely needed on such a pack.
- **Bit file** = one allocation bit per page, addressed as **16-bit WORDS** (corrected 2026-08-02): page `N` is bit `N%16` of word `N/16`, counting from the **LSB**. On the big-endian byte array that is `byte = (N>>3) ^ 1`, `bit = N&7` — so **page 0 lives in byte 1, not byte 0**. Authority: `ND-30.003.007 EN` appendix F.2, `PAGE = BLOCK*400B + WORD*20B + BIT` where `20B` = 16 (Norwegian edition `ND-30.003.7 NO` identical); SINTRAN's own allocator forms the word index with `SHR 4` at `TPAGF` 51043B. Blocks 0–6 are reserved system blocks and must never be allocated for data.
  - The bitmap must be a **whole number of 16-bit words**. `ceil(pages/8)` alone cuts the last word in half on any device whose page count is not a multiple of 16 — a 616-page floppy loses pages 608–615.
  - **This was wrong in all four ports until 2026-08-02**, byte-swapped within every word. It survived because reader and writer shared the error, so every round-trip test passed, and because the doc that asserted it cited a **popcount** match as proof — popcount is invariant under byte-swapping and could not have detected it. Never accept a "VERIFIED" whose check cannot fail for the thing it claims to verify.
  - Regression tests live in `ndfs-py/tests/test_bit_file_ordering.py`, `ndfs-ts/tests/bit-file-ordering.test.ts`, `ndfs-c/tests/test_bit_file.c` and `RetroFS.Tests/NdfsBitFileOrderingTests.cs`. They are deliberately **asymmetric** — the manual's `313B` worked example, the page formula, explicit byte positions, and the invariant that no page holding file data may read as free on a real pack.

## ND-100 even parity

Text files store **calculated even parity** (bit 7 set so each byte has an even number of 1-bits) — not mark parity. Applies to text types (`:MODE :SYMB :TEXT :C :BATC :FORT :PLAN` …), not to binary types (`:PROG :BPUN :DATA :VTM`). Read APIs take a `strip`/`set` mode. See `parity.*` in each implementation and the README's worked examples.

## XAT sidecar files

NDFS metadata (3-tier permissions, file-type flags, ND timestamps, ownership) has no host-FS equivalent, so extraction emits a `<file>.xat` JSON sidecar that restores all metadata on copy-back. See `xat.*` and `docs/NDFS-FORMAT.md#xat-sidecar-files`.

## Conventions (from the user's global instructions — these are hard rules)

- **C# rules do not apply here** (no C# in this repo), but the no-assumptions discipline does: do not present guesses as facts; verify against `docs/NDFS-FORMAT.md` or the C reference.
- **Always actually run the tests** before reporting success — never assume they pass. If you cannot run them, say so and ask.
- **Never create standalone/throwaway test programs.** Add cases to the existing unit-test suites and clean up after yourself.
- **Never mention Claude/AI in git commit messages.** Match the existing commit style: `area: short imperative summary`, often prefixed with the touched implementations, e.g. `c/py:`, `ts/py:`, `ndfs:`, `ndtool:`, `docs:`.
- On Windows this is a PowerShell environment; `cd e:\path` does not change drive on its own.
- When you create or modify a Markdown doc, give the user its full absolute path.
