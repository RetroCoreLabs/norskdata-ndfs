import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ImageTemplate } from '../src/types.js';
import { NDFS_PAGE_SIZE, MASTER_BLOCK_OFFSET, EXTENDED_INFO_OFFSET } from '../src/constants.js';
import { readUint32BE } from '../src/endian.js';

function createFS(pages: number = 200): NdfsFileSystem {
  return NdfsFileSystem.createImage({
    template: ImageTemplate.Custom,
    customPages: pages,
    directoryName: 'PERSIST',
  });
}

/** Read the on-disk "Unreserved Pages" field directly from page 0, bypassing the filesystem API. */
function readOnDiskUnreservedPages(fsys: NdfsFileSystem): number {
  const page0 = fsys.toBuffer().subarray(0, NDFS_PAGE_SIZE);
  return readUint32BE(page0, MASTER_BLOCK_OFFSET + 0x1c);
}

/** Read the raw 16-byte Extended Info Block (page 0, offset 0x07D0), including its checksum. */
function readExtendedInfoBytes(fsys: NdfsFileSystem): Uint8Array {
  const page0 = fsys.toBuffer().subarray(0, NDFS_PAGE_SIZE);
  return page0.slice(EXTENDED_INFO_OFFSET, EXTENDED_INFO_OFFSET + 16);
}

