# Disk sizes, declared capacity and boot sectors: findings for ndtool --create

Written 2026-10-18. Purpose: collect what is known (and what is not) before ndtool --create learns
to make Winchester and SCSI images, put boot code on page 0 and use valid disk sizes.
Related notes in this folder: FSCK-ANALYSIS-AND-PLAN.md, FSCK-FIX-PLAN.md, KERNEL-VERIFIED-CORRECTIONS.md, NDFS-FORMAT.md.

## 0. How to read the labels

| Label | Meaning |
|---|---|
| CHECKED-HERE | The bytes or the file in the named place were read during this work. |
| AGENT-REPORTED | A research agent read it and reported it; the report was read but the check was not repeated. |
| FROM THE DOC | Taken from a document, nobody checked it against bytes. |
| INFERRED | A reading of what the evidence suggests. Not proven. |
| UNKNOWN | Nobody has found it out. |

References to other projects use the project folder name only (this file must not hold machine paths):
"NDInsight" = the NDInsight project folder, "RetroFS" = the RetroFS project folder, "RonnyTest" and "ND" = the image folders
on drive F (RonnyTest\HDLC1 etc. and ND\SINTRAN-M - 2026\HDD etc.).

## 1. Declared capacity ("Pages Available", Extended Info Block)

Where: page 0 of the image, byte offset 0x07DC, 32 bit big endian (SINTRAN words 1756B-1757B). See NDFS-FORMAT.md.

Who writes it (CHECKED-HERE, in the SINTRAN L carve, NDInsight tools/sintran-segment-carver/versions/L-VSX-500/re/segments-ref/006-S3FS/006-S3FS.asm):
- At directory CREATION: `CRDIR` (136741B) calls `WDIRS` (040221B) at 137403B (pointer word at 137440B is 040221). `WDIRS` zeroes the
  8 extended-info words (040232B-040240B), calls `GSIZE` (037101B) through the pointer word at 040261B, stores the result in
  words 6-7 (`STD ,X 6` at 040246B), then calls `WXDIR` (037702B) through the pointer word at 040263B.
- At ENTER: `CHDSI` (037763B) reads the stored value and compares it with the `GSIZE` result. It writes it back only when the
  checksum is bad or zero (the same zero-fill, store, `WXDIR` path). (CHECKED-HERE for the compare; the write-back is
  described in NDInsight SINTRAN/Filesystem/code-logic/enter-directory.md.)
- What `GSIZE` returns: INFERRED (AGENT-REPORTED) = the 32 bit size from the name-table entry of the disk unit. Formula for a unit whose
  size is 0 (SCSI): UNKNOWN.
- What `CHDSI` does when the stored value differs from the `GSIZE` figure on a good checksum: UNKNOWN. AGENT-REPORTED, INFERRED only:
  it may write the label value into the in-core name table instead of refusing.

Relation to the boot sector (AGENT-REPORTED, numbers CHECKED by the agent on the images named in section 7): the 9 word geometry block in the
boot sector holds (words per sector 512, sectors per track, sectors per cylinder, a cylinder count, the pool cylinder). Pool cylinder times
sectors per cylinder divided by 2 equals the declared capacity: SMD 821 x 90 / 2 = 36945, Winchester 1011 x 72 / 2 = 36396.

## 2. The SINTRAN disk layout table (DTxxx entries)

Field list (CHECKED-HERE, NDInsight SINTRAN/NPL-SOURCE-2/S3VS-6.SYMB lines 625-668, version J, 1985): SECWO words per sector, SECTR sectors
per track, SECSY sectors per cylinder, MAXCY value of max cylinder, POLSY value of first cylinder in pool, REFOR format type (0 = track or
no reallocation, 10 = plus test sector address, 20 = sector reallocation), RESCY first reserved cylinder, ALTFO alternative format (address of
another DTxxx or 0), DISPN index. Element size DILEZ = 11 octal = 9 words.

Where the full text of all 24 entries is: NDInsight SINTRAN/NPL-SOURCE/s3vs-4.symb lines 2951-2978 (second copy at 19584-19611).
Version J (S3VS-6.SYMB) defines only 6 entries: DT014, DT021, DT023, DT045, DT028, DT074.
The same 24 entries exist as bytes in the L07 carve (segment 044-S3IDPIT.bin, table at 031041B). AGENT-REPORTED: the decoded bytes equal the
s3vs-4.symb text in 24 of 24 entries; M06 has the same 24 rows; K has 18 rows (no DT014 to DT074).

Decoded entries (AGENT-REPORTED; octal/decimal; SECWO is 1000 octal = 512 words = 1024 bytes in all of them, so 1 page = 2 sectors;
"pages(POLSY)" = POLSY x SECSY / 2; "pages(MAXCY+1)" = (MAXCY+1) x SECSY / 2; whether MAXCY is inclusive is UNKNOWN - no code that reads MAXCY was found):

