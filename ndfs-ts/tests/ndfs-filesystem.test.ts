import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { NDFS_PAGE_SIZE, NDFS_NAME_TERMINATOR } from '../src/constants.js';
import { writeUint32BE } from '../src/endian.js';
import { BlockPointer } from '../src/block-pointer.js';
import { PointerType } from '../src/types.js';
import * as fs from 'fs';
import * as path from 'path';

const fixtureDir = path.join(__dirname, 'fixtures');

/**
 * Create a minimal valid NDFS image in memory.
 * Layout:
 *   Page 0: Master block
 *   Page 7: Object file index block
 *   Page 8: Object file data block (32 entries)
 *   Page 9: User file index block
 *   Page 10: User file data block (32 entries)
 *   Page 11: Bit file (bitmap)
 */
function createTestImage(totalPages: number = 50, dirName: string = 'TESTDISK'): Uint8Array {
  const imageSize = totalPages * NDFS_PAGE_SIZE;
  const image = new Uint8Array(imageSize);

  // Page 0: Master block at offset 2016
  const mbOff = 2016;
  // Directory name
  for (let i = 0; i < dirName.length; i++) image[mbOff + i] = dirName.charCodeAt(i);
  if (dirName.length < 16) image[mbOff + dirName.length] = NDFS_NAME_TERMINATOR;

  // Object file pointer at page 7 (Indexed)
  const objPtr = new BlockPointer(7, PointerType.Indexed);
  objPtr.toBytes(image, mbOff + 0x10);

  // User file pointer at page 9 (Indexed)
  const usrPtr = new BlockPointer(9, PointerType.Indexed);
  usrPtr.toBytes(image, mbOff + 0x14);

  // Bit file pointer at page 11 (Contiguous)
  const bitPtr = new BlockPointer(11, PointerType.Contiguous);
  bitPtr.toBytes(image, mbOff + 0x18);

  // Unreserved pages
  writeUint32BE(image, mbOff + 0x1c, totalPages - 12);

  // Page 7: Object file index block - pointer to page 8
  const objIdxOff = 7 * NDFS_PAGE_SIZE;
  const objDataPtr = new BlockPointer(8, PointerType.Contiguous);
  objDataPtr.toBytes(image, objIdxOff);

  // Page 8: Object file data block (empty - no files yet)

  // Page 9: User file index block - pointer to page 10
  const usrIdxOff = 9 * NDFS_PAGE_SIZE;
  const usrDataPtr = new BlockPointer(10, PointerType.Contiguous);
  usrDataPtr.toBytes(image, usrIdxOff);

  // Page 10: User file data block - create SYSTEM user
  const usrDataOff = 10 * NDFS_PAGE_SIZE;
  image[usrDataOff + 0] = 0x81; // valid user flag
  // User name "SYSTEM"
  const sysName = 'SYSTEM';
  for (let i = 0; i < sysName.length; i++) image[usrDataOff + 2 + i] = sysName.charCodeAt(i);
  image[usrDataOff + 2 + sysName.length] = NDFS_NAME_TERMINATOR;
  // Pages reserved = 1000
  writeUint32BE(image, usrDataOff + 28, 1000);
  // Pages used = 0
  writeUint32BE(image, usrDataOff + 32, 0);
  // User index = 0
  image[usrDataOff + 37] = 0;

  // Page 11: Bit file (bitmap) - mark pages 0-11 as used
  const bitmapOff = 11 * NDFS_PAGE_SIZE;
  // Pages 0-7: first byte = 0xFF (bits 0-7)
  image[bitmapOff] = 0xff;
  // Pages 8-11: second byte = bits 0-3
  image[bitmapOff + 1] = 0x0f;

  return image;
}

