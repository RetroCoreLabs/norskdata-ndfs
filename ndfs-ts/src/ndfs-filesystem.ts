/**
 * NdfsFileSystem: main class for reading and writing NDFS disk images.
 *
 * Operates on a Uint8Array buffer representing the entire disk image.
 * Supports contiguous, indexed, and sub-indexed file allocation,
 * sparse files, user management, and quota tracking.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
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
  MAX_USER_FILE_POINTERS,
  FIRST_ALLOCATABLE_BLOCK,
  MAX_USERS,
  NDFS_NAME_MAX,
  NDFS_TYPE_MAX,
} from './constants.js';
import { readUint32BE, writeUint32BE } from './endian.js';
import { ObjectBlockCollisionError, ObjectBlockLimitError } from './errors.js';
import * as sintran from './sintran.js';
import { BlockPointer } from './block-pointer.js';
import { MasterBlock } from './master-block.js';
import { BitFile } from './bit-file.js';
import { UserFile } from './user-file.js';
import { ObjectFile } from './object-file.js';
import { UserEntry } from './user-entry.js';
import { UserFriend } from './user-friend.js';
import {
  ObjectEntry,
  computeFileNumber,
  ACCESS_DEFAULT,
  FT_INDEXED,
  FT_CONTIGUOUS,
} from './object-entry.js';
import { PointerType, FileEntry, BootFormat, BootControllerType, BootCode, ImageTemplate, ImageCreationOptions } from './types.js';
import { createNdfsImage } from './image-creator.js';
import { detectBootFormat, loadBootCode as loadBootCodeFromPage, detectControllerType } from './boot-loader.js';
import { stripParity, setParity } from './parity.js';

/** Parity mode for read/write operations. */
export type ParityMode = 'none' | 'strip' | 'set';
import { XatProperties, objectEntryToXat, xatToObjectEntry, XAT_KEY_HOLES } from './xat.js';

/** One entry returned by NdfsFileSystem.listFriends. */
export interface FriendInfo {
  index: number; // friend's user index
  name: string; // friend's user name, or '' if no such user
  bits: number; // raw 16-bit friend entry
  perms: string; // 'RWACD' / '-----'
}

export class NdfsFileSystem {
  private data: Uint8Array;
  private readOnly: boolean;
  private _unaligned: boolean = false;
  private masterBlock: MasterBlock;
  private bitFile: BitFile = new BitFile();
  /** bit-file pages that could not be read on load - see getDamageReport() */
  private unreadableBitFilePages = 0;
  private userFile: UserFile = new UserFile();
  private objectFile: ObjectFile = new ObjectFile();

  /**
   * Open an NDFS disk image from a buffer.
   * @param data - The raw disk image bytes. A trailing partial page (size not a
   *   whole multiple of 2048) is dropped and the image opens read-only.
   * @param readOnly - If true, write operations will throw.
   */
  constructor(data: Uint8Array | ArrayBuffer, readOnly: boolean = false) {
    if (data instanceof ArrayBuffer) {
      this.data = new Uint8Array(data);
    } else {
      // Make a copy to avoid external mutation
      this.data = new Uint8Array(data.length);
      this.data.set(data);
    }
    this.readOnly = readOnly;

    if (this.data.length < NDFS_PAGE_SIZE) {
      throw new Error('Image too small: must be at least one NDFS page (2048 bytes)');
    }

    // Tolerate images whose byte length is not a whole multiple of the page
    // size (a common dump artefact): work on whole pages only and drop the
    // trailing partial page, exactly as the reference ndfs does. Such images
    // are forced read-only so writing whole pages cannot lose the dropped tail.
    this._unaligned = this.data.length % NDFS_PAGE_SIZE !== 0;
    const pages = Math.floor(this.data.length / NDFS_PAGE_SIZE);
    if (this._unaligned) {
      this.data = this.data.subarray(0, pages * NDFS_PAGE_SIZE);
      this.readOnly = true;
    }

    // Parse master block
    const page0 = this.readPage(0);
    this.masterBlock = MasterBlock.fromBytes(page0);
    if (!this.masterBlock.isValid()) {
      throw new Error('Invalid NDFS master block');
    }
    this.masterBlock.imageSize = pages;

    // Load filesystem structures
    this.loadStructures();
  }

  /**
   * Create a new NDFS disk image from options.
   * Returns a fully initialized NdfsFileSystem ready for use.
   */
  static createImage(options: ImageCreationOptions): NdfsFileSystem {
    const imageData = createNdfsImage(options);
    return new NdfsFileSystem(imageData, false);
  }

  // ── Lifecycle ──────────────────────────────────────────────────────

  /** Export the current image as a new Uint8Array. */
  toBuffer(): Uint8Array {
    const copy = new Uint8Array(this.data.length);
    copy.set(this.data);
    return copy;
  }

  // ── Read operations ────────────────────────────────────────────────

  /** Get the parsed master block. */
  getMasterBlock(): MasterBlock {
    return this.masterBlock;
  }

  /** Get the volume/directory name. */
  getDirectoryName(): string {
    return this.masterBlock.directoryName;
  }

  /**
   * List directory contents.
   * - path="" or "/": lists users as directories.
   * - path="USERNAME": lists that user's files.
   */
  listDirectory(path: string = ''): FileEntry[] {
    const normalized = path.replace(/^\/+|\/+$/g, '');
    const entries: FileEntry[] = [];

    if (normalized === '') {
      // Root: list users as directories
      const users = this.userFile.getUsers();
      for (let i = 0; i < users.length; i++) {
        const u = users[i];
        entries.push({
          name: u.userName,
          type: '',
          fullName: u.userName,
          userName: u.userName,
          size: 0,
          pages: 0,
          isDirectory: true,
          lastModified: null,
        });
      }
    } else {
      const parts = normalized.split('/');
      if (parts.length > 1) {
        throw new Error('NDFS does not support subdirectories');
      }
      const userName = parts[0];
      const objects = this.objectFile.getUserObjects(userName);
      for (let i = 0; i < objects.length; i++) {
        const obj = objects[i];
        const fullName = obj.type ? `${obj.objectName}:${obj.type}` : obj.objectName;
        entries.push({
          name: obj.objectName,
          type: obj.type,
          fullName,
          userName: obj.userName,
          size: obj.bytesInFile,
          pages: obj.pagesInFile,
          isDirectory: false,
          lastModified: null,
        });
      }
    }
    return entries;
  }

  /**
   * Read a file's contents.
   * @param path - "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"
   * @param parity - Parity handling: 'none' (default, raw bytes),
   *   'strip' (clear bit 7, for reading ND text as ASCII).
   */
  readFile(path: string, parity: ParityMode = 'none'): Uint8Array {
    const obj = this.findObject(path);
    if (!obj) throw new Error(`File not found: ${path}`);
    const data = this.readObjectData(obj);
    if (parity === 'strip') return stripParity(data);
    return data;
  }

  /** Get file metadata, or null if not found. */
  getMetadata(path: string): FileEntry | null {
    const obj = this.findObject(path);
    if (!obj) return null;
    const fullName = obj.type ? `${obj.objectName}:${obj.type}` : obj.objectName;
    return {
      name: obj.objectName,
      type: obj.type,
      fullName,
      userName: obj.userName,
      size: obj.bytesInFile,
      pages: obj.pagesInFile,
      isDirectory: false,
      lastModified: null,
    };
  }

  /** Check if a file exists. */
  fileExists(path: string): boolean {
    return this.findObject(path) !== null;
  }

  // ── Write operations ───────────────────────────────────────────────

  /**
   * Write (create or overwrite) a file.
   * @param path - "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"
   * @param fileData - The raw bytes to write.
   * @param parity - Parity handling: 'none' (default, write raw bytes),
   *   'set' (apply ND-100 even parity before writing, for text files).
   */
  /**
   * Write (create or overwrite) a file.
   *
   * Every logical page is allocated a real disk block unless it is named in
   * `holes`. An all-zero page is NOT a hole: a hole is a property of the file,
   * so it has to be stated. Inferring one from the data produces a file SINTRAN
   * cannot read -- READ-BI and READ-PROGFILE fail on a hole with
   * "NO SUCH PAGE / ERROR IN ACCESSING INPUT FILE" -- see writeDataPageToIndex.
   *
   * @param holes Logical page indices to leave unallocated. The only
   *   legitimate source is the holes recorded from an original: the XAT
   *   sidecar's "ndfs.holes".
   * @param accessBits 15-bit access word to stamp on a NEWLY CREATED file, or
   *   undefined to inherit the owning user's default file access. Ignored
   *   entirely when overwriting an existing file.
   *
   *   Layout (offset 26 of the object entry, uint16 BE): bits 14-10 PUBLIC,
   *   9-5 FRIEND, 4-0 OWN; within each 5-bit tier: bit 4 Delete, bit 3
   *   Execute/Common, bit 2 Append, bit 1 Write, bit 0 Read.
   *
   *   PASS ACCESS_PUBLIC_READ WHEN BUILDING DISTRIBUTION MEDIA. A user
   *   created by a real SINTRAN carries defaultFileAccess 0x03FF -- owner
   *   and friends everything, PUBLIC NOTHING -- so files inherited from it
   *   are invisible to every other user, SYSTEM included, and @LIST-FILES
   *   from SYSTEM prints nothing while the pack itself is perfectly valid.
   *   Measured on a real ND product floppy (210580B01-XX-01D): every file on
   *   it carries 0x07FF, the same owner/friend rights plus PUBLIC READ (see
   *   ACCESS_PUBLIC_READ in object-entry.ts for the full measurement).
   */
  writeFile(
    path: string,
    fileData: Uint8Array,
    parity: ParityMode = 'none',
    holePages?: Iterable<number>,
    accessBits?: number,
  ): void {
    const holes: ReadonlySet<number> | undefined =
      holePages === undefined ? undefined : new Set(holePages);
    this.ensureWritable();

    if (parity === 'set') {
      fileData = setParity(fileData);
    }

    const { userName, objectName, fileType } = this.parsePath(path);
    if (!objectName) throw new Error('Invalid path: filename required');

    // Find user
    const user = this.resolveUser(userName);
    if (!user) throw new Error(`User not found: ${userName || '(default)'}`);

    // Calculate required pages. Quota tracks only REAL disk consumption:
    // sparse (all-zero) pages allocate no block (see writeDataPageToIndex's
    // zero-check, reused by countRealDataPages), and the index/sub-index
    // structural blocks are filesystem overhead, not user data.
    let requiredRealPages = this.countRealDataPages(fileData, holes);
    // Index and sub-index blocks are charged like data pages (SINTRAN, measured), so the
    // reservation must cover them too.
    const dataPagesForQuota = Math.max(1, Math.ceil(fileData.length / NDFS_PAGE_SIZE));
    requiredRealPages +=
      dataPagesForQuota <= MAX_OBJECT_FILE_POINTERS
        ? 1
        : 1 + Math.ceil(dataPagesForQuota / MAX_OBJECT_FILE_POINTERS);

    // Check for existing file. The TYPE is part of a SINTRAN file's identity,
    // so it has to be passed: without it AAA:MODE and AAA:LIST resolve to the
    // same entry and the second write silently overwrites the first's data
    // while keeping the first's type.
    const existing = this.objectFile.findObject(objectName, user.userName, fileType);

    // Determine additional pages needed, comparing against the existing
    // file's real (non-sparse) page count — not its logical page count.
    let additionalNeeded = requiredRealPages;
    if (existing && (existing.fileTypeFlags & FT_CONTIGUOUS) !== 0) {
      // A contiguous file already owns every page it will ever have, so a write can never
      // need more. Skipping the quota step matters: the check below would otherwise
      // expand the user's reservation for pages that are never allocated, and it would do
      // so before updateExistingContiguousFile rejects an oversized write.
      additionalNeeded = 0;
    } else if (existing) {
      const existingRealPages = this.countChargedPagesForObject(existing);
      additionalNeeded = requiredRealPages > existingRealPages ? requiredRealPages - existingRealPages : 0;
    }
    // The first file of a user also needs the user's first object data page.
    if (!existing && this.countUserObjectPages(user.userIndex) === 0) additionalNeeded++;

    // Check and expand quota if needed
    if (additionalNeeded > 0) {
      const availableToUser = user.pagesReserved - user.pagesUsed;
      if (availableToUser < additionalNeeded) {
        const expansion = additionalNeeded - availableToUser;
        const freeOnDisk = this.bitFile.getFreePages();
        if (freeOnDisk < expansion) {
          throw new Error(`Insufficient disk space: need ${expansion} pages, only ${freeOnDisk} available`);
        }
        user.pagesReserved += expansion;
      }
    }

    if (existing) {
      this.updateExistingFile(existing, user, fileData, holes);
    } else {
      this.createNewFile(objectName, fileType, user, fileData, holes, accessBits);
    }
    // updateExistingFile / createNewFile perform their own surgical metadata
    // writes (object page, owner user page, bitmap).
  }

