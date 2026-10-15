# NDFS — kernel-verified corrections

**Path:** `docs/KERNEL-VERIFIED-CORRECTIONS.md`

This library was originally reverse-engineered from disk images **without** the producing
code. It has now been corrected against the **real SINTRAN III kernel** — the carved
`006-S3FS` filesystem segment of an L-VSX-500 system — plus a set of real ND disks.

Where this document and any older doc disagree, **this one wins**. Every claim below cites
a kernel routine (octal address) and/or real disk bytes.

**Kernel anchors** (octal, in `006-S3FS`): `WXDIR` 37702B (ext-info writer), `RXDIR` 37643B
(reader), `CHDSI` 37763B (enter/validate), `REENB` 40162B (release), `TESTP` 51355B (bitmap
scanner), `RSPAG` 51120B (range reserve), `ALBIT` 137517B (create-directory placement).

All three language bindings (**C**, **TypeScript**, **Python**) carry these changes.

---

## 1. Checksum is an ADDITIVE SUM, not XOR

```
checksum(1750B) = (r1 + r2 + r3 + flag + system_number + pages_hi + pages_lo) & 0xFFFF
```

Both the writer `WXDIR` and the validator `CHDSI` run the identical `ADD ,X 0` accumulation
loop over words 1751B–1757B. There is no XOR; the system number is simply one of the seven
summed words.

The old `XOR six words, then + system_number` form matched PACK-ONE **by coincidence**: its
only two summed words sharing a set bit are `flag = 0x8000` and `pages_lo = 0x9051` (both
bit 15), which cancel under XOR and carry out past bit 15 under ADD — same low 16 bits. It
fails as soon as any two words share a *lower* set bit.

Verified on three real disks: `0x10B7`, `0x1051`, `0xC162`.

**API:** `ndfs_mb_ext_checksum()` / `MasterBlock.computeExtChecksum()` /
`MasterBlock.compute_ext_checksum()`.

## 2. No "valid low byte only" state

The kernel writes and compares the **full 16 bits**. The `ValidLowByteOnly` /
`NDFS_CHECKSUM_VALID_LOW_BYTE` state was a reader-side heuristic SINTRAN never produces and
has been **removed** from the enum in all three languages.

## 3. Flag word (1754B) bit 15 = "directory entered"

`CHDSI` **sets** bit 15 on enter (`BSET ONE 170`); `REENB` **clears** it on release
(`BSET ZRO 170`). A stored `0x8000` means the volume was left entered and not cleanly
released. Bits 0–14 have no known meaning — **preserve them verbatim**.

**System number 0 means NO OWNER recorded** — not "system zero". `CHDSI` (040113–040114)
treats a zero system number as unowned. Never report "entered by system 0".

## 4. Bad checksum ⇒ self-repair, not rejection

`CHDSI` **zeroes and rebuilds** the 8-word block (writes capacity, stamps owner + flag,
recomputes the checksum) rather than refusing the mount. It is a self-healing convenience
record, not a mount gate.

## 5. Allocation is HIGH → LOW, bounded by the DECLARED CAPACITY

`TESTP` (51355B) only ever **decrements** its bitmap word index (`AAX -1` at 51372B and
51401B — never `AAX 1`), bounded below by the block-7 floor. This matches `@CREATE-FILE`:
*"contiguous files are positioned in the highest page addresses."* The old upward scan
produced the opposite layout to genuine SINTRAN.

Two distinct numbers, not to be conflated:

- The **bitmap is sized to the physical DEVICE** (38,400 on a 75 MB SMD pack).
- The **allocation window is bounded by the declared CAPACITY** (`pages_available`). On
  PACK-ONE the top allocatable page is **36,944**; pages 36,945–38,399 are the drive's
  bad-sector spare region — free in the bitmap, but **never handed out**.

**Always clamp the ceiling to the device size.** The real Winchester `WD0.img` declares a
capacity of 36,396 pages in a file only 36,360 pages long; without the clamp the allocator
starts at a page that does not exist.

**API:** `bit_file.alloc_ceiling` / `BitFile.allocCeiling` / `BitFile.alloc_ceiling`.

## 6. Bit-file placement (`ALBIT` 137517B)

SINTRAN branches on whether an **explicit bit-file address** was supplied — **not** on
device size. The old "small disk vs big disk" layout switch was wrong: a small floppy on the
default path still lands mid-disk.

**Default path:** `bit_file = 9 * floor(floor(pages / 2) / 9)` — `floor(pages/2)` rounded
**down to a multiple of 9** (ALBIT 137526B–137532B: ÷2 → ÷9 → ×9).

