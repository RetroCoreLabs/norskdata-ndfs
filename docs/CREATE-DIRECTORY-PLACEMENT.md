# CREATE-DIRECTORY placement: where SINTRAN puts the critical blocks of a new directory

This document says where real SINTRAN M puts the bit file, the user file and the object file of a new
directory, how the numbers are worked out, and how that was checked. It covers the first block of every
known SMD and Winchester disk format. Floppies and SCSI were measured earlier and are in
`docs/CREATE-DIRECTORY-ORACLE.md`.

Labels used for every statement:

- **VERIFIED CODE** - read in the disassembly of the SINTRAN file system segment (006-S3FS) of the M carve.
- **MEASURED** - read from the image after real SINTRAN M (in the nd100x emulator) did the command.
- **INFERRED** - fits all measurements, but no instruction was found that says so.
- **UNKNOWN** - not settled.

Octal addresses in code are marked with `o`. Page numbers are decimal. Multi-byte values on disk are big-endian.

## 1. Short version

| What | Rule | Label |
|---|---|---|
| Bit file address BA, answer 0 or empty | `BA = 9 * floor(floor(capacity / 2) / 9)` | VERIFIED CODE (routine ALBIT) and MEASURED on 16 disk formats (section 4) |
| Bit file address, answer -1 | `BA = capacity - bit file pages` | MEASURED (SMD 75 MB); the code branch was not read |
| Bit file address, typed octal address | BA is that address | MEASURED earlier (see the oracle document); the code branch was not read |
| Bit file pages | `ceil(capacity / 16384)`, contiguous from BA | VERIFIED CODE (ALBIT: multiply by 16) and MEASURED |
| Four pages of the first user and the first object | hint `H = BA - 1`; start at page `H OR 15`; take the first free page going down, four times | VERIFIED CODE (routine RSBPG) and MEASURED on 16 formats |
| Order of the four pages | user data page, user index block, object data page, object index block | VERIFIED CODE and MEASURED |
| When the user and object pages are made | not by CREATE-DIRECTORY; the user pages on the first CREATE-USER, the object pages on the first CREATE-FILE | MEASURED |
| Bitmap above the capacity | pages from capacity up to the next multiple of 256 are marked used, but not past the end of the device | MEASURED on 8 formats (section 6) |

`capacity` is the declared directory size (Extended Info Block word 1756B-1757B on hard disks; the directory
size SINTRAN gives the disk type name), not the length of the image file. The `capacity` of every disk type is
in `ndtool --create list` (DIR PAGES).

## 2. Where this is in the SINTRAN code

Source of truth: the carved M version of the file system segment `006-S3FS` (load base 26000o), with the symbol
lists in the M06 and L07 symbol folders. The NPL source tree has no text for this segment (searches for CRDIR,
ALBIT, GPAGE, INSUS, CROBJ and RSPAG found only unrelated names), so everything here comes from disassembly.
The files are in the NDInsight repository: `tools/sintran-segment-carver/versions/M-VSX-500/segments/006-S3FS.bin`,
`SINTRAN/NPL-SOURCE/SYMBOLS/M06/FILSYS-SYMBOLS.SYMB.TXT` and `SINTRAN/Filesystem/create-directory-placement.md`.

### 2.1 The bit file address (routine ALBIT)

- ALBIT is at M06 `142171o` (L07: `137516o`). CRDIR (M06 `141374o`) calls it. VERIFIED CODE.
- At `142231o` the code does `RDIV`, at `142232o` `RMPY`, and the shift `SAD ZIN -1` (A:D shifted right one place,
  divide by 2) with the constant `SAT 9` (decimal 9) sits in a piece at `177341o` to `177343o`, reached by
  `JMP I` at `142227o`. That is `9 * floor(floor(capacity / 2) / 9)`. The 9 is a literal in the code, not read
  from the disk type table. VERIFIED CODE.
- The capacity comes from the routine GSIZE (M06 `040461o`) = the declared directory size, not the file size.
  VERIFIED CODE for the call, MEASURED for the effect: the formula gives the right BA only with the declared
  capacity (for DISC-75MB: 36945, not the 38400 pages of a real image).
