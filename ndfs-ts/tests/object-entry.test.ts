import { describe, it, expect } from 'vitest';
import { ObjectEntry } from '../src/object-entry.js';
import { BlockPointer } from '../src/block-pointer.js';
import { PointerType } from '../src/types.js';
import { ENTRY_SIZE, OBJECT_ENTRY_IN_USE, NDFS_NAME_TERMINATOR } from '../src/constants.js';
import { writeUint32BE } from '../src/endian.js';

function buildObjectBytes(
  name: string,
  type: string,
  userIndex: number,
  pages: number,
  bytes: number,
  pointerNative: number,
): Uint8Array {
  const buf = new Uint8Array(ENTRY_SIZE);
  buf[0] = OBJECT_ENTRY_IN_USE; // 0x80

  // Name at offset 2
  for (let i = 0; i < name.length && i < 16; i++) {
    buf[2 + i] = name.charCodeAt(i);
  }
  if (name.length < 16) buf[2 + name.length] = NDFS_NAME_TERMINATOR;

  // Type at offset 18
  for (let i = 0; i < type.length && i < 4; i++) {
    buf[18 + i] = type.charCodeAt(i);
  }
  if (type.length < 4) buf[18 + type.length] = NDFS_NAME_TERMINATOR;

  buf[34] = userIndex;
  writeUint32BE(buf, 52, pages);
  writeUint32BE(buf, 56, (bytes - 1) >>> 0); // stored as bytes-1; empty = 0xFFFFFFFF
  writeUint32BE(buf, 60, pointerNative);

  return buf;
}

