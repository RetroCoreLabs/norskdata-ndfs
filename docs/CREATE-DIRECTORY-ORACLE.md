# What SINTRAN's own @CREATE-DIRECTORY writes (measured with the emulator)

Written 2026-10-18. These are MEASURED results: the real SINTRAN III VSX/500 M (the Winchester system disk WD0-SINTRAN-M.IMG) was booted in the nd100x emulator
(version 1.0.22) on scratch copies, a BLANK 78643200 byte (38400 page) all-zero image was attached as the target, and SINTRAN's own commands were typed. After each
command the target image was compared byte for byte with a copy taken before it. Nothing was assumed. Original image files were never written.

Labels: MEASURED = seen in the bytes of the target image after the SINTRAN command. INFERRED = a reading of what was measured.

## Run 1: SMD target (device name DISC-75MB-1, unit 0), SINTRAN M booted from a Winchester disk

Command sequence typed on the console as user SYSTEM (empty password):

1. `@CREATE-DIRECTORY` - prompts: DIRECTORY NAME, DEVICE OR POOL NAME, DEVICE UNIT, "OCTAL BIT FILE ADDRESS (-1 END OF DISC)". Answers: ORACLE-SMD, DISC-75MB-1, 0, -1.
2. `@ENTER-DIRECTORY` (same name, device, unit).
3. `@CREATE-USER ORACLE-SMD:SYSTEM`   (syntax: [directory:]user; `@CREATE-USER SYSTEM(ORACLE-SMD)` is refused as illegal character)
4. `@GIVE-USER-SPACE ORACLE-SMD:SYSTEM,500`  (without this, @CREATE-FILE answers NO MORE PAGES AVAILABLE FOR THIS USER: a new user has quota 0)
5. `@CREATE-FILE (ORACLE-SMD:SYSTEM)TEST:DATA 3`
6. `@RELEASE-DIRECTORY ORACLE-SMD`
7. `@COPY-FILE (ORACLE-SMD:SYSTEM)COPY1:BATC,(PACK-ONE:SYSTEM)PED-J-PATCH-I:MODE` into a file made with `@CREATE-FILE ... COPY1:BATC 0` (destination first, source second), then release again.

### What each step wrote (MEASURED)