- The bit file size uses `MPY 20` (octal 20 = 16) in ALBIT: 16 bits per word times 1024 words per page = 16384
  pages per bit file page. VERIFIED CODE.
- The branch for a typed address and the branch for -1 were not read again. Their results are MEASURED only.

### 2.2 The allocator for the four pages (routine RSBPG)

RSBPG is at M06 `053020o`. It finds one free page and sets its bit.

1. It calls GSIZE and works out the last page of the directory (capacity - 1).
2. It reads word 17 of the directory table (GDDRT, M06 `051627o`) = BA, the bit file address.
3. The hint is `H = BA - 1`. VERIFIED CODE.
4. It turns H into: the bit file page number holding it (`H div 16384`), and the word number inside that page
   (`(H mod 16384) div 16`).
5. The first bit tested is bit 15 (the top bit) of that word, whatever the low four bits of H are. The code gets
   this by loading the test word from the constant `175376o` (BSKP ONE bit 15) after the earlier value was
   overwritten. Whether this is on purpose is UNKNOWN; the measurements show it is what the code does.
6. RSTPG (M06 `053244o`) reads the bit file page and tests the hint word with TSTDW (M06 `053365o`), which goes
   from bit 15 down to bit 0 and takes the first bit that is 0. A word of all ones is skipped. The found bit is
   set and the word is written back. The returned page number is `bit file page * 16384 + word * 16 + bit`.
7. If the hint word has no free bit, the code goes on to the neighbours: word +1 upwards (lowest bit first,
   TSTUP at M06 `053346o`), word -1 downwards, +2, -2 and so on, then neighbouring bit file pages. If everything
   is full it ends with error 31o. This part was read in the code but **no measurement has used it**: all 16
   measured formats are satisfied inside the hint word. UNKNOWN in practice.
8. RSBPG has no memory of the last allocation: every call starts again at the same hint. Pages taken by the
   earlier calls are skipped only because their bit is already 1.

Callers (M06): WUSER `056102o` (user data page, then WINDX for the index block), WOBJE `060414o` (object data
page, then WINDX), WINDX `055216o`. The data page is always allocated before the index block, so the data page
is the higher page. CRDIR itself never calls RSBPG, so CREATE-DIRECTORY writes only the label and the bit file.
All VERIFIED CODE.

### 2.3 Older SINTRAN versions

The routines RSBPG, RSTPG, TSTUP and TSTDW exist in the M06 symbol list only, not in J, K03 or L07. Older
packs made by older SINTRAN put the pages about 206 to 232 pages above BA. For those, the allocator is RSPAG
(L07 `051120o`), which takes the highest free page of the 256-page block that holds BA. For BA 18468 that block is
18432 to 18687 and the top is 18687. This reproduced the four older packs that were checked. It is a different
rule and it is **not** implemented in `ndtool`. INFERRED from the fit.

## 3. How it was measured

For every format, with the emulator (nd100x) and real SINTRAN M:

1. A zero-filled image file of the full SINTRAN layout length was made. The length matters: SINTRAN reads and
   writes above the directory size, and the emulator picks the disk geometry from the file length (the layout of
   each format is in `docs/DISK-GEOMETRY-AND-BOOT-FINDINGS.md`). Too short a file gives "Disk transfer error".
2. SINTRAN was booted from a system disk on the other controller type (Winchester system for SMD targets, SMD
   system for Winchester targets), with the blank image attached as unit 0.
3. Commands, with the prompt answers: `CREATE-DIRECTORY` (DIRECTORY NAME: ORACLE; DEVICE OR POOL NAME: the disk
   type name with unit and format letter, for example DISC-288MB-1-F; DEVICE UNIT: 0; FIXED(F) OR
   REMOVABLE(R): F where asked; OCTAL BIT FILE ADDRESS: 0), then `ENTER-DIRECTORY`, `CREATE-USER ORACLE:SYSTEM`,
   `GIVE-USER-SPACE ORACLE:SYSTEM,500`, `CREATE-FILE (ORACLE:SYSTEM)TEST:DATA 3`, `RELEASE-DIRECTORY ORACLE`.
