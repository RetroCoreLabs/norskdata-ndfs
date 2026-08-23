/**
 * NDFS user file: manages the collection of user entries.
 *
 * The user file is an indexed structure with up to 8 index pointers,
 * each pointing to a data page containing 32 user entries (64 bytes each).
 * Maximum 256 users (8 pages x 32 entries).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * HackerCorp Labs - https://github.com/HackerCorpLabs
 */

import {
  NDFS_PAGE_SIZE,
  ENTRIES_PER_PAGE,
  ENTRY_SIZE,
  MAX_USER_FILE_POINTERS,
  MAX_USERS,
} from './constants.js';
import { BlockPointer } from './block-pointer.js';
import { UserEntry } from './user-entry.js';

export class UserFile {
  indexPointer: BlockPointer | null = null;
  private entries: Map<number, UserEntry> = new Map();

  /**
   * User-file pages that could not be read on the last load. Greater than
   * zero means some users on this floppy have no name here.
   */
  unreadablePages = 0;

  /** Get all user entries as an array. */
  getUsers(): UserEntry[] {
    const result: UserEntry[] = [];
    this.entries.forEach((v) => result.push(v));
    return result;
  }

  /** Get a user by index. */
  getUser(index: number): UserEntry | null {
    return this.entries.get(index) ?? null;
  }

  /** Find a user by name (case-insensitive). */
  findUser(userName: string): UserEntry | null {
    const upper = userName.toUpperCase();
    const iter = this.entries.values();
    let next = iter.next();
    while (!next.done) {
      if (next.value.userName.toUpperCase() === upper) return next.value;
      next = iter.next();
    }
    return null;
  }

  /** Add or update a user entry. */
  addUser(entry: UserEntry): void {
    this.entries.set(entry.userIndex, entry);
  }

  /** Remove a user entry. */
  removeUser(index: number): boolean {
    return this.entries.delete(index);
  }

  /** Update a user's quota. */
  updateUserQuota(userIndex: number, newReservedPages: number): boolean {
    const user = this.entries.get(userIndex);
    if (!user) return false;
    user.pagesReserved = newReservedPages;
    return true;
  }

  /** Update a user's usage. */
  updateUserUsage(userIndex: number, pagesUsed: number): boolean {
    const user = this.entries.get(userIndex);
    if (!user) return false;
    user.pagesUsed = pagesUsed;
    return true;
  }

  /** Get the next available user index. */
  getNextAvailableIndex(): number {
    for (let i = 0; i < MAX_USERS; i++) {
      if (!this.entries.has(i)) return i;
    }
    return -1;
  }

  /** Get total pages reserved across all users. */
  getTotalPagesReserved(): number {
    let total = 0;
    this.entries.forEach((u) => {
      total += u.pagesReserved;
    });
    return total;
  }

  /** Get total pages used across all users. */
  getTotalPagesUsed(): number {
    let total = 0;
    this.entries.forEach((u) => {
      total += u.pagesUsed;
    });
    return total;
  }

  /** Clear all entries. */
  clear(): void {
    this.entries.clear();
  }

  /**
   * Load user entries from index block and data pages.
   * indexPage: the 2048-byte index block (contains up to 8 block pointers).
   * readPage: callback to read a data page by block ID.
   */
  loadFromPages(
    indexPage: Uint8Array,
    readPage: (blockId: number) => Uint8Array,
  ): void {
    this.entries.clear();
    this.unreadablePages = 0;

    // Read up to 8 pointers from the index block
    for (let i = 0; i < MAX_USER_FILE_POINTERS; i++) {
      const ptr = BlockPointer.fromBytes(indexPage, i * 4);
      if (!ptr.isValid()) continue;

      // One unreadable user page costs only the users on it. The page reads
      // fail on a damaged floppy, and the users named by the other pages are
      // still there to resolve object entries against.
      let dataPage: Uint8Array;
      try {
        dataPage = readPage(ptr.blockId);
      } catch {
        this.unreadablePages++;
        continue;
      }

      // Parse up to 32 user entries per page
      for (let j = 0; j < ENTRIES_PER_PAGE; j++) {
        const entryOffset = j * ENTRY_SIZE;
        const user = UserEntry.fromBytes(dataPage, entryOffset);
        if (user !== null) {
          this.entries.set(user.userIndex, user);
        }
      }
    }
  }

  /**
   * Serialize all user entries into page-aligned buffers.
   * Returns: { indexPage, dataPages: Map<pointerIndex, pageData> }
   */
  toPageBuffers(): { indexPage: Uint8Array; dataPages: Uint8Array[] } {
    const indexPage = new Uint8Array(NDFS_PAGE_SIZE);
    const dataPages: Uint8Array[] = [];

    // Determine which data pages are needed
    const pageMap = new Map<number, Uint8Array>();

    this.entries.forEach((user) => {
      const pageIndex = Math.floor(user.userIndex / ENTRIES_PER_PAGE);
      if (!pageMap.has(pageIndex)) {
        pageMap.set(pageIndex, new Uint8Array(NDFS_PAGE_SIZE));
      }
      const page = pageMap.get(pageIndex)!;
      const slotInPage = user.userIndex % ENTRIES_PER_PAGE;
      const bytes = user.toBytes();
      page.set(bytes, slotInPage * ENTRY_SIZE);
    });

    // Build data pages array in order
    for (let i = 0; i < MAX_USER_FILE_POINTERS; i++) {
      const page = pageMap.get(i);
      if (page) {
        dataPages.push(page);
      } else {
        dataPages.push(new Uint8Array(NDFS_PAGE_SIZE));
      }
    }

    return { indexPage, dataPages };
  }

  /**
   * Serialize the single user-file data page `pageIndex`, zero-filled.
   * Zero-fill clears the slot of any removed user.
   */
  toDataPage(pageIndex: number): Uint8Array {
    const page = new Uint8Array(NDFS_PAGE_SIZE);
    this.entries.forEach((user) => {
      if (Math.floor(user.userIndex / ENTRIES_PER_PAGE) === pageIndex) {
        const slotInPage = user.userIndex % ENTRIES_PER_PAGE;
        page.set(user.toBytes(), slotInPage * ENTRY_SIZE);
      }
    });
    return page;
  }
}
