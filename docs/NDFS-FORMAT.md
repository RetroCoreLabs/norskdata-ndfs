# NDFS On-Disk Format Specification

Nord Disk File System (NDFS) is the filesystem used by Norsk Data minicomputers (ND-100, ND-110, ND-120) running the Sintran-III operating system. This document specifies the binary on-disk format.

## Fundamentals

| Property | Value |
|----------|-------|
| Page (block) size | 2048 bytes (1024 x 16-bit words) |
| Byte order | Big-endian (network order) |
| String terminator | `0x27` (ASCII single quote `'`) |
| Maximum name length | 16 characters (7-bit ASCII) |
| Maximum type length | 4 characters |
| Entry size | 64 bytes (user and object entries) |
| Entries per page | 32 |
| Maximum users | 256 |
| Maximum friends per user | 8 |
| System blocks (reserved) | 0-6 |

## Page 0 Layout

Page 0 (first 2048 bytes of the image) contains three structures:

```
Offset      Size    Content
0x0000      1024    Boot sector (BPUN, FLOMON, or raw binary)
0x0400      1600    Reserved / unused
0x07D0      16      Extended Info Block (hard disks only)
0x07E0      32      Master Block
```

## Master Block (32 bytes at offset 0x07E0)

```
Offset  Size  Field              Format
0x00    16    Directory Name      ASCII, terminated by 0x27
0x10    4     Object File Ptr     BlockPointer (big-endian)
0x14    4     User File Ptr       BlockPointer (big-endian)
0x18    4     Bit File Ptr        BlockPointer (big-endian)
0x1C    4     Unreserved Pages    uint32 big-endian
```

### BlockPointer (4 bytes, big-endian)

```
Bits 31-30: Pointer Type
  00 = Contiguous (sequential pages from blockId)
  01 = Indexed (index block with up to 512 data page pointers)
  10 = Sub-Indexed (sub-index -> index blocks -> data pages)
  11 = Reserved (invalid)

Bits 29-0: Block ID (30-bit LBA page address)
```

A BlockPointer is valid when `blockId > 0` and `type != Reserved`.

### Pointer Type Usage

| Structure | Typical Type |
|-----------|-------------|
| Object File | Indexed (01) or Sub-Indexed (10) |
| User File | Indexed (01) |
| Bit File | Contiguous (00) |

## Extended Info Block (16 bytes at offset 0x07D0)

Present on hard disks only. Floppies (FLOMON format) do not have valid extended info.

```
Offset  Size  Field              ND Word    Format
0x07D0  2     Checksum           1750       uint16 big-endian
0x07D2  2     Reserved 1         1751       uint16 (must be 0)
0x07D4  2     Reserved 2         1752       uint16 (must be 0)
0x07D6  2     Reserved 3         1753       uint16 (must be 0)
0x07D8  2     Flag Word          1754       uint16 big-endian
0x07DA  2     System Number      1755       uint16 big-endian
0x07DC  4     Pages Available    1756-1757  uint32 big-endian
```

### Checksum Calculation

The checksum (word 1750B) is a plain **16-bit ADDITIVE SUM of the seven words that follow
it** (words 1751B–1757B), truncated to 16 bits:

```
checksum = (Reserved1 + Reserved2 + Reserved3
          + FlagWord + SystemNumber
          + PagesHi + PagesLo) & 0xFFFF
```

**Kernel-proven from both sides.** In the carved `006-S3FS` segment of a real SINTRAN III
image, the writer **`WXDIR` (37702B)** and the enter-directory validator **`CHDSI`
(37763B)** run the *identical* `ADD ,X 0` accumulation loop over these words:

```
037716  062000  ADD ,X 0        ; A += word   (ADD - not REXO)
037717  173401  AAX 1           ; next word
037720  146401  RADD AD1 0 DD   ; counter 1..7
037723  006000  STA ,X 0        ; store the sum in word 0
```

There is **no XOR**, and the system number is simply **one of the seven summed words** —
it is not added separately.