4. The image was copied after each step and the label, the bit file and the user and object pages were read.
5. The emulator was ended with the command `STOP-SYSTEM` each time.

## 4. Results: 16 disk formats, answer 0

All 16 were made by SINTRAN in this way. In every row the bit file address equals the formula and the four
pages equal the rule in section 1. Column "DT entry" is the layout table entry whose comment names the disk type
(from the comments in the layout table; the match of 288 MB-F, -E, -N and 450 MB-F, -N to a DT entry is by that
comment only).

| Device name | Family | DT entry (by name comment) | Image pages | Capacity | Bit file pages | Bit file address (SINTRAN) | Formula 9*floor(floor(cap/2)/9) | User data | User index | Object data | Object index |
|---|---|---|---|---|---|---|---|---|---|---|---|
| DISC-38MB-1 | SMD | DT037 | 18495 | 18486 | 2 | 9243 | 9243 | 9247 | 9246 | 9245 | 9242 |
| DISC-70MB-1 | SMD | DT070 | 37035 | 34765 | 3 | 17379 | 17379 | 17391 | 17390 | 17389 | 17388 |
| DISC-75MB-1 | SMD | DT075 | 37035 | 36945 | 3 | 18468 | 18468 | 18479 | 18478 | 18477 | 18476 |
| DISC-140MB-1-F | SMD | DT140 | 74070 | 69530 | 5 | 34758 | 34758 | 34767 | 34766 | 34765 | 34764 |
| DISC-288MB-1-R | SMD | DT288 | 140733 | 140391 | 9 | 70191 | 70191 | 70190 | 70189 | 70188 | 70187 |
| DISC-288MB-1-F | SMD | DT300 | 153576 | 140391 | 9 | 70191 | 70191 | 70190 | 70189 | 70188 | 70187 |
| DISC-288MB-1-E | SMD | DT310 | 158210 | 140391 | 9 | 70191 | 70191 | 70190 | 70189 | 70188 | 70187 |
| DISC-288MB-1-N | SMD | DT290 | 144025 | 140391 | 9 | 70191 | 70191 | 70190 | 70189 | 70188 | 70187 |
| DISC-450MB-1-F | SMD | DT460 | 230364 | 220584 | 14 | 110286 | 110286 | 110285 | 110284 | 110283 | 110282 |
| DISC-450MB-1-N | SMD | DT470 | 223820 | 220584 | 14 | 110286 | 110286 | 110285 | 110284 | 110283 | 110282 |
| DISC-14MB-1 | Winchester | DT014 | 6912 | 6912 | 1 | 3456 | 3456 | 3455 | 3454 | 3453 | 3452 |
| DISC-21MB-1 | Winchester | DT021 | 10890 | 10728 | 1 | 5364 | 5364 | 5375 | 5374 | 5373 | 5372 |
| DISC-23MB-1 | Winchester | DT023 | 11205 | 11016 | 1 | 5508 | 5508 | 5519 | 5518 | 5517 | 5516 |
| DISC-28MB-1 | Winchester | DT028 | 13797 | 13648 | 1 | 6822 | 6822 | 6831 | 6830 | 6829 | 6828 |
| DISC-45MB-1 | Winchester | DT045 | 22410 | 22032 | 2 | 11016 | 11016 | 11023 | 11022 | 11021 | 11020 |
| DISC-74MB-1 | Winchester | DT074 | 36792 | 36396 | 3 | 18198 | 18198 | 18207 | 18206 | 18205 | 18204 |

Checks done on this table:

- BA equals `9 * floor(floor(capacity / 2) / 9)` in all 16 rows: yes.
- The four pages equal the first four free pages going down from `(BA - 1) OR 15`, with the pages
  `0` and `BA .. BA + bit file pages - 1` taken: yes in all 16 rows (calculated from the bit file position alone).
- The sizes of the 288 MB and 450 MB disks give the same BA and capacity for all four letters R, F, E, N / F, N:
  the directory size comes from the disk type name table, not from the layout, so the four formats look the same
  inside the directory.

