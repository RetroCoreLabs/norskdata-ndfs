# Plan: ndtool --fsck that reports hard and repairs only what is safe

Status 2026-10-21: steps 1 to 4 below are built (ndfs-c and ndtool only), step 5 is not. What was built, and where it differs
from the plan, is in "What was built" at the end of this file. Evidence is in FSCK-ANALYSIS-AND-PLAN.md (same folder).
Implementation point: ndfs-c/src/filesystem.c, functions ndfs_fsck_ex and ndfs_fsck_undo; the command is ndfs-c/tools/ndtool/cmd_fsck.c.
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

## What was built (2026-10-21)

Checked by: the new tests in ndfs-c/tests/test_fsck.c (21 in the fsck suite, all pass), runs of `ndtool --fsck` on small
images made with ndtool and damaged by hand (dry run, refused without -f, repair, check again, undo giving a byte-identical image,
orphan recovery, salvage), and `ndtool --fsck` on two real SINTRAN M system disks (WD0-SINTRAN-M.IMG and BIGDISK0-M-103.IMG), where the
report is the same as before except the new INFO line and the verdict.

| Plan step | State |
|---|---|
| 2 verdict, exit codes, failed files, -v | DONE. Exit code 3 added (ndtool could not do what was asked). The owner of each block is kept in the reference map, so a cross-link names both files and the page of each. |
| 3 R1, R3, R4, undo file, -n, -f | DONE. The undo data is made by the library: the first write to a page during a repair copies the old page (block number and 2048 bytes). ndtool writes IMAGE.fsck-undo before it writes the image. |
| 4 R5 and --salvage | DONE. R5: each run of consecutive orphan pages that hold data becomes a contiguous file USER/ORPHAN-n:DATA (n = first block) of the user given to --recover-orphans, the pages stay where they are, and the user's reservation is raised if the files would not fit. |
| 5 R6 (recompute pages_used) | NOT done. The plan says it waits for T7, T8 and T9, which need the emulator. |
| R2 (set bits of blocks 0 to 6) | NOT done, on purpose. A directory made by SINTRAN M's CREATE-DIRECTORY leaves blocks 1 to 6 clear in the bit file (measured on one image from a CREATE-DIRECTORY run, a SCSI disk, 2026-10-21), while the two real system disks WD0-SINTRAN-M.IMG and BIGDISK0-M-103.IMG have them set. fsck prints an INFO line and repairs nothing. |

Differences from the text above:
- Condition 4 of section 1 (a pointer into the reserved blocks 0 to 6) is not FAILED. It is an INFO line ("pointers into the reserved blocks 1-6 were ignored") because
  the old code comment says such pointers are stale bytes in unused index slots on rewritten packs, and the two real packs above show 6 of them while being usable.
  Pointers at or above the device size ARE FAILED.
- R5 names the file ORPHAN-n:DATA and gives it to the user named on the command line. The plan wrote LOSTFOUND/ORPHAN-n:DATA, which cannot be both a user and an owner.
- The Extended Info Block checksum (R3) is reported as a WARNING (DAMAGED) only when the block is there: a floppy has none, and a FLOMON boot area is not one.
- A repair needs -f (or -n for a list). Whether SINTRAN has the directory entered cannot be seen in the file; the Extended Info flag is printed when it is set.

Not tested: --fix on an image that SINTRAN or the emulator still has entered; images over 100000 findings (the lists are cut at 100000 and the cut is printed).