> **Correction.** Earlier revisions of this document specified
> `(pagesLo XOR pagesHi XOR FlagWord XOR Reserved1..3) + SystemNumber`. That is **wrong**.
> It reproduced the PACK-ONE sample only by coincidence: the only two summed words sharing
> a set bit are `FlagWord = 0x8000` and `pagesLo = 0x9051` (both bit 15). Under ADD the two
> bit-15s carry out past bit 15 and are lost; under XOR they cancel — identical low 16 bits.
> The XOR form breaks as soon as any two words share a *lower* set bit.
>
> Verified against three real disks: `SMD0.IMG` → `0x10B7`, `BIGDISK0-K.IMG` → `0x1051`,
> `scsi-1.img` → `0xC162`.

### Validation States

| State | Condition |
|-------|-----------|
| Valid | Stored checksum == calculated checksum (full 16 bits) |
| Invalid | No match |

> There is deliberately **no "ValidLowByteOnly" state**. The kernel writes (`WXDIR`) and
> compares (`CHDSI`) the **full 16-bit** value; no code path accepts a low-byte-only match.
> That state was a reader-side heuristic SINTRAN never produces, and it has been removed.

### Flag Word (1754B)

| Bit | Meaning |
|-----|---------|
| 15 (`0x8000`) | **Directory entered / in use** — a cross-system mount interlock. `CHDSI` (37763B) *sets* it on enter (`BSET ONE 170`); `REENB` (40162B) *clears* it on release (`BSET ZRO 170`). |
| 0–14 | No known meaning on this SINTRAN revision. **Preserve verbatim.** |

A stored `0x8000` means the volume was **left entered** (not cleanly released) by the
system in the System Number word.

> **System Number 0 means NO OWNER recorded** — not "system zero". `CHDSI` (040113–040114)
> loads the stored number and, if it is zero, proceeds as if the directory is unowned. Never
> report a volume as "entered by system 0".

### Bad-checksum behaviour

SINTRAN does **not** refuse the mount on a bad checksum. `CHDSI` **zeroes the 8-word
extended-info block and rebuilds it** (writes the capacity, stamps the owner and flag, then
recomputes the checksum via `WXDIR`). The extended-info block is a *self-healing* record,
not a mount gate.

### FLOMON Detection

If all extended info fields are zero and a FLOMON boot signature is present in the boot sector, the image is a floppy. Extended info is marked invalid for floppies.

## User Entry (64 bytes)

```
Offset  Size  Field              Format
0       1     Flags              0x81 = valid user entry
1       1     Enter Count        Login counter (0-255)
2       16    User Name          ASCII, terminated by 0x27
18      2     Password           uint16 big-endian
20      4     Date Created       ND timestamp, big-endian
24      4     Last Date Entered  ND timestamp, big-endian
28      4     Pages Reserved     uint32 big-endian (quota)
32      4     Pages Used         uint32 big-endian
36      1     Directory Index    Multi-directory support
37      1     User Index         0-255
38      2     Reserved
40      2     Default File Access uint16 big-endian (default: 0x04FF)
42      6     Reserved / tracking (byte 47 = mxobl/acobl nibbles)
48      16    Friends            8 x 2-byte entries, big-endian
```

**Offsets verified against real images** (`DefaultFileAccess`@40 reads 0x04FF/0x03FF;
friends@48).

> **The BIT LAYOUTS above are carried by ND documentation, not by that check
> (clarified 2026-08-02).** Reading a value at an offset tests the offset, not a bit split:
> `0x04FF` is an equally readable number under any partition, so nothing there could have
> failed. The friend word (bit 15 exists, 12 directory, 11 common, 10 append, 9 write, 8 read,
> 7-0 index) is stated bit by bit in `ND-860228-2` appendix C and corroborated by
> `ND-30.003.007`'s FRIEND TABLE. The access tiers are 14-10 public / 9-5 friend / 4-0 own,
> five bits each, proved by ND's `002377` worked example. Loaders identify the owning user by the stored **User Index byte
(offset 37)**, not by physical position; users are placed at physical slot
`userIndex % 32` in page `userIndex / 32` (8 pages × 32 = 256 users max).

### User Flags

- Bit 7: Entry Used (must be 1)
- Bit 0: User/Object flag (1 = user entry)
- Valid user: `(flags & 0x81) == 0x81`

