import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ImageTemplate } from '../src/types.js';
import { NDFS_PAGE_SIZE } from '../src/constants.js';
import { ACCESS_PUBLIC_READ } from '../src/object-entry.js';

function createFS(pages: number = 200): NdfsFileSystem {
  return NdfsFileSystem.createImage({
    template: ImageTemplate.Custom,
    customPages: pages,
    directoryName: 'WRITETEST',
  });
}

describe('WriteOperations', () => {
  it('writes a small file (<1 page) and reads it back', () => {
    const fs = createFS();
    const content = new TextEncoder().encode('small file content');
    fs.writeFile('SYSTEM/SMALL:DATA', content);
    const read = fs.readFile('SYSTEM/SMALL:DATA');
    expect(new TextDecoder().decode(read)).toBe('small file content');
  });

  it('writes exactly 1 page file', () => {
    const fs = createFS();
    const data = new Uint8Array(NDFS_PAGE_SIZE);
    for (let i = 0; i < NDFS_PAGE_SIZE; i++) data[i] = i & 0xFF;
    fs.writeFile('SYSTEM/ONEPAGE:DATA', data);
    const read = fs.readFile('SYSTEM/ONEPAGE:DATA');
    expect(read.length).toBe(NDFS_PAGE_SIZE);
    for (let i = 0; i < NDFS_PAGE_SIZE; i++) {
      expect(read[i]).toBe(data[i]);
    }
  });

  it('writes a multi-page file (10+ pages)', () => {
    const fs = createFS();
    const pages = 12;
    const data = new Uint8Array(pages * NDFS_PAGE_SIZE);
    for (let i = 0; i < data.length; i++) data[i] = (i * 3) & 0xFF;
    fs.writeFile('SYSTEM/MULTIPAGE:DATA', data);

    const read = fs.readFile('SYSTEM/MULTIPAGE:DATA');
    expect(read.length).toBe(data.length);
    for (let i = 0; i < data.length; i++) {
      expect(read[i]).toBe(data[i]);
    }
  });

  it('writes a large file (>512 pages, sub-indexed)', () => {
    // Need a big image: 600 data pages + index blocks + system blocks
    const fs = NdfsFileSystem.createImage({
      template: ImageTemplate.Custom,
      customPages: 2000,
      directoryName: 'BIGWRITE',
    });
    // Update SYSTEM user quota to handle this
    fs.updateUserQuota(0, 2000);

    const pageCount = 600;
    const data = new Uint8Array(pageCount * NDFS_PAGE_SIZE);
    // Fill with a recognizable pattern
    for (let i = 0; i < data.length; i++) data[i] = (i * 11 + 7) & 0xFF;
    fs.writeFile('SYSTEM/BIGFILE:DATA', data);

    const meta = fs.getMetadata('SYSTEM/BIGFILE:DATA');
    expect(meta).not.toBeNull();
    expect(meta!.pages).toBe(600);

    // Read back and verify
    const read = fs.readFile('SYSTEM/BIGFILE:DATA');
    expect(read.length).toBe(data.length);
    // Spot-check various positions
    for (let p = 0; p < pageCount; p += 50) {
      const off = p * NDFS_PAGE_SIZE;
      expect(read[off]).toBe(data[off]);
      expect(read[off + 100]).toBe(data[off + 100]);
    }
  });

  it('overwrites existing file with different size', () => {
    const fs = createFS();
    const v1 = new TextEncoder().encode('version 1');
    fs.writeFile('SYSTEM/OVERWRITE:DATA', v1);
    expect(new TextDecoder().decode(fs.readFile('SYSTEM/OVERWRITE:DATA'))).toBe('version 1');

    const v2 = new Uint8Array(NDFS_PAGE_SIZE * 3);
    v2.fill(0xBB);
    fs.writeFile('SYSTEM/OVERWRITE:DATA', v2);
    const read = fs.readFile('SYSTEM/OVERWRITE:DATA');
    expect(read.length).toBe(NDFS_PAGE_SIZE * 3);
    expect(read[0]).toBe(0xBB);
  });

  it('write file auto-expands user quota', () => {
    const fs = NdfsFileSystem.createImage({
      template: ImageTemplate.Custom,
      customPages: 200,
      directoryName: 'QUOTATEST',
    });
    // SYSTEM user has quota of 200 (capped to min(pages, 1000))
    // Write a file that may need quota expansion
    const user = fs.getUser(0)!;
    const origQuota = user.pagesReserved;

    // Write enough data to potentially exceed the original quota
    const bigData = new Uint8Array(NDFS_PAGE_SIZE * 50);
    bigData.fill(0xCC);
    fs.writeFile('SYSTEM/BIG:DATA', bigData);

    // Verify the file was written
    const read = fs.readFile('SYSTEM/BIG:DATA');
    expect(read.length).toBe(bigData.length);
  });

  it('write file when disk full returns error', () => {
    // Create a very small filesystem
    const fs = NdfsFileSystem.createImage({
      template: ImageTemplate.Custom,
      customPages: 30,
      directoryName: 'FULL',
    });

    // Try to write more data than disk can hold
    // With 30 pages, system uses ~6, leaving ~24 free
    // Writing 25 pages of data plus index blocks should fail
    const hugeData = new Uint8Array(NDFS_PAGE_SIZE * 28);
    hugeData.fill(0xDD);
    expect(() => fs.writeFile('SYSTEM/HUGE:DATA', hugeData)).toThrow();
  });

  it('delete file verifies blocks freed in bitmap', () => {
    const fs = createFS();
    const freeBefore = fs.getFreePages();

    const data = new Uint8Array(NDFS_PAGE_SIZE * 5);
    data.fill(0xEE);
    fs.writeFile('SYSTEM/TODELETE:DATA', data);
    expect(fs.getFreePages()).toBeLessThan(freeBefore);

    fs.deleteFile('SYSTEM/TODELETE:DATA');
    expect(fs.getFreePages()).toBe(freeBefore);
  });

  it('delete file updates user pages_used', () => {
    const fs = createFS();
    fs.writeFile('SYSTEM/DELME:DATA', new TextEncoder().encode('delete me'));
    const userAfterWrite = fs.getUser(0)!;
    expect(userAfterWrite.pagesUsed).toBeGreaterThan(0);

    fs.deleteFile('SYSTEM/DELME:DATA');
    const userAfterDelete = fs.getUser(0)!;
    expect(userAfterDelete.pagesUsed).toBe(0);
  });

  it('rename file, verify new name readable', () => {
    const fs = createFS();
    fs.writeFile('SYSTEM/OLDNAME:DATA', new TextEncoder().encode('renamed content'));
    fs.rename('SYSTEM/OLDNAME:DATA', 'NEWNAME:TEXT');

    expect(fs.fileExists('SYSTEM/OLDNAME:DATA')).toBe(false);
    expect(fs.fileExists('SYSTEM/NEWNAME:TEXT')).toBe(true);

    const content = new TextDecoder().decode(fs.readFile('SYSTEM/NEWNAME:TEXT'));
    expect(content).toBe('renamed content');
  });

  // writeFile's accessBits parameter must override the owning user's
  // inherited defaultFileAccess outright, on a NEWLY CREATED file.
  //
  // Set explicitly to the restrictive, PUBLIC-nothing shape (0x03FF) rather
  // than relied on as-created: unlike ndfs-c's ndfs_ue_init (which stamps a
  // fresh user 0x04FF, granting PUBLIC READ), ndfs-ts's image creator
  // currently never writes defaultFileAccess for SYSTEM at all, so a
  // freshly created image's SYSTEM already reads 0 here -- a real,
  // pre-existing C/TS divergence, tracked separately, that this test
  // deliberately does not depend on either way. getUser returns the live
  // entry, not a copy, so mutating it here is enough.
  it('accessBits override wins over an inherited restrictive default', () => {
    const fs = createFS();
    const system = fs.getUser(0)!;
    system.defaultFileAccess = 0x03ff; // the restrictive, PUBLIC-nothing shape

    // Without an override, the new file inherits the (now-restrictive)
    // default unchanged -- proves the override is opt-in, not a global
    // behavior change.
    fs.writeFile('SYSTEM/INHERIT:DATA', new TextEncoder().encode('x'));
    const inherited = fs.getObjectEntry('INHERIT', 'SYSTEM')!;
    expect(inherited.accessBits).toBe(0x03ff);

    // With ACCESS_PUBLIC_READ passed explicitly, the new file gets THAT
    // instead, regardless of what SYSTEM's own default now says.
    fs.writeFile('SYSTEM/PUBRD:DATA', new TextEncoder().encode('x'), 'none', undefined, ACCESS_PUBLIC_READ);
    const explicit = fs.getObjectEntry('PUBRD', 'SYSTEM')!;
    expect(explicit.accessBits).toBe(ACCESS_PUBLIC_READ);
  });
});