  /** Delete a file. */
  deleteFile(path: string): void {
    this.ensureWritable();

    const obj = this.findObject(path);
    if (!obj) throw new Error(`File not found: ${path}`);

    // Resolve the file's real data-block pointers BEFORE freeing them, so the
    // quota refund below can count exactly which pages were real vs. sparse.
    const chargedPages = this.countChargedPagesForObject(obj);

    // Free blocks
    if (obj.filePointer && obj.filePointer.blockId > 0) {
      this.freeFileBlocks(obj);
    }

    // Update user pages used. Quota tracks only REAL disk consumption: count
    // actual data blocks freed (sparse holes report blockId===0 in
    // dataPointers), never the index/sub-index structural block itself.
    const user = this.userFile.getUser(obj.userIndex);
    if (user) {
      user.pagesUsed = user.pagesUsed >= chargedPages ? user.pagesUsed - chargedPages : 0;
    }

    // Surgical writes: capture index/owner before removal, then rewrite the
    // freed object's page (zero-fill clears the slot), the owner's user page,
    // and the allocation bitmap.
    const freedIndex = obj.objectIndex;
    const owner = obj.userIndex;
    this.objectFile.removeObject(obj.objectIndex);
    this.writeObjectPage(freedIndex);
    this.writeUserPage(owner);
    this.writeBitFile();
  }

  /** Rename a file. */
  rename(oldPath: string, newPath: string): void {
    this.ensureWritable();

    const obj = this.findObject(oldPath);
    if (!obj) throw new Error(`File not found: ${oldPath}`);

    const { objectName, fileType } = this.parsePath(newPath);
    if (!objectName) throw new Error('Invalid new path');

    obj.objectName = objectName.toUpperCase().substring(0, NDFS_NAME_MAX);
    obj.type = fileType.toUpperCase().substring(0, NDFS_TYPE_MAX);
    // Rename touches only this file's object entry.
    this.writeObjectPage(obj.objectIndex);
  }

  // ── User management ────────────────────────────────────────────────

  /** Get all users. */
  getUsers(): UserEntry[] {
    return this.userFile.getUsers();
  }

  /** Get a user by index. */
  getUser(index: number): UserEntry | null {
    return this.userFile.getUser(index);
  }

  /** Add a new user. */
  addUser(name: string, reservedPages: number): boolean {
    this.ensureWritable();

    if (this.userFile.findUser(name)) return false; // already exists
    const idx = this.userFile.getNextAvailableIndex();
    if (idx < 0) return false; // no slots

    const user = new UserEntry();
    user.setName(name);
    user.userIndex = idx;
    user.pagesReserved = reservedPages;
    this.userFile.addUser(user);
    // The new user's slot may land in a UserFile directory page that has
    // never been allocated on disk (e.g. a freshly created small image only
    // pre-allocates the first page). Without this, writeUserPage() below
    // silently no-ops when the page pointer is invalid, so the user would
    // exist only in memory and vanish on the next mount.
    this.ensureUserDirPage(user.userIndex);
    // Add-user touches only the UserFile page holding this new user.
    this.writeUserPage(user.userIndex);
    // Flush the bitmap (and the master block's cached free-page count) in
    // case ensureUserDirPage() above allocated a fresh directory page.
    this.writeBitFile();
    return true;
  }

  /** Remove a user (only if they have no files). */
  removeUser(index: number): boolean {
    this.ensureWritable();

    const files = this.objectFile.getUserObjects(index);
    if (files.length > 0) return false; // user has files

    const ok = this.userFile.removeUser(index);
    // Removed user's UserFile page (slot zeroed by rebuild).
    if (ok) this.writeUserPage(index);
    return ok;
  }

  /** Update a user's page quota. */
  updateUserQuota(index: number, newPages: number): boolean {
    this.ensureWritable();
    const ok = this.userFile.updateUserQuota(index, newPages);
    if (ok) this.writeUserPage(index);
    return ok;
  }

  // ------------------------------------------------------------------
  // Object blocks: files per user
  // ------------------------------------------------------------------

  /**
   * Pick a free object slot for `user`, growing into a new object block if needed.
   *
   * A user area holds 256 files per object block, one by default and up to
   * `user.maxObjectBlocks` (raised with `@GIVE-OBJECT-BLOCKS`, or
   * {@link giveObjectBlocks} here). Blocks are allocated on demand, so when every
   * allocated block is full this takes the first slot of the next one and raises
   * the user's allocated count to match - which is what SINTRAN does, and what
   * makes creating file 257 work at all.
   *
   * @throws {ObjectBlockLimitError} when the user is genuinely at their ceiling,
   * rather than merely past 256 files. Read `isHardLimit` to tell a grantable
   * limit from the SINTRAN maximum of 4096 files.
   * @throws {ObjectBlockCollisionError} when the next block's pages belong to
   * another user.
   */
  private allocateUserObjectSlot(user: UserEntry): number {
    // A user's blocks all sit at slot (user % 64) of an index-block group. Block 0 is in
    // the HOME group (user / 64); further blocks take the same slot in a HIGHER group,
    // skipping any group whose slot belongs to a user that already exists.
    //
    // That skipping is what SINTRAN does, measured live 2026-08-02: user 8 growing past
    // 256 files did NOT take group 1 (user 72 lives there). SINTRAN goes further and
    // RELOCATES an overflow block when the rightful home owner later needs the group;
    // this library instead declines to place a block in another user's home at all, so it
    // never produces a layout needing relocation.
    const groupAvailable = (group: number): boolean => {
      const homeOwner =
        group * USERS_PER_INDEX_BLOCK + (user.userIndex % USERS_PER_INDEX_BLOCK);
      if (homeOwner === user.userIndex) return true;
      if (homeOwner >= MAX_USERS) return true;
      return this.userFile.getUser(homeOwner) === null;
    };

    const slot = this.objectFile.findFreeUserSlot(
      user.userIndex,
      user.maxObjectBlocks,
      user.allocatedObjectBlocks,
      groupAvailable,
    );

    if (slot < 0) {
      throw new ObjectBlockLimitError(
        user.userName,
        user.maxObjectBlocks,
        user.allocatedObjectBlocks,
        user.maxObjectBlocks >= MAX_OBJECT_BLOCKS,
      );
    }

    // The number of distinct groups this user occupies IS the allocated block count, which
    // byte 47's low nibble records.
    const groups = new Set(this.objectFile.groupsUsedBy(user.userIndex));
    groups.add(Math.floor(slot / ENTRIES_PER_PAGE / PAGES_PER_INDEX_BLOCK));
    user.allocatedObjectBlocks = Math.max(
      1,
      Math.min(user.maxObjectBlocks, groups.size),
    );

    return slot;
  }

  /**
   * Grant a user additional object blocks, raising the number of files they may hold.
   *
   * The library equivalent of the SINTRAN operator command
   * `@GIVE-OBJECT-BLOCKS (<directory>:)<user>,<count>`, which likewise ADDS to
   * the maximum rather than setting it. Each block is 256 files, so granting 3
   * blocks to a default user takes their ceiling from 256 to 1024 - exactly what
   * `@USER-STATISTICS` then reports as MAXIMUM NUMBER OF FILES. VERIFIED on a
   * live SINTRAN III K pack: user BIGMAN went from 256 to 1024 after
   * `@GIVE-OBJECT-BLOCKS BIGMAN,3`, with byte 47 reading back `0x31`.
   *
   * What this deliberately does NOT do:
   *  - It does not allocate the blocks. Only the MAXIMUM moves; a block is
   *    allocated the first time a file needs it, as SINTRAN does. A user granted
   *    4 blocks who holds 300 files still has `allocatedObjectBlocks` of 2.
   *  - It does not reserve disk pages. Object blocks cap the NUMBER of files,
   *    not their size; page quota is separate (`@GIVE-USER-SPACE`).
   *  - It does not convert an Indexed object file to SubIndexed. A second block
   *    lives at page 512 and up, which an Indexed object file cannot address at
   *    all, so growth into it fails later - when a file actually needs the block.
   *
   * @param indexOrName User index, or user name (case-insensitive).
   * @param count Object blocks to ADD. Must be 1 or more. Defaults to 1.
   * @returns The user's new maximum object block count, 2..16.
   * @throws {Error} when no such user exists, when `count` is below 1, or when
   * the grant would push the maximum past the SINTRAN limit of 16 blocks. The
   * limit case reports how many blocks are still grantable.
   */
  giveObjectBlocks(indexOrName: number | string, count = 1): number {
    this.ensureWritable();

    if (count < 1) throw new Error(`count must be 1 or more, got ${count}`);

    const user = this.requireUser(indexOrName);
    const newMax = user.maxObjectBlocks + count;
    if (newMax > MAX_OBJECT_BLOCKS) {
      // Refuse rather than clamp: clamping would leave the caller believing it
      // received what it asked for.
      const grantable = MAX_OBJECT_BLOCKS - user.maxObjectBlocks;
      throw new Error(
        `Cannot give user ${user.userName} ${count} more object block(s): they ` +
          `have ${user.maxObjectBlocks} and SINTRAN allows at most ` +
          `${MAX_OBJECT_BLOCKS} (${MAX_OBJECT_BLOCKS * FILES_PER_OBJECT_BLOCK} ` +
          `files). At most ${grantable} more can be granted.`,
      );
    }

    user.maxObjectBlocks = newMax;
    this.writeUserPage(user.userIndex);
    return newMax;
  }