describe('ObjectEntry', () => {
  describe('fromBytes', () => {
    it('parses valid object entry', () => {
      const data = buildObjectBytes('TESTFILE', 'DATA', 0, 5, 10240, 0x40000064);
      const entry = ObjectEntry.fromBytes(data, 0);
      expect(entry).not.toBeNull();
      expect(entry!.objectName).toBe('TESTFILE');
      expect(entry!.type).toBe('DATA');
      expect(entry!.userIndex).toBe(0);
      expect(entry!.pagesInFile).toBe(5);
      expect(entry!.bytesInFile).toBe(10240);
      expect(entry!.filePointer).not.toBeNull();
      expect(entry!.filePointer!.blockId).toBe(100);
      expect(entry!.filePointer!.type).toBe(PointerType.Indexed);
    });

    it('returns null for unused entry', () => {
      const data = new Uint8Array(ENTRY_SIZE);
      data[0] = 0x00; // not in use
      expect(ObjectEntry.fromBytes(data, 0)).toBeNull();
    });

    it('handles 1-byte file correctly (bytes stored as 0)', () => {
      const data = buildObjectBytes('TINY', 'DATA', 0, 1, 1, 0x00000010);
      const entry = ObjectEntry.fromBytes(data, 0);
      expect(entry!.bytesInFile).toBe(1);
    });

    // SINTRAN stores an empty file as 0xFFFFFFFF (0 - 1): nd100x SMD0.IMG holds it in ten
    // SINTRAN-made entries such as (SYSTEM)SINTRAN:DATA. Before 2026-11-04 the writer stored 0,
    // which reads back as 1 byte.
    it('stores an empty file as 0xFFFFFFFF and reads it back as 0 bytes', () => {
      const data = buildObjectBytes('EMPTY', 'DATA', 0, 0, 0, 0x00000010);
      expect(Array.from(data.slice(56, 60))).toEqual([0xff, 0xff, 0xff, 0xff]);
      const entry = ObjectEntry.fromBytes(data, 0);
      expect(entry!.bytesInFile).toBe(0);
      const out = new Uint8Array(ENTRY_SIZE);
      entry!.toBytes(out, 0);
      expect(Array.from(out.slice(56, 60))).toEqual([0xff, 0xff, 0xff, 0xff]);
    });
  });

  describe('fullName', () => {
    it('returns NAME:TYPE format', () => {
      const entry = new ObjectEntry();
      entry.objectName = 'README';
      entry.type = 'TEXT';
      expect(entry.fullName).toBe('README:TEXT');
    });
  });

  describe('fileTypeAsText', () => {
    it('returns DATA for type 0', () => {
      const entry = new ObjectEntry();
      entry.fileType = 0;
      expect(entry.fileTypeAsText).toBe('DATA');
    });
    it('returns PROG for type 1', () => {
      const entry = new ObjectEntry();
      entry.fileType = 1;
      expect(entry.fileTypeAsText).toBe('PROG');
    });
  });

  describe('toBytes round-trip', () => {
    it('round-trips all fields', () => {
      const entry = new ObjectEntry();
      entry.objectName = 'MYFILE';
      entry.type = 'PROG';
      entry.userIndex = 3;
      entry.pagesInFile = 10;
      entry.bytesInFile = 20000;
      entry.filePointer = new BlockPointer(50, PointerType.Indexed);

      const buf = new Uint8Array(ENTRY_SIZE);
      entry.toBytes(buf, 0);

      const parsed = ObjectEntry.fromBytes(buf, 0);
      expect(parsed).not.toBeNull();
      expect(parsed!.objectName).toBe('MYFILE');
      expect(parsed!.type).toBe('PROG');
      expect(parsed!.userIndex).toBe(3);
      expect(parsed!.pagesInFile).toBe(10);
      expect(parsed!.bytesInFile).toBe(20000);
      expect(parsed!.filePointer!.blockId).toBe(50);
      expect(parsed!.filePointer!.type).toBe(PointerType.Indexed);
    });

    it('round-trips extended fields (versioning, access, dates, open counts)', () => {
      const entry = new ObjectEntry();
      entry.objectName = 'LOAD';
      entry.type = 'MODE';
      entry.nextVersion = 20;
      entry.prevVersion = 20;
      entry.accessBits = 0x00ff;
      entry.fileTypeFlags = 0x08; // Indexed
      entry.deviceNumber = 0;
      entry.totalOpenCount = 145;
      entry.dateCreated = 0x01020304;
      entry.lastDateRead = 0x05060708;
      entry.lastDateWritten = 0x090a0b0c;
      entry.pagesInFile = 5;
      entry.bytesInFile = 4621;
      entry.filePointer = new BlockPointer(18419, PointerType.Indexed);

      const buf = new Uint8Array(ENTRY_SIZE);
      entry.toBytes(buf, 0);
      const p = ObjectEntry.fromBytes(buf, 0)!;
      expect(p.nextVersion).toBe(20);
      expect(p.prevVersion).toBe(20);
      expect(p.accessBits).toBe(0x00ff);
      expect(p.fileTypeFlags).toBe(0x08);
      expect(p.totalOpenCount).toBe(145);
      expect(p.dateCreated).toBe(0x01020304);
      expect(p.lastDateRead).toBe(0x05060708);
      expect(p.lastDateWritten).toBe(0x090a0b0c);

      // Encoding stable.
      const buf2 = new Uint8Array(ENTRY_SIZE);
      p.toBytes(buf2, 0);
      expect(Array.from(buf2)).toEqual(Array.from(buf));
    });

    it('preserves unmodelled bytes via raw on re-serialize', () => {
      const buf = new Uint8Array(ENTRY_SIZE);
      for (let i = 0; i < ENTRY_SIZE; i++) buf[i] = i + 1;
      buf[0] = OBJECT_ENTRY_IN_USE;
      const p = ObjectEntry.fromBytes(buf, 0)!;
      p.accessBits = 0x03ff;
      const out = new Uint8Array(ENTRY_SIZE);
      p.toBytes(out, 0);
      // bytes 33 and 35 are unmodelled and must survive
      expect(out[33]).toBe(buf[33]);
      expect(out[35]).toBe(buf[35]);
      expect(out[26]).toBe(0x03);
      expect(out[27]).toBe(0xff);
    });
  });
});