## User Friend Entry (2 bytes, big-endian)

```
Bit 15:     Entry used (1 = active)
Bits 14-13: Reserved
Bit 12:     Directory access
Bit 11:     Common access
Bit 10:     Append access
Bit 9:      Write access
Bit 8:      Read access
Bits 7-0:   Friend user index (0-255)
```

## Object (File) Entry (64 bytes)

```
Offset  Size  Field              Format
0       2     Header             word; bit15 in use, bit14 write-open, bit12 modified
2       16    Object Name        ASCII, terminated by 0x27 then NULs
18      4     Type               ASCII (max 4 chars), terminated by 0x27
22      2     Next Version       uint16 BE (object index of next version)
24      2     Prev Version       uint16 BE (object index of previous version)
26      2     Access Bits        uint16 BE (15-bit, see Access Permissions)
28      2     File Type Flags    uint16 BE: T P S I C A M L (Indexed=0x08, etc.)
30      2     Device Number      uint16 BE
32      1     File Type Code     0=DATA, 1=PROG, 2=SYMB, 3=TEXT
34      2     Object Index       uint16 BE = (UserIndex << 8) | fileEntry
                                 (byte 34 = owning user index; byte 35 = slot)
36      2     Current Open Count uint16 BE
38      2     Total Open Count   uint16 BE
40      4     Date Created       ND timestamp, uint32 BE
44      4     Last Date Read     ND timestamp, uint32 BE
48      4     Last Date Written  ND timestamp, uint32 BE
52      4     Pages in File      uint32 big-endian (data pages; excludes index)
56      4     Bytes in File - 1  uint32 big-endian (actual bytes = stored + 1)
60      4     File Pointer       BlockPointer, big-endian
```

**Important**:
- Bytes-in-file stores `actual_size - 1`; add 1 when reading.
- A single-version file is **self-referential**: Next = Prev = its own Object
  Index. Zeroed version pointers make SINTRAN report a broken chain (`;2`).
- **Object Index encodes ownership and physical position**: the high byte is
  the owning user, and the object file is partitioned so user *U* owns object
  slots `U*256 .. U*256+255` (index-block pointer slots `U*8 .. U*8+7`). SINTRAN
  derives the owner from the slot's physical position; a file must be written
  into its owner's region, not the first free global slot.
  **This describes ONE object block. A user may own up to 16** - see
  "Object blocks" below. The 256-per-user figure held for every pack this
  project had seen, but it is the DEFAULT, not the limit.
- Name/Type fields use a **single** `0x27` terminator followed by NULs (not a
  field padded with terminators).

## Bit File (Allocation Bitmap)

One bit per page. Stored contiguously starting at the Bit File Pointer's block ID.

### Bit Layout

```
byte_index = blockId / 8
bit_index  = blockId % 8
is_used    = (bitmap[byte_index] & (1 << bit_index)) != 0
```

### System Blocks

Blocks 0-6 are reserved for system use and must never be allocated for file data.

### Bitmap size vs. allocation window

These are **two different numbers** and must not be conflated:

- The **bitmap is sized to the PHYSICAL DEVICE** (e.g. 38,400 bits on a 75 MB SMD pack).
  Its popcount over the whole device is what a disk tool reports as used/free.
- The **allocation window is bounded by the DECLARED CAPACITY** (`PagesAvailable`, words
  1756B–1757B). On PACK-ONE that is 36,945, so the highest allocatable page is **36,944**.
  Pages 36,945–38,399 are the drive's **bad-sector spare region**: they stay `0` (free) in
  the bitmap yet are **never handed out**.

Verified on the real disk: page 36,944 is used; pages 36,945+ are all free and unreachable.

