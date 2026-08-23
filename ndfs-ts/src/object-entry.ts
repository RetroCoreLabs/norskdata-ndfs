/**
 * NDFS object (file) entry: 64-byte record in the object file.
 *
 * Byte offsets:
 *   0:     Header (bit 7 = in use, i.e. 0x80)
 *   1:     Reserved
 *   2-17:  Object name (16 bytes, terminated by 0x27)
 *   18-21: File type (4 bytes, terminated by 0x27)
 *   22-23: Next version (u16)
 *   24-25: Previous version (u16)
 *   26-27: Access bits (u16, 3x5-bit OWN/FRIEND/PUBLIC)
 *   28-29: File type flags (u16: L M A C I B P T)
 *   30-31: Device number (u16)
 *   32:    File type code (0=DATA, 1=PROG, 2=SYMB, 3=TEXT)
 *   34:    User index (owner) / object-index word
 *   36-37: Current open count (u16)
 *   38-39: Total open count (u16)
 *   40-43: Date created (u32, ND timestamp)
 *   44-47: Last date opened for read (u32, ND timestamp)
 *   48-51: Last date opened for write (u32, ND timestamp)
 *   52-55: Pages in file (32-bit, big-endian)
 *   56-59: Bytes in file - 1 (32-bit, big-endian; actual = stored + 1)
 *   60-63: File pointer (BlockPointer, big-endian)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * HackerCorp Labs - https://github.com/HackerCorpLabs
 */

import {
  ENTRY_SIZE,
  OBJECT_ENTRY_IN_USE,
  NDFS_NAME_MAX,
  NDFS_TYPE_MAX,
  NDFS_NAME_TERMINATOR,
} from './constants.js';
import {
  readUint16BE,
  writeUint16BE,
  readUint32BE,
  writeUint32BE,
} from './endian.js';
import { readNdfsName, writeNdfsName } from './ndfs-name.js';
import { BlockPointer } from './block-pointer.js';
import { PointerType } from './types.js';

/** Object file_type_flags bits (offset 28): "L M A C I B P T". */
export const FT_TERMINAL = 1 << 0;
export const FT_PERIPHERAL = 1 << 1;
export const FT_SPOOLING = 1 << 2;
export const FT_INDEXED = 1 << 3;
export const FT_CONTIGUOUS = 1 << 4;
export const FT_ALLOCATED = 1 << 5;
export const FT_MAGTAPE = 1 << 6;
export const FT_LIBRARY = 1 << 7;

/** Default access for a new file: OWN + FRIEND all rights, PUBLIC none. */
export const ACCESS_DEFAULT = 0x03ff;

/**
 * What a real ND PRODUCT FLOPPY carries on every file: the same OWN/FRIEND
 * rights as ACCESS_DEFAULT plus PUBLIC READ.
 *
 * MEASURED on 210580B01-XX-01D (genuine ND Ethernet distribution floppy): all
 * 12 files read 0x07FF. This is not cosmetic -- PUBLIC is the tier every user
 * who is neither the owner nor a friend falls into, SYSTEM included, so with
 * PUBLIC = 0 the files are invisible from any other user and @LIST-FILES
 * prints nothing while the pack itself verifies clean. Proved by adding one
 * 0x03FF file beside those 12: SINTRAN listed the original twelve and
 * skipped the new one; flipping that single field to 0x07FF made it appear.
 *
 * Pass this as writeFile's accessBits when building distribution media;
 * ACCESS_DEFAULT (or the owning user's own default) is right for a file a
 * user creates for itself on a live machine.
 */
export const ACCESS_PUBLIC_READ = 0x07ff;

export class ObjectEntry {
  header: number = OBJECT_ENTRY_IN_USE;
  headerWord: number = OBJECT_ENTRY_IN_USE << 8;
  objectIndex: number = 0;

