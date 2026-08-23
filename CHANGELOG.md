# Changelog

All notable changes to the NDFS libraries (`ndfs-c`, `ndfs-py`, `ndfs-ts`, `ndtool`).

## 0.0.7 - 2026-08-23

### Fixed - one unreadable page hid every file on the floppy

0.0.6 stopped a damaged user file from taking the listing down. The same thing
was still true one level lower: a single page that could not be read anywhere
in the object file, or in the bit file, failed the whole load. A lost page must
cost only what was written on that page.

* `ndfs-ts` / `ndfs-py` - an object-file DATA page that cannot be read now
  skips its own 32 entries and carries on, and the slots are still counted so
  every entry after it keeps the object index it has on the media. An index
  page under a sub-index is skipped the same way. A user-file data page is
  skipped per page instead of abandoning the whole user file, so users named
  by the pages that do read survive. Bit-file pages that cannot be read leave
  their own pages reading as free instead of failing the load.
* `ndfs-c` - already skipped all four; it now counts them like the other two.

The top-level object-file index page is deliberately still fatal in all three:
if that page is gone there is nothing left to name the files.

### Added - `getDamageReport()` / `get_damage_report()` / `ndfs_get_damage_report()`

Three counts - object pages, user pages, bit-file pages - all zero on an intact
image. Above zero the listing is what survived a damaged floppy, and a caller
that publishes the listing can say so. Nothing says WHICH entries were lost;
that cannot be known from the media.

### Tests

`ndfs-ts/tests/damaged-media.test.ts`, `ndfs-py/tests/test_damaged_media.py`
and `ndfs-c/tests/test_damaged_media.c` - the same 8 cases, same names, same
numbers in all three. A 360 KB floppy is built with 40 files (32 on the first
object data page, 8 on the second) and a pointer is aimed at a page outside the
image, which is how the failure reaches the loader on real media.

Measured over an archive of 1102 floppies: 13 images that reported nothing now
parse. That is not the same as 13 readable disks - the confirmation that a
listing is real belongs to the caller, and only two of the 13 have one.

## 0.0.6 - 2026-08-18

### Fixed - a damaged user file hid the whole file listing

All three ports loaded the object file only after the user file had been read,
so a user index page that could not be read took the listing down with it. The
object file does not depend on the user file, and it is what names every file
on the disk.

* `ndfs-ts` - `loadStructures()` nested the object-file load inside the
  user-file branch, and `readPage()` throwing on a damaged page threw out of
  the constructor. The user file is now read on its own, its failure caught,
  and the object file loaded either way.
* `ndfs-py` - `_load_structures()` had the same nesting; same fix.
* `ndfs-c` - `load_structures()` returned `NDFS_ERR_CORRUPT` when the user
  index page could not be read, so the whole image failed to load. It now
  carries on with no user names.

Objects keep their user index when no name can be resolved; nothing else is
lost.

Seen on a 154 page SINTRAN floppy (directory `N-10-102-I`) whose master block
and object file are intact and whose page 151, the user index, holds an object
entry page left from an older backup. The parse failed with "Block 268453449
out of range" and reported nothing; it now lists `MACM-1718K:BPUN` and
`SINTRAN-I:DATA`.

### Fixed - three write-path bugs found installing the COSMOS TCP/IP kit

* **Sparse holes must be declared, never inferred from an all-zero page.**
  The old writers turned any full page of zeros into a hole; that is
  invisible on read-back through this library, but SINTRAN's READ-BI and
  READ-PROGFILE refuse a hole with `NO SUCH PAGE` / `ERROR IN ACCESSING
  INPUT FILE`. Measured 2026-08-18 on a SINTRAN III VSX-K pack:
  `(TCP-IP)TCP-SER-B1..B3-B05:BPUN` arrived with 48/62/53 invented holes and
  failed to load; bank 0, which has no zero pages, loaded fine. Every write
  path now allocates every logical page for real; a new
  `ndfs_write_file_holes` (C) / `holes=` (Python) / `holePages` (TS)
  parameter lets a caller restore holes explicitly recorded in an XAT
  sidecar's `ndfs.holes` key, the only legitimate source of a hole.
* **A new file now inherits the owning user's default file access**
  (`@SET-DEFAULT-FILE-ACCESS`) instead of a flat owner-and-friends-only
  default. The flat default left files unreadable by SYSTEM, which is
  neither owner nor friend of an ordinary user, so a `:BPUN` written into a
  user's area could not be read by the RT-LOADER meant to load it.