Verified: `36945 → 18468`, `616 → 306`, `61036 → 30510`. Plain `pages/2` gives 18472 for
PACK-ONE — **off by 4**.

The **9 is pages per track** (SMD: 18 sectors × 1024 B = 18,432 B = 9 pages of 2048), which
is why `@CREATE-DIRECTORY` says the bit file starts "at a track boundary". It is a
hard-coded immediate (`SAT 11`), so SINTRAN rounds to 9 regardless of drive.

**Override path** (address `BA` given): `bit = BA`, `object = BA − 4`, `user = BA − 2`.

The default-path **object/user base is APPROXIMATE** — from the `CRDIR` scan loop
(137173B–137352B), with no clean closed form (empirically `bit+216` SMD, `bit+206` SCSI,
`bit+202` floppy). Only bit-exactness with a SINTRAN-created image is affected; readers are
pointer-driven.

## 7. `|user − object| == 2` — NOT always `+2`

The object and user index blocks are always exactly **2 apart**, but the **direction
varies**: `+2` on floppies and SMD/SCSI, but `−2` on the real Winchester `WD0.img`
(`object = 32771, user = 32769`). **Testing for `+2` alone falsely rejects every Winchester
volume.**

## 8. Drive geometries — every drive reserves spare

| Drive | Capacity | Device | Spare |
|---|---|---|---|
| Floppy, SINTRAN format 0 | 154 | 154 | — |
| Floppy, SINTRAN format 17₈ | 616 | 616 | — |
| SMD 75 MB (DISC-75MB) | 36,945 | 38,400 | 1,455 |
| Winchester 74 MB (Micropolis 1325, ST-506/MFM) | 36,396 | **36,864** | 468 |
| SCSI 126 MB (ND-3201) | 61,036 | 64,656 | 3,620 |

- **The device is ALWAYS larger than the capacity** — never smaller. The old Winchester
  template had `file_blocks = 36360` against a declared capacity of 36,396: the created
  image's **last 36 pages did not exist**.
- **Spare is a property of the DRIVE, not a percentage of capacity.** The same 36,396-page
  capacity carries different spare on different drives, so it can never be derived by
  formula. Templates must carry real measured geometry.
- **Floppies: only two formats exist.** ND-60.128.5 (`SET-FLOPPY-FORMAT`) states the file
  system "can only be used with formats 0 and 17₈" — 154 and 616 pages. Real 156- and
  640-page images are **padded** 154/616 files, not formats.
- The Micropolis 1325 is rated 71.3 MB at 17 × 512 B sectors (= 34,816 ND pages). ND formats
  it with **1024-byte sectors at 9 sectors/track**: `1024 × 8 × 9 × 1024 = 75,497,472 B` =
  **36,864 pages** = exactly 72.0 MiB — the measured device size. *(Device size = measured
  fact; the cyl/head/sector fit = inference.)*

## 9. Boot detection — and a trap

Detection is by **CPU opcode signature**: word 0 is `PIOF` (octal 150405) or `IOF`
(octal 150401). FLOMON requires Address = Count = Checksum = **all three zero** after `!`.
The old *"non-zero, non-uniform data"* heuristic false-negatived `SMD0.IMG` and
false-positived the space-filled `FLOPPY.IMG`.

> **Do NOT scan the whole page for `IOXT` and treat it as an overriding SCSI signature.**
> Page 0 is **code followed by data**, and a word scan cannot tell them apart: any *data*
> word equal to `0xD10D` looks exactly like an `IOXT` instruction. The real `1325.img` (a
> Micropolis 1325 → **Winchester**) has 23 genuine `IOX` in the Winchester window at words
> 79–570, then 17 `0xD10D` **data** words at 714–929 — past the end of its code. An
> IOXT-first rule misclassifies it as SCSI. **Scan in order and take the first signature.**

Also: *"the first 1024 bytes are the boot sector"* is a simplification — live boot code can
run to byte **1999** (word 1747B), right up to the extended-info block.

---

## Provenance

Carved `006-S3FS` SINTRAN L bytes; real disks `SMD0.IMG` / `BIGDISK0-K.IMG` (PACK-ONE, SMD),
`WD0.img` + `1325.img` (Winchester / Micropolis 1325), `scsi-1.img` (ND-3201 SCSI), and ~50
real ND floppies. Manuals: ND-60.128.5 (SINTRAN III Reference), ND-30.003.007 (System
Supervisor).
