/**
 * Reading a floppy whose pages will not read.
 *
 * A real ND floppy comes off the drive with individual pages unreadable. The
 * file listing is what the archive is kept for, so a lost page must cost only
 * what was written ON that page:
 *
 *   - an object-file data page  -> its own 32 entries, nothing else
 *   - an object-file index page -> the entries the pages under it name
 *   - a user-file data page     -> the names of the users on it
 *   - a bit-file page           -> the allocation state it covered
 *
 * A page that cannot be read is modelled here the way it happens on the media:
 * the pointer names a page outside the image, so readPage() fails. Every case
 * has the same name in tests/damaged-media.test.ts, tests/test_damaged_media.py
 * and tests/test_damaged_media.c, and asserts the same numbers.
 */

import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { BlockPointer } from '../src/block-pointer.js';
import { PointerType, ImageTemplate } from '../src/types.js';
import { NDFS_PAGE_SIZE, MASTER_BLOCK_OFFSET } from '../src/constants.js';

/** A page number no image in these tests is long enough to hold. */
const UNREADABLE = 9000;

const OBJECT_PTR = MASTER_BLOCK_OFFSET + 0x10;
const USER_PTR = MASTER_BLOCK_OFFSET + 0x14;
const BIT_PTR = MASTER_BLOCK_OFFSET + 0x18;

/** 40 files: 32 in the first object-file data page, 8 in the second. */
const FILE_COUNT = 40;

function makeFloppy(fileCount: number = FILE_COUNT): Uint8Array {
  const fs = NdfsFileSystem.createImage({
    template: ImageTemplate.Floppy360KB,
    directoryName: 'DAMAGED',
  });
  for (let i = 0; i < fileCount; i++) {
    fs.writeFile(`SYSTEM/FILE-${i}:DATA`, new Uint8Array([i & 0xff]));
  }
  return fs.toBuffer();
}

function pointAway(image: Uint8Array, offset: number, type: PointerType = PointerType.Contiguous): void {
  new BlockPointer(UNREADABLE, type).toBytes(image, offset);
}

/** Break one pointer slot of the object file's index page. */
function breakObjectDataPage(image: Uint8Array, slot: number): void {
  const indexPage = BlockPointer.fromBytes(image, OBJECT_PTR).blockId;
  pointAway(image, indexPage * NDFS_PAGE_SIZE + slot * 4);
}

/** Break the single pointer slot of the user file's index page. */
function breakUserDataPage(image: Uint8Array, slot: number = 0): void {
  const indexPage = BlockPointer.fromBytes(image, USER_PTR).blockId;
  pointAway(image, indexPage * NDFS_PAGE_SIZE + slot * 4);
}

function names(fs: NdfsFileSystem): string[] {
  return fs.getObjectEntries().map(o => o.fullName).sort();
}

describe('damaged media', () => {
  describe('an intact floppy is the baseline', () => {
    it('lists every file and reports no damage', () => {
      const fs = new NdfsFileSystem(makeFloppy());
      expect(fs.getObjectEntries().length).toBe(FILE_COUNT);
      expect(fs.getDamageReport()).toEqual({ objectPages: 0, userPages: 0, bitFilePages: 0 });
      expect(fs.getUsers().map(u => u.userName)).toEqual(['SYSTEM']);
    });
  });

  describe('an object-file data page that will not read', () => {
    it('costs its own 32 entries and no others', () => {
      const image = makeFloppy();
      const intact = names(new NdfsFileSystem(image.slice()));
      breakObjectDataPage(image, 0);
      const fs = new NdfsFileSystem(image);

      expect(fs.getObjectEntries().length).toBe(FILE_COUNT - 32);
      expect(fs.getDamageReport().objectPages).toBe(1);
      // the 8 that survive are the 8 the intact image holds on the second page
      for (const n of names(fs)) expect(intact).toContain(n);
    });

    it('leaves the surviving entries at the object index they have on the media', () => {
      const image = makeFloppy();
      const before = new Map(
        new NdfsFileSystem(image.slice()).getObjectEntries().map(o => [o.fullName, o.objectIndex]),
      );
      breakObjectDataPage(image, 0);
      for (const o of new NdfsFileSystem(image).getObjectEntries()) {
        expect(o.objectIndex).toBe(before.get(o.fullName));
      }
    });

    it('two lost pages are counted separately', () => {
      const image = makeFloppy();
      breakObjectDataPage(image, 0);
      breakObjectDataPage(image, 1);
      const fs = new NdfsFileSystem(image);
      expect(fs.getObjectEntries().length).toBe(0);
      expect(fs.getDamageReport().objectPages).toBe(2);
    });
  });

  describe('the object-file index page itself', () => {
    it('cannot be worked around - there is nothing left to name the files', () => {
      const image = makeFloppy();
      pointAway(image, OBJECT_PTR, PointerType.Indexed);
      expect(() => new NdfsFileSystem(image)).toThrow();
    });
  });

  describe('a user-file data page that will not read', () => {
    it('costs the user names and not one file', () => {
      const image = makeFloppy();
      breakUserDataPage(image);
      const fs = new NdfsFileSystem(image);

      expect(fs.getObjectEntries().length).toBe(FILE_COUNT);
      expect(fs.getUsers().length).toBe(0);
      expect(fs.getDamageReport().userPages).toBe(1);
      expect(fs.getDamageReport().objectPages).toBe(0);
      // the objects keep the user index the media gives them
      for (const o of fs.getObjectEntries()) expect(o.userIndex).toBe(0);
    });
  });

  describe('a bit-file page that will not read', () => {
    it('costs the allocation state and not the listing', () => {
      const image = makeFloppy();
      pointAway(image, BIT_PTR);
      const fs = new NdfsFileSystem(image);

      expect(fs.getObjectEntries().length).toBe(FILE_COUNT);
      expect(fs.getDamageReport().bitFilePages).toBe(1);
      expect(fs.getDamageReport().objectPages).toBe(0);
    });
  });

  describe('all three at once', () => {
    it('reports each count and still lists what survived', () => {
      const image = makeFloppy();
      breakObjectDataPage(image, 0);
      breakUserDataPage(image);
      pointAway(image, BIT_PTR);
      const fs = new NdfsFileSystem(image);

      expect(fs.getObjectEntries().length).toBe(FILE_COUNT - 32);
      expect(fs.getDamageReport()).toEqual({ objectPages: 1, userPages: 1, bitFilePages: 1 });
      expect(fs.getDirectoryName()).toBe('DAMAGED');
    });
  });
});
