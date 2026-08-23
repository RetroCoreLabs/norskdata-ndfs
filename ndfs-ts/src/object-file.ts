/**
 * NDFS object file: manages the collection of file (object) entries.
 *
 * Can be indexed (single index block with up to 512 pointers to data pages)
 * or sub-indexed (sub-index block with 512 pointers to index blocks).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * HackerCorp Labs - https://github.com/HackerCorpLabs
 */

import {
  NDFS_PAGE_SIZE,
  ENTRIES_PER_PAGE,
  ENTRY_SIZE,
  FILES_PER_OBJECT_BLOCK,
  MAX_OBJECT_BLOCKS,
  MAX_OBJECT_FILE_POINTERS,
  PAGES_PER_INDEX_BLOCK,
  PAGES_PER_OBJECT_BLOCK,
  USERS_PER_INDEX_BLOCK,
} from './constants.js';
import { BlockPointer } from './block-pointer.js';
import { ObjectEntry, computeFileNumber } from './object-entry.js';
import { PointerType } from './types.js';

export class ObjectFile {
  indexPointer: BlockPointer | null = null;
  private entries: Map<number, ObjectEntry> = new Map();
  private nextIndex: number = 0;

  /**
   * Object-file pages that could not be read on the last load.
   *
   * Greater than zero means the listing is what survived on a damaged floppy,
   * not the whole file list - each lost page took up to 32 entries with it.
   */
  unreadablePages = 0;

  /** Get all object entries. */
  getObjects(): ObjectEntry[] {
    const result: ObjectEntry[] = [];
    this.entries.forEach((v) => result.push(v));
    return result;
  }

  /** Get an object entry by index. */
  getObject(index: number): ObjectEntry | null {
    return this.entries.get(index) ?? null;
  }