> **What "declared capacity" is, in plain words (added for ndtool --fsck).**
> It is one 32-bit number stored on page 0 of the image, in the Extended Info Block at byte
> offset `0x07DC` (SINTRAN words 1756B-1757B, "Pages Available"). Who writes it, and when:
> the entering routine `CHDSI` (37763B) READS it and compares it with a figure it derives from
> the drive geometry; it WRITES it only when the checksum is bad or zero (zero the 8 words,
> store the geometry figure, recompute the checksum) - both VERIFIED in the kernel bytes (see
> NDInsight SINTRAN/Filesystem/code-logic/enter-directory.md). It is also written when the
> directory is CREATED. Read in the carved kernel segment 006-S3FS disassembly
> (NDInsight tools/sintran-segment-carver/versions/L-VSX-500/re/segments-ref/006-S3FS/006-S3FS.asm):
> `CRDIR` (136741B) calls `WDIRS` (040221B) at 137403B (the pointer word at 137440B is 040221).
> `WDIRS` zeroes the 8 extended-info words (loop at 040236B), calls `GSIZE` (037101B) through
> the pointer word at 040261B, stores the result into words 6-7 (`STD ,X 6` at 040246B) and then
> calls `WXDIR` through the pointer word at 040263B. What `GSIZE` computes inside is NOT read
> yet (its source is the drive geometry, per the CHDSI description). What `CHDSI` does when the
> stored value differs from the geometry figure on a good checksum is also not recorded.
> The bitmap is as long as the physical device; the declared capacity
> says how far up SINTRAN may hand out pages. Everything from the capacity to the end of the
> device is the drive's spare area for bad sectors, so no file can own a page there.
>
> **The bits in that spare area are NOT the same on every image.** Measured (read only) on
> real images, all with 38,400 pages on the device:
>
> | Image | Declared capacity | Bits set in spare area |
> |---|---|---|
> | BIGDISK0-K-100.IMG | 36,945 | none (all free) |
> | BIGDISK0-M-103.IMG | 36,945 | all 1,455 |
> | WD0-SINTRAN-M.IMG | 36,396 | all 2,004 |
>
> Who sets those bits on the M-103 and WD0 images is **not known** (not checked in SINTRAN
> source, not traced). Because of this, `ndtool --fsck` does not call a used bit with no
> owner at or above the declared capacity an orphan. It counts them and prints one INFO line
> ("N pages at or above the declared capacity (C) are marked used in the bitmap"). A used bit
> with no owner BELOW the capacity is still reported as an orphan warning. The code is the
> `Phase 3` loop in `ndfs_fsck` (ndfs-c/src/filesystem.c); the regression test is
> `test_fsck_spare_area_marked_used_is_info_not_orphan` in ndfs-c/tests/test_fsck.c.

> **Always clamp the ceiling to the device size.** The real Winchester `WD0.img` declares a
> capacity of **36,396** pages in a file only **36,360** pages long. Without the clamp, a
> downward allocator would start at page 36,395 — which does not exist in the file.

### Allocation direction: HIGH → LOW

**SINTRAN allocates from the TOP of the volume downward**, not upward from block 7.

The scanner **`TESTP` (51355B)** only ever *decrements* its bitmap word index — `AAX -1` at
**51372B** and **51401B**, never `AAX 1` — bounded below by the block-7 floor:

```
051363  LDT ,B 10          ; T = the block-7 floor word
051364  SKP IF DX LST ST   ; while X >= floor ...
051372  173777  AAX -1     ; X-- : step to the NEXT-LOWER bitmap word
051401  173777  AAX -1     ; X-- : (the other scan path)
```

This matches the `@CREATE-FILE` rule that *"contiguous files are positioned in the highest
page addresses"*. The contiguous range reserve `RSPAG` (51120B) drives the same downward
`TESTP` scan.

> **Correction.** Earlier revisions of this library scanned **upward** from block 7. That
> produces the exact opposite layout to genuine SINTRAN. Reading an existing bitmap is
> direction-agnostic — it is the **allocation choice** that must descend.

## File Allocation

### Contiguous (Type 00)

Data pages are sequential starting at `blockId`:
```
Page 0: blockId + 0
Page 1: blockId + 1
Page N: blockId + N
```

### Indexed (Type 01)

The `blockId` points to an index block containing up to 512 BlockPointers (4 bytes each). Each pointer references a data page. Maximum file size: 512 pages (~1 MB).

```
Index Block (2048 bytes):
  [0-3]:     BlockPointer to data page 0
  [4-7]:     BlockPointer to data page 1
  ...
  [2044-2047]: BlockPointer to data page 511
```

### Sub-Indexed (Type 10)