  /**
   * Lower a user's object-block maximum.
   *
   * **This has no SINTRAN equivalent.** No command to remove object blocks is
   * documented in the manuals, nor was one found while carving the kernel -
   * consistent with the structure, since a file's number is derived from the
   * block it sits in, so dropping a block would orphan every file in it. This
   * exists only so a tool can undo its own grant on an image it is building, and
   * refuses anything that would strand a file.
   *
   * @param indexOrName User index, or user name (case-insensitive).
   * @param count Object blocks to remove. Must be 1 or more. Defaults to 1.
   * @returns The user's new maximum object block count.
   * @throws {Error} when no such user exists, when `count` is below 1, or when
   * the reduction would drop the maximum below the allocated count.
   */
  takeObjectBlocks(indexOrName: number | string, count = 1): number {
    this.ensureWritable();

    if (count < 1) throw new Error(`count must be 1 or more, got ${count}`);

    const user = this.requireUser(indexOrName);
    const newMax = user.maxObjectBlocks - count;
    if (newMax < user.allocatedObjectBlocks) {
      throw new Error(
        `Cannot take ${count} object block(s) from user ${user.userName}: ` +
          `${user.allocatedObjectBlocks} of ${user.maxObjectBlocks} are allocated ` +
          `and hold files. Lowering the maximum below the allocated count would ` +
          `orphan them.`,
      );
    }
    if (newMax < 1) throw new Error('A user must keep at least one object block');

    user.maxObjectBlocks = newMax;
    this.writeUserPage(user.userIndex);
    return newMax;
  }

  /**
   * Report a user's object-block position: what they hold and what they may hold.
   *
   * This is the query a user interface needs in order to answer "why can I not
   * add another file". Blocks are allocated on demand, so `allocatedObjectBlocks`
   * climbs by itself as files are created; only `maxObjectBlocks` needs an
   * operator.
   *
   * @param indexOrName User index, or user name (case-insensitive).
   * @throws {Error} when no such user exists.
   */
  getObjectBlockInfo(indexOrName: number | string): {
    userName: string;
    userIndex: number;
    allocatedObjectBlocks: number;
    maxObjectBlocks: number;
    filesInUse: number;
    maxFiles: number;
    allocatedFiles: number;
    canGrant: boolean;
    grantableBlocks: number;
  } {
    const user = this.requireUser(indexOrName);

    // Counted live rather than read from a stored total: SINTRAN never compacts
    // the object table, so deletions leave holes and any cached figure drifts.
    const filesInUse = this.objectFile.getUserObjects(user.userIndex).length;

    return {
      userName: user.userName,
      userIndex: user.userIndex,
      allocatedObjectBlocks: user.allocatedObjectBlocks,
      maxObjectBlocks: user.maxObjectBlocks,
      filesInUse,
      maxFiles: user.maxObjectBlocks * FILES_PER_OBJECT_BLOCK,
      allocatedFiles: user.allocatedObjectBlocks * FILES_PER_OBJECT_BLOCK,
      canGrant: user.maxObjectBlocks < MAX_OBJECT_BLOCKS,
      grantableBlocks: MAX_OBJECT_BLOCKS - user.maxObjectBlocks,
    };
  }

  /** Look a user up by index or name, throwing when absent. */
  private requireUser(indexOrName: number | string): UserEntry {
    const user =
      typeof indexOrName === 'number'
        ? this.userFile.getUser(indexOrName)
        : this.userFile.findUser(indexOrName);
    if (!user) throw new Error(`User not found: ${indexOrName}`);
    return user;
  }

  /** Clear a user's password (set to 0). */
  clearUserPassword(indexOrName: number | string): boolean {
    this.ensureWritable();

    let user: UserEntry | null;
    if (typeof indexOrName === 'number') {
      user = this.userFile.getUser(indexOrName);
    } else {
      user = this.userFile.findUser(indexOrName);
    }
    if (!user) return false;

    user.password = 0;
    this.writeUserPage(user.userIndex);
    return true;
  }

  // ── Friends ────────────────────────────────────────────────────────
  //
  // A user has 0..8 friends in its own entry; a friend grants another user
  // RWACD rights to this user's files. Owner/friend may be a name or a
  // decimal index (0-255). Persists only the owner's user page.

  /** Resolve a user ref (name or index 0-255) to a UserEntry, or null. */
  private resolveUserRef(ref: number | string): UserEntry | null {
    if (typeof ref === 'number') return this.userFile.getUser(ref);
    if (/^\d+$/.test(ref)) {
      const v = parseInt(ref, 10);
      return v >= 0 && v <= 255 ? this.userFile.getUser(v) : null;
    }
    return this.userFile.findUser(ref);
  }

  /** Resolve a friend ref to a user index (numeric = literal index). */
  private resolveFriendIndex(ref: number | string): number {
    if (typeof ref === 'number') {
      if (ref < 0 || ref > 255) throw new Error(`User index out of range: ${ref}`);
      return ref;
    }
    if (/^\d+$/.test(ref)) {
      const v = parseInt(ref, 10);
      if (v < 0 || v > 255) throw new Error(`User index out of range: ${ref}`);
      return v;
    }
    const u = this.userFile.findUser(ref);
    if (!u) throw new Error(`No such user: ${ref}`);
    return u.userIndex;
  }

  /** List a user's friends. Throws if the user does not exist. */
  listFriends(userRef: number | string): FriendInfo[] {
    const owner = this.resolveUserRef(userRef);
    if (!owner) throw new Error(`No such user: ${userRef}`);
    const out: FriendInfo[] = [];
    for (const f of owner.friends) {
      if (!f.entryUsed) continue;
      const idx = f.friendUserIndex;
      const fu = this.userFile.getUser(idx);
      out.push({
        index: idx,
        name: fu ? fu.userName : '',
        bits: f.bits,
        perms: f.getPermissionString(),
      });
    }
    return out;
  }

  /**
   * Add a friend to a user with the given permission letters (default 'RWA').
   * Throws if owner/friend unknown, already a friend, or the list is full.
   */
  addFriend(userRef: number | string, friendRef: number | string, perms: string = 'RWA'): void {
    this.ensureWritable();
    const owner = this.resolveUserRef(userRef);
    if (!owner) throw new Error(`No such user: ${userRef}`);
    const friendIndex = this.resolveFriendIndex(friendRef);
    const permBits = UserFriend.parsePermissions(perms ? perms : 'RWA');
    if (owner.isFriend(friendIndex)) throw new Error(`Already a friend: ${friendRef}`);
    if (!owner.addFriend(friendIndex, permBits)) throw new Error('Friend list is full (max 8)');
    this.writeUserPage(owner.userIndex);
  }

  /** Remove a friend from a user. Throws if owner unknown or not a friend. */
  removeFriend(userRef: number | string, friendRef: number | string): void {
    this.ensureWritable();
    const owner = this.resolveUserRef(userRef);
    if (!owner) throw new Error(`No such user: ${userRef}`);
    const friendIndex = this.resolveFriendIndex(friendRef);
    if (!owner.removeFriend(friendIndex)) throw new Error(`Not a friend: ${friendRef}`);
    this.writeUserPage(owner.userIndex);
  }

  // ── Bitmap queries ─────────────────────────────────────────────────

  isBlockUsed(blockId: number): boolean {
    return this.bitFile.isBlockUsed(blockId);
  }

  /**
   * True if the image size was not a whole multiple of the page size. The
   * trailing partial page was dropped and the filesystem forced read-only.
   * False for cleanly page-aligned images.
   */
  get unaligned(): boolean {
    return this._unaligned;
  }

  getFreePages(): number {
    return this.bitFile.getFreePages();
  }

  getUsedPages(): number {
    return this.bitFile.calcUsedPages();
  }

  // ── Low-level access ───────────────────────────────────────────────

  getObjectEntries(): ObjectEntry[] {
    return this.objectFile.getObjects();
  }

  /**
   * Look up one object entry. Pass fileType to distinguish files that share a
   * name, e.g. TCP-IP-LO-D02:MODE from TCP-IP-LO-D02:LIST.
   */
  getObjectEntry(name: string, userName: string, fileType?: string): ObjectEntry | null {
    return this.objectFile.findObject(name, userName, fileType);
  }

  // ── Boot loader ────────────────────────────────────────────────────

  /** Detect the boot format of this image. */
  detectBootFormat(): BootFormat {
    const page0 = this.readPage(0);
    return detectBootFormat(page0);
  }

  /** Load boot code from this image, or null if no boot format detected. */
  loadBootCode(): BootCode | null {
    const page0 = this.readPage(0);
    return loadBootCodeFromPage(page0);
  }

  /** Check if this image contains bootable code. */
  isBootable(): boolean {
    return this.detectBootFormat() !== BootFormat.None;
  }

  /**
   * Detect the hard-disk controller family (SMD/ECC, Winchester, SCSI) targeted by
   * a raw-binary bootstrap. Returns BootControllerType.Unknown for BPUN/FloMon
   * (floppy) boots or non-bootable disks.
   */
  detectBootControllerType(): BootControllerType {
    const page0 = this.readPage(0);
    return detectControllerType(page0);
  }

  // ── Diagnostics ────────────────────────────────────────────────────

  /** Basic integrity verification. */
  verifyIntegrity(): boolean {
    if (!this.masterBlock.isValid()) return false;

    // Check all file blocks are marked in bitmap
    const objects = this.objectFile.getObjects();
    for (let i = 0; i < objects.length; i++) {
      const obj = objects[i];
      if (!obj.filePointer || !obj.filePointer.isValid()) continue;

      // Check index block
      if (!this.bitFile.isBlockUsed(obj.filePointer.blockId)) return false;
    }
    return true;
  }

  /** Generate a text report about the filesystem. */
  generateReport(): string {
    const totalPages = this.data.length / NDFS_PAGE_SIZE;
    const usedPages = this.bitFile.calcUsedPages();
    const freePages = this.bitFile.getFreePages();
    const users = this.userFile.getUsers();
    const objects = this.objectFile.getObjects();

    let report = `NDFS Filesystem Report\n`;
    report += `======================\n`;
    report += `Volume: ${this.masterBlock.directoryName}\n`;
    report += `Total pages: ${totalPages}\n`;
    report += `Used pages: ${usedPages}\n`;
    report += `Free pages: ${freePages}\n`;
    report += `Users: ${users.length}\n`;
    report += `Files: ${objects.length}\n\n`;

    report += `Users:\n`;
    for (let i = 0; i < users.length; i++) {
      const u = users[i];
      report += `  [${u.userIndex}] ${u.userName} - Reserved: ${u.pagesReserved}, Used: ${u.pagesUsed}\n`;
    }

    report += `\nFiles:\n`;
    for (let i = 0; i < objects.length; i++) {
      const o = objects[i];
      report += `  ${o.userName}/${o.objectName}:${o.type} - ${o.bytesInFile} bytes (${o.pagesInFile} pages)\n`;
    }

    return report;
  }

  // ── XAT (Extended Attribute) support ────────────────────────────

  /**
   * Get XAT properties for a file, or null if not found.
   * @param path - "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"
   */
  getFileProperties(path: string): XatProperties | null {
    const obj = this.findObject(path);
    if (!obj) return null;
    return objectEntryToXat(obj, this.findHoles(path));
  }

  /**
   * Ascending logical page indices of a file's sparse holes; empty when solid.
   *
   * Contiguous files are solid by construction, so this is only ever non-empty
   * for Indexed and SubIndexed ones.
   * @param path - "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"
   */
  private findHoles(path: string): number[] {
    const blocks = this.getFileBlocks(path);
    const holes: number[] = [];
    for (let i = 0; i < blocks.length; i++) {
      if (blocks[i] === 0) holes.push(i);
    }
    return holes;
  }