  /**
   * The file number SINTRAN reports ("FILE n") - per user, and stable for the
   * life of the file. NOT the same as {@link objectIndex}.
   *
   * `objectIndex` is the raw PHYSICAL position in the object file
   * (`page*32 + slotInPage`) and is the write-back key. The two coincide only
   * inside a user's FIRST object block: a user may own up to 16 blocks of 256
   * files, and block `n` sits at object-file pages `n*512 + user*8`, so past
   * the first block the physical position encodes both block and slot.
   *
   * Worked example from a verified pack: `F0500` is `FILE 307` owned by user 8
   * and sits at physical position 18483. The old model reads that as file
   * `18483 & 0xFF` = 51 owned by `18483 >> 8` = 72 - a user that does not
   * exist. Both wrong; only {@link computeFileNumber} gives 307.
   *
   * Doc: NDInsight SINTRAN/XMSG/DOC/NDFS-OBJECT-BLOCKS-DECODED-2026-08-01.md
   */
  fileNumber: number = 0;
  objectName: string = '';
  type: string = 'DATA';
  userName: string = '';
  userIndex: number = 0;
  fileType: number = 0; // 0=DATA, 1=PROG, 2=SYMB, 3=TEXT
  pagesInFile: number = 0;
  bytesInFile: number = 0;
  filePointer: BlockPointer | null = null;
  accessBits: number = 0;
  nextVersion: number = 0;
  prevVersion: number = 0;
  fileTypeFlags: number = 0;
  deviceNumber: number = 0;
  diskObjectIndex: number = 0;
  currentOpenCount: number = 0;
  totalOpenCount: number = 0;
  dateCreated: number = 0;
  lastDateRead: number = 0;
  lastDateWritten: number = 0;
  /** Verbatim on-disk 64 bytes, used as the base when re-serializing so
   * unmodelled bytes survive. Null for freshly-built entries. */
  raw: Uint8Array | null = null;

  /** Full name in "NAME:TYPE" format. */
  get fullName(): string {
    return this.type ? `${this.objectName}:${this.type}` : this.objectName;
  }

  /** File type as text string. */
  get fileTypeAsText(): string {
    switch (this.fileType) {
      case 0:
        return 'DATA';
      case 1:
        return 'PROG';
      case 2:
        return 'SYMB';
      case 3:
        return 'TEXT';
      default:
        return `TYPE${this.fileType}`;
    }
  }

  /**
   * Parse an object entry from 64 bytes at offset.
   * Returns null if the entry is not in use (bit 7 of byte 0 not set).
   */
  static fromBytes(data: Uint8Array, offset: number): ObjectEntry | null {
    if (data.length < offset + ENTRY_SIZE) {
      throw new Error('Insufficient data for object entry');
    }

    // Check in-use bit
    if ((data[offset] & OBJECT_ENTRY_IN_USE) === 0) return null;

    const entry = new ObjectEntry();

    // Preserve the verbatim 64 bytes so re-serialization never loses fields
    // we do not explicitly model.
    entry.raw = data.slice(offset, offset + ENTRY_SIZE);

    entry.header = data[offset];
    entry.headerWord = readUint16BE(data, offset + 0);

    // Object name (16 bytes at offset+2)
    entry.objectName = readNdfsName(data, offset + 2, NDFS_NAME_MAX);

    // File type (4 bytes at offset+18). Preserve an empty type as-is — do NOT
    // default to 'DATA'. A parse must faithfully represent what is on disk;
    // defaulting here corrupts files whose type is intentionally empty (e.g.
    // TERMINAL: 27 00 00 00) on write-back. (Matches RetroFS.NDFS.)
    entry.type = readNdfsName(data, offset + 18, NDFS_TYPE_MAX);

    // Versioning, access, flags, device (offsets 22-31)
    entry.nextVersion = readUint16BE(data, offset + 22);
    entry.prevVersion = readUint16BE(data, offset + 24);
    entry.accessBits = readUint16BE(data, offset + 26);
    entry.fileTypeFlags = readUint16BE(data, offset + 28);
    entry.deviceNumber = readUint16BE(data, offset + 30);

    // File type code (byte 32)
    entry.fileType = data[offset + 32];

    // User index (byte 34) / object-index word
    entry.userIndex = data[offset + 34];
    entry.diskObjectIndex = readUint16BE(data, offset + 34);

    // Open counts and timestamps (offsets 36-51, big-endian)
    entry.currentOpenCount = readUint16BE(data, offset + 36);
    entry.totalOpenCount = readUint16BE(data, offset + 38);
    entry.dateCreated = readUint32BE(data, offset + 40);
    entry.lastDateRead = readUint32BE(data, offset + 44);
    entry.lastDateWritten = readUint32BE(data, offset + 48);

    // Pages in file (bytes 52-55, big-endian)
    entry.pagesInFile = readUint32BE(data, offset + 52);

    // Bytes in file (bytes 56-59, big-endian) + 1
    entry.bytesInFile = readUint32BE(data, offset + 56) + 1;

    // File pointer (bytes 60-63)
    entry.filePointer = BlockPointer.fromBytes(data, offset + 60);

    return entry;
  }