The `blockId` points to a sub-index block containing up to 512 pointers to index blocks. Each index block contains up to 512 data page pointers. Maximum file size: 262,144 pages (~512 MB).

```
Sub-Index Block -> Index Block 0 -> Data pages 0-511
                -> Index Block 1 -> Data pages 512-1023
                -> ...
```

### Sparse Files

A BlockPointer with `blockId == 0` in an index block represents a sparse hole. When read, the corresponding page is filled with zeros. No disk space is allocated for sparse holes.

## User File Structure

The User File Pointer (from the master block) points to an **indexed** structure:

```
Index Block: up to 8 BlockPointers
  -> Data Page 0: User entries 0-31
  -> Data Page 1: User entries 32-63
  ...
  -> Data Page 7: User entries 224-255
```

Maximum 256 users (8 pages x 32 entries/page).

## Object File Structure

The Object File Pointer points to an **indexed** or **sub-indexed** structure:

```
Indexed:     Index Block -> up to 512 data pages (16,384 files max)
Sub-Indexed: Sub-Index -> up to 512 index blocks -> 262,144 data pages
```

**Per-user partition (critical):** the object file is partitioned by user.
User *U* owns the **8 consecutive index-block pointer slots** `U*8 .. U*8+7`,
i.e. 8 data pages × 32 entries = **256 object slots per user**, spanning object
indices `U*256 .. U*256+255`. A file's Object Index (offset 34) therefore
encodes `(U << 8) | fileEntry`, and a new file MUST be placed in a free slot of
its owner's region (allocating/linking that user's directory page on demand) —
not the first free global slot. Writing to a global slot puts the file in the
wrong user's region as far as SINTRAN is concerned.

### Object blocks — a user can own more than one region

**Corrected 2026-08-01.** The paragraph above describes a single **object
block**. A SINTRAN user area holds 256 files per object block, **one block by
default and up to 16 (4096 files) on version K**. The operator raises the
ceiling with `@GIVE-OBJECT-BLOCKS`; `@USER-STATISTICS` reports the result as
`MAXIMUM NUMBER OF FILES`. Blocks are allocated on demand as files are created.

The counts live in **user-entry byte 47, as two ZERO-BASED nibbles**:

| nibble | meaning |
|---|---|
| high | `MXOBL - 1` — maximum object blocks this user may have |
| low  | `ACOBL - 1` — object blocks actually allocated so far |

A default user therefore stores `0x00` (1 max, 1 allocated), which is why every
pack examined before this correction showed byte 47 = 0 and the extra structure
stayed invisible.

**Placement.** Object block *n* (0-based) for user *U* occupies pages:

```
n*512 + U*8  ..  n*512 + U*8 + 7
```

i.e. successive blocks of the same user are a **stride of 512 pages** apart —
exactly one index block. The object file is effectively a two-dimensional array
indexed by (block, user).

**The file number is therefore NOT the raw physical position:**

```
block      = page / 512
slot       = (page % 512 - U*8) * 32 + entryInPage
fileNumber = block * 256 + slot
```

Deriving the number from physical position alone is correct only for a user's
first block, and silently wrong for every file beyond it.

**Verified** on a live SINTRAN III K pack with a control group: user BIGMAN,
given 3 extra blocks and holding 482 files, stores byte 47 = `0x31` (MXOBL 4,
matching its reported maximum of 1024; ACOBL 2, matching ceil(482/256)) with its
entries at pages 64-71 and 576-583, while every other user on the same pack
stores `0x00`. Independently, the file `F0500` reports as `FILE 307` both to
SINTRAN and through the formula above.

Full evidence:
`NDInsight SINTRAN/XMSG/DOC/NDFS-OBJECT-BLOCKS-DECODED-2026-08-01.md`.

## Path Format

NDFS uses a flat hierarchy with users as virtual directories:

```
/                        Root (lists users)
/USERNAME                User directory (lists user's files)
/USERNAME/FILENAME:TYPE  File reference (colon separates name from type)
```

The colon (`:`) is the extension separator. A period (`.`) may also be used and will be converted to colon internally.

## ND Timestamp Format (32-bit)