  /**
   * Uppercase a file type and drop the padding SINTRAN leaves on it - a type
   * read off a pack can carry a trailing apostrophe and spaces.
   */
  private static normalizeType(fileType: string | undefined): string {
    return (fileType ?? '').toUpperCase().trim().replace(/'+$/, '').trim();
  }

  /**
   * Find an object by name and user, and by type when one is given.
   *
   * A SINTRAN file is identified by NAME:TYPE, and two files may share a name
   * while differing only in type - a real pack carries both
   * (TCP-IP)TCP-IP-LO-D02:MODE and :LIST, and (TCP-IP)SKP-C00 exists as
   * :DEFS, :IMPT and :INTL at once.
   *
   * Matching on the name alone made writeFile treat those as one file:
   * writing AAA:MODE after AAA:LIST overwrote the LIST entry's data while
   * leaving its type as LIST, so the MODE file vanished and the LIST file
   * silently held the wrong bytes. Measured 2026-08-19 while installing the
   * COSMOS TCP/IP kit - TCP-IP-LO-D02:MODE and TCP-START-D02:MODE were both
   * lost that way.
   *
   * Omitting fileType keeps the old name-only behaviour, for callers that
   * genuinely do not know the type.
   */
  findObject(objectName: string, userName: string, fileType?: string): ObjectEntry | null {
    const nameUpper = objectName.toUpperCase();
    const userUpper = userName.toUpperCase();
    const typeUpper = fileType ? ObjectFile.normalizeType(fileType) : null;
    const iter = this.entries.values();
    let next = iter.next();
    while (!next.done) {
      const entry = next.value;
      if (
        entry.objectName.toUpperCase() === nameUpper &&
        entry.userName.toUpperCase() === userUpper &&
        (typeUpper === null || ObjectFile.normalizeType(entry.type) === typeUpper)
      ) {
        return entry;
      }
      next = iter.next();
    }
    return null;
  }

  /** Add or update an object entry. */
  addObject(entry: ObjectEntry): void {
    this.entries.set(entry.objectIndex, entry);
    if (entry.objectIndex >= this.nextIndex) {
      this.nextIndex = entry.objectIndex + 1;
    }
  }

  /** Remove an object entry. */
  removeObject(index: number): boolean {
    return this.entries.delete(index);
  }

  /** Get objects belonging to a user (by name or index). */
  getUserObjects(userNameOrIndex: string | number): ObjectEntry[] {
    const result: ObjectEntry[] = [];
    this.entries.forEach((entry) => {
      if (typeof userNameOrIndex === 'string') {
        if (entry.userName.toUpperCase() === userNameOrIndex.toUpperCase()) {
          result.push(entry);
        }
      } else {
        if (entry.userIndex === userNameOrIndex) result.push(entry);
      }
    });
    return result;
  }

  /** Get next available object index. */
  getNextAvailableIndex(): number {
    // Find first gap
    for (let i = 0; i < this.nextIndex + 1; i++) {
      if (!this.entries.has(i)) return i;
    }
    return this.nextIndex;
  }

  /**
   * Physical object-file position of a slot inside one of a user's object blocks.
   *
   * Object block `block` for user `userIndex` occupies pages
   * `block*512 + user*8 .. +7`, and each page holds 32 entries, so:
   *
   * ```
   * page     = block*512 + user*8 + slotInBlock/32
   * physical = page*32 + slotInBlock%32
   * ```
   *
   * `slotInBlock` is 0..255. The value returned is what goes in
   * {@link ObjectEntry.objectIndex}; the user-VISIBLE number is
   * `block*256 + slotInBlock` (see {@link computeFileNumber}). The two coincide
   * only inside a user's FIRST block, which is why the old flat `user<<8` model
   * looked correct for years.
   */
  static physicalSlot(userIndex: number, block: number, slotInBlock: number): number {
    // Compatibility: assumes the block-th block lives in group home+block, which is where
    // an UNOBSTRUCTED overflow block lands. Prefer physicalSlotInGroup when the group is
    // known, because SINTRAN skips groups whose slot is already spoken for.
    return ObjectFile.physicalSlotInGroup(
      userIndex,
      Math.floor(userIndex / USERS_PER_INDEX_BLOCK) + block,
      slotInBlock,
    );
  }

  /**
   * Physical position of a slot in a given index-block group.
   *
   * A user always occupies slot `user % 64` of a group, eight pages wide. The HOME group
   * is `user / 64`, and that pairing reduces to the flat `page = user*8` for block 0.
   */
  static physicalSlotInGroup(userIndex: number, group: number, slotInBlock: number): number {
    const page =
      group * PAGES_PER_INDEX_BLOCK +
      (userIndex % USERS_PER_INDEX_BLOCK) * PAGES_PER_OBJECT_BLOCK +
      Math.floor(slotInBlock / ENTRIES_PER_PAGE);
    return page * ENTRIES_PER_PAGE + (slotInBlock % ENTRIES_PER_PAGE);
  }

  /**
   * Find the next free object slot for a user, across ALL their object blocks.
   *
   * A user area holds 256 files per object block, one by default and up to
   * `maxObjectBlocks` (16 max, 4096 files) once the operator has run
   * `@GIVE-OBJECT-BLOCKS`. Blocks are allocated on demand, so this searches every
   * block already allocated and, if they are all full, returns the first slot of
   * the NEXT block while one is still permitted.
   *
   * The caller must raise the user's allocated count when the returned slot falls
   * in a block beyond the current one, and must make the page exist. See
   * `NdfsFileSystem.allocateUserObjectSlot`.
   *
   * Defaults of 1/1 reproduce the historical single-block behaviour, so callers
   * that have not been taught about object blocks keep working unchanged.
   *
   * **Block 1 and above overlap the high-user region.** An index block holds 512
   * pages = 64 users at 8 pages each, so index-block group `g` is where this
   * library places users `g*64 .. g*64+63`. SINTRAN instead puts user U's block
   * `n` in group `n` at the same offset - so a second block for user U lands
   * exactly where user `64+U`'s FIRST block would go. This function only computes
   * placement; the caller must refuse to grow into block `n` when user
   * `n*64 + userIndex` exists.
   *
   * @returns The physical slot, or -1 when every permitted block is full - which
   * now genuinely means the user is at their SINTRAN limit rather than merely
   * past 256 files.
   */
  /**
   * Find the next free object slot for a user, across all their object blocks.
   *
   * A user's blocks all sit at slot `user % 64` of an index-block group; the HOME group
   * `user / 64` holds block 0, and overflow blocks take the same slot in a HIGHER group.
   *
   * **SINTRAN skips a group whose slot is already spoken for**, and relocates the block if
   * that group's home owner later needs it (measured live 2026-08-02). This library instead
   * never places a block in another user's home, so it produces no layout needing
   * relocation - `groupAvailable` is how the file-system layer supplies that check.
   *
   * @returns The physical slot, or -1 when every permitted block is full.
   */
  findFreeUserSlot(
    userIndex: number,
    maxObjectBlocks = 1,
    allocatedObjectBlocks = 1,
    groupAvailable?: (group: number) => boolean,
  ): number {
    const maxBlocks = Math.max(1, Math.min(MAX_OBJECT_BLOCKS, maxObjectBlocks));
    const allocated = Math.max(1, Math.min(maxBlocks, allocatedObjectBlocks));
    const home = Math.floor(userIndex / USERS_PER_INDEX_BLOCK);

    // Groups already occupied, plus HOME - block 0 must be searched even when the user has
    // no entries yet. Holes are normal: SINTRAN never compacts.
    const occupied = new Set(this.groupsUsedBy(userIndex));
    occupied.add(home);
    for (const group of Array.from(occupied).sort((a, b) => a - b)) {
      for (let slot = 0; slot < FILES_PER_OBJECT_BLOCK; slot++) {
        const physical = ObjectFile.physicalSlotInGroup(userIndex, group, slot);
        if (!this.entries.has(physical)) return physical;
      }
    }

    // Every occupied block is full: open the next one if the maximum allows.
    if (allocated >= maxBlocks) return -1;

    for (let group = home; group < MAX_OBJECT_FILE_POINTERS; group++) {
      if (occupied.has(group)) continue;
      if (groupAvailable && !groupAvailable(group)) continue;
      const first = ObjectFile.physicalSlotInGroup(userIndex, group, 0);
      if (!this.entries.has(first)) return first;
    }

    return -1;
  }

  /** The index-block groups this user has entries in, ascending. */
  groupsUsedBy(userIndex: number): number[] {
    const groups = new Set<number>();
    this.entries.forEach((e) => {
      if (e.userIndex !== userIndex) return;
      groups.add(Math.floor(e.objectIndex / ENTRIES_PER_PAGE / PAGES_PER_INDEX_BLOCK));
    });
    return Array.from(groups).sort((a, b) => a - b);
  }

  /**
   * Assign each entry the number SINTRAN reports - the 307 in "FILE 307".
   *
   * The number is `logicalBlock * 256 + slotInBlock`, where **logicalBlock is the ORDINAL
   * RANK of the entry's index-block group among the groups that user occupies**, ascending
   * - NOT the physical group number.
   *
   * That only shows up once an overflow block has been relocated, and SINTRAN relocates
   * them freely. Measured live 2026-08-02: user 8 with 300 files held block 0 in group 0
   * and its overflow in group 2; creating files for user 136 (home = group 2, slot 8) moved
   * user 8's overflow to group 3, and user 200 then moved it to group 4. All 300 files
   * survived and SINTRAN reported them throughout as FILE 0..299.
   *
   * The old `block = page / 512` gave that user 0..255 then 1024..1067 where SINTRAN says
   * 256..299. It looked right on the earlier BIGMAN pack only because nothing had displaced
   * that pack's overflow block, so rank equalled group number.
   */
  private assignFileNumbers(): void {
    const byUser = new Map<number, ObjectEntry[]>();
    this.entries.forEach((e) => {
      const list = byUser.get(e.userIndex);
      if (list) list.push(e);
      else byUser.set(e.userIndex, [e]);
    });

    byUser.forEach((entries, userIndex) => {
      const groups = Array.from(
        new Set(
          entries.map((e) =>
            Math.floor(e.objectIndex / ENTRIES_PER_PAGE / PAGES_PER_INDEX_BLOCK),
          ),
        ),
      ).sort((a, b) => a - b);
      const rank = new Map<number, number>();
      groups.forEach((g, i) => rank.set(g, i));

      for (const e of entries) {
        const page = Math.floor(e.objectIndex / ENTRIES_PER_PAGE);
        const group = Math.floor(page / PAGES_PER_INDEX_BLOCK);
        // The slot WITHIN a group is (user % 64): 64 users fill one 512-page group.
        const slotBase = (userIndex % USERS_PER_INDEX_BLOCK) * PAGES_PER_OBJECT_BLOCK;
        const offsetPages = (page % PAGES_PER_INDEX_BLOCK) - slotBase;
        if (offsetPages < 0 || offsetPages >= PAGES_PER_OBJECT_BLOCK) {
          e.fileNumber = -1;
          continue;
        }
        const slot = offsetPages * ENTRIES_PER_PAGE + (e.objectIndex % ENTRIES_PER_PAGE);
        e.fileNumber = (rank.get(group) ?? 0) * FILES_PER_OBJECT_BLOCK + slot;
      }
    });
  }

  /** Get total pages used by all objects. */
  getTotalPagesUsed(): number {
    let total = 0;
    this.entries.forEach((e) => {
      total += e.pagesInFile;
    });
    return total;
  }

  /** Clear all entries. */
  clear(): void {
    this.entries.clear();
    this.nextIndex = 0;
  }

  /**
   * Load entries from an indexed or sub-indexed structure.
   * pointer: the object file's block pointer (from master block).
   * readPage: callback to read a page by block ID.
   */
  loadFromPages(
    pointer: BlockPointer,
    readPage: (blockId: number) => Uint8Array,
  ): void {
    this.entries.clear();
    this.unreadablePages = 0;
    this.indexPointer = pointer;
    let globalObjectIndex = 0;

    if (pointer.type === PointerType.Indexed) {
      // Single index block -> up to 512 data page pointers
      const indexPage = readPage(pointer.blockId);
      this.loadObjectsFromIndexBlock(indexPage, readPage, globalObjectIndex);
      // File numbers can only be assigned once every entry is loaded.
      this.assignFileNumbers();
      return;
    } else if (pointer.type === PointerType.SubIndexed) {
      // Sub-index block -> up to 512 index block pointers
      const subIndexPage = readPage(pointer.blockId);
      for (let i = 0; i < MAX_OBJECT_FILE_POINTERS; i++) {
        const indexPtr = BlockPointer.fromBytes(subIndexPage, i * 4);
        if (!indexPtr.isValid()) continue;

        // A page of a damaged floppy cannot be read at all, and one lost
        // index page must not cost the entries the other pages still name.
        let indexPage: Uint8Array;
        try {
          indexPage = readPage(indexPtr.blockId);
        } catch {
          this.unreadablePages++;
          continue;
        }
        globalObjectIndex = this.loadObjectsFromIndexBlock(
          indexPage,
          readPage,
          globalObjectIndex,
        );
      }
    }

    // File numbers can only be assigned once every entry is loaded.
    this.assignFileNumbers();
  }

  private loadObjectsFromIndexBlock(
    indexPage: Uint8Array,
    readPage: (blockId: number) => Uint8Array,
    startIndex: number,
  ): number {
    let objectIndex = startIndex;

    for (let i = 0; i < MAX_OBJECT_FILE_POINTERS; i++) {
      const ptr = BlockPointer.fromBytes(indexPage, i * 4);
      if (!ptr.isValid()) {
        objectIndex += ENTRIES_PER_PAGE;
        continue;
      }

      // Same again one level down: an unreadable data page loses its own 32
      // entries and nothing else. The slots are still counted, so every entry
      // after it keeps the object index it has on the media.
      let dataPage: Uint8Array;
      try {
        dataPage = readPage(ptr.blockId);
      } catch {
        this.unreadablePages++;
        objectIndex += ENTRIES_PER_PAGE;
        continue;
      }
      for (let j = 0; j < ENTRIES_PER_PAGE; j++) {
        const entry = ObjectEntry.fromBytes(dataPage, j * ENTRY_SIZE);
        if (entry !== null) {
          entry.objectIndex = objectIndex + j;
          // The number SINTRAN shows is NOT the physical position: a user's
          // files can span several object blocks, each a whole index block
          // apart. computeFileNumber undoes that. Without this every loaded
          // entry reports file number 0.
          // Provisional; assignFileNumbers() recomputes it by group RANK once the
          // whole object file is loaded, which is the only correct basis.
          entry.fileNumber = computeFileNumber(entry.objectIndex, entry.userIndex);
          this.entries.set(entry.objectIndex, entry);
          if (entry.objectIndex >= this.nextIndex) {
            this.nextIndex = entry.objectIndex + 1;
          }
        }
      }
      objectIndex += ENTRIES_PER_PAGE;
    }

    return objectIndex;
  }

  /**
   * Serialize all object entries into page-aligned buffers.
   * Returns the data pages that contain entries.
   */
  toDataPages(): Map<number, Uint8Array> {
    const pageMap = new Map<number, Uint8Array>();

    this.entries.forEach((entry) => {
      const pageIndex = Math.floor(entry.objectIndex / ENTRIES_PER_PAGE);
      if (!pageMap.has(pageIndex)) {
        pageMap.set(pageIndex, new Uint8Array(NDFS_PAGE_SIZE));
      }
      const page = pageMap.get(pageIndex)!;
      const slotInPage = entry.objectIndex % ENTRIES_PER_PAGE;
      entry.toBytes(page, slotInPage * ENTRY_SIZE);
    });

    return pageMap;
  }

  /**
   * Serialize the single object-file data page `pageIndex`, zero-filled.
   * Zero-fill clears any slot freed by a deletion so a removed file does not
   * reappear when the image is reloaded.
   */
  toDataPage(pageIndex: number): Uint8Array {
    const page = new Uint8Array(NDFS_PAGE_SIZE);
    this.entries.forEach((entry) => {
      if (Math.floor(entry.objectIndex / ENTRIES_PER_PAGE) === pageIndex) {
        const slotInPage = entry.objectIndex % ENTRIES_PER_PAGE;
        entry.toBytes(page, slotInPage * ENTRY_SIZE);
      }
    });
    return page;
  }
}
