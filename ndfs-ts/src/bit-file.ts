/**
 * NDFS bit file (allocation bitmap): one bit per page, 0=free, 1=used.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { NDFS_PAGE_SIZE, FIRST_ALLOCATABLE_BLOCK } from './constants.js';
import { BlockPointer } from './block-pointer.js';

export class BitFile {
  indexPointer: BlockPointer | null = null;

  /**
   * Pages the bitmap covers. SINTRAN sizes the bitmap to the PHYSICAL DEVICE (e.g. 38400
   * on a 75MB SMD pack), not to the declared capacity.
   */
  totalPages: number = 0;

  /**
   * Highest allocatable page + 1 — the descending allocator's ceiling.
   *
   * SINTRAN bounds the allocatable window by the directory's DECLARED CAPACITY
   * (extPagesAvailable, words 1756B-1757B), not by the device size. On PACK-ONE the
   * capacity is 36945, so the highest allocatable page is 36944; pages 36945..38399 are a
   * deliberate free-but-unreachable gap (the drive's bad-sector spare region) — they stay
   * 0 in the bitmap yet are never handed out.
   *
   * Defaults to totalPages when the capacity is unknown (FLOMON floppies carry no valid
   * extended-info block). Always clamped to totalPages: the real Winchester WD0.img
   * declares a capacity 36 pages LARGER than the file, and allocating there would run off
   * the end.
   */
  allocCeiling: number = 0;

  /**
   * Page where the bit file itself starts (BA). 0 = not known; the SINTRAN placement rules that
   * start from BA then fall back to the top-down search.
   */
  bitAddress: number = 0;

  /**
   * The 256 page block of the last page given by {@link findFreeDataBlock}, or null for a fresh
   * bit file (SINTRAN's "cold start").
   */
  cursorBlock: number | null = null;

  private bitmap: Uint8Array | null = null;

  /** Initialize a new empty bitmap for the given total page count. */
  initialize(totalPages: number): void {
    this.totalPages = totalPages;
    this.cursorBlock = null;
    // Until the caller supplies the declared capacity, the whole device is allocatable.
    this.allocCeiling = totalPages;
    // Rounded up to a whole number of 16-bit WORDS. The bit file is word addressed
    // (see bitPosition), so an odd byte count would leave the last word half present
    // and its high-byte pages past the end of the buffer.
    const bitmapBytes = (Math.ceil(totalPages / 8) + 1) & ~1;
    this.bitmap = new Uint8Array(bitmapBytes);
  }

  /** Load bitmap data from raw bytes, padded to a whole number of 16-bit words. */
  loadBitmap(data: Uint8Array): void {
    this.cursorBlock = null;
    this.bitmap = new Uint8Array((data.length + 1) & ~1);
    this.bitmap.set(data);
  }

  /**
   * Byte index and bit index of `blockId` in the on-disk bitmap.
   *
   * Page N lives in 16-bit WORD N/16, at bit N%16 counting from the LSB.
   *
   * AUTHORITY: SINTRAN III System Supervisor, ND-30.003.007 EN, appendix F.2
   * "Bit-File":
   *
   * ```
   * PAGE = BLOCK*400B + WORD*20B + BIT
   * ```
   *
   * 20B is 16 decimal, so ONE 16-BIT WORD MAPS 16 PAGES - not one byte mapping 8. And
   * because the formula ADDS the bit number, page x sits at bit 0, the least
   * significant bit. The Norwegian edition (ND-30.003.7 NO) carries the identical
   * formula, and SINTRAN's own allocator agrees: TPAGF at 51043B forms the word index
   * with `SHA ZIN SHR 4`, i.e. page/16.
   *
   * The image is big-endian, so the word at index w occupies bytes w*2 (high) and
   * w*2+1 (low). Page N therefore lands in the OTHER byte of the pair from where a
   * naive "byte N/8, bit N%8" scheme would put it, which reduces to flipping the low
   * bit of the byte index.
   *
   * WHY THIS MATTERS: this library previously used the naive byte scheme. Being wrong
   * in both reader and writer, it was self-consistent and every round-trip test
   * passed - but on a pack written by real SINTRAN it reads the allocation map with
   * every byte pair swapped. Measured on three genuine ND media, the naive scheme
   * reports 32, 4 and 3 pages that demonstrably hold file data as FREE; this scheme
   * reports none. Handing those pages out would have overwritten live files.
   */
  private static bitPosition(blockId: number): [number, number] {
    return [(blockId >>> 3) ^ 1, blockId & 7];
  }

  /** Check if a block is marked as used. */
  isBlockUsed(blockId: number): boolean {
    if (this.bitmap === null || blockId >= this.totalPages) return false;
    const [byteIndex, bitIndex] = BitFile.bitPosition(blockId);
    return (this.bitmap[byteIndex] & (1 << bitIndex)) !== 0;
  }

  /** Mark a block as used. */
  markBlockUsed(blockId: number): void {
    if (this.bitmap === null || blockId >= this.totalPages) {
      throw new RangeError(`Block ID ${blockId} out of range`);
    }
    const [byteIndex, bitIndex] = BitFile.bitPosition(blockId);
    this.bitmap[byteIndex] |= 1 << bitIndex;
  }

  /** Mark a block as free. */
  markBlockFree(blockId: number): void {
    if (this.bitmap === null || blockId >= this.totalPages) {
      throw new RangeError(`Block ID ${blockId} out of range`);
    }
    const [byteIndex, bitIndex] = BitFile.bitPosition(blockId);
    this.bitmap[byteIndex] &= ~(1 << bitIndex);
  }

  /** Count total used pages. */
  calcUsedPages(): number {
    if (this.bitmap === null) return 0;
    let count = 0;
    for (let i = 0; i < this.totalPages; i++) {
      if (this.isBlockUsed(i)) count++;
    }
    return count;
  }

  /** Get number of free pages. */
  getFreePages(): number {
    return this.totalPages - this.calcUsedPages();
  }

  /**
   * Highest page the allocator may consider (inclusive), or -1 if there is no usable
   * window. Bounded by {@link allocCeiling} — the DECLARED CAPACITY, not the device size —
   * and clamped to the bitmap so a stale ceiling can never index past it.
   */
  private topScanIndex(): number {
    let ceiling = this.allocCeiling;
    if (ceiling <= 0 || ceiling > this.totalPages) ceiling = this.totalPages;
    if (ceiling <= FIRST_ALLOCATABLE_BLOCK) return -1;
    return ceiling - 1;
  }

  /**
   * Find a single free block, scanning HIGH -> LOW.
   *
   * KERNEL-CORRECTED: SINTRAN allocates from the TOP of the volume downward. The scanner
   * TESTP (51355B) only ever DECREMENTS its bitmap word index (`AAX -1` at 51372B and
   * 51401B) — it never increments — bounded below by the block-7 floor. This matches the
   * @CREATE-FILE rule "contiguous files are positioned in the highest page addresses".
   * The old upward scan produced the opposite layout to genuine SINTRAN.
   *
   * Blocks 0-6 are system-reserved and are never handed out.
   * Returns the block ID, or -1 if no free block exists.
   */
  findFirstFreeBlock(): number {
    for (let i = this.topScanIndex(); i >= FIRST_ALLOCATABLE_BLOCK; i--) {
      if (!this.isBlockUsed(i)) return i;
    }
    return -1;
  }

  // ---- SINTRAN placement rules (RSBPG and RSPAG) -------------------------------------------
  // See docs/NDFS-ALLOCATION-RULES.md and docs/CREATE-DIRECTORY-PLACEMENT.md.

  private ceiling(): number {
    let c = this.allocCeiling;
    if (c <= 0 || c > this.totalPages) c = this.totalPages;
    return c;
  }

  private pageOk(page: number, ceiling: number): boolean {
    return page >= FIRST_ALLOCATABLE_BLOCK && page < ceiling && !this.isBlockUsed(page);
  }

  /**
   * SINTRAN's RSBPG rule: index blocks, user and object pages.
   *
   * Start at page `hint OR 15` (the top bit of the bitmap word that holds the hint) and take the
   * first free page going down. If the word is full: word+1 (lowest bit first), word-1 (highest
   * bit first), word+2, word-2 and so on. Pages below the first allocatable block and at or above
   * the ceiling are never given. The page is NOT marked used. Returns -1 when there is none.
   */
  findFreeNear(hint: number): number {
    const ceiling = this.ceiling();
    if (ceiling <= FIRST_ALLOCATABLE_BLOCK) return -1;
    const nwords = (ceiling + 15) >>> 4;
    let word = hint >>> 4;
    if (word >= nwords) word = nwords - 1;
    for (let bit = 15; bit >= 0; bit--) {
      const p = word * 16 + bit;
      if (this.pageOk(p, ceiling)) return p;
    }
    for (let step = 1; step <= nwords; step++) {
      let w = word + step;
      if (w < nwords) {
        for (let bit = 0; bit < 16; bit++) {
          const p = w * 16 + bit;
          if (this.pageOk(p, ceiling)) return p;
        }
      }
      if (word >= step) {
        w = word - step;
        for (let bit = 15; bit >= 0; bit--) {
          const p = w * 16 + bit;
          if (this.pageOk(p, ceiling)) return p;
        }
      }
    }
    return -1;
  }

  /**
   * RSBPG rule with the hint taken from the bit file: `bitAddress - 1`. Falls back to
   * {@link findFirstFreeBlock} when `bitAddress` is 0.
   */
  findFreeHint(): number {
    if (this.bitAddress === 0) return this.findFirstFreeBlock();
    return this.findFreeNear(this.bitAddress - 1);
  }

  /**
   * First 256 page block RSPAG looks in when nothing was allocated yet: lastPage =
   * (ceiling-1) div 16384, BP = (bitAddress+1) div 16384; lastPage > BP: block 0 of lastPage;
   * lastPage = BP: the block of bitAddress+1; lastPage < BP: block 63 of lastPage. Returns page div 256.
   */
  coldStartBlock(): number {
    const ceiling = this.ceiling();
    const lastPage = (ceiling > 0 ? ceiling - 1 : 0) >>> 14;
    const baPage = (this.bitAddress + 1) >>> 14;
    if (lastPage > baPage) return lastPage * 64;
    if (lastPage === baPage) return (this.bitAddress + 1) >>> 8;
    return lastPage * 64 + 63;
  }

  /**
   * SINTRAN's RSPAG rule: data pages of indexed files and files of one page.
   *
   * Search 256 page blocks: the cursor block (or the cold start block), then +1, -1, +2, -2 ...;
   * in a block the LOWEST free page wins. On success the cursor moves to the block of the page
   * found. The page is NOT marked used. Falls back to {@link findFirstFreeBlock} when `bitAddress`
   * is 0. Returns -1 when the disk is full.
   */
  findFreeDataBlock(): number {
    if (this.bitAddress === 0) return this.findFirstFreeBlock();
    const ceiling = this.ceiling();
    if (ceiling <= FIRST_ALLOCATABLE_BLOCK) return -1;
    const nblocks = (ceiling + 255) >>> 8;
    let b0 = this.cursorBlock !== null ? this.cursorBlock : this.coldStartBlock();
    if (b0 >= nblocks) b0 = nblocks - 1;
    for (let step = 0; step <= nblocks; step++) {
      const cands: number[] = [];
      if (step === 0) {
        cands.push(b0);
      } else {
        if (b0 + step < nblocks) cands.push(b0 + step);
        if (b0 >= step) cands.push(b0 - step);
      }
      for (const b of cands) {
        for (let k = 0; k < 256; k++) {
          const p = b * 256 + k;
          if (this.pageOk(p, ceiling)) {
            this.cursorBlock = b;
            return p;
          }
        }
      }
    }
    return -1;
  }

  /**
   * Find a contiguous run of free blocks, scanning HIGH -> LOW so the run lands in the
   * highest available addresses (SINTRAN's downward range reserve, RSPAG 51120B -> TESTP).
   * Returns the LOWEST block of the run (its start), or -1.
   */
  findFreeBlockRange(blocksNeeded: number): number {
    if (blocksNeeded === 0 || blocksNeeded > this.totalPages) return -1;

    const top = this.topScanIndex();
    if (top < FIRST_ALLOCATABLE_BLOCK) return -1;

    let consecutiveFree = 0;
    for (let i = top; i >= FIRST_ALLOCATABLE_BLOCK; i--) {
      if (!this.isBlockUsed(i)) {
        consecutiveFree++;
        // i is the lowest block of the run.
        if (consecutiveFree >= blocksNeeded) return i;
      } else {
        consecutiveFree = 0;
      }
    }
    return -1;
  }

  /**
   * Allocate a range of blocks (mark as used).
   * Blocks 0-6 cannot be allocated. Returns false if any block is already used.
   */
  allocateBlocks(startBlock: number, count: number): boolean {
    if (startBlock < FIRST_ALLOCATABLE_BLOCK) return false;
    if (startBlock + count > this.totalPages) return false;

    // Check all blocks are free
    for (let i = startBlock; i < startBlock + count; i++) {
      if (this.isBlockUsed(i)) return false;
    }

    // Mark all blocks as used
    for (let i = startBlock; i < startBlock + count; i++) {
      this.markBlockUsed(i);
    }
    return true;
  }

  /** Free a range of blocks. */
  freeBlocks(startBlock: number, count: number): void {
    for (let i = startBlock; i < startBlock + count && i < this.totalPages; i++) {
      this.markBlockFree(i);
    }
  }

  /** Get a copy of the raw bitmap data. */
  getBitmapData(): Uint8Array {
    if (this.bitmap === null) return new Uint8Array(0);
    const copy = new Uint8Array(this.bitmap.length);
    copy.set(this.bitmap);
    return copy;
  }

  /**
   * Write bitmap data into page-aligned buffers for disk writing.
   * Returns an array of page buffers to write starting at the pointer's block ID.
   */
  toPageBuffers(maxPages?: number): Uint8Array[] {
    if (this.bitmap === null) return [];

    // SINTRAN sizes the bit file from the declared capacity, which can be fewer pages than the
    // device length gives; maxPages limits the count.
    let pagesNeeded = Math.ceil(this.bitmap.length / NDFS_PAGE_SIZE);
    if (maxPages !== undefined && maxPages < pagesNeeded) pagesNeeded = maxPages;
    const pages: Uint8Array[] = [];

    for (let i = 0; i < pagesNeeded; i++) {
      const page = new Uint8Array(NDFS_PAGE_SIZE);
      const srcOffset = i * NDFS_PAGE_SIZE;
      const bytesToCopy = Math.min(NDFS_PAGE_SIZE, this.bitmap.length - srcOffset);
      if (bytesToCopy > 0) {
        page.set(this.bitmap.subarray(srcOffset, srcOffset + bytesToCopy));
      }
      pages.push(page);
    }
    return pages;
  }
}