| DT | Source comment | SECTR | SECSY | MAXCY | POLSY | REFOR | RESCY | ALTFO | heads (SECSY/SECTR) | pages(POLSY) | pages(MAXCY+1) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| DT014 | DISC-14MB | 14/12 | 44/36 | 577/383 | 1124/596 | 0 | 0 | 0 | 3 | 10728 | 6912 |
| DT021 | DISC-21MB | 14/12 | 44/36 | 1134/604 | 1124/596 | 0 | 0 | 0 | 3 | 10728 | 10890 |
| DT023 | DISC-23MB | 11/9 | 33/27 | 1475/829 | 1462/818 | 0 | 0 | 0 | 3 | 11043 | 11205 |
| DT045 | DISC-45MB | 11/9 | 66/54 | 1475/829 | 1462/818 | 0 | 0 | 0 | 6 | 22086 | 22410 |
| DT028 | DISC-28MB | 11/9 | 33/27 | 1775/1021 | 1763/1011 | 0 | 1776/1022 | 0 | 3 | 13648 | 13797 |
| DT074 | DISC-74MB | 11/9 | 110/72 | 1775/1021 | 1763/1011 | 0 | 1776/1022 | 0 | 8 | 36396 | 36792 |
| DT030 | DISC-30/60/90MB | 22/18 | 22/18 | 1466/822 | 1465/821 | 10/8 | 0 | 0 | 1 | 7389 | 7407 |
| DT037 | DISC-38MB | 22/18 | 132/90 | 632/410 | 0 | 10/8 | 0 | 0 | 5 | 0 | 18495 |
| DT070 | DISC-70MB | 22/18 | 132/90 | 1466/822 | 1434/796 | 10/8 | 1465/821 | 0 | 5 | 35820 | 37035 |
| DT075 | DISC-75MB | 22/18 | 132/90 | 1466/822 | 1465/821 | 10/8 | 0 | 0 | 5 | 36945 | 37035 |
| DT140 | DISC-140MB-F | 22/18 | 264/180 | 1466/822 | 1441/801 | 10/8 | 1465/821 | DT135 | 10 | 72090 | 74070 |
| DT135 | DISC-140MB-S | 21/17 | 252/170 | 1466/822 | 1462/818 | 20/16 | 1465/821 | DT140 | 10 | 69530 | 69955 |
| DT160 | DISC-2-75MB | 22/18 | 264/180 | 1466/822 | 1465/821 | 10/8 | 0 | 0 | 10 | 73890 | 74070 |
| DT288 | DISC-288MB-R | 22/18 | 526/342 | 1466/822 | 1465/821 | 10/8 | 0 | 0 | 19 | 140391 | 140733 |
| DT285 | DISC-288MB-S | 21/17 | 630/408 | 1306/710 | 1303/707 | 20/16 | 1305/709 | DT300 | 24 | 144228 | 145044 |
| DT300 | DISC-288MB-F | 22/18 | 660/432 | 1306/710 | 1260/688 | 10/8 | 1305/709 | DT285 | 24 | 148608 | 153576 |
| DT450 | DISC-450MB-S | 32/26 | 1160/624 | 1306/710 | 1303/707 | 20/16 | 1305/709 | DT460 | 24 | 220584 | 221832 |
| DT460 | DISC-450MB-F | 33/27 | 1210/648 | 1306/710 | 1260/688 | 10/8 | 1305/709 | DT450 | 24 | 222912 | 230364 |
| DT310 | DISC-288MB-E | 32/26 | 404/260 | 2300/1216 | 2267/1207 | 20/16 | 2277/1215 | 0 | 10 | 156910 | 158210 |
| DT470 | DISC-450MB-N | 37/31 | 1115/589 | 1367/759 | 1363/755 | 20/16 | 1366/758 | 0 | 19 | 222347 | 223820 |
| DT290 | DISC-288MB-N | 43/35 | 536/350 | 1466/822 | 1460/816 | 20/16 | 1465/821 | 0 | 10 | 142800 | 144025 |
| DTSSS | SCSI DISK | all zero except SECWO | | | | | | | | | |
| DTOD1 | OPTICAL DISK (RS=1024) | 36/30 | 0 | 0 | 0 | 0 | 0 | 0 | | | |
| DTOD2 | OPTICAL DISK (RS=2048), SECWO 2000 octal | 36/30 | 0 | 0 | 0 | 0 | 0 | 0 | | | |

Which family each entry belongs to (AGENT-REPORTED, from the generation conditionals in s3vs-4.symb; line numbers there):
- Winchester (ST-506, device 500 octal): DT014, DT021, DT023, DT045, DT028, DT074 (conditional before DT014, line 2949).
- SMD ("big disc", device 1540 octal): DT030, DT037, DT070, DT075, DT140, DT135, DT160, DT288, DT285, DT300, DT450, DT460, DT310, DT470, DT290 (line 2958).
- SCSI: DTSSS; optical: DTOD1, DTOD2 (conditional at line 2975).
- Floppy, streamer, tape: no DT entry. DT010, DT033, DT066 are named in the selector list but defined nowhere (menu types 33 MB and 66 MB have no table).