## 5. Worked examples

**DISC-75MB (SMD).** capacity 36945. `floor(36945 / 2) = 18472`; `9 * floor(18472 / 9) = 9 * 2052 = 18468`. Bit file
pages: `ceil(36945 / 16384) = 3`, so pages 18468 to 18470. H = 18467. `18467 OR 15 = 18479` (word 1154 covers 18464 to
18479). Going down from 18479: free, so user data 18479; user index 18478; object data 18477; object index 18476.
Matches the table.

**DISC-288MB (any letter).** capacity 140391. `floor(140391 / 2) = 70195`; `9 * floor(70195 / 9) = 70191`. Bit file
pages: 9 (140391 / 16384 = 8.57), so pages 70191 to 70199. H = 70190; `70190 OR 15 = 70191`, which is the first
page of the bit file, so it is taken. Going down: 70190 is free, so user data 70190; user index 70189; object data
70188; object index 70187. This is the case where the bit file itself starts inside the hint word, and
the pages end up directly below it.

**DISC-14MB (Winchester).** capacity 6912. `floor(6912 / 2) = 3456`; `9 * floor(3456 / 9) = 3456`. One bit file page, page
3456. H = 3455; `3455 OR 15 = 3455`. User data 3455; user index 3454; object data 3453; object index 3452.

## 6. The bitmap above the capacity

SINTRAN marks the pages from the capacity up to the next multiple of 256 as used, but it never marks a page that
is past the end of the device. Pages below the capacity are free except page 0, the bit file, and the pages
the directory has used. Read from the bitmap right after CREATE-DIRECTORY (MEASURED):

| Device name | Capacity | Image pages | Next multiple of 256 | Used range above the capacity |
|---|---|---|---|---|
| DISC-38MB-1 | 18486 | 18495 | 18688 (past the end) | 18486 to 18494 |
| DISC-75MB-1 | 36945 | 37035 | 37120 (past the end) | 36945 to 37034 |
| DISC-288MB-1 (R or F) | 140391 | 140733 or 153576 | 140544 | 140391 to 140543 |
| DISC-450MB-1-F | 220584 | 230364 | 220672 | 220584 to 220671 |
| DISC-14MB-1 | 6912 | 6912 | 6912 | none |
| DISC-21MB-1 | 10728 | 10890 | 10752 | 10728 to 10751 |
| DISC-74MB-1 | 36396 | 36792 | 36608 | 36396 to 36607 |

Who sets these bits (SINTRAN itself in CREATE-DIRECTORY, or the format step) is not known from code. The rule
is MEASURED only.

## 7. What is not settled

1. The spiral search (section 2.2 point 7) is read in the code, never measured.
2. The place of the second user data page and the second object data page (after 32 users or 32 files) is not
   measured.
3. The branches of ALBIT for -1 and for a typed address were not read again; -1 was only measured on the 75 MB
   SMD, not on the other 15 formats. The typed address was measured on earlier runs only.
4. The formats listed as sub-divided disks (DISC-2-75MB, DISC-3-75MB, DISC-4-70MB, DISC-6-70MB, DISC-2-70MB-F) and
   the 225 MB disks were not run. DISC-30MB, 60MB and 90MB halt SINTRAN (ERRFATAL).
5. The routine RSPAG variant for old SINTRAN (section 2.3) is not in `ndtool`.
6. Nobody has booted a created image; only ENTER-DIRECTORY, DIR and CREATE-FILE were run on them.

## 8. How `ndtool` follows this

`ndfs_create_image` in `ndfs-c/src/image_creator.c` works out BA with the formula (answer 0), `capacity - bit file pages`
(answer -1) or the typed address, marks page 0, the bit file and the range above the capacity, and then
places the four pages with the function `alloc_near_hint`, which is the RSBPG rule above. For all 16 formats
in section 4 the images made by `ndtool --create DISC-xx` have the same bit file address, user data and index
pages, and object data and index pages as the images made by SINTRAN, and the bitmaps are the same except for the
3-page test file that was only in the SINTRAN images. `ndtool --create list` prints the sizes.