```
Bits 31-26: Year (0-63, add 1950 for calendar year)
Bits 25-22: Month (1-12)
Bits 21-17: Day (1-31)
Bits 16-12: Hour (0-23)
Bits 11-6:  Minute (0-59)
Bits 5-0:   Second (0-59)
```

Valid year range: 1950-2013.

## Access Permissions (15-bit)

```
Bits 14-10: Public permissions  (5 bits)
Bits 9-5:   Friend permissions  (5 bits)
Bits 4-0:   Owner permissions   (5 bits)

Per tier (5 bits):
  Bit 4: Delete
  Bit 3: Execute/Common
  Bit 2: Append
  Bit 1: Write
  Bit 0: Read
```

Default: `0x04FF` (owner full, friends read/write, public read).

## Boot Sector Formats

The first 1024 bytes of page 0 may contain boot code in one of these formats:

### BPUN (Bootable Punched Tape)

```
ASCII Preamble:
  Octal digits terminated by CR (0x0D)
  B (start address) = value before last CR
  C (boot address) = value after last CR before '!'

'!' delimiter (0x21)

Binary Section:
  Address:  2 bytes big-endian (load address)
  Count:    2 bytes big-endian (word count)
  Data:     count * 2 bytes (program words)
  Checksum: 2 bytes big-endian (sum of all words)
  Action:   2 bytes big-endian (0 = execute, else stay in OPCOM)
```

### FLOMON (Floppy Monitor)

A BPUN variant where address=0 and count=0 after the `!` delimiter. Used on ND floppy disks.

### Binary (raw hard-disk bootstrap)

Raw ND-100 machine code with no BPUN/FLOMON markers. Detected by the **CPU opcode
signature**, not by a data heuristic.

A genuine bootstrap *must* disable interrupts (and usually paging) before it touches the
controller, so the **first word** is always one of exactly two opcodes:

| Opcode | Octal | Word | Meaning |
|--------|-------|------|---------|
| `PIOF` | 150405 | `0xD105` | interrupts **and** paging off |
| `IOF`  | 150401 | `0xD101` | interrupts off |

Nothing else can legally start boot code. The controller family is then taken from the
first I/O instruction:

| Controller | Signature |
|------------|-----------|
| SMD / ECC | literal `IOX` in the octal device window 1540–1547 / 1550–1557 / 540–547 / 550–557 |
| Winchester (ST-506 / MFM) | literal `IOX` in the octal window 500–517 |
| Floppy | literal `IOX` in the octal window 1560–1577 |
| SCSI (NCR-5386) | `IOXT` (octal 150415) — device number taken from the **T register** |

> **Correction.** Earlier revisions described Binary detection as *"checking for non-zero,
> non-uniform data in the first 1024 bytes."* That heuristic was wrong in both directions:
> it **false-negatived** `SMD0.IMG` (a real SMD bootstrap, reported as *None*) and
> **false-positived** `FLOPPY.IMG` (a space-filled `0x40` boot area, reported as *Binary*).
> The shipping code uses the opcode signature above.

> **Trap — do not "optimise" the controller scan.** Page 0 is boot **code followed by
> data**, and a word scan cannot tell them apart: any *data* word equal to `0150415`
> (`0xD10D`) is indistinguishable from an `IOXT` instruction. Treating "IOXT anywhere on the
> page" as an overriding SCSI signature therefore **misclassifies real Winchester disks**.
> The real `1325.img` (a Micropolis 1325, ST-506/MFM) has 23 genuine `IOX` instructions in
> the Winchester window across words 79–570, then 17 *data* words equal to `0xD10D` at words
> 714–929 — past the end of its code. Scan **in order and take the first signature**: the
> Winchester disk hits its real `IOX` first, and a genuine SCSI disk (which has *no*
> controller-window `IOX` at all) falls through to its early `IOXT`.

> **Boot region size.** "The first 1024 bytes are the boot sector" is a simplification. Live
> boot code can run all the way up to the extended-info block — i.e. bytes **0–1999**
> (words 0B–1747B). Do not assume structured data before offset `0x07D0`.

## Disk Image Templates

Geometries below are **measured from real ND disk images**. Every real drive reserves a
**spare (bad-sector remap) region**, so *File Blocks* (the physical device) is always
**larger** than *Pages* (the declared, usable capacity) — never smaller.