## 3. The list of valid SMD and Winchester sizes (name table)

Source: NDInsight tools/sintran-segment-carver/versions/L-VSX-500/segments/101-S3DNAM.bin and M-VSX-500/segments/101-S3DNAM.bin. CHECKED-HERE (own decode: names are 8 words of packed ASCII ending in an apostrophe, the 32 bit number follows in the next two words): both versions hold the same 15 non-zero sizes plus size 0 for all SCSI names.
Each disk unit name carries one fixed 32 bit number (INFERRED to be pages: it matches the capacities that real disks declare). The set:

6912, 7389, 8000, 10728, 11016, 13648, 18486, 22032, 34765, 36396, 36945, 69530, 110292, 140391, 220584 pages; floppy units 154 (no DT entry). 7389 is DISC-30MB, DISC-60MB and DISC-90MB (the agent's summary list had left it out).

Examples: DISC-14MB = 6912, DISC-21MB = 10728, DISC-23MB = 11016, DISC-28MB = 13648, DISC-45MB = 22032, DISC-38MB = 18486,
DISC-70MB = 34765, DISC-74MB = 36396, DISC-75MB = 36945, DISC-140MB = 69530, DISC-225MB = 110292, DISC-288MB = 140391, DISC-450MB = 220584.
The same number is used for one nominal size in several formats (R, F, E, N, S). The word "valid" is the agent's reading: no code was found that
refuses a label capacity that is not in this list. All 112 SCSI names and the optical names carry size 0.

The K release notes say the same in plain words (CHECKED-HERE, NDInsight SINTRAN/Release-Documentation, K-version file, line 13371):
"For SMD and Winchester disks, the directory size is contained in the device name (for example DISC-70MB-1, DISC-74MB, DISC-2-225MB-1-F).
On SCSI disks, the directory size is set to the total disk capacity divided by the number of subunits on the device."
Section 22.8 of that file: `@SET-MASS-STORAGE-SIZE` sets a SCSI directory size in pages (decimal); the example for "SMD 450 MB" is 220584 pages,
which equals the 450 MB entry in the table above.

### 3a. Which drive each disk type name is (CHECKED-HERE: SINTRAN III System Supervisor manual ND-30.003.7, Appendix C "Device names", table 27; sizes from the carved name table)

Name format `DISC-x-yyMB-z-a`: z = controller number (1 to 4, only 1 or 2 on small types), x = number of directories on a sub-divided disk, yy = size of ONE directory in MB, a = F (fixed) or R (removable); the carve also has E and N. The manual says the MACM "disk type" menu groups the names that are the
same hardware and differ only in capacity (for example MACM type 11 is "initialized for DISC-38MB-1, DISC-70MB-1, DISC-75MB-1", and type 4 DISC-30MB has the alternatives DISC-60MB and DISC-90MB).

| Name (base) | Drive model in the manual | Pages (carved name table, L and M) | Created by real SINTRAN M in the emulator? |
|---|---|---|---|
| DISC-10MB | CDC 9427 Hawk cartridge | not in the table | not tried |
| DISC-14MB | CDC 9410 Finch | 6912 | not tried |
| DISC-16MB | Micropolis 1304, ST506 | 8000 | not tried |
| DISC-21MB | CDC 9410 Finch | 10728 | not tried |
| DISC-23MB | Micropolis 1304, ST506 | 11016 | not tried |
| DISC-28MB | Micropolis 1325, ST506 | 13648 | label written (capacity 13648), but the emulator's Winchester stores the pages non-linearly: layout not usable as evidence |
| DISC-30MB | CDC 9448 CMD big cartridge | 7389 | NO: SINTRAN asked "FIXED(F) OR REMOVABLE(R)" and then halted with "System malfunction. Sintran halt in ERRFATAL" |
| DISC-33MB | CDC 9762 SMD, ND-IO | not in the table | not tried |
| DISC-38MB | CDC 9762 SMD, ND-IO | 18486 | YES (run 6) |
| DISC-45MB | Micropolis 1304/1325, ST506 | 22032 | label written, non-linear in the emulator |
| DISC-50MB | CDC 9448 CMD | not in the table | not tried |
| DISC-60MB | CDC 9762 SMD, ND-IO | 7389 (same as DISC-30MB and DISC-90MB) | not tried |
| DISC-70MB | CDC 9710 RSD | 34765 | YES (run 5) |
| DISC-74MB | Micropolis 1325, ST506 | 36396 | YES (run 4, 36864 page file) |
| DISC-75MB | CDC 9762 SMD | 36945 | YES (runs 1 to 3) |
| DISC-90MB | CDC 9448 CMD | 7389 | not tried |
| DISC-140MB-F, DISC-2-70MB-F (sub-divided) | Fujitsu M2322K | 69530, 34765 | 140MB: label written, data non-linear in the emulator |
| DISC-2-75MB (-F), DISC-3-75MB | CDC 9730 MMD (150 MB); CDC 9766 SMD (225 MB) | 36945 | not tried |
| DISC-225MB-R | CDC 9766 SMD | 110292 | not tried |
| DISC-288MB-R, DISC-4-70MB-R | CDC 9766 SMD | 140391, 34765 | NO: "OUTSIDE DEVICE LIMITS" (the emulator's SMD cannot address it) |
| DISC-288MB-F, DISC-4-70MB-F | CDC 9715 FSD 340 | 140391, 34765 | not tried |
| DISC-288MB-E, DISC-4-70MB-E | CDC 9720 EMD 368 | 140391, 34765 | not tried |
| DISC-450MB-F, DISC-2-225MB-F, DISC-6-70MB-F | CDC 9715 FSD 515 | 220584, 110292, 34765 | 450MB: NO, "OUTSIDE DEVICE LIMITS" |
| DISC-450MB-E, DISC-2-225MB-E, DISC-6-70MB-E | NEC D3525-H | 220584, 110292, 34765 | not tried |
| names with N (DISC-288MB-N, DISC-450MB-N, ...) | not in this manual edition | 140391, 110292, 220584, 34765 | not tried |
| FLOPPY-DISC-1, -2 | floppy controller, SS/SD or DS/DD, 5 1/4 and 8 inch | 154 | YES for 154 and 616 page images (runs 9, 10, 13) |
| DISC-SCSI-1 and up, DISC-OPTICAL-1 to 4 | SCSI, optical | size 0 in the table (set from the drive) | SCSI YES only for the exact 64656 page image with its real last page (run 12) |

So the emulator can only prove the 75, 70 and 38 MB SMD packs, the 74 MB Winchester, the floppies and the one SCSI image. Everything else in this table is VALID as a SINTRAN device name (it is in the manual and the carved name table) but has NOT been created
in this session. DISC-30MB, DISC-60MB and DISC-90MB are CMD or ND-IO cartridge types, not the CDC 9762 SMD pack family that nd100x emulates; creating DISC-30MB halts SINTRAN in the emulator.

## 4. SCSI sizing

- No table of valid SCSI sizes was found (AGENT-REPORTED): DTSSS has zero geometry; all SCSI names have size 0.
- Checks the kernel does make (AGENT-REPORTED, NPL file IP-P2-SCSI-DISK.NPL): the block size must be a power of two; a control record page is
  checksummed and must hold a plausible partition count; transfers are limited by a value UHLIM taken from that record.
- Capacity formula for SCSI (declared capacity in the label): UNKNOWN. Experiment that would settle it: in the emulator break at 040025B in segment
  006-S3FS (right after the GSIZE call, during @ENTER-DIRECTORY on a SCSI image) and record A and D for two SCSI images of different sizes.
- K release notes (CHECKED-HERE, lines 13064-13090): SCSI supported drives 60, 125, 150, 310 and 630 MB (5 1/4 inch and 8 inch) and a 1 GB optical
  drive; a magnetic disk can be divided into 2, 3, 4, 5, 6 or 8 subunits, an optical disk into 2 to 8. A subunit should be at least 125 MB to use the disk.
- The SCSI figures 61036 (capacity) and 64656 (device) in KERNEL-VERIFIED-CORRECTIONS.md section 8 are FROM THE DOC, not checked. AGENT-REPORTED: 64656 equals the
  emulator's default SCSI geometry (898 x 8 x 18 / 2), so it is an emulator figure, not a SINTRAN figure.

## 5. When did a size appear in which SINTRAN version?

Symbol lists exist for J, K03, L07 and M06 (NDInsight SINTRAN/NPL-SOURCE/SYMBOLS/<version>/). CHECKED-HERE: which DT names each list contains (octal address):

| Table | J | K03 | L07 | M06 |
|---|---|---|---|---|
| DT014 DT021 DT023 DT045 | 036637 036650 036661 036672 | - | 031041 031052 031063 031074 | 035756 035767 036000 036011 |
| DT028 DT074 | - (text defines them) | - | 031105 031116 | 036022 036033 |
| DT030 DT037 DT070 DT075 | - | 012261 012272 012303 012314 | 031127 031140 031151 031162 | 036044 036055 036066 036077 |
| DT140 DT135 DT160 DT288 DT285 DT300 DT450 DT460 | - | present (012325 .. 012424) | present (031173 .. 031272) | present (036110 .. 036207) |
| DT290 DT310 DT470 DTSSS | - | present | present | present |
| DTOD1 DTOD2 | - | - | 031347 031360 | 036264 036275 |

Why this does not date a size (CHECKED-HERE):
- The version J source (S3VS-6.SYMB lines 630-633) already names DT030 to DT460 in its selector list, but defines only six entries.
- The J symbol list lacks DT028 and DT074 although the J text defines them; K03 lacks DT014 to DT074 although L07 and M06 have them.
  A symbol list shows what one generated system contained, not what the version can do.
- AGENT-REPORTED: the bytes of the table are in segment 044-S3IDPIT.bin of the K, L and M carves; K has 18 rows, L and M have 24 rows.

What the release notes give (CHECKED-HERE): SCSI disks and streamers first appear in the K release notes (line 898: "The new SCSI controller and corresponding
magnetic disk and streamer tape drives are now supported"). The J, L, M and N release files were searched only for lines with MB, disk type, SCSI and SMD; they
show controller counts and "you will get a list of disk types", but no list of new sizes was found there. Not done yet: read the J, L, M and N release
notes around new disk drives, and SINTRAN-III-Release-History.md, then write "size, first version seen, evidence".

## 6. Real Winchester images (CHECKED-HERE, read only)

Found through the `.ini` files in the ND folders (SINTRAN-M - 2026\RetroCore.ini: `attach wd0 HDD/WD0-SINTRAN-M.IMG`; OLD\SINTRAN-M-OLD\retrocore.ini: `Attach HD0 HDD/WD0-M.img`).

| Image | File size | Pages | Declared capacity | Pool geometry in boot sector | Used bits at or above capacity | Highest used bit |
|---|---|---|---|---|---|---|
| ND\OLD\SINTRAN-M-OLD\HDD\WD0-M.IMG | 74465280 | 36360 | 36396 (36 more than the file) | 512, 9, 72, 1021, 1011, 0, 1022, 0, 13 | 0 (36 pages are not in the file) | 33278 |
| same folder, WD0-M-CLEAN-INSTALL.IMG | 74465280 | 36360 | 36396 | same | 0 | 33278 |
| same folder, wd0-empty.img | 74465280 | 36360 | all zero (not formatted) | none | 0 | none |
| ND\SINTRAN-M - 2026\HDD\WD0-SINTRAN-M.IMG | 78643200 | 38400 | 36396 | same | 2004 (all of them) | 38399 |

- Neither 36360 nor 36864 pages is a measured real-drive size in the ND\OLD and ND\SINTRAN-M - 2026 images above: the old WD0-M images are 36 pages shorter than the
  capacity they declare; the 2026 image is a 38400 page file. These were made by emulator or tools.
- CHECKED-HERE on the Micropolis 1325 image (ND\HDD\1325.img on the D drive copy of the ND folder): 75497472 bytes = exactly 36864 pages, declared capacity 36396 (checksum OK),
  object file index block 18428, user file index block 18430, bit file 18198, boot geometry block 512, 9, 72, 1021, 1011, 0, 1022, 0, 13, no used bit above the capacity.
  So 36864 IS measured on a real Winchester image. ndfs-py and ndfs-ts already use these numbers; ndfs-c now does too.
- CHECKED-HERE on the SCSI image scsi-1.img (same folder): 132415488 bytes = 64656 pages, declared capacity 61036 (checksum OK), object 30716, user 30718, bit file 30510 (= 9 x floor(floor(61036/2)/9)),
  4 bitmap pages, no used bit above the capacity, boot geometry block 512 then zeros and 30. The LAST page (64655) is not empty: 38 non-zero words, first word 036 octal; the same 38 words (word sum 54294)
  are in SCSI-K.img and tor-disk.img (RonnyTest\HDLC1); scsi-125.img has 40 non-zero words with a different content. Pages 64640 to 64654 are all zero. What those words are is UNKNOWN
  (the NPL source mentions a "control record" in the last block; 036 octal = 30 is also the index of the SCSI disk layout entry DTSSS, which is only a coincidence of numbers until shown otherwise).
- 36396 as capacity is confirmed by the table (DT074 / DISC-74MB), by the geometry (1011 x 72 / 2) and by the images.
- 36864 (Winchester device size in KERNEL-VERIFIED-CORRECTIONS.md and in RetroFS) is now measured on 1325.img (above). AGENT-REPORTED, INFERRED: 36864 = 1024 cylinders x 72 sectors / 2;
  the table says MAXCY 1021 and RESCY 1022, so the extra 2 cylinders have no source text.
- 38400 (SMD 75MB device size) and 36360 (Winchester device size in image_creator.c): no source found (AGENT-REPORTED). 38400 pages is exactly 75 MiB.

Spare pages above the declared capacity, by SINTRAN version, on SMD images (CHECKED-HERE, count of used bits at or above capacity 36945):

| SINTRAN version of the image | Images | Used bits above capacity |
|---|---|---|
| K | RonnyTest HDLC1 BIGDISK0-K-100.IMG, ND SINTRAN-K bigdisk0-K.img | 0 |
| L | RonnyTest HDLC2 BIGDISK0-L-102.IMG, HDLC1 BIGDISK0-L2-100.IMG, ND SINTRAN-L - 2026 BIGDISK0-L.IMG | exactly 1: page 36952 |
| M | RonnyTest HDLC3M BIGDISK0-M-103.IMG | all 1455 |
| M (Winchester) | ND SINTRAN-M - 2026 WD0-SINTRAN-M.IMG | all 2004 |

INFERRED: the M version marks everything above the declared capacity as used when it creates or first enters the directory; K does not; the single L page 36952
is 36944 with the lowest 8 flipped, which is what the old byte-swapped bitmap bug (fixed 0.0.5) would set, so it is probably old-tool damage and not SINTRAN behaviour.
Who sets those bits is UNKNOWN. The fsck rule built on this (spare area counted separately, INFO line) is in ndfs-c/src/filesystem.c, Phase 3 of ndfs_fsck.

## 7. Boot sectors

The catalogue exists (not in this project): NDInsight tools/boot-floppy/boot-sectors/ (18 hard disk page-0 specimens, one floppy stream, the unpatched 192 word load
program) with the analysis in NDInsight tools/boot-floppy/DISC-BOOT-SECTOR-ANATOMY.md and DISC-BOOTSTRAP.md, and the RetroFS copy of four specimens in
RetroFS src/RetroFS.NDFS/Creation/NdfsBootBlobs.cs. All of this section is AGENT-REPORTED unless marked.

Page 0: bytes 0-1999 boot area (1000 words), 2000-2015 Extended Info Block, 2016-2047 Master Block. A hard disk boot area is raw ND-100 code that starts with PIOF; no header, no checksum.
A floppy boot area is a byte-expanded BPUN/FLOMON stream.

Who writes the boot area on a real system: SINTRAN itself at COLD-START and RESTART-SYSTEM (routine FILL2 in PH-P2-OPPSTART.NPL, per DISC-BOOTSTRAP.md). It copies the 192 word load
program plus a 744 word swap driver and patches about 12 parameter words (NOBLK, KLHDE, KLIOX, DYBLS, XSWTP, KLRC1, LDRAD, ADR2B, YSWTY, KBLSZ for SCSI). The agent compared the
shipped load program with a specimen: 176 of 192 words equal; the 16 that differ are exactly the patch words. How page 0 gets onto a brand new, never booted pack: NOT FOUND.

Families (first 8 hex of the hash of bytes 0-1999; donor names as the source gives them):

| Hash | Controller | Donors | Layout generation (page word 0o27) |
|---|---|---|---|
| 296ed770, 80ba323f | SMD | BIGDISK0-K, c3-k-bd-clean | K-like 176546 |
| ec962fc2, 059ac510 | SMD | BIGDISK0-L, c3-k-bd, BIGDISK0 | L-like 176536 |
| 86e648bd, 21382ad9 | SMD | BIGDISK0-M, a.IMG | M-like 000315 |
| 0ab983b4, 05afdb0f, c6f30aba | Winchester | 1325.img, WD0.img, WD0-L, disk-dump | L-like |
| d9df77b6 | Winchester | tingo_raw_debug.img | K-like |
| e2da6491 | Winchester | WD0-M.IMG, sintran_m, HD0, COPYTEST | M-like |
| d90b55c5, 70c3d994, 47f5dc0d, 9411182f | SCSI | scsi-1, scsi-k, tor-disk, HD00_imaged, SI1 | K-like |
| a47ce5c4 | SCSI | sintran_iii_m05_st31200n | M-like |
| 1da707c1, 8329440e | unknown | BIGDISK0-H, WD, BDH; disk-dump-2k | other |

What differs (AGENT-REPORTED, checked by the agent on the specimens):
- Word 2 does NOT identify the controller (older RetroFS notes say it does; counter-examples: SMD 296ed770 has 175345, SCSI a47ce5c4 has 170412). Word 2 follows the SINTRAN build.
  The reliable markers: the class word YSWTY (SMD 1, Winchester 2, SCSI 3, at page word 0o67 in K-like and 0o77 in L-like layouts) and literal IOX instructions versus IOXT for SCSI.
  boot_loader.c in this library classifies by those instruction forms (read, CHECKED-HERE).
- The whole driver differs per controller: 810 words differ between an SMD and a SCSI specimen, 892 between SMD and Winchester.
- Device number: SCSI needs one word (0o213); SMD needs 32 instruction edits and Winchester 25.
- Geometry block, 9 words anchored by 512: SMD 512, 18, 90, 822, 821, ...; Winchester 512, 9, 72, 1021, 1011, 0, 1022, 0, 13; SCSI 512 and zeros except slot +8 = 30.
  The meaning of slots +3, +5, +6 and +8 is UNKNOWN. The block follows the drive type, not the file length (same Winchester block in files of 74.4, 75.5 and 78.6 MB).
- No image size appears as a word anywhere in the 18 boot areas; the size is only in the Extended Info Block.
- Build-dependent words: LDRAD and ADR2B (body B addresses) differ between SINTRAN builds (38 words between two K specimens, all addresses).
- Floppy: the 1.2 MB real floppies carry a different loader payload from the 360 KB one (907 of 1000 payload bytes differ). RetroFS uses the 360 KB loader for both templates.

What RetroFS does today (read by the agent): picks one of four frozen 2000 byte blobs (SMD, Winchester, SCSI, floppy) and copies it to byte 0; it patches nothing. A patcher
exists only as a plan in RetroFS docs/ND-BOOT-SECTOR-PLAN.md. RetroFS templates: Smd75MB capacity 36945 / file blocks 38400; Winchester74MB 36396 / 36864; Scsi126MB 61036 / 64656.

Open before ndtool can make bootable images (UNKNOWN unless noted):
1. Does a copied donor boot area boot a new image? The load program reads from disk address 0:0o400 ("first block of SEGFILE 0"); nobody has tested a created image booting.
2. Which donor generation (K, L or M layout) must match which SINTRAN build on the disk. SINTRAN rewrites page 0 itself at every cold start, so a wrong page may be repaired (INFERRED).
3. The M-like layout is not decoded; RetroFS embeds three different generations (K-like SMD, L-like Winchester, K-like SCSI).
4. Geometry for drive types other than the three donors.
5. Where the SCSI block size patch lives; whether one SCSI donor works for every SCSI size (INFERRED yes, no size word found).
6. Floppy: which loader for which size.
7. boot_loader.c in this library was written against the older "BPUN" reading of hard disk boot areas; whether it misreports the 2000 byte raw areas was not re-checked.

## 7a. What FLOMON really is (CHECKED-HERE on real floppy images, read only)

Question: is FLOMON boot code in page 0, or a file that the boot sector loads? Answer from the bytes: it is boot code in page 0. It is NOT an empty marker, which is what
NDFS-FORMAT.md, boot-sector.md in NDInsight and boot_loader.c say (they read the three words after the "!" as address, count and checksum, all zero).

How the page was read: bytes 0 to 999 of the boot area are a BYTE stream; each byte sits in its own 16-bit word (high byte 0) on the bootable floppies (about 745 of the 1000
words have a non-zero high byte on those images; the others hold zero). Read that way, the stream is:

    preamble "0/2" CR LF "2", then "!" (0x21 at stream byte 6), then a binary record:
    address 2 bytes = 0, word count 2 bytes = 64, 128 data bytes, checksum 2 bytes, 2 more bytes.

The stored checksum equals the 16 bit sum of the 64 data words on every bootable floppy tested:
- 154 page floppy N-10-102-I.img: checksum 0xC3D6 stored and computed.
- VSXL1.IMG (616 pages), N-250306K05--01D.img (640 pages), VSXM1.IMG (616 pages): checksum 0x287E stored and computed.
The last two bytes of the record read 246 (VSXL1, N-250306K05), 64 (VSXM1) and 94 (the 154 page floppy): meaning UNKNOWN (the BPUN action code is 2 bytes in the NDInsight description; these do not look like 0 or 1).

What the 64 word record is (disassembled with NDInsight tools/boot-floppy/tools/nd100_disasm.py, instruction table from the nd100-definitions project): a first-stage FLOPPY
reader. It talks only to device addresses 1560 to 1567 octal (the floppy controller window: IOX 1560 input, 1562 input/status, 1563, 1565, 1567 output), starts a controller
command, waits for status, then reads words from IOX 1560 into memory with a loop (STA 0,B,X; JNC) and jumps through a pointer (JMP I to address 000070). Which floppy sector it reads and
how many words: UNKNOWN (not traced).

After the record the stream continues to byte 999: 857 more bytes, of which 746 are non-zero (VSXM1): text-like bytes with the parity bit set, spaces (0x40) and what looks like names ("DFL'", "LC-IE" with parity).
That is probably the floppy monitor itself, which the first stage brings in; NOT proven.

Floppies without this stream: data floppies have a zero boot area (250360M04-XX-01D, ND-PATCH-SIN-M, FLOPPY.IMG which is all 0x40), or raw 16 bit words in every word (ND-10080J, ND-10337B, 210580B01, 211024E02: high byte
non-zero in all 1000 words, no "!" in the low bytes). The FLOMON byte stream is therefore on the system floppies only. Whether the raw-word ones are bootable by another route: UNKNOWN.

Consequences for ndtool: a bootable floppy needs the whole 1000 word boot area copied from a donor system floppy (VSXL1, VSXM1 style), not just a preamble and three zero words. The
DUMP-BOOTSTRAP statement in boot-creation.md (a BPUN file copied onto page 0; FROM THE DOC) agrees: the source is a file, FLOPPY-MONITOR:BPUN, whose bytes then sit in page 0. That the bytes in page 0 equal such a file
byte for byte: UNKNOWN (no FLOPPY-MONITOR:BPUN was compared). boot_loader.c in this library would report these floppies as FLOMON; that is a name only.

## 8. What was changed in ndfs-c and ndtool --create (CHECKED-HERE: built in a scratch folder; created images compared with the real-SINTRAN results)

ndfs_create_image (ndfs-c/src/image_creator.c) now follows the measured behaviour of real SINTRAN M; the old approximations are gone. See CREATE-DIRECTORY-ORACLE.md for the measurements. Summary:
- Capacity: floppy 154 and 616 pages (no Extended Info Block), SMD 75MB 36945 (file 38400), Winchester 74MB 36396 (file 36864), SCSI 126MB 61036 (file 64656, plus the real last page), custom = the page count, disk type names = their SINTRAN size.
- Bit file address from the answer: 0 = 9 x floor(floor(capacity/2)/9), -1 = capacity minus bitmap pages, n = n. Bitmap pages = ceil(capacity/16384). Page 0 and the bit file pages are marked used; on hard disks longer than the capacity the pages from the capacity to the next multiple of 256 are marked used.
- The four pages of the user file and object file are the first free pages going down from (BA-1) OR 15 (SINTRAN's RSBPG).
- Unreserved Pages = capacity - 1 - bitmap pages - 2 - 1 - the SYSTEM quota.
- Boot area: the real boot area for the template (boot_blobs.c), none, or your own (boot_data / --boot-from / --boot-file).
- ndtool: --create list, --create DISC-xx names, --bit-file, --boot, --boot-from, --boot-file. A custom size of 1000 pages or less gets no Extended Info Block.
- Tests: test_image_creator.c (new expected pointers, bit file answers, the formula, bits above the capacity, no Extended Info Block on floppies, boot code written and override, SCSI last page, disk type table), test_boot_loader.c (images made with NDFS_BOOTCODE_NONE because those tests inject their own boot area), test_fsck.c.

Compared with the real-SINTRAN images (user index, object index, bit file): SMD 75MB 18478/18476/18468, Winchester 74MB 18206/18204/18198, SMD 70MB 17390/17388/17379, SMD 38MB 9246/9242/9243, floppy 616 318/316/306, floppy 154 78/76/72, SCSI 30508/30506/30510:
all equal to what the real SINTRAN produced. The SYSTEM user quota is not SINTRAN's (SINTRAN creates a user with quota 0; ndtool gives capacity/2, or up to 500 for small disks).

Not done:
- ndfs-py and ndfs-ts still use the old approximations and have no SCSI template or disk type table.
- The library's user quota accounting (pages_used) still ignores index blocks and the object-entry page, which SINTRAN charges (CREATE-DIRECTORY-ORACLE.md); fsck Phase 4 has the same rule. A fresh image therefore has pages_used 0 where SINTRAN would show 1 for the object page.
- After the first file the library rewrites the master block's Unreserved Pages with its own free page count, not SINTRAN's formula.
- Booting a created image in the emulator; the boot areas are from different SINTRAN generations (K, L, K).
- The C# libraries (RetroFS and RetroIO) have not been changed.

## 9. Boot test of the embedded boot areas (2026-10-20)

MEASURED in the nd100x emulator, booting from a Winchester controller (`boot device = wd.0.0`) a copy of a real SINTRAN M
system disk (38400 pages) with bytes 0 to 1999 of page 0 replaced:

| Boot area in page 0 | Result after 90 seconds |
|---|---|
| the disk's own (hash e2da6491) | SINTRAN III RUNNING |
| the embedded Winchester area (donor 1325.img, hash 0ab983b4) | no console output at all |
| the embedded SMD area (hash 296ed770) | no console output at all |
| the embedded SCSI area (hash d90b55c5) | no console output at all |

Why (CHECKED by comparing the words): the boot area of this M system differs from all three embedded areas in 839 to 906 of
1000 words. The embedded areas are from older SINTRAN generations (K, L, K); the code is laid out one word apart from the
M layout, so the words that carry the disk-specific data (where the system is, the geometry block) are not at the same
place. An embedded boot area is therefore only usable on a disk whose system is of the same generation, and not on a
system copied from an M disk. What is NOT known: whether an image made by `ndtool` with a K or L system copied onto it
boots; nobody has put a system on a created image. The boot areas stay "not verified".

A created image cannot be booted at all as it is: it holds a boot area and an empty directory, but no SINTRAN. A real test of a
created disk means installing SINTRAN onto it from the installation floppies (the sections about floppies above) and then booting it.
The test in this section only shows that a boot area of one generation does not start a system of another generation.

To make a bootable created image the boot area has to come from a disk of the same SINTRAN generation as the system that
is copied onto it. `ndtool --create --boot-from IMAGE` does that (it copies bytes 0 to 1999 of page 0 of IMAGE).