| Step | Pages that changed | What changed |
|---|---|---|
| 1 CREATE-DIRECTORY | page 0 and the 3 bitmap pages only (43 non-zero bytes in the whole image) | Extended Info Block: checksum, capacity 36945 (0x9051) at 0x07DC, flag word 0, system number 0. Master block: name "ORACLE-SMD" + 0x27, object pointer 0, user pointer 0, bit file pointer type 0 block 36942, unreserved 36941. Bitmap: page 0 used; pages 36942 to 37119 used. |
| 2 ENTER-DIRECTORY | page 0 only, 4 bytes | flag word 0x8000, system number 210 (0xD2, this machine's CPU number), new checksum. Nothing else. |
| 3 first CREATE-USER | pages 0, 36940, 36941, 36944 | user file index block at 36940 (one pointer, type 0, to 36941), user data page 36941 holds the SYSTEM entry, user pointer in the master block = indexed, 36940; unreserved 36941 -> 36939; two more bitmap bits (36940, 36941). |
| 4 GIVE-USER-SPACE | the user entry | reserved (bytes 28-31) = 500. |
| 5 first CREATE-FILE | pages 0, 36938, 36939, 36941, 36944 | object file index block at 36938 (one pointer to 36939), object data page 36939 holds the file entry, object pointer = indexed, 36938; the file is 3 CONTIGUOUS pages at 36935-36937 (just below the object index block); unreserved 36939 -> 36438 after the 500 quota and the object pages. |
| 6 RELEASE-DIRECTORY | pages 0 and 36941 | flag word back to 0 (system number 210 stays), checksum updated, and the USER ENTRY's pages_used written: 4 (= 3 data pages + 1 object-entry page). Until the release, pages_used on disk was 0: SINTRAN keeps it in memory. |
| 7 indexed file by COPY-FILE | pages 36864, 36934, 36939, 36941, 36944 | file entry COPY1:BATC: pointer type 1 (indexed), index block 36934, pages-in-file 1, bytes-1 = 357; the one data page is at 36864 (far from the index block); user pages_used 4 -> 6 (= +1 data page +1 INDEX BLOCK). |

### Findings that change what the libraries assume

1. **CREATE-DIRECTORY alone writes ONLY the label, the Extended Info Block and the bit file.** The user file, the object file and every other structure are created LAZILY: the user file at the first CREATE-USER, the object file at the first CREATE-FILE.
   (NDInsight create-directory.md says CRDIR writes all three structures; the measurement says it does not.)
2. **Placement with the answer -1 ("end of disc")**: bit file at 36942 = declared capacity 36945 minus the 3 bitmap pages. Then user file index block = bit file address - 2 (36940), user data page 36941; object file index block = bit file address - 4 (36938), object data page 36939. So the "override" rule in create-directory-placement.md (object = BA-4, user = BA-2) is confirmed for the object and user index blocks.
   The answer 0 / empty (the mid-disc formula 9 x floor(floor(pages/2)/9)) and an explicit octal address have NOT been measured yet.
3. **Quota rule, confirmed by SINTRAN's own writes**: user pages_used = data pages + index blocks + the user's object-entry pages. 3-page contiguous file: 3 + 1 = 4. Adding a 1-page indexed file: 4 + 1 + 1 = 6. The libraries' rule "index blocks are never charged" (ndfs-c/src/filesystem.c count_real_data_pages_in_object and fsck Phase 4) is WRONG for files made by SINTRAN.
4. The new user entry: flags 0x81, name "SYSTEM" + 0x27, created date at bytes 20-23 (ND timestamp of the moment), default file access 0x03FF at bytes 40-41, password 0, last entered 0, enter count 0, directory index 0, user index 0.
5. The new file entry: header 0x8000, name + 0x27, type "DATA" + 0x27 (type field), next/previous version = its own object index (0x0000 for slot 0, 0x0001 for slot 1), access 0x04F7, file type flags 0x0010 for a contiguous file and 0x0008 for an indexed file, created date, bytes-in-file stored as size minus 1 (0xFFFFFFFF = 0 bytes), pointer type 0 (contiguous, block = first page) or 1 (indexed, block = index block).
6. A contiguous 3-page file is allocated downward from just under the object index block (pages 36935-36937); an indexed file's data page and index block need not be neighbours (36864 and 36934).
7. In the object file's first data page, bytes 2000-2047 contain stale label bytes (name ORACLE-SMD, capacity 0x9051): SINTRAN wrote that page from a buffer that still held page 0. Real SINTRAN images have ordinary file entry bytes there. Reproducing the stale bytes is not needed.
8. The bitmap above the capacity: after CREATE-DIRECTORY, pages 36945 to 37119 (175 pages above the declared capacity) are marked used besides the bit file's own 3 pages. Why exactly up to 37119: UNKNOWN (37120 = 2320 x 16, the end of a 16-bit bitmap word; not proven to be the reason). Earlier images (M-103: all 1455 above capacity; K-100: none) differ, so this region depends on the SINTRAN version and on history.

## Runs 2 to 13: other bit file answers, other disk types (MEASURED, same method)

System disks used (copies): Winchester WD0-SINTRAN-M.IMG for SMD, floppy and SCSI targets; the M-version SMD image BIGDISK0-M-103.IMG for Winchester targets (an SMD target must be created from a
Winchester-booted system and a Winchester target from an SMD-booted system, because only one of the two controllers can hold the system disk). Every run: CREATE-DIRECTORY, ENTER-DIRECTORY, CREATE-USER, GIVE-USER-SPACE 500, CREATE-FILE of 3 pages, RELEASE-DIRECTORY, STOP-SYSTEM.

| Run | Target (blank image) | Answer to the bit file prompt | Capacity written | Bit file | Object index / data | User index / data | 3 page file |
|---|---|---|---|---|---|---|---|
| 1 | SMD DISC-75MB-1, 38400 pages | -1 | 36945 | 36942 (3 pages) | 36938 / 36939 | 36940 / 36941 | 36935-36937 |
| 2 | same | 0 | 36945 | 18468 | 18476 / 18477 | 18478 / 18479 | 36942-36944 |
| 3 | same | 40000 octal (16384) | 36945 | 16384 | 16380 / 16381 | 16382 / 16383 | 36942-36944 |
| 4 | Winchester DISC-74MB-1, 36864 pages | 0 | 36396 | 18198 | 18204 / 18205 | 18206 / 18207 | 36393-36395 |
| 5 | SMD DISC-70MB-1, 34765 pages (file = capacity) | 0 | 34765 | 17379 | 17388 / 17389 | 17390 / 17391 | 34762-34764 |
| 6 | SMD DISC-38MB-1, 18486 pages (file = capacity) | 0 | 18486 | 9243 (2 pages) | 9242 / 9245 | 9246 / 9247 | 18483-18485 |
| 9, 13 | floppy FLOPPY-DISC-1, 154 pages | 0 | none written | 72 (1 page) | 76 / 77 | 78 / 79 | 151-153 |
| 10 | floppy FLOPPY-DISC-1, 616 pages | 0 | none written | 306 (1 page) | 316 / 317 | 318 / 319 | 613-615 |
| 12 | SCSI DISC-SCSI-1, 64656 pages + the real last page | 0 | 61036 | 30510 (4 pages) | 30506 / 30507 | 30508 / 30509 | 61033-61035 |

Rules read from the table:
1. **The bit file address.** Answer -1 = "end of disc": address = capacity - number of bitmap pages (36945 - 3 = 36942). Answer 0 = default: address = 9 x floor(floor(capacity/2) / 9) (SMD 75MB 18468, Winchester 18198, SMD 70MB 17379, SMD 38MB 9243, floppy 616: 306, floppy 154: 72, SCSI 30510).
   Any other number = that octal address, used as is. The default formula is exactly the one in the older notes (create-directory-placement.md); the measurement confirms it on six disk types.
2. **Object and user blocks with an explicit address or -1**: user index block = address - 2, object index block = address - 4 (index block first, data page right after it).
3. **Object and user blocks**: a free-page search going down from (BA-1) OR 15, see the section "Where the user file and object file go". That explains why the distance differs per disk type and why SCSI and the explicit and -1 answers end up below the bit file. It is neither a table nor a formula in capacity or geometry.
4. **Pages above the declared capacity** (hard disks only): right after CREATE-DIRECTORY the bitmap marks used every page from the capacity up to the next multiple of 256, if the device is longer: SMD 75MB 36945-37119 (37120 = 145 x 256), Winchester 36396-36607 (36608 = 143 x 256), SCSI 61036-61183 (61184 = 239 x 256). When the file ends at the capacity (runs 5, 6) nothing is marked. Floppies: nothing.
5. **Floppies get NO Extended Info Block at all** (all zero: no checksum, no capacity, no flag word, no system number). The device size comes from the floppy controller (a 154 page image gives bit file 72, a 616 page image 306).
6. **Master block "Unreserved Pages" (offset 0x1C)** = capacity - 1 (page 0) - bitmap pages - 2 (user file: index block + data page) - 1 (object file: index block) - the sum of the user quotas. This matched in all nine runs (e.g. SMD 75MB: 36945 - 1 - 3 - 2 - 1 - 500 = 36438; SCSI 61036 - 1 - 4 - 2 - 1 - 500 = 60528; floppy 154 with quota 50: 154 - 1 - 1 - 2 - 1 - 50 = 99).
   A quota larger than the unreserved pages is refused with "NOT SO MUCH SPACE UNRESERVED IN DIRECTORY" (a 500 page quota on a 154 page floppy).
7. **The system number** written by ENTER-DIRECTORY is the machine's CPU number (210 for the WD0-SINTRAN-M system, 103 for BIGDISK0-M-103), kept after RELEASE-DIRECTORY; the flag word 0x8000 is cleared by the release.
8. **Page 0 bytes 0-1999 (the boot area) stay all zero** in every run, for every disk type. CREATE-DIRECTORY, ENTER, CREATE-USER, CREATE-FILE and RELEASE never write boot code.

### Where the user file and object file go: the rule (READ in the SINTRAN M code by an analysis agent, then checked against all rows)

Source: segment 006-S3FS of the M carve (NDInsight tools/sintran-segment-carver/versions/M-VSX-500/segments/006-S3FS.bin), routine RSBPG at M06 address 053020 octal, called from WUSER (user data page, then WINDX for the user index block) and WOBJE (object data page, then WINDX for the object index block).
Nothing in RSBPG, RSTPG, TSTDW, TSTUP, WUSER, WOBJE or WINDX reads the capacity (except to compute BA), the sectors per track, the sectors per cylinder or the disk layout table. The agent's full report is not in the repo; its conclusions:

    H   = BA - 1                       (BA = the bit file address)
    top = H OR 15                      (the highest page of the 16-page bitmap word that holds H)
    user data page   = highest FREE page at or below top
    user index block = the next free page below that
    object data page = the next free page below that     (made at the first CREATE-FILE)
    object index blk = the next free page below that

"Free" = bit 0 in the bitmap; the bit file's own pages and page 0 are already marked used, so they are skipped. The start is always the TOP bit of the word (a quirk of the code: the bit number of H is lost before it is used).
This reproduces all measured rows: SMD 75MB (BA 36942, 18468, 16384), Winchester 74MB (18198), SMD 70MB (17379), SMD 38MB (9243: 9247, 9246, 9245 then 9242, because the 2-page bit file sits at 9243-9244), floppy 616 (306: 319, 318, 317, 316) and 154 (72: 79, 78, 77, 76),
and SCSI (BA 30510: the bit file fills 30510 and 30511, so the pages are 30509, 30508, 30507, 30506). Rows with only the label pointers readable (Winchester 28MB, 45MB, SMD 140MB) also match.
SCSI has no special case. The rule is the same for every disk type and only needs BA and the bitmap.

NOT settled (no measurement): fewer than 4 free pages in the home word (the code then spirals to the next word up, the next word down, ...), and the second user or object data page (the 33rd user or file).
Older SINTRAN K and L made the pages in a different way (routine RSPAG: the highest free page of the 256-page block that holds BA, e.g. 18687 for BA 18468), which gives the +206 to +232 distances of the real older packs.

### SCSI needs a valid last page (MEASURED)
- An all-zero 64656 page image: CREATE-DIRECTORY fails with a disk transfer error (status 5020B, function 66B, at disk address 0) and the log shows the SCSI READ CAPACITY function (42B) before it.
- The same image with only its LAST page copied from the real scsi-1.img: every command works (run 12). SINTRAN did not change that page.
- Images of 40000 and 70000 pages with the same last page: the same transfer error again. So the last page must match the size of the disk; its 38 non-zero words (the first is 036 octal, a second group of words starts at word index 512) are not decoded.
  Consequence for ndtool: a SCSI image of any size other than 64656 pages cannot be made bootable/usable until that record is decoded. UNKNOWN. The capacity 61036 written to the label did not come from the image size (64656) in any simple way; where it comes from is UNKNOWN (probably the same record).

### Winchester images that are NOT usable as layout evidence
Runs with Winchester DISC-28MB-1 and DISC-45MB-1 (blank images of exactly 13648 and 22032 pages): the emulator's Winchester disk maps SINTRAN's page numbers to file positions with its own geometry (the data landed at file pages 18184+ and 14688+ while the pointers say 6822 and 11016, and one file grew to 18217 pages). Their label pointers (page 0) are right but the file layout is not linear. Only the 74MB Winchester (run 4) is linear.

### Still to run
- Boot area for floppies and hard disks (@DEVICE-FUNCTION ... DUMP-BOOTSTRAP, and where SINTRAN writes the hard disk boot page at cold start), the second user and the second file (second object block, user numbering).
- DISC-140MB (the F and S formats), the sub-divided names (2-70MB, 4-70MB, 6-70MB, 2-75MB, 3-75MB, 2-225MB), 225MB and DISC-30MB/60MB/90MB.
- Booting a created image.

## Entering images made by ndtool (MEASURED, nd100x emulator, real SINTRAN M)

Image made with ndtool --create, attached to a second controller, then in SINTRAN: ENTER-DIRECTORY ORACLE <device name> 0, DIR, CREATE-FILE (ORACLE:SYSTEM)TEST:DATA 3,
RELEASE-DIRECTORY. SMD images were run from a Winchester system disk, Winchester images from an SMD system disk. After each run ndtool -t showed the file and ndtool --fsck said CLEAN.

| Image | Device name | DIR result (unreserved / unused / total pages) |
|---|---|---|
| smd75 template | DISC-75MB-1 | 18466 / 36937 / 36945 |
| winchester74 template | DISC-74MB-1 | 18191 / 36388 / 36396 |
| floppy360 template | FLOPPY-DISC-1 | 0 / 148 / 154 |
| scsi126 template | DISC-SCSI-1 | 30510 / 61027 / 61036 |
| DISC-38MB | DISC-38MB-1 | 9237 / 18479 / 18486 |
| DISC-70MB | DISC-70MB-1 | 17376 / 34757 / 34765 |
| DISC-288MB-R | DISC-288MB-1-R | 70183 / 140377 / 140391 |
| DISC-288MB-F | DISC-288MB-1-F | 70183 / 140377 / 140391 |
| DISC-450MB-F | DISC-450MB-1-F | 110274 / 220565 / 220584 |
| DISC-450MB-N | DISC-450MB-1-N | 110274 / 220565 / 220584 |
| DISC-74MB, 45MB, 28MB, 23MB, 21MB, 14MB | DISC-nn-1 | all entered, DIR and CREATE-FILE worked (sizes 36396, 22032, 13648, 11016, 10728, 6912) |

A blank 75 MB image (no directory) gives "DIRECTORY NOT ON SPECIFIED UNIT" on ENTER-DIRECTORY: that is the expected failure and the control for the table above.

### What went wrong first, and why (MEASURED / read from the emulator source)
- DISC-288MB and DISC-450MB images made at the directory size gave "Disk transfer error" and "BIT-FILE TRANSFER ERROR" on ENTER-DIRECTORY (and the same errors earlier for CREATE-DIRECTORY).
- SINTRAN sends the controller a cylinder, head and sector worked out from the layout entry (DT table) of the disk type; the controller turns that into a file position with its own
  head and sector counts. The SMD controller of nd100x chose its counts from the image file length with three fixed windows and a 75 MB default, so 450 MB was never selectable and
  a 288 MB image only if its length was 301,989,888 bytes (+4096). The Winchester controller always used the Micropolis 1325 counts (8 heads, 9 sectors), right only for 74 MB.
  Both are changed in nd100x (src/devices/smd, src/devices/winchester): all SINTRAN layouts are known, the choice is by exact layout length, else a default per size class, and a
  formatN key in the [controller.smd.N] / [controller.wd.N] section forces the layout (documented in nd100x docs/MACHINE-CONFIG-DESIGN.md).
- The image must also be as long as the full layout (cylinders * heads * sectors * 1024 bytes), because SINTRAN reads and writes above the directory size. ndtool --create DISC-xx now
  makes that length where the layout is known (the layout_pages column).
- A 288 MB F image padded to 460,000,000 bytes (the size rule then picks DT450): without formatN the same transfer errors; with format0 = 288MB-F it works. So the controller layout does
  matter for SINTRAN, and an image of unknown format needs the key.
- ndfs-c bug found by this: the library sized the bit file from the image length, SINTRAN sizes it from the directory size (ceil(directory size / 16384) pages). For DISC-288MB-F
  (device 153576 pages, directory 140391) that is 9 pages, not 10, and fsck reported "1 blocks referenced by files but marked FREE". Fixed in ndfs-c/src/filesystem.c (load, write, fsck).

### The format letters in the device names (from documents)
Sources (NDInsight): SINTRAN-COMMANDS-REFERENCE (device name list), ND-211067-1-EN (DISC-TEMA-I, new discs), ND-30.003.007 SINTRAN III System Supervisor (disk type menu), and the carved layout table.
- F = fixed ("DISC-288MB-1-F: 288-Megabyte disk controller 1 (fixed)"); R = removable; E = EMD (the drive is EMD-9720); N = NEC (the drive is NEC D2352A).
- S: DT135 (DISC-140MB-S), DT285 (DISC-288MB-S) and DT450 (DISC-450MB-S) are named in the layout table comments, but no device name list found has an -S name. What S stands for: UNKNOWN.
- Layout entry per name (layout table comments, read, not run): 288MB-R DT288 (19 heads x 18 sectors x 823 cylinders), 288MB-S DT285 (24 x 17 x 711), 288MB-F DT300 (24 x 18 x 711),
  288MB-E DT310 (10 x 26 x 1217), 288MB-N DT290 (10 x 35 x 823), 450MB-S DT450 (24 x 26 x 711), 450MB-F DT460 (24 x 27 x 711), 450MB-N DT470 (19 x 31 x 760).
  Run in the emulator with real SINTRAN: 288MB-R, 288MB-F, 450MB-F, 450MB-N. Not run: 288MB-E, 288MB-N, and the S formats.
- The System Supervisor manual says the alternatives for a disk type "differ only in disk storage capacity": DISC-288MB-R (DISC-225MB-R / DISC-3-75MB / DISC-4-70MB-R), DISC-288MB-F (DISC-4-70MB-F),
  DISC-450MB (DISC-2-225MB / DISC-6-70MB-F).

## Later runs: all SMD and Winchester formats

The placement of the bit file and of the four user and object pages was measured again on 16 SMD and Winchester formats (answer 0) and is written up in `docs/CREATE-DIRECTORY-PLACEMENT.md`, with the code that does it. The rules for everything after the first file (indexed files, contiguous files, expanding, deleting, quota) are in `docs/NDFS-ALLOCATION-RULES.md`.

## Images made by ndtool, entered by real SINTRAN M

Method: boot SINTRAN M from a system disk on the OTHER controller type (Winchester system for SMD targets, SMD system for Winchester targets), attach the image as unit 0, then `ENTER-DIRECTORY ORACLE`, `DIR ORACLE`, `CREATE-FILE (ORACLE:SYSTEM)TEST:DATA 3`, `RELEASE-DIRECTORY ORACLE`. After that the image was read back with `ndtool -t` and `ndtool --fsck`. MEASURED.

| Image | Result |
|---|---|
| `smd75`, `winchester74`, `floppy360` (154 pages), `scsi126` | entered, listed by `DIR`, file created, fsck clean |
| SMD 38, 70, 288 R, 288 F, 450 F, 450 N (made by `ndtool --create DISC-xx`) | the same |
| Winchester 74, 45, 28, 23, 21, 14 (made by `ndtool --create DISC-xx`) | the same |
| SMD 288 R/F and 450 F/N made at the directory size only (capacity, not the full layout length) | `Disk transfer error` / `BIT-FILE TRANSFER ERROR` |

Why the last row failed, and what was changed in the emulator (nd100x commits `1089303` and `22ae035`):

* SINTRAN sends cylinder, head and sector, worked out from the disk layout table (DT table) of the format named in the device name (for example `DISC-288MB-1-F` is DT300: 24 heads, 18 sectors, 711 cylinders). The controller turns that back into a file position with its own geometry.
* The SMD controller picked its geometry from the image file size with a few fixed rules and could not select the 450 MB geometry; the Winchester controller always used the Micropolis 1325 geometry (8 heads, 9 sectors).
* The controllers now know the layouts DT014 to DT470, choose the largest layout that fits in the file, and an `.ini` key `format0 = 288MB-F` (or `DT300`) in the controller section forces the layout of unit 0 where the file length cannot tell formats apart. Proof: a 288 MB F image padded to 460 MB was refused by SINTRAN without the key and worked with it.
* An image must be at least as long as its full layout (cylinders x heads x sectors x 1024 bytes). `ndtool --create DISC-xx` now writes that length (`ndtool --create list` shows it as IMG PAGES).

What the letters at the end of the 288 MB and 450 MB names mean is not known. They select different formats: 288MB-R is DT288 (19 heads, 18 sectors, 823 cylinders), 288MB-F is DT300 (24, 18, 711), 288MB-E is DT310 (10, 26, 1217), 288MB-N is DT290 (10, 35, 823), 450MB-F is DT460 (24, 27, 711), 450MB-N is DT470 (19, 31, 760). The letter-to-layout match comes from the comments in the DT table.

Nothing was booted: only `ENTER-DIRECTORY`, `DIR`, `CREATE-FILE` and `RELEASE-DIRECTORY` were run on these images.