**Spare is a property of the DRIVE, not a percentage of capacity.** The same 36,396-page
capacity sits on different drives with different spare, so it can never be derived by
formula — templates must carry real measured geometry.

| Template | Pages (capacity) | File Blocks (device) | Spare | Obj Block | User Block | Bit Block |
|----------|------------------|----------------------|-------|-----------|------------|-----------|
| Floppy — SINTRAN format 0 | 154 | 154 | — | 149 | 151 | 153 |
| Floppy — SINTRAN format 17₈ | 616 | 616 | — | 611 | 613 | 615 |
| SMD 75MB (DISC-75MB) | 36,945 | 38,400 | 1,455 | 18,684 | 18,686 | **18,468** |
| Winchester 74MB (Micropolis 1325, ST-506/MFM) | 36,396 | **36,864** | 468 | 18,428 | 18,430 | 18,198 |

Notes:

- **Floppies: only two formats exist.** ND-60.128.5 (`SET-FLOPPY-FORMAT`) states the SINTRAN
  file system "can only be used with formats 0 and 17₈" — i.e. **154 and 616 pages**. Real
  156- and 640-page images are *padded* 154/616 files, not formats.
- **SMD bit block corrected** from 18,472 → **18,468** (see *Bit-file placement* below).
- **Winchester device corrected** from 36,360 → **36,864**. The old 36,360 was *smaller than
  the declared 36,396-page capacity*, so a created image's last 36 pages did not exist. The
  real drive is a Micropolis 1325 (1024 cyl × 8 heads, ST-506/MFM), measured at 36,864 pages
  = exactly 72.0 MiB across 7 real images.

### Bit-file placement (`ALBIT` 137517B)

SINTRAN branches on whether `@CREATE-DIRECTORY` was given an **explicit bit-file address** —
**not** on device size. (The old "small disk vs big disk" layout switch was wrong: a small
floppy created on the default path still lands mid-disk.)

**Default path:**

```
bit_file = 9 * floor(floor(declared_pages / 2) / 9)
```

i.e. `floor(pages/2)` rounded **down to a multiple of 9** (`ALBIT` 137526B–137532B: ÷2 → ÷9
→ ×9). Verified on every real disk: `36945 → 18468`, `616 → 306`, `61036 → 30510`. Plain
`pages/2` gives 18472 for PACK-ONE — off by 4.

The **9 is pages per track** (SMD: 18 sectors/track × 1024 B = 18,432 B = 9 pages of 2048),
which is exactly why `@CREATE-DIRECTORY` documents the bit file as starting "at a track
boundary". Note it is a hard-coded immediate (`SAT 11`), so SINTRAN rounds to 9 regardless
of the actual drive.

**Override path** (an address `BA` was supplied): `bit = BA`, `object = BA − 4`,
`user = BA − 2`.

The default-path **object/user base is approximate** — it comes from the `CRDIR` scan loop
(137173B–137352B) and has no clean closed form (empirically `bit+216` on SMD, `bit+206` on
SCSI, `bit+202` on floppy). Only bit-exactness with a SINTRAN-created image is affected; the
reader is pointer-driven and reads any placement correctly.

### The `|user − object| == 2` invariant

The object and user index blocks are always **exactly 2 blocks apart** — but **not always
`user = object + 2`**. On floppies and SMD/SCSI it is `+2`; on the real **Winchester**
`WD0.img` it is `object = 32771, user = 32769`, i.e. **`−2`**. Code that tests for `+2`
alone will falsely reject every Winchester volume.

### Block Placement Strategy

**Floppies**: System blocks near the end (`pages - 5`, `pages - 3`, `pages - 1`).

**Hard disks**: Bit file at 50% (`pages / 2`), object/user files at 85% (`floor(pages * 0.85)`).

### Spare Block Overhead

| Controller | Overhead |
|-----------|----------|
| SMD | 3.94% |
| SCSI | 5.93% |
| Winchester | 0% |
| Floppy | 0% |

## Persistence Order

When writing changes to disk, structures must be persisted in this order:
1. Bit File (allocation bitmap)
2. User File
3. Object File