describe('WritePersistence', () => {
  it('write file, export to buffer, re-open, verify data byte-for-byte', () => {
    const fs1 = createFS();
    const data = new Uint8Array(500);
    for (let i = 0; i < data.length; i++) data[i] = (i * 7 + 13) & 0xFF;
    fs1.writeFile('SYSTEM/PERSIST:DATA', data);

    const buf = fs1.toBuffer();
    const fs2 = new NdfsFileSystem(buf);
    const read = fs2.readFile('SYSTEM/PERSIST:DATA');
    expect(read.length).toBe(data.length);
    for (let i = 0; i < data.length; i++) {
      expect(read[i]).toBe(data[i]);
    }
  });

  it('write multiple files, re-mount, verify all present', () => {
    const fs1 = createFS();
    for (let i = 0; i < 5; i++) {
      fs1.writeFile(`SYSTEM/FILE${i}:DATA`, new TextEncoder().encode(`content-${i}`));
    }

    const fs2 = new NdfsFileSystem(fs1.toBuffer());
    const entries = fs2.listDirectory('SYSTEM');
    expect(entries.length).toBe(5);

    for (let i = 0; i < 5; i++) {
      const content = new TextDecoder().decode(fs2.readFile(`SYSTEM/FILE${i}:DATA`));
      expect(content).toBe(`content-${i}`);
    }
  });

  it('add user, write to buffer, re-mount, verify user exists', () => {
    const fs1 = createFS();
    fs1.addUser('TESTUSER', 100);

    const fs2 = new NdfsFileSystem(fs1.toBuffer());
    const users = fs2.getUsers();
    expect(users.length).toBe(2);
    const found = users.find((u) => u.userName === 'TESTUSER');
    expect(found).toBeDefined();
    expect(found!.pagesReserved).toBe(100);
  });

  it('delete file, re-mount, verify deleted', () => {
    const fs1 = createFS();
    fs1.writeFile('SYSTEM/TEMP:DATA', new TextEncoder().encode('temporary'));
    fs1.deleteFile('SYSTEM/TEMP:DATA');

    const fs2 = new NdfsFileSystem(fs1.toBuffer());
    expect(fs2.fileExists('SYSTEM/TEMP:DATA')).toBe(false);
    expect(fs2.listDirectory('SYSTEM').length).toBe(0);
  });

  it('modify user quota, re-mount, verify', () => {
    const fs1 = createFS();
    fs1.updateUserQuota(0, 9999);

    const fs2 = new NdfsFileSystem(fs1.toBuffer());
    expect(fs2.getUser(0)!.pagesReserved).toBe(9999);
  });

  it('new files get a self-referential version chain and [user|slot] index', () => {
    const fs = createFS();
    fs.writeFile('SYSTEM/A:TEXT', new Uint8Array([1, 2, 3]));
    fs.writeFile('SYSTEM/B:TEXT', new Uint8Array([4, 5, 6]));
    for (const e of fs.getObjectEntries()) {
      expect(e.nextVersion).toBe(e.diskObjectIndex);
      expect(e.prevVersion).toBe(e.diskObjectIndex);
      expect((e.diskObjectIndex >> 8) & 0xff).toBe(e.userIndex);
    }
  });

  it('new file lands in the owning user region (object index high byte = user)', () => {
    const fs = createFS();
    fs.addUser('GUEST', 200);
    const guest = fs.listDirectory('').find((e) => e.name === 'GUEST');
    expect(guest).toBeDefined();
    fs.writeFile('GUEST/HELLO:TEXT', new Uint8Array([1, 2]));
    const e = fs.getObjectEntry('HELLO', 'GUEST')!;
    expect(e.userIndex).not.toBe(0);
    expect((e.diskObjectIndex >> 8) & 0xff).toBe(e.userIndex);
    expect(e.objectIndex).toBeGreaterThanOrEqual(e.userIndex * 256);
    expect(e.objectIndex).toBeLessThan((e.userIndex + 1) * 256);
    expect(e.nextVersion).toBe(e.diskObjectIndex);
    // survives a round-trip
    const fs2 = new NdfsFileSystem(fs.toBuffer());
    const e2 = fs2.getObjectEntry('HELLO', 'GUEST')!;
    expect((e2.diskObjectIndex >> 8) & 0xff).toBe(e2.userIndex);
  });

  it('deleting the sole file on a page clears it (no reappear)', () => {
    const fs = createFS();
    fs.writeFile('SYSTEM/ONLY:DATA', new Uint8Array([1]));
    fs.deleteFile('SYSTEM/ONLY:DATA');
    const fs2 = new NdfsFileSystem(fs.toBuffer());
    expect(fs2.fileExists('SYSTEM/ONLY:DATA')).toBe(false);
    expect(fs2.getObjectEntries().length).toBe(0);
  });

  it('deleted file stays deleted after export + re-open', () => {
    const fs = createFS();
    fs.writeFile('SYSTEM/A:TEXT', new Uint8Array([1]));
    fs.writeFile('SYSTEM/B:TEXT', new Uint8Array([2]));
    fs.deleteFile('SYSTEM/A:TEXT');
    const fs2 = new NdfsFileSystem(fs.toBuffer());
    expect(fs2.fileExists('SYSTEM/A:TEXT')).toBe(false);
    expect(fs2.fileExists('SYSTEM/B:TEXT')).toBe(true);
  });

  it('friend add/list/remove + persistence + error cases', () => {
    const fs = createFS();
    fs.addUser('ALICE', 100);
    fs.addUser('BOB', 100);

    fs.addFriend('ALICE', 'BOB', 'RW');
    const fr = fs.listFriends('ALICE');
    expect(fr.length).toBe(1);
    expect(fr[0].name).toBe('BOB');
    expect(fr[0].perms).toBe('RW---');

    // Already a friend -> throws.
    expect(() => fs.addFriend('ALICE', 'BOB', 'R')).toThrow();
    // Unknown friend name -> throws.
    expect(() => fs.addFriend('ALICE', 'NOBODY', 'R')).toThrow();

    // Survives export + reload.
    const fs2 = new NdfsFileSystem(fs.toBuffer());
    const fr2 = fs2.listFriends('ALICE');
    expect(fr2.length).toBe(1);
    expect(fr2[0].name).toBe('BOB');

    // Remove.
    fs.removeFriend('ALICE', 'BOB');
    expect(fs.listFriends('ALICE').length).toBe(0);
    expect(() => fs.removeFriend('ALICE', 'BOB')).toThrow();
  });

  // Bug 1+2 regression: an unrelated mutation must not corrupt other files'
  // metadata. A file with an empty type must keep it after an unrelated
  // add-user. The original port re-serialized EVERY object entry on every
  // mutation and defaulted empty types to 'DATA' on read, so adding a user
  // corrupted unrelated files. Writes are now surgical and empty types are
  // preserved.
  it('surgical write preserves an empty type across an unrelated mutation', () => {
    const fs = createFS();
    // File with an EMPTY type (path carries no :TYPE suffix).
    fs.writeFile('SYSTEM/NOTYPE', new Uint8Array([0x78]));
    expect(fs.getMetadata('SYSTEM/NOTYPE')!.type).toBe('');

    // Unrelated mutation: add a brand-new user.
    fs.addUser('BACKUP', 50);

    // Export + reload: the empty type must survive, not become 'DATA'.
    const fs2 = new NdfsFileSystem(fs.toBuffer());
    expect(fs2.getMetadata('SYSTEM/NOTYPE')!.type).toBe('');
  });

  // The master block's "Unreserved Pages" field (page 0, offset 0x1C of the 32-byte master block) is the
  // pool for user quotas, not a free page count. SINTRAN keeps it as capacity - bit file pages - 4 minus
  // the sum of the users' reserved pages (measured, docs/NDFS-ALLOCATION-RULES.md section 7a). Files do
  // not change it; giving, changing and taking away quota does.
  it('master block unreserved pages: fresh image value', () => {
    const fs = createFS();
    // createFS makes a custom image; SYSTEM holds the default quota.
    const sys = fs.getUser(0)!;
    const total = fs.getFreePages() + fs.getUsedPages();
    expect(readOnDiskUnreservedPages(fs)).toBe(total - 1 - 4 - sys.pagesReserved);
  });

  it('master block unreserved pages: files do not change the pool', () => {
    const fs = createFS();
    const base = readOnDiskUnreservedPages(fs);
    fs.writeFile('SYSTEM/A:DATA', new Uint8Array(3000));
    expect(readOnDiskUnreservedPages(fs)).toBe(base);
    fs.deleteFile('SYSTEM/A:DATA');
    expect(readOnDiskUnreservedPages(fs)).toBe(base);
  });

  it('master block unreserved pages: quota changes move the pool', () => {
    const fs = createFS(2000);
    const base = readOnDiskUnreservedPages(fs);
    expect(fs.addUser('ALPHA', 100)).toBe(true);
    expect(readOnDiskUnreservedPages(fs)).toBe(base - 100);
    const index = fs.getUsers().find((u) => u.userName === 'ALPHA')!.userIndex;
    expect(fs.updateUserQuota(index, 300)).toBe(true);
    expect(readOnDiskUnreservedPages(fs)).toBe(base - 300);
    expect(fs.updateUserQuota(index, 50)).toBe(true);
    expect(readOnDiskUnreservedPages(fs)).toBe(base - 50);
    expect(fs.removeUser(index)).toBe(true);
    expect(readOnDiskUnreservedPages(fs)).toBe(base);
  });

  it('does not touch the Extended Info Block checksum when persisting unreserved pages', () => {
    const fs = createFS();
    const extBefore = readExtendedInfoBytes(fs);

    fs.writeFile('SYSTEM/A:DATA', new Uint8Array(1000));
    fs.writeFile('SYSTEM/B:DATA', new Uint8Array(2000));
    fs.deleteFile('SYSTEM/A:DATA');
    fs.addUser('BACKUP', 50);

    const extAfter = readExtendedInfoBytes(fs);
    expect(Array.from(extAfter)).toEqual(Array.from(extBefore));
  });
});