* **Object lookup (Python/TS) now matches by TYPE, not just NAME.**
  `NAME:TYPE` is a SINTRAN file's real identity — two files commonly share a
  name and differ only in type, e.g. `(TCP-IP)SKP-C00:DEFS`/`:IMPT`/`:INTL`.
  Matching on name alone made a write to `AAA:MODE` silently overwrite an
  existing `AAA:LIST`'s data while leaving its type as `LIST`. Measured
  2026-08-19 installing the same kit: `TCP-IP-LO-D02:MODE` and
  `TCP-START-D02:MODE` were both lost this way. `ndfs-c`'s object lookup
  already matched on type, so `ndfs-c` needed no change for this one.

Declaring every page real exposed an O(n²) allocation-scan cost in
`ndfs-py`'s bit file (each allocation rescanned the bitmap from the top, and
the used-page count was recomputed by a full walk on every quota check).
`ndfs-py` now caches a scan cursor and a running used-page count.
`ndfs-c` and `ndfs-ts` have the same rescan cost, not yet optimized — their
30,000-file stress test takes ~9 minutes and ~3 minutes respectively but
still passes.

## 0.0.5 — 2026-08-02

**Upgrade strongly recommended. Earlier versions can corrupt genuine SINTRAN packs on write.**

### Fixed — allocation bitmap was byte-swapped (data corruption)

The bit file is an array of **16-bit words**: page `N` is bit `N%16` of word `N/16`, counting
from the LSB. Every earlier version addressed it as byte `N/8`, bit `N%8`, which on a
big-endian image swaps the two bytes of every word.

Consequence: the free-page search could return pages SINTRAN had already allocated, so
**writing to a real pack could overwrite live file data**. Reading was unaffected in practice,
which is why `list` and `extract` always looked correct.

Authority: `ND-30.003.007 EN SINTRAN III System Supervisor`, appendix F.2 —
`PAGE = BLOCK*400B + WORD*20B + BIT`, where `20B` is 16 decimal (identical formula in the
Norwegian edition `ND-30.003.7 NO`); and SINTRAN's own allocator, which forms the word index
with `SHA ZIN SHR 4` (page/16) at `TPAGF` 51043B.

Measured on three genuine ND media — pages that demonstrably hold file data yet were reported
FREE:

| Image | old | new |
|---|---|---|
| `BIGDISK0-L.IMG` (75 MB pack) | 32 | **0** |
| `210319H02-XX-01D.img` (floppy) | 4 | **0** |
| `Nd-210523I01-XX-01D.img` (floppy) | 3 | **0** |

### Fixed — bitmap truncated on devices not a multiple of 16 pages

The bitmap is now rounded up to a whole 16-bit word. `ceil(pages/8)` alone leaves the final
word half present: a 616-page ND floppy silently lost pages 608-615.

### Fixed — file numbers wrong for a relocated overflow object block

SINTRAN relocates a user's overflow object block when another user needs that index-block
group. The logical block is therefore the **ordinal rank** of the group among the groups that
user occupies, not the physical group number. On a captured 201-user pack the old formula
numbered one user's files 0..255 then **1024..1067**, where SINTRAN reports **256..299**.

Because the rank depends on the user's whole set of groups, file numbering is no longer a pure
function of `(position, user)` — it is applied as a post-load pass.

### Changed — overflow blocks no longer refuse on a "collision"

An earlier guard refused to grow a user into a block whose group belonged to another user.
SINTRAN does not refuse: it **skips** to a free group, and relocates later if the home owner
needs it. These libraries now skip too, and additionally never place a block in an existing
user's home group, so they cannot produce a layout SINTRAN would have to rearrange.

Note also that the "multi-user" and "multi-block" placements were never rival models:
`(U//64)*512 + (U%64)*8` is identically `8U`.

### Added — regression tests that could actually have caught the above

The previous suites round-tripped through this library: written with convention X, read back
with convention X, which passes for any self-consistent convention including a wrong one. The
one check against real data was a **popcount** comparison, and popcount is invariant under
byte-swapping — it could not have failed even in principle.

The new tests are deliberately **asymmetric**, checked against artefacts this project did not
produce:

- ND's own worked example from appendix F.2 (the `313B` bit-file word)
- the `PAGE = BLOCK*400B + WORD*20B + BIT` formula, and explicit byte-position assertions
- the invariant that no page holding real file data may read as free, on genuine ND media
- `testdata/BIGDISK0-K-201users.img.gz` — a real SINTRAN III K pack captured with 201 users
  and a thrice-relocated overflow block, which no synthetic image can reproduce

Each was verified to **fail** against the pre-0.0.5 code.

### Test counts

TypeScript 396, Python 475, C 261.

## 0.0.4 and earlier

See git history. **Do not use for writing to genuine SINTRAN media** — see the bitmap defect
above.