## XAT Sidecar Files

### The Problem

NDFS files carry rich metadata that has no equivalent on modern filesystems: 3-tier access permissions (Own/Friend/Public), file type flags (Indexed, Contiguous, Allocated, etc.), ND-100 timestamps, user ownership, and file type codes (DATA, PROG, SYMB, TEXT). When a file is copied from an NDFS disk image to a Windows, Linux, or macOS filesystem, all of this metadata is lost. If the file is later copied back into an NDFS image, it arrives as a plain DATA file with default permissions -- the original identity is gone.

This is particularly problematic for archival work, where disk images are extracted for analysis and then reconstituted, and for development workflows where source files are edited on a modern system and then transferred back to an ND-100 emulator.

### The Solution: XAT Files

XAT (Extended Attribute) sidecar files preserve NDFS metadata alongside extracted data files. For every file extracted from an NDFS image, a companion `.xat` file is created containing the metadata as JSON. When the file is copied back, the `.xat` file is read and all metadata is restored.

```
NDFS image                Host filesystem
-----------               ---------------
SYSTEM/README:TEXT   -->   README.TEXT        (file data)
                          README.TEXT.xat    (NDFS metadata as JSON)
```

### File Format

XAT files use simple JSON with `ndfs.*` prefixed keys:

```json
{
  "ndfs.object_name": "README",
  "ndfs.type": "TEXT",
  "ndfs.user_name": "SYSTEM",
  "ndfs.user_index": 0,
  "ndfs.access_bits": 1279,
  "ndfs.file_type_flags": 8,
  "ndfs.file_type": 3,
  "ndfs.device_number": 0,
  "ndfs.next_version": 0,
  "ndfs.prev_version": 0,
  "ndfs.pages_in_file": 1,
  "ndfs.bytes_in_file": 1024,
  "ndfs.date_created": 0,
  "ndfs.last_read_date": 0,
  "ndfs.last_write_date": 0
}
```

### Properties

| Key | Type | Description |
|-----|------|-------------|
| `ndfs.object_name` | string | File name (max 16 chars, uppercase) |
| `ndfs.type` | string | File type extension (max 4 chars: DATA, PROG, SYMB, TEXT, MODE, C, etc.) |
| `ndfs.user_name` | string | Owner user name |
| `ndfs.user_index` | integer | Owner user index (0-255) |
| `ndfs.access_bits` | integer | 15-bit permission encoding (see Access Permissions above) |
| `ndfs.file_type_flags` | integer | Bit field: Terminal(0x01), Peripheral(0x02), Spooling(0x04), Indexed(0x08), Contiguous(0x10), Allocated(0x20), MagneticTape(0x40), Library(0x80) |
| `ndfs.file_type` | integer | Type code: 0=DATA, 1=PROG, 2=SYMB, 3=TEXT |
| `ndfs.device_number` | integer | Logical device number (restored on import) |
| `ndfs.next_version` | integer | Next version pointer (recorded only, NOT restored -- references object-table slots that change on import) |
| `ndfs.prev_version` | integer | Previous version pointer (recorded only, NOT restored) |
| `ndfs.pages_in_file` | integer | File size in 2048-byte pages |
| `ndfs.bytes_in_file` | integer | File size in bytes |
| `ndfs.date_created` | integer | Creation date (ND timestamp, see ND Timestamp Format) |
| `ndfs.last_read_date` | integer | Last read date (ND timestamp) |
| `ndfs.last_write_date` | integer | Last write date (ND timestamp) |

### Filename Convention

The XAT filename is the data filename with `.xat` appended:

```
README.TEXT      -->  README.TEXT.xat
PROGRAM.PROG     -->  PROGRAM.PROG.xat
data             -->  data.xat
```

### Round-Trip Workflow

1. **Extract**: Read file from NDFS, write data to host file, serialize metadata to `.xat`
2. **Edit**: Modify the data file on the host system (metadata in `.xat` is untouched)
3. **Import**: Read data file and `.xat` from host, write to NDFS with restored metadata

This preserves the file's identity across the round-trip: its permissions, type, ownership, and dates survive even though the host filesystem cannot represent them natively.