  /**
   * Read a file's data along with its XAT properties.
   * @param path - "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"
   */
  readFileWithProperties(path: string): { data: Uint8Array; properties: XatProperties } {
    const obj = this.findObject(path);
    if (!obj) throw new Error(`File not found: ${path}`);
    const data = this.readObjectData(obj);
    const properties = objectEntryToXat(obj, this.findHoles(path));
    return { data, properties };
  }

  /**
   * Write a file and apply XAT properties to restore metadata.
   * The file is written first, then the XAT properties are applied
   * to the resulting object entry.
   *
   * @param path - "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"
   * @param data - The raw bytes to write.
   * @param properties - XAT properties to apply after writing.
   */
  writeFileWithProperties(path: string, data: Uint8Array, properties: XatProperties): void {
    // The one path that may legitimately create holes: the sidecar's
    // `ndfs.holes` records where the ORIGINAL file's holes were, so restoring
    // them reproduces the original rather than inventing holes from whatever
    // happens to be zero (see writeFile).
    const recorded = (properties as Record<string, unknown>)[XAT_KEY_HOLES];
    const holes = Array.isArray(recorded) ? (recorded as number[]) : undefined;
    this.writeFile(path, data, 'none', holes);

    // Now find the written entry and apply XAT properties
    const obj = this.findObject(path);
    if (obj) {
      // Only apply status-related fields, not size/pages (those are set by writeFile)
      if (typeof properties['ndfs.access_bits'] === 'number') {
        obj.accessBits = properties['ndfs.access_bits'] as number;
      }
      if (typeof properties['ndfs.file_type'] === 'number') {
        obj.fileType = properties['ndfs.file_type'] as number;
      }
      if (typeof properties['ndfs.date_created'] === 'number') {
        obj.dateCreated = properties['ndfs.date_created'] as number;
      }
      if (typeof properties['ndfs.last_read_date'] === 'number') {
        obj.lastDateRead = properties['ndfs.last_read_date'] as number;
      }
      if (typeof properties['ndfs.last_write_date'] === 'number') {
        obj.lastDateWritten = properties['ndfs.last_write_date'] as number;
      }
      // Properties change only this file's object entry.
      this.writeObjectPage(obj.objectIndex);
    }
  }

  // ══════════════════════════════════════════════════════════════════
  //  Private implementation
  // ══════════════════════════════════════════════════════════════════

  // -- SINTRAN initial commands ------------------------------------------

  /**
   * Return the ordered data-block IDs of a file (0 marks a sparse hole).
   *
   * The list is indexed by LOGICAL page, so a hole keeps its own position and
   * the pages after it are not shifted.
   *
   * FIXED 2026-07-30. This used to walk the index for `pagesInFile` slots. That
   * is wrong for a sparse file: `pagesInFile` counts the pages really allocated,
   * while the index is as long as the file's logical extent, holes included.
   * Measured on `(SYSTEM)S3-CONFIG-E01:PROG` in `BIGDISK0-K-103.IMG`:
   * `pagesInFile` is 89, the last used index slot is 98, and slots 54..63 are
   * holes. Walking only 89 slots stopped at 88, dropping the last 10 real pages
   * while still reporting the 10 holes -- 79 real blocks instead of 89, with
   * every page past slot 53 at the wrong offset.
   *
   * Confirmed independently on real hardware: transferring that file between two
   * ND-100s sends 89 messages carrying page numbers 0..53 then 64..98.
   */
  getFileBlocks(path: string): number[] {
    const obj = this.findObject(path);
    if (!obj) throw new Error(`File not found: ${path}`);
    const blocks: number[] = [];
    const fp = obj.filePointer;
    if (!fp || fp.blockId === 0) return blocks;

    // Logical extent, holes included. Contiguous files have no holes, so their
    // own page count is the honest bound and is kept.
    const logicalPages = Math.ceil(obj.bytesInFile / NDFS_PAGE_SIZE);

    if (fp.type === PointerType.Contiguous) {
      for (let i = 0; i < obj.pagesInFile; i++) blocks.push(fp.blockId + i);
    } else if (fp.type === PointerType.Indexed) {
      const ib = this.readPage(fp.blockId);
      for (let i = 0; i < Math.min(logicalPages, MAX_OBJECT_FILE_POINTERS); i++) {
        blocks.push(BlockPointer.fromBytes(ib, i * 4).blockId);
      }
    } else if (fp.type === PointerType.SubIndexed) {
      const sib = this.readPage(fp.blockId);
      let remaining = logicalPages;
      for (let si = 0; si < MAX_OBJECT_FILE_POINTERS && remaining > 0; si++) {
        const ip = BlockPointer.fromBytes(sib, si * 4);
        if (ip.blockId === 0) {
          // an empty entry is 512 holes, not the end of the file
          for (let j = 0; j < MAX_OBJECT_FILE_POINTERS && remaining > 0; j++) {
            blocks.push(0);
            remaining--;
          }
          continue;
        }
        const ib = this.readPage(ip.blockId);
        for (let j = 0; j < MAX_OBJECT_FILE_POINTERS && remaining > 0; j++) {
          blocks.push(BlockPointer.fromBytes(ib, j * 4).blockId);
          remaining--;
        }
      }
    }
    return blocks;
  }

  /**
   * Overwrite `data` at file-relative `fileOffset` IN PLACE, touching only the
   * page(s) that overlap. The file's allocation never changes, so this is safe
   * on a large contiguous system file (e.g. SEGFIL0).
   */
  patchFileRegion(path: string, fileOffset: number, data: Uint8Array): void {
    this.ensureWritable();
    if (data.length === 0) return;
    const blocks = this.getFileBlocks(path);
    const end = fileOffset + data.length;
    const startPage = Math.floor(fileOffset / NDFS_PAGE_SIZE);
    const endPage = Math.floor((end - 1) / NDFS_PAGE_SIZE);
    if (endPage >= blocks.length) throw new Error('patch region extends past the file');
    for (let pg = startPage; pg <= endPage; pg++) {
      if (blocks[pg] === 0) throw new Error('patch region overlaps a sparse hole');
      const page = new Uint8Array(this.readPage(blocks[pg]));
      const pageStart = pg * NDFS_PAGE_SIZE;
      const ovStart = Math.max(fileOffset, pageStart);
      const ovEnd = Math.min(end, pageStart + NDFS_PAGE_SIZE);
      page.set(data.subarray(ovStart - fileOffset, ovEnd - fileOffset), ovStart - pageStart);
      this.writePage(blocks[pg], page);
    }
  }