  /** Serialize to a 64-byte region in a buffer. */
  toBytes(buffer: Uint8Array, offset: number): void {
    if (buffer.length < offset + ENTRY_SIZE) {
      throw new Error('Insufficient buffer for object entry');
    }

    // Base on the original on-disk bytes when available so unmodelled bytes
    // (e.g. byte 33, the low byte of the object-index word) survive; otherwise
    // start from zero.
    if (this.raw && this.raw.length === ENTRY_SIZE) {
      buffer.set(this.raw, offset);
    } else {
      buffer.fill(0, offset, offset + ENTRY_SIZE);
    }

    // Header: a loaded entry keeps its original header word (used/write/
    // modified/terminal bits) via the raw copy; a fresh entry gets in-use.
    if (!this.raw || this.raw.length !== ENTRY_SIZE) {
      buffer[offset] = OBJECT_ENTRY_IN_USE;
    }

    // Object name
    writeNdfsName(buffer, offset + 2, this.objectName, NDFS_NAME_MAX);

    // File type string
    writeNdfsName(buffer, offset + 18, this.type, NDFS_TYPE_MAX);

    // Versioning, access, flags, device (offsets 22-31)
    writeUint16BE(buffer, offset + 22, this.nextVersion);
    writeUint16BE(buffer, offset + 24, this.prevVersion);
    writeUint16BE(buffer, offset + 26, this.accessBits);
    writeUint16BE(buffer, offset + 28, this.fileTypeFlags);
    writeUint16BE(buffer, offset + 30, this.deviceNumber);

    // File type code
    buffer[offset + 32] = this.fileType & 0xff;

    // Object index word at 34: high byte = user index, low byte = file slot
    // (keeps the version pointers, which equal this word, consistent).
    buffer[offset + 34] = this.userIndex & 0xff;
    buffer[offset + 35] = this.diskObjectIndex & 0xff;

    // Open counts and timestamps (offsets 36-51)
    writeUint16BE(buffer, offset + 36, this.currentOpenCount);
    writeUint16BE(buffer, offset + 38, this.totalOpenCount);
    writeUint32BE(buffer, offset + 40, this.dateCreated);
    writeUint32BE(buffer, offset + 44, this.lastDateRead);
    writeUint32BE(buffer, offset + 48, this.lastDateWritten);

    // Pages in file
    writeUint32BE(buffer, offset + 52, this.pagesInFile);

    // Bytes in file - 1
    const bytesMinusOne = this.bytesInFile > 0 ? this.bytesInFile - 1 : 0;
    writeUint32BE(buffer, offset + 56, bytesMinusOne);

    // File pointer
    if (this.filePointer) {
      this.filePointer.toBytes(buffer, offset + 60);
    }
  }
}

/**
 * Convert a physical object-file position into the file number SINTRAN reports.
 *
 * A user's files live in up to 16 object blocks of 256 files each, and block
 * `n` occupies object-file pages `n*512 + user*8 .. +7`. So the number the user
 * sees is:
 *
 * ```
 * block      = page / 512
 * slot       = (page % 512 - user*8) * 32 + slotInPage
 * fileNumber = block * 256 + slot
 * ```
 *
 * Using the raw position instead is correct only for a user's first block.
 *
 * @param objectIndex Physical position, `page*32 + slotInPage`.
 * @param userIndex   Owning user, from byte 34 of the entry.
 * @returns The per-user file number, or `-1` when the position lies outside any
 *          object block belonging to `userIndex`. Returning -1 is deliberate: a
 *          caller spotting a mismatch beats silently getting a plausible but
 *          wrong number.
 */
export function computeFileNumber(objectIndex: number, userIndex: number): number {
  const entriesPerPage = 32;
  const pagesPerObjectBlock = 8;
  const pagesPerIndexBlock = 512;
  const filesPerObjectBlock = pagesPerObjectBlock * entriesPerPage;

  const page = Math.floor(objectIndex / entriesPerPage);
  const slotInPage = objectIndex % entriesPerPage;

  const block = Math.floor(page / pagesPerIndexBlock);
  const pageWithinBlockRow = page % pagesPerIndexBlock;
  const firstPageOfUser = userIndex * pagesPerObjectBlock;

  const offsetPages = pageWithinBlockRow - firstPageOfUser;
  if (offsetPages < 0 || offsetPages >= pagesPerObjectBlock) {
    return -1;
  }

  const slot = offsetPages * entriesPerPage + slotInPage;
  return block * filesPerObjectBlock + slot;
}