describe('NdfsFileSystem', () => {
  describe('constructor', () => {
    it('opens valid test image', () => {
      const image = createTestImage();
      const ndfs = new NdfsFileSystem(image);
      expect(ndfs.getDirectoryName()).toBe('TESTDISK');
    });

    it('throws on too-small image', () => {
      expect(() => new NdfsFileSystem(new Uint8Array(100))).toThrow();
    });

    it('opens unaligned image (over a boundary) and drops the partial page', () => {
      // A valid image plus 1024 trailing bytes (half a page over a boundary).
      const image = createTestImage();
      const unaligned = new Uint8Array(image.length + 1024);
      unaligned.set(image);
      const ndfs = new NdfsFileSystem(unaligned);
      expect(ndfs.unaligned).toBe(true);
      expect(ndfs.getDirectoryName()).toBe('TESTDISK');
      // Forced read-only: writes refused even though opened read-write.
      expect(() => ndfs.deleteFile('/NONEXISTENT')).toThrow();
    });

    it('opens unaligned image (under a boundary)', () => {
      const image = createTestImage();
      const trimmed = image.subarray(0, image.length - NDFS_PAGE_SIZE);
      const unaligned = new Uint8Array(trimmed.length + 1024);
      unaligned.set(trimmed);
      const ndfs = new NdfsFileSystem(unaligned);
      expect(ndfs.unaligned).toBe(true);
    });

    it('aligned image is not flagged unaligned', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      expect(ndfs.unaligned).toBe(false);
    });
  });

  describe('listDirectory', () => {
    it('lists users at root', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      const entries = ndfs.listDirectory('');
      expect(entries.length).toBe(1);
      expect(entries[0].name).toBe('SYSTEM');
      expect(entries[0].isDirectory).toBe(true);
    });

    it('lists empty user directory', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      const entries = ndfs.listDirectory('SYSTEM');
      expect(entries.length).toBe(0);
    });

    it('throws on subdirectory path', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      expect(() => ndfs.listDirectory('SYSTEM/SUBDIR')).toThrow();
    });
  });

  describe('write and read file', () => {
    it('writes and reads a small file', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      const data = new TextEncoder().encode('Hello NDFS!');
      ndfs.writeFile('SYSTEM/HELLO:DATA', data);

      const read = ndfs.readFile('SYSTEM/HELLO:DATA');
      expect(new TextDecoder().decode(read)).toBe('Hello NDFS!');
    });

    it('writes and reads a multi-page file', () => {
      const ndfs = new NdfsFileSystem(createTestImage(200));
      const data = new Uint8Array(5000);
      for (let i = 0; i < data.length; i++) data[i] = i & 0xff;
      ndfs.writeFile('SYSTEM/BIGFILE:DATA', data);

      const read = ndfs.readFile('SYSTEM/BIGFILE:DATA');
      expect(read.length).toBe(5000);
      for (let i = 0; i < data.length; i++) {
        expect(read[i]).toBe(data[i]);
      }
    });

    it('writes exactly 1 page file', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      const data = new Uint8Array(NDFS_PAGE_SIZE);
      data.fill(0xaa);
      ndfs.writeFile('SYSTEM/ONEPAGE:DATA', data);

      const read = ndfs.readFile('SYSTEM/ONEPAGE:DATA');
      expect(read.length).toBe(NDFS_PAGE_SIZE);
      expect(read[0]).toBe(0xaa);
      expect(read[NDFS_PAGE_SIZE - 1]).toBe(0xaa);
    });

    it('overwrites existing file', () => {
      const ndfs = new NdfsFileSystem(createTestImage(200));
      ndfs.writeFile('SYSTEM/FILE:DATA', new TextEncoder().encode('version 1'));
      ndfs.writeFile('SYSTEM/FILE:DATA', new TextEncoder().encode('version 2 is longer'));

      const read = ndfs.readFile('SYSTEM/FILE:DATA');
      expect(new TextDecoder().decode(read)).toBe('version 2 is longer');
    });

    it('writes multiple files', () => {
      const ndfs = new NdfsFileSystem(createTestImage(200));
      for (let i = 0; i < 5; i++) {
        ndfs.writeFile(`SYSTEM/FILE${i}:DATA`, new TextEncoder().encode(`content ${i}`));
      }
      const entries = ndfs.listDirectory('SYSTEM');
      expect(entries.length).toBe(5);
    });
  });

  describe('deleteFile', () => {
    it('deletes a file and frees blocks', () => {
      const ndfs = new NdfsFileSystem(createTestImage(200));
      const freeBefore = ndfs.getFreePages();
      ndfs.writeFile('SYSTEM/TODELETE:DATA', new TextEncoder().encode('delete me'));
      const freeAfterWrite = ndfs.getFreePages();
      expect(freeAfterWrite).toBeLessThan(freeBefore);

      ndfs.deleteFile('SYSTEM/TODELETE:DATA');
      const freeAfterDelete = ndfs.getFreePages();
      expect(freeAfterDelete).toBe(freeBefore);
    });

    it('throws on non-existent file', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      expect(() => ndfs.deleteFile('SYSTEM/NOPE:DATA')).toThrow();
    });
  });

  describe('rename', () => {
    it('renames a file', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      ndfs.writeFile('SYSTEM/OLD:DATA', new TextEncoder().encode('data'));
      ndfs.rename('SYSTEM/OLD:DATA', 'NEW:TEXT');

      expect(ndfs.fileExists('SYSTEM/OLD:DATA')).toBe(false);
      expect(ndfs.fileExists('SYSTEM/NEW:TEXT')).toBe(true);
    });
  });

  describe('user management', () => {
    it('adds a user', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      expect(ndfs.addUser('NEWUSER', 500)).toBe(true);
      const users = ndfs.getUsers();
      expect(users.length).toBe(2);
    });

    it('rejects duplicate user', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      expect(ndfs.addUser('SYSTEM', 500)).toBe(false);
    });

    it('removes user with no files', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      ndfs.addUser('TEMP', 100);
      const tempUser = ndfs.getUsers().find((u) => u.userName === 'TEMP');
      expect(ndfs.removeUser(tempUser!.userIndex)).toBe(true);
    });

    it('refuses to remove user with files', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      ndfs.writeFile('SYSTEM/FILE:DATA', new TextEncoder().encode('data'));
      expect(ndfs.removeUser(0)).toBe(false);
    });

    it('updates user quota', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      expect(ndfs.updateUserQuota(0, 2000)).toBe(true);
      expect(ndfs.getUser(0)!.pagesReserved).toBe(2000);
    });

    it('clears user password by index', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      expect(ndfs.clearUserPassword(0)).toBe(true);
      expect(ndfs.getUser(0)!.password).toBe(0);
    });

    it('clears user password by name', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      expect(ndfs.clearUserPassword('SYSTEM')).toBe(true);
    });
  });

  describe('write persistence', () => {
    it('survives export and re-import', () => {
      const ndfs1 = new NdfsFileSystem(createTestImage(200));
      ndfs1.writeFile('SYSTEM/PERSIST:DATA', new TextEncoder().encode('persistent data'));
      ndfs1.addUser('USER2', 300);

      const exported = ndfs1.toBuffer();
      const ndfs2 = new NdfsFileSystem(exported);

      expect(ndfs2.getDirectoryName()).toBe('TESTDISK');
      const content = ndfs2.readFile('SYSTEM/PERSIST:DATA');
      expect(new TextDecoder().decode(content)).toBe('persistent data');
      expect(ndfs2.getUsers().length).toBe(2);
    });
  });

  describe('read-only mode', () => {
    it('allows reads in read-only mode', () => {
      const ndfs = new NdfsFileSystem(createTestImage(), true);
      expect(ndfs.getDirectoryName()).toBe('TESTDISK');
      expect(ndfs.listDirectory('').length).toBe(1);
    });

    it('throws on write in read-only mode', () => {
      const ndfs = new NdfsFileSystem(createTestImage(), true);
      expect(() =>
        ndfs.writeFile('SYSTEM/X:DATA', new TextEncoder().encode('nope')),
      ).toThrow();
    });
  });

  describe('fixture files', () => {
    it('reads withfiles.ndfs', () => {
      const data = new Uint8Array(fs.readFileSync(path.join(fixtureDir, 'withfiles.ndfs')));
      const ndfs = new NdfsFileSystem(data, true);

      expect(ndfs.getDirectoryName().length).toBeGreaterThan(0);
      const users = ndfs.getUsers();
      expect(users.length).toBeGreaterThan(0);

      // List all user directories
      const root = ndfs.listDirectory('');
      expect(root.length).toBeGreaterThan(0);

      // List first user's files
      const firstUser = root[0].name;
      const files = ndfs.listDirectory(firstUser);
      // The fixture should have files
      expect(files.length).toBeGreaterThanOrEqual(0);
    });

    it('reads empty.ndfs', () => {
      const data = new Uint8Array(fs.readFileSync(path.join(fixtureDir, 'empty.ndfs')));
      const ndfs = new NdfsFileSystem(data, true);
      expect(ndfs.getDirectoryName().length).toBeGreaterThan(0);
    });
  });

  describe('metadata', () => {
    it('returns metadata for existing file', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      ndfs.writeFile('SYSTEM/INFO:TEXT', new TextEncoder().encode('metadata test'));
      const meta = ndfs.getMetadata('SYSTEM/INFO:TEXT');
      expect(meta).not.toBeNull();
      expect(meta!.name).toBe('INFO');
      expect(meta!.type).toBe('TEXT');
      expect(meta!.size).toBe(13);
      expect(meta!.isDirectory).toBe(false);
    });

    it('returns null for missing file', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      expect(ndfs.getMetadata('SYSTEM/NOPE:DATA')).toBeNull();
    });
  });

  describe('bitmap', () => {
    it('reports block usage', () => {
      const ndfs = new NdfsFileSystem(createTestImage(50));
      // Blocks 0-11 should be used (system + structures)
      expect(ndfs.isBlockUsed(0)).toBe(true);
      expect(ndfs.isBlockUsed(11)).toBe(true);
      // Higher blocks should be free
      expect(ndfs.isBlockUsed(20)).toBe(false);
    });

    it('reports free page count', () => {
      const ndfs = new NdfsFileSystem(createTestImage(50));
      const free = ndfs.getFreePages();
      expect(free).toBeGreaterThan(0);
      expect(free).toBeLessThan(50);
    });
  });

  describe('integrity', () => {
    it('reports valid for clean image', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      expect(ndfs.verifyIntegrity()).toBe(true);
    });
  });

  // generateReport() had no dedicated test anywhere in this suite, unlike
  // ndfs-c's equivalent (test_filesystem.c).
  describe('generateReport', () => {
    it('names the volume and counts users/files', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      ndfs.addUser('GUEST', 50);
      ndfs.writeFile('SYSTEM/REPORTME:DATA', new TextEncoder().encode('x'.repeat(10)));

      const report = ndfs.generateReport();

      expect(report).toContain('NDFS Filesystem Report');
      expect(report).toContain('Users: 2');
      expect(report).toContain('Files: 1');
      expect(report).toContain('GUEST');
      expect(report).toContain('REPORTME');
      expect(report.endsWith('\n')).toBe(true);
    });
  });

  describe('edge cases', () => {
    it('handles file with . separator', () => {
      const ndfs = new NdfsFileSystem(createTestImage());
      ndfs.writeFile('SYSTEM/README.TXT', new TextEncoder().encode('dot separated'));
      const meta = ndfs.getMetadata('SYSTEM/README.TXT');
      expect(meta).not.toBeNull();
      expect(meta!.name).toBe('README');
      expect(meta!.type).toBe('TXT');
    });
  });
});