  /** Path (USER/NAME:TYPE) of the SEGFIL0/SEGFIL object, or null. */
  private findSegfilPath(): string | null {
    for (const obj of this.getObjectEntries()) {
      const name = obj.objectName.toUpperCase();
      if (name === 'SEGFIL0' || name === 'SEGFIL') {
        const t = (obj.type ?? '').replace(/['\s]+$/, '');
        return t ? `${obj.userName}/${obj.objectName}:${t}` : `${obj.userName}/${obj.objectName}`;
      }
    }
    return null;
  }

  /** Locate and decode the SINTRAN initial-command buffer, or null. */
  readInitialCommands(): sintran.InitialCommands | null {
    const path = this.findSegfilPath();
    if (path === null) return null;
    return sintran.locate(this.readFile(path));
  }

  /**
   * Replace the initial-command buffer and write it back to SEGFIL0 in place.
   * Returns false when the pack has no locatable buffer.
   */
  writeInitialCommands(commands: readonly string[]): boolean {
    this.ensureWritable();
    const path = this.findSegfilPath();
    if (path === null) return false;
    const loc = sintran.locate(this.readFile(path));
    if (!loc) return false;
    const { bytes, textLen } = sintran.encodeBuffer(commands);
    this.patchFileRegion(path, loc.byteOffset, sintran.buildRegionImage(bytes, textLen));
    return true;
  }

  /**
   * Repair a header-corrupted buffer the normal locator can no longer see, by
   * matching the surviving command text. Returns { byteOffset, score } or null
   * when no unambiguous target is found.
   */
  repairInitialCommands(commands: readonly string[]): { byteOffset: number; score: number } | null {
    this.ensureWritable();
    const path = this.findSegfilPath();
    if (path === null) return null;
    const target = sintran.locateRepairTarget(this.readFile(path));
    if (!target) return null;
    const { bytes, textLen } = sintran.encodeBuffer(commands);
    this.patchFileRegion(path, target.byteOffset, sintran.buildRegionImage(bytes, textLen));
    return target;
  }

  /** Enumerate every INIBU-containing segment candidate (diagnostic). */
  diagnoseInitialCommands(): sintran.InitCmdCandidate[] {
    const path = this.findSegfilPath();
    if (path === null) return [];
    return sintran.enumerateCandidates(this.readFile(path));
  }

  /**
   * What could not be read off this image.
   *
   * All three counts are zero on an intact floppy. Any of them above zero
   * means the listing is what survived a damaged one: each lost object page
   * took up to 32 file entries with it, each lost user page took up to 32
   * user names, and a lost bit-file page leaves the pages it covered reading
   * as free. Nothing here says WHICH entries were lost - that cannot be known
   * from the media - only that the picture is incomplete.
   */
  getDamageReport(): { objectPages: number; userPages: number; bitFilePages: number } {
    return {
      objectPages: this.objectFile.unreadablePages,
      userPages: this.userFile.unreadablePages,
      bitFilePages: this.unreadableBitFilePages,
    };
  }

  private readPage(blockId: number): Uint8Array {
    const offset = blockId * NDFS_PAGE_SIZE;
    if (offset + NDFS_PAGE_SIZE > this.data.length) {
      throw new Error(`Block ${blockId} out of range`);
    }
    return this.data.subarray(offset, offset + NDFS_PAGE_SIZE);
  }

  private writePage(blockId: number, pageData: Uint8Array): void {
    const offset = blockId * NDFS_PAGE_SIZE;
    if (offset + NDFS_PAGE_SIZE > this.data.length) {
      throw new Error(`Block ${blockId} out of range`);
    }
    this.data.set(pageData.subarray(0, NDFS_PAGE_SIZE), offset);
  }

  private loadStructures(): void {
    const mb = this.masterBlock;

    // Load user file. The object file does not depend on it, so a user file
    // that cannot be read must not take the file listing down with it: a
    // floppy whose user index page is damaged still has an intact object file
    // naming every file on the disk, and reading one page of a damaged floppy
    // throws out of readPage(). Without user names the objects keep their user
    // index and nothing else is lost.
    const userMap = new Map<number, string>();
    if (mb.userFilePointer && mb.userFilePointer.isValid()) {
      try {
        const indexPage = this.readPage(mb.userFilePointer.blockId);
        this.userFile.loadFromPages(indexPage, (id) => this.readPage(id));

        const users = this.userFile.getUsers();
        for (let i = 0; i < users.length; i++) {
          userMap.set(users[i].userIndex, users[i].userName);
        }
      } catch {
        // damaged user file: carry on with no user names
      }
    }

    // Load object file
    if (mb.objectFilePointer && mb.objectFilePointer.isValid()) {
      this.objectFile.loadFromPages(mb.objectFilePointer, (id) => this.readPage(id));

      // Resolve user names on objects
      const objects = this.objectFile.getObjects();
      for (let i = 0; i < objects.length; i++) {
        const name = userMap.get(objects[i].userIndex);
        if (name) objects[i].userName = name;
      }
    }

    // Load bit file
    if (mb.bitFilePointer && mb.bitFilePointer.isValid()) {
      const totalPages = this.data.length / NDFS_PAGE_SIZE;
      this.bitFile.initialize(totalPages);
      this.bitFile.bitAddress = mb.bitFilePointer.blockId;

      // Bound the ALLOCATION window by the directory's declared capacity (extPagesAvailable,
      // words 1756B-1757B) when the extended-info block is valid. SINTRAN never hands out a page
      // at or above the capacity; the gap to the physical device is the drive's spare region.
      // The `<= totalPages` clamp matters: a real Winchester image declares a capacity larger
      // than the file. Falls back to the whole device when there is no valid extended info.
      if (mb.extValid && mb.extPagesAvailable > 0 && mb.extPagesAvailable <= totalPages) {
        this.bitFile.allocCeiling = mb.extPagesAvailable;
      }

      // Determine bitmap size in pages
      // Whole 16-bit words - the bit file is word addressed, so ceil(pages/8) cuts the
      // last word in half on any device whose page count is not a multiple of 16 and
      // loses the pages living in its high byte. A 616-page ND floppy loses 608..615.
      const bitmapBytes = (Math.ceil(totalPages / 8) + 1) & ~1;
      // SINTRAN sizes the bit file from the declared capacity, not from the device length
      // (docs/NDFS-ALLOCATION-RULES.md); the bitmap in memory still covers the whole device,
      // the pages past the on-disk bit file read as free.
      const bitmapPages = NdfsFileSystem.bitFileDiskPages(mb, totalPages);

      // Read contiguous bitmap pages
      // A bitmap page that cannot be read leaves its own pages reading as
      // free; it must not cost the file listing, which is what the disk is
      // kept for. The pages that do read are loaded.
      const bitmapData = new Uint8Array(Math.max(bitmapPages * NDFS_PAGE_SIZE, bitmapBytes));
      for (let i = 0; i < bitmapPages; i++) {
        try {
          const page = this.readPage(mb.bitFilePointer.blockId + i);
          bitmapData.set(page, i * NDFS_PAGE_SIZE);
        } catch {
          this.unreadableBitFilePages++;
        }
      }
      this.bitFile.loadBitmap(bitmapData.subarray(0, bitmapBytes));
    }
  }

  /**
   * Pages the bit file occupies on disk: ceil(capacity / 16384), the capacity being the declared
   * directory size when a valid Extended Info Block gives it, else the device length.
   */
  private static bitFileDiskPages(mb: MasterBlock, totalPages: number): number {
    let basis = totalPages;
    if (mb.extValid && mb.extPagesAvailable > 0 && mb.extPagesAvailable <= totalPages) {
      basis = mb.extPagesAvailable;
    }
    const bitmapBytes = (Math.ceil(basis / 8) + 1) & ~1;
    return Math.ceil(bitmapBytes / NDFS_PAGE_SIZE);
  }

  /**
   * Index and sub-index blocks that belong to `obj`: none for a contiguous file, one for an
   * Indexed file, the top block plus one per valid slot for a SubIndexed file. SINTRAN charges
   * them to the owner like data pages (docs/NDFS-ALLOCATION-RULES.md section 7).
   */
  private countIndexBlocksForObject(obj: ObjectEntry): number {
    if (!obj.filePointer || obj.filePointer.blockId === 0) return 0;
    if (obj.filePointer.type === PointerType.Indexed) return 1;
    if (obj.filePointer.type === PointerType.SubIndexed) {
      const subIndexPage = this.readPage(obj.filePointer.blockId);
      let count = 1;
      for (let si = 0; si < MAX_OBJECT_FILE_POINTERS; si++) {
        if (BlockPointer.fromBytes(subIndexPage, si * 4).isValid()) count++;
      }
      return count;
    }
    return 0;
  }

  /** Pages SINTRAN charges to the owner for one file: real data pages + index blocks. */
  private countChargedPagesForObject(obj: ObjectEntry): number {
    return this.countRealDataPagesInList(this.resolveDataPointers(obj)) + this.countIndexBlocksForObject(obj);
  }

  /**
   * Object data pages that belong to one user: the valid pointers in the user's 8 slots of the
   * object index (slot = user*8 .. user*8+7, in the user's group of 64). Each is charged to the user.
   */
  private countUserObjectPages(userIndex: number): number {
    const mb = this.masterBlock;
    if (!mb.objectFilePointer || !mb.objectFilePointer.isValid()) return 0;
    const group = Math.floor(userIndex / USERS_PER_INDEX_BLOCK);
    const first = (userIndex % USERS_PER_INDEX_BLOCK) * PAGES_PER_OBJECT_BLOCK;
    let indexPage: Uint8Array;
    if (mb.objectFilePointer.type === PointerType.Indexed) {
      if (group !== 0) return 0;
      indexPage = this.readPage(mb.objectFilePointer.blockId);
    } else if (mb.objectFilePointer.type === PointerType.SubIndexed) {
      const subPage = this.readPage(mb.objectFilePointer.blockId);
      const gp = BlockPointer.fromBytes(subPage, group * 4);
      if (!gp.isValid()) return 0;
      indexPage = this.readPage(gp.blockId);
    } else {
      return 0;
    }
    let count = 0;
    for (let k = 0; k < PAGES_PER_OBJECT_BLOCK; k++) {
      if (BlockPointer.fromBytes(indexPage, (first + k) * 4).isValid()) count++;
    }
    return count;
  }

  private readObjectData(obj: ObjectEntry): Uint8Array {
    if (!obj.filePointer || obj.filePointer.blockId === 0) {
      return new Uint8Array(0);
    }

    const result = new Uint8Array(obj.bytesInFile);
    let bytesRead = 0;

    if (obj.filePointer.type === PointerType.Contiguous) {
      // Sequential pages starting at blockId
      for (let i = 0; i < obj.pagesInFile && bytesRead < obj.bytesInFile; i++) {
        const page = this.readPage(obj.filePointer.blockId + i);
        const toCopy = Math.min(NDFS_PAGE_SIZE, obj.bytesInFile - bytesRead);
        result.set(page.subarray(0, toCopy), bytesRead);
        bytesRead += toCopy;
      }
    } else if (obj.filePointer.type === PointerType.Indexed) {
      bytesRead = this.readIndexedData(obj.filePointer.blockId, obj, result);
    } else if (obj.filePointer.type === PointerType.SubIndexed) {
      bytesRead = this.readSubIndexedData(obj.filePointer.blockId, obj, result);
    }

    return result;
  }

  private readIndexedData(
    indexBlockId: number,
    obj: ObjectEntry,
    result: Uint8Array,
  ): number {
    const indexPage = this.readPage(indexBlockId);
    let bytesRead = 0;

    for (let i = 0; i < MAX_OBJECT_FILE_POINTERS && bytesRead < obj.bytesInFile; i++) {
      const ptr = BlockPointer.fromBytes(indexPage, i * 4);

      if (ptr.blockId === 0) {
        // Sparse hole: fill with zeros
        const toCopy = Math.min(NDFS_PAGE_SIZE, obj.bytesInFile - bytesRead);
        // result is already zero-initialized
        bytesRead += toCopy;
      } else {
        const page = this.readPage(ptr.blockId);
        const toCopy = Math.min(NDFS_PAGE_SIZE, obj.bytesInFile - bytesRead);
        result.set(page.subarray(0, toCopy), bytesRead);
        bytesRead += toCopy;
      }
    }
    return bytesRead;
  }

  private readSubIndexedData(
    subIndexBlockId: number,
    obj: ObjectEntry,
    result: Uint8Array,
  ): number {
    const subIndexPage = this.readPage(subIndexBlockId);
    let bytesRead = 0;

    for (let si = 0; si < MAX_OBJECT_FILE_POINTERS && bytesRead < obj.bytesInFile; si++) {
      const indexPtr = BlockPointer.fromBytes(subIndexPage, si * 4);
      // Entry si covers file bytes from si * 512 pages on, set or not: an empty entry is a
      // hole of 512 pages (SINTRAN III leaves the entry of a group with no page in it empty -
      // a domain of the ND Linker B01, 2026-10-14: entries 0, 2, 4, 6 set). The position
      // must move past it.
      bytesRead = si * MAX_OBJECT_FILE_POINTERS * NDFS_PAGE_SIZE;
      if (bytesRead >= obj.bytesInFile) break;
      if (!indexPtr.isValid()) continue;

      const indexPage = this.readPage(indexPtr.blockId);
      for (let i = 0; i < MAX_OBJECT_FILE_POINTERS && bytesRead < obj.bytesInFile; i++) {
        const dataPtr = BlockPointer.fromBytes(indexPage, i * 4);
        if (dataPtr.blockId === 0) {
          const toCopy = Math.min(NDFS_PAGE_SIZE, obj.bytesInFile - bytesRead);
          bytesRead += toCopy;
        } else {
          const page = this.readPage(dataPtr.blockId);
          const toCopy = Math.min(NDFS_PAGE_SIZE, obj.bytesInFile - bytesRead);
          result.set(page.subarray(0, toCopy), bytesRead);
          bytesRead += toCopy;
        }
      }
    }
    return bytesRead;
  }

  /**
   * Allocate blocks and write file data to disk.
   * Returns { topBlockId, pointerType, indexBlocksUsed } describing the allocation.
   * Supports indexed (<=512 pages) and sub-indexed (>512 pages) layouts.
   */
  private allocateAndWriteData(
    fileData: Uint8Array,
    dataPages: number,
    holes?: ReadonlySet<number>,
  ): { topBlockId: number; pointerType: PointerType; indexBlocksUsed: number } {
    const useSubIndexed = dataPages > MAX_OBJECT_FILE_POINTERS;

    if (!useSubIndexed) {
      // Simple indexed allocation
      const indexBlockId = this.bitFile.findFreeHint();
      if (indexBlockId < 0) throw new Error('No free blocks for index block');
      this.bitFile.markBlockUsed(indexBlockId);

      const indexPage = new Uint8Array(NDFS_PAGE_SIZE);

      for (let i = 0; i < dataPages; i++) {
        this.writeDataPageToIndex(fileData, i, indexPage, i, holes);
      }

      this.writePage(indexBlockId, indexPage);
      return { topBlockId: indexBlockId, pointerType: PointerType.Indexed, indexBlocksUsed: 1 };
    }

    // Sub-indexed allocation: sub-index block -> index blocks -> data blocks
    const subIndexBlockId = this.bitFile.findFreeHint();
    if (subIndexBlockId < 0) throw new Error('No free blocks for sub-index block');
    this.bitFile.markBlockUsed(subIndexBlockId);

    const subIndexPage = new Uint8Array(NDFS_PAGE_SIZE);
    const numIndexBlocks = Math.ceil(dataPages / MAX_OBJECT_FILE_POINTERS);
    let indexBlocksUsed = 1; // count the sub-index block

    for (let si = 0; si < numIndexBlocks; si++) {
      const idxBlockId = this.bitFile.findFreeHint();
      if (idxBlockId < 0) throw new Error('No free blocks for index block');
      this.bitFile.markBlockUsed(idxBlockId);
      indexBlocksUsed++;

      const idxPtr = new BlockPointer(idxBlockId, PointerType.Contiguous);
      idxPtr.toBytes(subIndexPage, si * 4);

      const indexPage = new Uint8Array(NDFS_PAGE_SIZE);
      const startPage = si * MAX_OBJECT_FILE_POINTERS;
      const endPage = Math.min(startPage + MAX_OBJECT_FILE_POINTERS, dataPages);

      for (let i = startPage; i < endPage; i++) {
        this.writeDataPageToIndex(fileData, i, indexPage, i - startPage, holes);
      }

      this.writePage(idxBlockId, indexPage);
    }

    this.writePage(subIndexBlockId, subIndexPage);
    return { topBlockId: subIndexBlockId, pointerType: PointerType.SubIndexed, indexBlocksUsed };
  }

  /**
   * Write a single data page and store its pointer in an index page.
   *
   * A page becomes a sparse hole ONLY when `holes` names it. What the page
   * contains never decides it.
   *
   * It used to: any page that was entirely zero and filled a whole page was
   * silently made a hole. That was wrong in a way this library could not see,
   * because reading such a file back returns the same bytes either way.
   * SINTRAN is not so forgiving -- READ-BI and READ-PROGFILE stop at a hole
   * with
   *
   *     NO SUCH PAGE
   *     ERROR IN ACCESSING INPUT FILE
   *
   * so a :BPUN or :PROG copied onto a pack that way is a file the machine
   * refuses to load. Measured on a SINTRAN III VSX-K pack 2026-08-18: the
   * (TCP-IP)TCP-SER-B0..B3-B05:BPUN PIOC bank images arrived with 0/48/62/53
   * invented holes, and banks 1-3 all failed with NO SUCH PAGE while bank 0
   * (which had no all-zero page) loaded. The same files on genuine ND media
   * have no holes at all -- a 128 KB bank dump is fully allocated even where
   * the bank is empty.
   *
   * Holes are still supported; they just have to be declared, which is what
   * the XAT sidecar's "ndfs.holes" records them for.
   */
  private writeDataPageToIndex(
    fileData: Uint8Array,
    dataPageIndex: number,
    indexPage: Uint8Array,
    slotInIndex: number,
    holes?: ReadonlySet<number>,
  ): void {
    const pageOffset = dataPageIndex * NDFS_PAGE_SIZE;
    const pageEnd = Math.min(pageOffset + NDFS_PAGE_SIZE, fileData.length);
    const pageSlice = fileData.subarray(pageOffset, pageEnd);

    if (holes && holes.has(dataPageIndex)) {
      // Sparse hole: BlockPointer 0, no disk block consumed.
      writeUint32BE(indexPage, slotInIndex * 4, 0);
    } else {
      const dataBlockId = this.bitFile.findFreeDataBlock();
      if (dataBlockId < 0) throw new Error('No free blocks for data');
      this.bitFile.markBlockUsed(dataBlockId);

      const dataPage = new Uint8Array(NDFS_PAGE_SIZE);
      dataPage.set(pageSlice);
      this.writePage(dataBlockId, dataPage);

      const dataPtr = new BlockPointer(dataBlockId, PointerType.Contiguous);
      dataPtr.toBytes(indexPage, slotInIndex * 4);
    }
  }

  /**
   * Ensure the object-file directory data page holding `objectIndex` exists,
   * allocating and linking it on demand (SINTRAN/RetroCore do this so each
   * user's region grows as needed). The page index in the object-file index
   * block is objectIndex/32; for user U that maps to slots U*8..U*8+7.
   */
  private ensureObjectDirPage(objectIndex: number): void {
    const mb = this.masterBlock;
    if (!mb.objectFilePointer || !mb.objectFilePointer.isValid()) return;
    const pageIdx = Math.floor(objectIndex / ENTRIES_PER_PAGE);

    if (mb.objectFilePointer.type === PointerType.Indexed && pageIdx < MAX_OBJECT_FILE_POINTERS) {
      // Fits in the existing single-level index -- original behavior, unchanged.
      this.ensureObjectDirPageInGroup(mb.objectFilePointer.blockId, pageIdx, pageIdx);
      return;
    }

    if (mb.objectFilePointer.type === PointerType.Indexed) {
      // One-time conversion: the object file has grown past MAX_OBJECT_FILE_POINTERS
      // (512) directory pages, so a plain Indexed structure can no longer address
      // `pageIdx`. Wrap the existing Indexed block as group 0 of a new SubIndexed
      // structure. No data migration -- the old block's directory-page pointers for
      // pageIdx 0..511 stay exactly where they are; we only add a level of
      // indirection above it (mirrors AllocateAndWriteData's own SubIndexed growth).
      const oldIndexBlockId = mb.objectFilePointer.blockId;

      const subIndexBlock = this.bitFile.findFreeHint();
      if (subIndexBlock < 0) throw new Error('No free block for object-file sub-index conversion');
      this.bitFile.markBlockUsed(subIndexBlock);

      const subIndexBuffer = new Uint8Array(NDFS_PAGE_SIZE);
      new BlockPointer(oldIndexBlockId, PointerType.Contiguous).toBytes(subIndexBuffer, 0);
      this.writePage(subIndexBlock, subIndexBuffer);

      mb.objectFilePointer = new BlockPointer(subIndexBlock, PointerType.SubIndexed);
      this.persistMasterBlock();
    }

    // SubIndexed case (either already was, or was just converted above).
    const subIdx = Math.floor(pageIdx / MAX_OBJECT_FILE_POINTERS);
    const innerIdx = pageIdx % MAX_OBJECT_FILE_POINTERS;
    const subPage = this.readPage(mb.objectFilePointer.blockId);
    let subPtr = BlockPointer.fromBytes(subPage, subIdx * 4);
    if (!subPtr.isValid()) {
      const ib = this.bitFile.findFreeHint();
      if (ib < 0) throw new Error('No free block for object-file group index page');
      this.bitFile.markBlockUsed(ib);
      this.writePage(ib, new Uint8Array(NDFS_PAGE_SIZE));
      new BlockPointer(ib, PointerType.Contiguous).toBytes(subPage, subIdx * 4);
      this.writePage(mb.objectFilePointer.blockId, subPage);
      subPtr = new BlockPointer(ib, PointerType.Contiguous);
    }
    this.ensureObjectDirPageInGroup(subPtr.blockId, innerIdx, pageIdx);
  }

  /**
   * Ensure the directory data page at `pageIdx` within a single Indexed block
   * (either the object file's top-level block, or one SubIndexed group's own
   * index block) exists, allocating and linking it on demand.
   */
  private ensureObjectDirPageInGroup(indexBlockId: number, pageIdx: number, globalPageIdx: number): void {
    const indexPage = this.readPage(indexBlockId);
    const ptr = BlockPointer.fromBytes(indexPage, pageIdx * 4);
    if (ptr.isValid()) return; // already allocated and linked

    const blk = this.bitFile.findFreeHint();
    if (blk < 0) throw new Error('No free blocks for object directory page');
    this.bitFile.markBlockUsed(blk);
    // SINTRAN charges an object data page to its owner (8 pages per user, 64 users per index block).
    const ownerUser =
      Math.floor(globalPageIdx / PAGES_PER_INDEX_BLOCK) * USERS_PER_INDEX_BLOCK +
      Math.floor((globalPageIdx % PAGES_PER_INDEX_BLOCK) / PAGES_PER_OBJECT_BLOCK);
    const owner = this.userFile.getUser(ownerUser);
    if (owner) owner.pagesUsed++;
    this.writePage(blk, new Uint8Array(NDFS_PAGE_SIZE));
    new BlockPointer(blk, PointerType.Contiguous).toBytes(indexPage, pageIdx * 4);
    this.writePage(indexBlockId, indexPage);
  }

  /**
   * Ensure the UserFile directory data page holding `userIndex` exists,
   * allocating and linking it on demand. Mirrors ensureObjectDirPage()'s
   * Indexed-branch logic, but the UserFile index block is always a plain
   * Indexed structure (max 256 users fits in a single 512-pointer index
   * block, per docs\NDFS-FORMAT.md's "User File | Indexed (01)" table), so
   * there is no SubIndexed case to handle here.
   */
  private ensureUserDirPage(userIndex: number): void {
    const mb = this.masterBlock;
    if (!mb.userFilePointer || !mb.userFilePointer.isValid()) return;
    const pageIdx = Math.floor(userIndex / ENTRIES_PER_PAGE);
    if (pageIdx >= MAX_USER_FILE_POINTERS) return;

    const indexPage = this.readPage(mb.userFilePointer.blockId);
    const ptr = BlockPointer.fromBytes(indexPage, pageIdx * 4);
    if (ptr.isValid()) return; // page already allocated on disk

    const blk = this.bitFile.findFreeHint();
    if (blk < 0) throw new Error('No free blocks for user directory page');
    this.bitFile.markBlockUsed(blk);
    this.writePage(blk, new Uint8Array(NDFS_PAGE_SIZE));
    new BlockPointer(blk, PointerType.Contiguous).toBytes(indexPage, pageIdx * 4);
    this.writePage(mb.userFilePointer.blockId, indexPage);
  }

  private createNewFile(
    objectName: string,
    fileType: string,
    user: UserEntry,
    fileData: Uint8Array,
    holes?: ReadonlySet<number>,
    accessBits?: number,
  ): void {
    let dataPages = Math.ceil(fileData.length / NDFS_PAGE_SIZE);
    if (dataPages === 0) dataPages = 1;

    // Choose the object slot inside the owning user's region, and make sure
    // that user's directory page exists, before allocating file data.
    const slot = this.allocateUserObjectSlot(user);
    this.ensureObjectDirPage(slot);

    const { topBlockId, pointerType, indexBlocksUsed } = this.allocateAndWriteData(fileData, dataPages, holes);

    // Create object entry
    const entry = new ObjectEntry();
    entry.objectIndex = slot;
    entry.objectName = objectName.toUpperCase().substring(0, NDFS_NAME_MAX);
    entry.type = fileType.toUpperCase().substring(0, NDFS_TYPE_MAX);
    entry.userIndex = user.userIndex;
    // The number SINTRAN shows is NOT the physical position: a user's files can
    // span several object blocks, each a whole index block apart. Compute it on
    // create as well as on load, or a freshly created file reports 0 until the
    // image is reloaded.
    entry.fileNumber = computeFileNumber(entry.objectIndex, entry.userIndex);
    entry.userName = user.userName;
    // pagesInFile counts the pages ACTUALLY ALLOCATED, not the logical extent.
    //
    // Corrected 2026-07-30 to match SINTRAN. On a real pack (SYSTEM)S3-CONFIG-E01:PROG
    // records 89 while its index spans 99 slots with 10 holes, and the machine itself
    // reports "No of pages in file: 89" while transferring page numbers 0..53 then 64..98.
    // Writing the logical extent charged the user quota for holes that occupy no disk.
    //
    // The logical extent remains ceil(bytesInFile / NDFS_PAGE_SIZE), which is what
    // getFileBlocks uses to walk the index.
    entry.pagesInFile = this.countRealDataPages(fileData, holes);
    entry.bytesInFile = fileData.length > 0 ? fileData.length : 1;
    entry.filePointer = new BlockPointer(topBlockId, pointerType);
    // A new file inherits the OWNING USER's default file access, which is what
    // SINTRAN does (@SET-DEFAULT-FILE-ACCESS sets the value copied onto every
    // file the user then creates). It used to be a flat ACCESS_DEFAULT (0x03FF:
    // owner and friends everything, PUBLIC nothing), which ignored the user
    // entry and left the file unreadable by anyone else -- SYSTEM included,
    // since SYSTEM is neither owner nor friend of an ordinary user. That bites
    // at once in practice: the RT-LOADER runs as SYSTEM, so a :BPUN copied into
    // a user area could not be read by the very thing meant to load it. A
    // brand-new user gets 0x04FF, which grants PUBLIC READ. Falls back to
    // ACCESS_DEFAULT only when the user entry carries no default at all.
    //
    // The allocation flag, self-referential version chain and object index
    // ([user|slot]) below keep SINTRAN from seeing a broken version chain
    // (";2") and refusing to open the file.
    //
    // An EXPLICIT accessBits always wins over inheritance: inheriting is
    // right for a file created on a live machine, but wrong for distribution
    // media, where the owning user's default may grant PUBLIC nothing and
    // make every file invisible to SYSTEM. See writeFile's docstring and
    // ACCESS_PUBLIC_READ (object-entry.ts) for the measurement.
    entry.accessBits =
      accessBits !== undefined
        ? accessBits & 0x7fff
        : user.defaultFileAccess
          ? user.defaultFileAccess & 0x7fff
          : ACCESS_DEFAULT;
    entry.fileTypeFlags =
      pointerType === PointerType.Contiguous ? FT_CONTIGUOUS : FT_INDEXED;
    // objectIndex already encodes [user|fileEntry] (chosen in the user region).
    entry.diskObjectIndex = entry.objectIndex;
    entry.nextVersion = entry.objectIndex;
    entry.prevVersion = entry.objectIndex;
    this.objectFile.addObject(entry);

    // Update user pages used. Quota tracks only REAL disk consumption: sparse
    // holes allocate no block (see writeDataPageToIndex's zero-check, reused
    // by countRealDataPages) and the index/sub-index structural blocks are
    // filesystem overhead, not charged against the user.
    user.pagesUsed += this.countRealDataPages(fileData, holes) + indexBlocksUsed;

    // Surgical writes: new object's page, owner's user page, bitmap.
    this.writeObjectPage(entry.objectIndex);
    this.writeUserPage(user.userIndex);
    this.writeBitFile();
  }

  /**
   * Create an empty contiguous file occupying a fixed run of `pages` pages.
   *
   * This is SINTRAN's `@CREATE-FILE name pages` -- the form that produces a file the
   * machine reports as CONTINUOUS FILE. Every page is allocated up front and the pages
   * are consecutive on the pack, but the file itself is created empty.
   *
   * The reservation is fixed for the life of the file. A later write of more than
   * `pages` pages throws instead of growing the file or converting it to an indexed one,
   * because the pages following the run belong to other files and cannot be claimed --
   * SINTRAN cannot grow a continuous file either.
   *
   * @param path  "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE".
   * @param pages Number of pages to reserve. Must be at least 1.
   */
  createContiguousFile(path: string, pages: number): void {
    this.ensureWritable();

    if (!Number.isInteger(pages) || pages < 1) {
      throw new Error('A contiguous file needs at least one page');
    }

    const { userName, objectName, fileType } = this.parsePath(path);
    if (!objectName) throw new Error('Invalid path: filename required');

    const user = this.resolveUser(userName);
    if (!user) throw new Error(`User not found: ${userName || '(default)'}`);

    // Same as writeFile: NAME:TYPE identifies the file, so a contiguous
    // FOO:DATA must not collide with an existing FOO:PROG.
    if (this.objectFile.findObject(objectName, user.userName, fileType)) {
      throw new Error(`File already exists: ${path}`);
    }

    // Every page of a contiguous file is real disk, so the whole run is charged to the
    // user's quota at once -- there are no holes to discount, unlike an indexed file.
    const availableToUser = user.pagesReserved - user.pagesUsed;
    if (availableToUser < pages) {
      const expansion = pages - availableToUser;
      const freeOnDisk = this.bitFile.getFreePages();
      if (freeOnDisk < expansion) {
        throw new Error(
          `Insufficient disk space: need ${expansion} more pages for the quota, ` +
            `only ${freeOnDisk} available`,
        );
      }
      user.pagesReserved += expansion;
    }

    const slot = this.allocateUserObjectSlot(user);
    this.ensureObjectDirPage(slot);

    // The defining property: one run, no index block. findFreeBlockRange is the only
    // allocator that can promise consecutive blocks -- the sparse allocator picks each
    // page independently and would scatter them.
    // A file of one page does not take the top-down run rule: SINTRAN gives it the page the data
    // page allocator (RSPAG) would give (docs/NDFS-ALLOCATION-RULES.md section 3).
    const startBlock = pages === 1 ? this.bitFile.findFreeDataBlock() : this.bitFile.findFreeBlockRange(pages);
    if (startBlock < 0) {
      throw new Error(`No contiguous run of ${pages} free pages is available on this pack`);
    }

    this.bitFile.allocateBlocks(startBlock, pages);

    // Zero the run. A freshly created file must not expose whatever a deleted file left
    // behind on those pages.
    const emptyPage = new Uint8Array(NDFS_PAGE_SIZE);
    for (let i = 0; i < pages; i++) {
      this.writePage(startBlock + i, emptyPage);
    }

    const entry = new ObjectEntry();
    entry.objectIndex = slot;
    entry.objectName = objectName.toUpperCase().substring(0, NDFS_NAME_MAX);
    entry.type = fileType.toUpperCase().substring(0, NDFS_TYPE_MAX);
    entry.userIndex = user.userIndex;
    // The number SINTRAN shows is NOT the physical position: a user's files can
    // span several object blocks, each a whole index block apart. Compute it on
    // create as well as on load, or a freshly created file reports 0 until the
    // image is reloaded.
    entry.fileNumber = computeFileNumber(entry.objectIndex, entry.userIndex);
    entry.userName = user.userName;

    // Unlike an indexed file, allocated and logical extent are the same here, and both
    // equal the reservation -- a contiguous run has no holes.
    entry.pagesInFile = pages;

    // Created empty: the pages exist, but no byte of them is file content yet. This is
    // what the live machine reports for a freshly created contiguous file.
    entry.bytesInFile = 0;

    // Points straight at the first data page. There is no index block to walk: page N of
    // the file is simply block (filePointer.blockId + N).
    entry.filePointer = new BlockPointer(startBlock, PointerType.Contiguous);
    entry.accessBits = ACCESS_DEFAULT;

    // Contiguous, not indexed -- the flag SINTRAN prints as "CONTINUOUS FILE".
    entry.fileTypeFlags = FT_CONTIGUOUS;
    entry.diskObjectIndex = entry.objectIndex;
    entry.nextVersion = entry.objectIndex;
    entry.prevVersion = entry.objectIndex;
    this.objectFile.addObject(entry);

    user.pagesUsed += pages;

    this.writeObjectPage(entry.objectIndex);
    this.writeUserPage(user.userIndex);
    this.writeBitFile();
    this.persistMasterBlock();
  }

  /**
   * Write into an existing contiguous file, in place and within its reservation.
   *
   * The run is never reallocated, so unlike the indexed path this neither frees nor
   * claims blocks, and the user's page count does not move: the pages were charged at
   * creation and stay charged until the file is deleted.
   */
  private updateExistingContiguousFile(existing: ObjectEntry, fileData: Uint8Array): void {
    const reservedPages = existing.pagesInFile;
    const neededPages = Math.ceil(fileData.length / NDFS_PAGE_SIZE);

    if (neededPages > reservedPages) {
      throw new Error(
        `Cannot write ${fileData.length} bytes (${neededPages} pages) to contiguous file ` +
          `'${existing.objectName}': it reserved ${reservedPages} pages and a contiguous ` +
          'file cannot grow.',
      );
    }

    if (!existing.filePointer || existing.filePointer.blockId === 0) {
      throw new Error(`Contiguous file '${existing.objectName}' has no allocated run`);
    }

    const startBlock = existing.filePointer.blockId;

    // Write the run straight through. EVERY page of the reservation is rewritten, not
    // just the pages the data covers, so bytes left over from a previous longer write
    // cannot reappear past the new end of file.
    for (let i = 0; i < reservedPages; i++) {
      const page = new Uint8Array(NDFS_PAGE_SIZE);
      const offset = i * NDFS_PAGE_SIZE;
      if (offset < fileData.length) {
        page.set(fileData.subarray(offset, Math.min(offset + NDFS_PAGE_SIZE, fileData.length)));
      }
      this.writePage(startBlock + i, page);
    }

    existing.bytesInFile = fileData.length;

    // pagesInFile stays at the reservation. For a contiguous file the pages are allocated
    // whether or not the data reaches them, so counting only the written pages would
    // under-report the disk this file actually holds.
    this.writeObjectPage(existing.objectIndex);
  }

  private updateExistingFile(
    existing: ObjectEntry,
    user: UserEntry,
    fileData: Uint8Array,
    holes?: ReadonlySet<number>,
  ): void {
    // A contiguous file keeps the run it was created with. It is never converted to an
    // indexed file behind the caller's back, and it never grows: SINTRAN cannot grow a
    // CONTINUOUS file either, because the pages after the run belong to someone else.
    if ((existing.fileTypeFlags & FT_CONTIGUOUS) !== 0) {
      this.updateExistingContiguousFile(existing, fileData);
      return;
    }

    // Resolve the OLD file's real (non-sparse) data-block pointers BEFORE
    // freeing them, so the refund below counts exactly which old pages were
    // real vs. sparse (never the index/sub-index structural blocks that
    // freeFileBlocks() also releases).
    const oldRealPages = this.countChargedPagesForObject(existing);

    // Free old blocks
    this.freeFileBlocks(existing);

    // Subtract old usage
    user.pagesUsed = user.pagesUsed >= oldRealPages ? user.pagesUsed - oldRealPages : 0;

    // Allocate new blocks
    let dataPages = Math.ceil(fileData.length / NDFS_PAGE_SIZE);
    if (dataPages === 0) dataPages = 1;

    const { topBlockId, pointerType, indexBlocksUsed } = this.allocateAndWriteData(fileData, dataPages, holes);

    // Update existing entry
    // Allocated count, not the logical extent - see the note at the create site.
    existing.pagesInFile = this.countRealDataPages(fileData, holes);
    existing.bytesInFile = fileData.length > 0 ? fileData.length : 1;
    existing.filePointer = new BlockPointer(topBlockId, pointerType);

    // Update user pages used. Quota tracks only REAL disk consumption: charge
    // only the new file's real (non-sparse) data pages, never the
    // index/sub-index structural block overhead.
    user.pagesUsed += this.countRealDataPages(fileData, holes) + indexBlocksUsed;

    // Surgical writes: the file's object page, owner's user page, bitmap.
    this.writeObjectPage(existing.objectIndex);
    this.writeUserPage(user.userIndex);
    this.writeBitFile();
  }

  private freeFileBlocks(obj: ObjectEntry): void {
    if (!obj.filePointer || obj.filePointer.blockId === 0) return;

    if (obj.filePointer.type === PointerType.Contiguous) {
      // Contiguous files have no sparse holes or structural blocks: every
      // page from blockId..blockId+pagesInFile-1 is a real, directly-freeable block.
      this.bitFile.freeBlocks(obj.filePointer.blockId, obj.pagesInFile);
      return;
    }

    // Free the real (non-sparse) data blocks, resolved the same way
    // countRealDataPagesInList() counts them for quota accounting.
    const dataPointers = this.resolveDataPointers(obj);
    for (let i = 0; i < dataPointers.length; i++) {
      if (dataPointers[i].blockId > 0) {
        this.bitFile.markBlockFree(dataPointers[i].blockId);
      }
    }

    if (obj.filePointer.type === PointerType.Indexed) {
      // Free the index block itself
      this.bitFile.markBlockFree(obj.filePointer.blockId);
    } else if (obj.filePointer.type === PointerType.SubIndexed) {
      const subIndexPage = this.readPage(obj.filePointer.blockId);
      for (let si = 0; si < MAX_OBJECT_FILE_POINTERS; si++) {
        const indexPtr = BlockPointer.fromBytes(subIndexPage, si * 4);
        if (!indexPtr.isValid()) continue;
        // Free each group index block (structural, not user data).
        this.bitFile.markBlockFree(indexPtr.blockId);
      }
      // Free the top-level sub-index block itself.
      this.bitFile.markBlockFree(obj.filePointer.blockId);
    }
  }

  /**
   * Resolve a file's real data-block pointers into a flat list, one entry per
   * logical data page — excludes the index/sub-index structural blocks
   * entirely. Sparse holes report blockId===0 (BlockPointer.isValid() false),
   * matching writeDataPageToIndex()'s sparse encoding. Used both to free real
   * data blocks (freeFileBlocks) and to count real (non-sparse) pages for
   * user quota accounting (countRealDataPagesInList).
   */
  private resolveDataPointers(obj: ObjectEntry): BlockPointer[] {
    const pointers: BlockPointer[] = [];
    if (!obj.filePointer || obj.filePointer.blockId === 0) return pointers;

    if (obj.filePointer.type === PointerType.Contiguous) {
      // Contiguous files have no sparse holes: every page is a real block.
      for (let i = 0; i < obj.pagesInFile; i++) {
        pointers.push(new BlockPointer(obj.filePointer.blockId + i, PointerType.Contiguous));
      }
    } else if (obj.filePointer.type === PointerType.Indexed) {
      const indexPage = this.readPage(obj.filePointer.blockId);
      for (let i = 0; i < obj.pagesInFile && i < MAX_OBJECT_FILE_POINTERS; i++) {
        pointers.push(BlockPointer.fromBytes(indexPage, i * 4));
      }
    } else if (obj.filePointer.type === PointerType.SubIndexed) {
      const subIndexPage = this.readPage(obj.filePointer.blockId);
      let pageCount = 0;
      for (let si = 0; si < MAX_OBJECT_FILE_POINTERS && pageCount < obj.pagesInFile; si++) {
        const indexPtr = BlockPointer.fromBytes(subIndexPage, si * 4);
        if (!indexPtr.isValid()) continue;
        const indexPage = this.readPage(indexPtr.blockId);
        for (let i = 0; i < MAX_OBJECT_FILE_POINTERS && pageCount < obj.pagesInFile; i++) {
          pointers.push(BlockPointer.fromBytes(indexPage, i * 4));
          pageCount++;
        }
      }
    }
    return pointers;
  }

  /**
   * Count how many of a file's logical pages are NOT sparse holes — i.e. how
   * many pages writeDataPageToIndex() will actually allocate a real disk
   * block for. User quota (pagesUsed) must track only real disk consumption:
   * the logical page count counts sparse holes as if they consumed space,
   * and the index/sub-index structural blocks are filesystem overhead, not
   * user data.
   */
  private countRealDataPages(data: Uint8Array, holes?: ReadonlySet<number>): number {
    let filePages = Math.ceil(data.length / NDFS_PAGE_SIZE);
    if (filePages === 0) filePages = 1;

    let realPages = 0;
    for (let i = 0; i < filePages; i++) {
      // Mirrors writeDataPageToIndex() exactly: a page is a hole only when it
      // was DECLARED one. Its contents do not enter into it.
      if (!(holes && holes.has(i))) realPages++;
    }
    return realPages;
  }

  /**
   * Count real (non-sparse) data-block pointers in an already-resolved block
   * list, as returned by resolveDataPointers(). Sparse holes report
   * blockId===0, matching writeDataPageToIndex()'s sparse encoding.
   */
  private countRealDataPagesInList(pointers: BlockPointer[]): number {
    let count = 0;
    for (let i = 0; i < pointers.length; i++) {
      if (pointers[i].blockId > 0) count++;
    }
    return count;
  }

  /**
   * Persist all three structures to the image buffer.
   * Order: BitFile -> UserFile -> ObjectFile (matching C# reference).
   */
  // NDFS writes are immediate and surgical (matching RetroCommander/RetroCore):
  // a mutation rewrites ONLY the block(s) it touched, never the whole
  // filesystem. Each helper rebuilds a single 2048-byte page from the
  // in-memory model; rebuilding zero-filled also clears freed slots so a
  // deleted file/user does not reappear on reload.

  /** Write the BitFile allocation bitmap (small, contiguous). */
  private writeBitFile(): void {
    const mb = this.masterBlock;
    if (!mb.bitFilePointer || !mb.bitFilePointer.isValid()) return;
    const pages = this.bitFile.toPageBuffers(
      NdfsFileSystem.bitFileDiskPages(mb, this.bitFile.totalPages),
    );
    for (let i = 0; i < pages.length; i++) {
      this.writePage(mb.bitFilePointer.blockId + i, pages[i]);
    }
    // The bitmap just changed, so the master block's cached "Unreserved
    // Pages" free-page count (page 0, offset 0x1C of the master block) is
    // now stale. Real SINTRAN trusts this cached count instead of rescanning
    // the bitmap, so every caller of writeBitFile() (createNewFile,
    // updateExistingFile, deleteFile) must keep it in sync.
    this.persistMasterBlock();
  }

  /**
   * Rewrite only the 32-byte master block region of page 0 with the current
   * free-page count from the live bit file. Must NOT disturb the adjacent
   * Extended Info Block (page 0 offset 0x07D0) or its checksum -- RetroCore
   * (the golden reference) never rewrites that checksum, and
   * MasterBlock.writeToBytes() only clears/writes its own 32-byte region
   * starting at MASTER_BLOCK_OFFSET (0x07E0), so this is safe.
   */
  private persistMasterBlock(): void {
    this.masterBlock.unreservedPages = this.bitFile.getFreePages();
    const page0 = this.readPage(0);
    this.masterBlock.writeToBytes(page0);
    this.writePage(0, page0);
  }

  /** Write only the UserFile data page holding `userIndex`. */
  private writeUserPage(userIndex: number): void {
    const mb = this.masterBlock;
    if (!mb.userFilePointer || !mb.userFilePointer.isValid()) return;
    const pageIndex = Math.floor(userIndex / ENTRIES_PER_PAGE);
    if (pageIndex >= MAX_USER_FILE_POINTERS) return;
    const indexPage = this.readPage(mb.userFilePointer.blockId);
    const ptr = BlockPointer.fromBytes(indexPage, pageIndex * 4);
    if (!ptr.isValid()) return;
    this.writePage(ptr.blockId, this.userFile.toDataPage(pageIndex));
  }

  /** Resolve the on-disk data block backing ObjectFile page `pageIndex`. */
  private objectPageBlock(pageIndex: number): number | null {
    const mb = this.masterBlock;
    if (!mb.objectFilePointer || !mb.objectFilePointer.isValid()) return null;
    if (mb.objectFilePointer.type === PointerType.Indexed) {
      if (pageIndex >= MAX_OBJECT_FILE_POINTERS) return null;
      const indexPage = this.readPage(mb.objectFilePointer.blockId);
      const ptr = BlockPointer.fromBytes(indexPage, pageIndex * 4);
      return ptr.isValid() ? ptr.blockId : null;
    }
    if (mb.objectFilePointer.type === PointerType.SubIndexed) {
      const subIdx = Math.floor(pageIndex / MAX_OBJECT_FILE_POINTERS);
      const innerIdx = pageIndex % MAX_OBJECT_FILE_POINTERS;
      const subIndexPage = this.readPage(mb.objectFilePointer.blockId);
      const subPtr = BlockPointer.fromBytes(subIndexPage, subIdx * 4);
      if (!subPtr.isValid()) return null;
      const innerIndexPage = this.readPage(subPtr.blockId);
      const dataPtr = BlockPointer.fromBytes(innerIndexPage, innerIdx * 4);
      return dataPtr.isValid() ? dataPtr.blockId : null;
    }
    return null;
  }

  /**
   * Write only the ObjectFile data page holding `objectIndex`.
   * Rebuilt zero-filled, which clears any slot freed by a delete.
   */
  private writeObjectPage(objectIndex: number): void {
    const pageIndex = Math.floor(objectIndex / ENTRIES_PER_PAGE);
    const dataBlock = this.objectPageBlock(pageIndex);
    if (dataBlock === null) return;
    this.writePage(dataBlock, this.objectFile.toDataPage(pageIndex));
  }

  private findObject(path: string): ObjectEntry | null {
    const { userName, objectName, fileType } = this.parsePath(path);
    const searchName = objectName
      ? fileType
        ? `${objectName}:${fileType}`
        : objectName
      : '';

    const objects = this.objectFile.getObjects();
    for (let i = 0; i < objects.length; i++) {
      const o = objects[i];
      if (userName && o.userName.toUpperCase() !== userName.toUpperCase()) continue;

      const fullName = o.type ? `${o.objectName}:${o.type}` : o.objectName;
      if (fullName.toUpperCase() === searchName.toUpperCase()) return o;
      // Also try matching object name only (without type)
      if (!fileType && o.objectName.toUpperCase() === objectName.toUpperCase()) return o;
    }
    return null;
  }

  private parsePath(path: string): {
    userName: string;
    objectName: string;
    fileType: string;
  } {
    const normalized = path.replace(/^\/+|\/+$/g, '');
    const parts = normalized.split('/');

    let userName = '';
    let fileNamePart = '';

    if (parts.length >= 2) {
      userName = parts[0];
      fileNamePart = parts.slice(1).join('/');
    } else {
      fileNamePart = parts[0];
    }

    // Split filename into name and type (separator is : or last .)
    let objectName = fileNamePart;
    let fileType = '';

    const colonIdx = fileNamePart.indexOf(':');
    if (colonIdx >= 0) {
      objectName = fileNamePart.substring(0, colonIdx);
      fileType = fileNamePart.substring(colonIdx + 1);
    } else {
      const dotIdx = fileNamePart.lastIndexOf('.');
      if (dotIdx >= 0) {
        objectName = fileNamePart.substring(0, dotIdx);
        fileType = fileNamePart.substring(dotIdx + 1);
      }
    }

    return { userName, objectName, fileType };
  }

  private resolveUser(userName: string): UserEntry | null {
    if (userName) {
      return this.userFile.findUser(userName);
    }
    // Default to first user
    const users = this.userFile.getUsers();
    return users.length > 0 ? users[0] : null;
  }

  private ensureWritable(): void {
    if (this.readOnly) {
      throw new Error('Filesystem is read-only');
    }
  }
}
