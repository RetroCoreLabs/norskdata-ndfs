# norskdata-ndfs

[![Release builds](https://github.com/RetroCoreLabs/norskdata-ndfs/actions/workflows/release.yml/badge.svg)](https://github.com/RetroCoreLabs/norskdata-ndfs/actions/workflows/release.yml)
[![Latest release](https://img.shields.io/github/v/release/RetroCoreLabs/norskdata-ndfs?sort=semver)](https://github.com/RetroCoreLabs/norskdata-ndfs/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

Read and write NDFS (Norsk Data File System) disk images from Sintran-III minicomputers.

> ## ⚠ Upgrade to 0.0.5 — earlier versions corrupt real SINTRAN disks on write
>
> **Every version before 0.0.5 read and wrote the allocation bitmap byte-swapped.** If you
> have written to a genuine SINTRAN-written pack with 0.0.4 or earlier, **check it** — the
> free-page search could hand out pages SINTRAN had already allocated, overwriting live file
> data.
>
> ### What was wrong
>
> | | Before 0.0.5 | 0.0.5 |
> |---|---|---|
> | **Bit file addressing** | byte `N/8`, bit `N%8` | 16-bit **word** `N/16`, bit `N%16` |
> | **Bitmap size** | `ceil(pages/8)` bytes | rounded up to a whole 16-bit word |
> | **File number** | from the **physical** index-block group | from the group's **ordinal rank** |
>
> **1. The bit file is word-addressed, not byte-addressed.** SINTRAN stores it as 16-bit
> words — `PAGE = BLOCK*400B + WORD*20B + BIT`, `ND-30.003.007` appendix F.2, where `20B` is
> 16 — so on a big-endian image page 0 lives in byte **1**, not byte 0. Measured on three
> genuine ND media, the old scheme reported **32, 4 and 3 pages that demonstrably hold file
> data as FREE**. The new scheme reports none.
>
> *Reading was unaffected in practice*, which is why `list`/`extract` always looked correct.
> Writing was not.
>
> **2. Devices whose page count is not a multiple of 16 lost their last pages.** A 616-page
> ND floppy silently dropped pages 608-615, because `ceil(616/8) = 77` bytes leaves the
> final 16-bit word half present.
>
> **3. File numbers were wrong for any relocated overflow object block.** SINTRAN moves a
> user's second object block when another user needs that group, so the number must come from
> the group's ordinal **rank**, not its index. On a captured 201-user pack the old formula
> numbered a user's files 0..255 then **1024..1067** where SINTRAN says **256..299**.
>
> ### Why the test suites did not catch any of it
>
> Every test round-tripped through this library: written with convention X, read back with
> convention X. That passes for **any** self-consistent convention, including a wrong one. The
> one check made against real data was a **popcount** comparison — and popcount is invariant
> under byte-swapping, so it could not have failed even in principle.
>
> 0.0.5 adds deliberately **asymmetric** regression tests, checked against ND's own manual
> (the appendix F.2 worked example), against SINTRAN's own allocator, and against packs
> SINTRAN itself wrote — including one captured with 201 users and a thrice-relocated object
> block. Each was verified to **fail** against the old code.
>
> Full detail: [`docs/NDFS-OBJECT-BLOCKS-SPEC.md`](docs/NDFS-OBJECT-BLOCKS-SPEC.md) and
> [`docs/NDFS-FORMAT.md`](docs/NDFS-FORMAT.md).


Three standalone libraries with identical APIs, plus a CLI tool:

| Library | Language | Tests | Install |
|---------|----------|-------|---------|
| [ndfs-ts](ndfs-ts/) | TypeScript | 494 | `npm install norskdata-ndfs` |
| [ndfs-py](ndfs-py/) | Python | 552 | `pip install norskdata-ndfs` |
| [ndfs-c](ndfs-c/) | C99 | 344 | `cmake .. && make` |
| [ndtool](ndfs-c/tools/ndtool/README.md) | CLI (C) | -- | Built with libndfs |

**Total: 1390 tests (counted 2026-10-29). No external dependencies in any library.**

## What It Does

- **Read and write files** to/from NDFS disk images (contiguous, indexed, sub-indexed allocation with sparse hole support)
- **Create new images** from templates (360KB floppy, 1.2MB floppy, 75MB SMD, 74MB Winchester, custom)
- **User management** (add, remove, quota with quotaadd/quotadel, password clearing, friends list/add/remove, max 256 users)
- **ND-100 even parity** (strip on read, set on write -- proper calculated parity per byte, not mark parity)
- **XAT sidecar files** for preserving NDFS metadata (permissions, file types, dates) when copying to/from host filesystems
- **Boot loader detection** (BPUN, FLOMON, Binary formats)
- **SINTRAN initial commands** (read / write / repair the `@LIST-INITIAL-COMMANDS` buffer that lives at kernel symbol INIBU inside `SEGFIL0`)
- **SINTRAN CPU number and type** (read / change the CPU number in `SYSNO` + `FCPUN` and the type in `HWINF+2` of both disk copies of the resident data page, rewrite `33CPN`/`33CPU`, clear the "entered by system N" directory stamp; `ndtool --cpu-identity` / `--set-cpu`, all three libraries)
- **SINTRAN segments** (list every segment with its name and disk page, export and import one; `ndtool --segments` / `--export-segment` / `--import-segment`, all three libraries)
- **Filesystem check** (fsck: bitmap consistency, orphaned blocks, cross-links, quota verification)
- **Interactive shell** (ndtool --shell: ls, cat, hexdump, edit, get, put, rm, mv, bitmap, fsck, stat, users, save)

## Quick Examples

### TypeScript

```typescript
import { NdfsFileSystem, ImageTemplate } from 'norskdata-ndfs';

// Read a text file with parity stripped
const ndfs = new NdfsFileSystem(imageData);
const text = ndfs.readFile('SYSTEM/STARTUP:MODE', 'strip');

// Write with ND-100 even parity
ndfs.writeFile('RONNY/SOURCE:C', sourceBytes, 'set');
```

### Python

```python
from ndfs import NdfsFileSystem, ImageTemplate, ImageCreationOptions

# Create a new floppy image
ndfs = NdfsFileSystem.create_image(ImageCreationOptions(
    template=ImageTemplate.Floppy360KB,
    directory_name='MY-DISK',
))

# Write with parity, read with parity stripped
ndfs.write_file('SYSTEM/HELLO:TEXT', b'Hello!', parity='set')
text = ndfs.read_file('SYSTEM/HELLO:TEXT', parity='strip')
```

### C

```c
#include <ndfs/ndfs.h>

ndfs_filesystem_t *fs = NULL;
ndfs_image_options_t opts;
ndfs_image_options_init(&opts);
opts.template_type = NDFS_TMPL_FLOPPY_360KB;
ndfs_create_image(&fs, &opts);

// Write with parity
ndfs_write_file_parity(fs, "SYSTEM/HELLO:TEXT",
    (const uint8_t *)"Hello!", 6, NDFS_PARITY_SET);

// Read with parity stripped
uint8_t *data; size_t size;
ndfs_read_file_parity(fs, "SYSTEM/HELLO:TEXT",
    NDFS_PARITY_STRIP, &data, &size);
```

### SINTRAN initial commands

The commands SINTRAN runs first at every restart (`@LIST-INITIAL-COMMANDS`) live
at the kernel symbol INIBU inside `(SYSTEM)SEGFIL0:DATA` — no file form. Read,
replace, or repair them offline (see [docs/SINTRAN-INITIAL-COMMANDS-SPEC.md](docs/SINTRAN-INITIAL-COMMANDS-SPEC.md)):

```typescript
const ic = ndfs.readInitialCommands();        // -> { version, byteOffset, commands, ... } | null
ndfs.writeInitialCommands([                    // replace the whole buffer, in place
  'ENTER-DIRECTORY,,DISC-SCSI-1,0', 'SET-AVAILABLE',
]);
// If the buffer header was clobbered so the locator can't see it:
ndfs.repairInitialCommands([ 'ENTER-DIRECTORY,,DISC-SCSI-1,0', 'SET-AVAILABLE' ]);
```

```python
ic = ndfs.read_initial_commands()             # -> InitialCommands | None
ndfs.write_initial_commands(['ENTER-DIRECTORY,,DISC-SCSI-1,0', 'SET-AVAILABLE'])
```

### SINTRAN CPU number, CPU type and segments

A SINTRAN pack holds its CPU number twice (`SYSNO`, `FCPUN`) and its CPU type once (`HWINF+2`)
in the resident data page, stored on disk in the segments `S3IDPIT` (warm start) and `S3SDPIT`
(cold start). Two nodes with the same number break COSMOS routing, and a pack entered by another
system is refused at `@ENTER-DIRECTORY`. Change both, offline, on a COPY of the pack (the
derivation is in the NDInsight chapter `SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md`):

```bash
ndtool -f pack.IMG --cpu-identity               # both copies, the symbols, the directory stamp
ndtool -f pack.IMG --set-cpu 21238:9883         # number 21238, type 9883, stamp cleared
ndtool -f pack.IMG --segments                   # every segment, its name and disk page
ndtool -f pack.IMG --export-segment S3SDPIT     # S3SDPIT.seg + S3SDPIT.txt
```

```typescript
const ident = readCpuIdentity(ndfs);            // CpuIdentity: copies[0].sysno, .fcpun, .cpuType ...
writeCpuIdentity(ndfs, 21238, 9883);            // writes both copies, 33CPN/33CPU, clears the stamp
const segs = listSegments(ndfs);                // SegmentList: segfiles[0..4], segments[].diskPage
const data = exportSegment(ndfs, segs, findSegment(segs, 'S3SDPIT'));
```

```python
from ndfs import read_cpu_identity, write_cpu_identity, list_segments, find_segment, export_segment
ident = read_cpu_identity(ndfs)                 # CpuIdentity: copies[0].sysno, .fcpun, .cpu_type ...
write_cpu_identity(ndfs, 21238, 9883)           # writes both copies, 33CPN/33CPU, clears the stamp
segs = list_segments(ndfs)                      # SegmentList: segfiles[0..4], segments[].disk_page
data = export_segment(ndfs, segs, find_segment(segs, 'S3SDPIT'))
```

### ndtool CLI

```bash
ndtool -t disk.ndfs                          # List all files
ndtool -i -v disk.ndfs                       # Info with bitmap visualization
ndtool --fsck disk.ndfs                      # Full filesystem check
ndtool -x -d -l -o output/ disk.ndfs        # Extract all (binaries safe: no -p)
ndtool -x -p -d -l -o output/ disk.ndfs     # TEXT ONLY: -p strips bit 7 and DESTROYS :BPUN/:PROG/:DATA binaries
ndtool -p --put source.c RONNY/SOURCE:C disk.ndfs   # Copy in with parity
ndtool --create floppy360 --name MYDISK new.ndfs     # Create new image
ndtool --shell disk.ndfs                     # Interactive mode
```

See the [ndtool README](ndfs-c/tools/ndtool/README.md) for the full command reference.

## ND-100 Even Parity

The ND-100 stores text files with even parity. Bit 7 of each byte is set so the total number of 1-bits is even:

```
'H' = 0x48 (01001000) -> 2 ones (even) -> bit 7 = 0 -> 0x48
' ' = 0x20 (00100000) -> 1 one  (odd)  -> bit 7 = 1 -> 0xA0
'W' = 0x57 (01010111) -> 5 ones (odd)  -> bit 7 = 1 -> 0xD7
```

Applies to text types: :MODE, :SYMB, :TEXT, :C, :BATC, :FORT, :PLAN, etc.
Does **not** apply to binary types: :PROG, :BPUN, :DATA, :VTM.

## XAT Sidecar Files

NDFS files carry rich metadata (3-tier permissions, file type flags, ND timestamps, user ownership) that has no equivalent on modern filesystems. XAT sidecar files preserve this metadata as JSON when files are extracted:

```
SYSTEM/README:TEXT  -->  README.TEXT      (file data)
                        README.TEXT.xat  (NDFS metadata as JSON)
```

When copying back, the `.xat` file restores all metadata. Essential for archival round-trips and ND-100 development workflows. See the [format spec](docs/NDFS-FORMAT.md#xat-sidecar-files) for details.

## On-Disk Format

See [NDFS-FORMAT.md](docs/NDFS-FORMAT.md) for the complete binary format specification covering master blocks, block pointers, user/object entries, allocation bitmaps, boot sectors, and XAT sidecar files.

### Verified against the real SINTRAN kernel

This library was originally reverse-engineered from disk images **without** the producing
code. It has since been corrected against the **real SINTRAN III kernel** (the carved
`006-S3FS` filesystem segment of an L-VSX-500 system) plus a set of real ND disks.

See **[KERNEL-VERIFIED-CORRECTIONS.md](docs/KERNEL-VERIFIED-CORRECTIONS.md)** for the full
list with kernel addresses and disk evidence. The headline corrections:

- The extended-info **checksum is an ADDITIVE SUM**, not XOR (the XOR form matched the
  sample disk only by coincidence).
- There is **no "valid low byte only"** checksum state — the kernel compares all 16 bits.
- **Allocation runs HIGH → LOW**, bounded by the *declared capacity*, not upward from
  block 7.
- **Bit-file placement** is `9 * floor(floor(pages/2) / 9)` — a track boundary — not
  `pages/2`.
- Flag-word **bit 15 = "directory entered"**; **system number 0 means *no owner***.
- Every real drive reserves a **bad-sector spare region**, so the device is always *larger*
  than the declared capacity — and the spare is a property of the **drive**, never a
  percentage.

## Building

```bash
# TypeScript
cd ndfs-ts && npm install && npm test

# Python
cd ndfs-py && PYTHONPATH=src python -m pytest tests/ -v

# C library + ndtool
cd ndfs-c && mkdir build && cd build && cmake .. && make && ./ndfs_tests
```

Or, from the repository root, use the convenience Makefile:

```bash
make            # build the ndtool CLI (Release)
make release    # build ndtool, statically linked where supported
make test       # build + run the C unit tests
make help       # list all targets
```

## Releasing a new version

The version number is written by hand in **four files**. There is no script that keeps them
in step, so change all four to the same number. Paths are from the repository root.

| File | What to change |
|------|----------------|
| `ndfs-c/CMakeLists.txt` | line 2: `project(ndfs VERSION X.Y.Z LANGUAGES C)` |
| `ndfs-c/tools/ndtool/ndtool.h` | line 13: `#define NDTOOL_VERSION "X.Y.Z"` (this is what `ndtool` prints) |
| `ndfs-py/pyproject.toml` | line 3: `version = "X.Y.Z"` |
| `ndfs-ts/package.json` | line 3: `"version": "X.Y.Z"` |

To check that none was missed, search for the old number. Only `CHANGELOG.md` should still
hold it (old entries):

```bash
grep -rn "0.0.8" --include=* . | grep -vE "node_modules|/build|\.git/|package-lock"
```

Steps, in this order:

1. Change the four files above.
2. In `CHANGELOG.md`, add a heading `## X.Y.Z - YYYY-MM-DD` at the top, with the changes under it.
3. Run the tests: `make test`, `make test-py`, `make test-ts`. The C tests take about 10 minutes.
4. Commit, for example `Version X.Y.Z`, and push to `main`.
5. Tag that commit and push the tag. The tag **must** start with `v`:

   ```bash
   git tag vX.Y.Z
   git push origin vX.Y.Z
   ```

Pushing the tag starts the [release workflow](.github/workflows/release.yml). It builds
`ndtool` for Windows, Linux and macOS and publishes the GitHub Release. The archive names
get their version from the **tag**, not from the files above, so a tag that does not match
the files gives archives whose names say one number while `ndtool --version` says another.

Not part of the version: `ndfs-ts/package-lock.json` is ignored by git and is not committed.

## Downloads

Pre-built `ndtool` binaries for **Windows (x64)**, **Linux (x64 / arm64)**, and
**macOS (Apple Silicon)** are attached to every
[GitHub Release](https://github.com/RetroCoreLabs/norskdata-ndfs/releases/latest).
Each archive contains a single self-contained binary with no external runtime
dependencies. Releases are built automatically by the
[release workflow](.github/workflows/release.yml) when a `v*` tag is pushed.

## License

MIT - Copyright (c) 1985-2026 Ronny Hansen, [RetroCore Labs](https://github.com/RetroCoreLabs)
