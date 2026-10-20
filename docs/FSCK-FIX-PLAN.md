# Plan: ndtool --fsck that reports hard and repairs only what is safe

Status: PLAN ONLY. No code written. Evidence is in FSCK-ANALYSIS-AND-PLAN.md (same folder).
Implementation point: ndfs-c/src/filesystem.c, function ndfs_fsck; options in ndfs-c/tools/ndtool/ndtool.c.
Tests go into the existing ndfs-c/tests/test_fsck.c (no throwaway test programs).

## 1. Hard verdict (new, always printed, last lines of the report)

    IMAGE VERDICT: FAILED          (or DAMAGED, or CLEAN)
    FAILED FILES: 3
      SYSTEM/FILSYS-SYMBOLS:SYMB  page 0 (block 36944) is also used by SYSTEM/SET-CPU-K:MODE (index block)
      ...

Levels and exit codes of ndtool --fsck (so scripts can stop on them):

| Verdict | Exit code | Condition |
|---|---|---|
| CLEAN | 0 | no errors, no warnings |
| DAMAGED | 1 | only warnings that lose no data (orphans, quota difference, stale flags) |
| FAILED | 2 | any condition below |

FAILED image conditions (each one also names the files it hits):
1. Master block invalid, or object/user/bit file pointer invalid or out of range.
2. Any block referenced by two files or structures (cross-link). BOTH owners are listed as FAILED; fsck cannot tell which one holds the
   right bytes (example: Update 4, page 36944 holds an index block where FILSYS-SYMBOLS page 0 should be).
3. Any page referenced by a file but marked free in the bitmap.
4. Any pointer to a page outside the device, or into the reserved blocks 0-6.
5. A structure that cannot be read (index block, sub-index block, object page, user page outside the image).
6. An object entry with an invalid pointer type 11, or with pages in file but a null pointer.

FAILED file rule: a file is listed with the first reason and the page position inside the file, e.g. "data page 4 (block 36558)".
A file is never called FAILED because of an orphan or a quota number. The failed-file list is printed in full, never cut short, and the
same list is available as one line per file with -v for scripting: FAILED|USER/NAME:TYPE|page-in-file|block|reason.

## 2. What may be repaired (--fix), and why it is safe

Default stays read only. --fix without -n writes; --fix -n prints what it would do and writes nothing.
Rule: a repair is allowed only if (a) the right value follows from the bytes on disk alone, and (b) the repair cannot lose data.

| # | Finding | Repair | Safe because |
|---|---|---|---|
| R1 | Page referenced by a file but free in bitmap | set the bit | adds protection, frees nothing |
| R2 | Bits for blocks 0-6 not set | set them | those blocks are never allocatable (NDFS-FORMAT.md) |
| R3 | Extended Info Block checksum wrong | recompute and store | SINTRAN itself rebuilds it on enter (CHDSI/WXDIR) |
| R4 | Orphan page, ALL 2048 bytes zero | clear the bit | nothing stored there |
| R5 | Orphan page with data | NOT freed. With --recover-orphans: copy each run of orphan pages into a new file LOSTFOUND/ORPHAN-<firstblock>:DATA owned by a chosen user, the pages stay where they are, then the file index points at them | no byte is lost or moved |
| R6 | pages_used differs from the SINTRAN rule (data + index blocks + object-entry pages) | recompute and store, ONLY with --fix-quota, ONLY after TODO T7/T8 confirm the rule | rule is proven for 8 of 11 users only |

Not repaired, only reported (reason in brackets):
- Cross-links (which owner is right is unknown; a copy-out of both files with the shared pages named is offered instead: --salvage DIR).
- Pages at or above the declared capacity (not orphans; both conventions exist on real images: Update 3).
- Master block Unreserved Pages (meaning unknown; no formula fits; TODO T9).
- Extended Info flag 0x8000 left-entered (this is the mount interlock, not ours to clear).
- Garbage pointers inside unused index slots (could be a hole marker SINTRAN reads; report only).

## 3. Safety rules for --fix

1. Repairs R1-R5 depend on the reference map. If Phase 2 hit ANY of failed conditions 1, 4 or 5 (map incomplete), no orphan repair runs
   (R4, R5): a block that looks unreferenced may belong to a structure that could not be read.
2. Before the first write, write the undo file IMAGE.fsck-undo next to the image: page number plus the old 2048 bytes of every page that will
   change. --fsck-undo IMAGE reapplies it. Nothing else is copied; the image itself is edited in place.
3. Writes go through the existing block-IO seam, one touched page at a time (same model as the rest of the library).
4. Refuse to run --fix when the image is still entered by a running emulator or SINTRAN: not detectable from the file, so print
   "stop the emulator first" and require -f to continue.
5. Every repair prints one line: "R4 block 16099: bit cleared (was used, page all zero)".
6. After repairs, fsck runs again on the result and prints the new verdict; the exit code is that of the second run.

## 4. Options (ndtool --fsck ...)

| Option | Meaning |
|---|---|
| -v | verbose report: orphan runs, both owners of every cross-link with page position in each file, per-user quota table (data, index blocks, object pages, stored, difference), files with 0 bytes |
| --fix | do repairs R1-R4 |
| -n | with --fix: show only |
| --recover-orphans USER | R5 into LOSTFOUND/ files of USER |
| --fix-quota | R6 (off until T7/T8 are done) |
| --salvage DIR | copy out every FAILED file, with a text file naming its shared pages |
| --fsck-undo | put back the pages from IMAGE.fsck-undo |

## 5. Tests to add to ndfs-c/tests/test_fsck.c
- Image with a cross-link made on purpose: verdict FAILED, exit 2, both files listed, --fix changes nothing.
- Referenced-but-free page: --fix sets the bit, second run clean.
- All-zero orphan: freed. Orphan with data: kept, and recovered into a file with --recover-orphans, byte compare of the content.
- Orphans above declared capacity: no warning at all.
- Bad Extended Info checksum: fixed.
- Map incomplete (pointer out of range): no orphan repair, message says why.
- Undo file: fix, then undo, image byte-identical to the start.
- Quota rule table from the analysis: the 8 matching users report no warning once R6 is enabled.
Port to ndfs-py/ndfs-ts is NOT needed (fsck is C only by design).

## 6. Order of work
1. DONE (code and test written, NOT built or run): Phase 3 counts pages at or above declared capacity separately and prints an INFO line (removes 1455 and 2004 false orphans).
2. Verdict, exit codes, failed-file list, -v report (read only; no risk).
3. R1, R2, R3, R4 with undo file and the -n mode.
4. R5 and --salvage.
5. R6 only after TODO T7/T8/T9 are answered by the emulator experiments.
