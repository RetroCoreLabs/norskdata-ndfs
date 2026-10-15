/**
 * Real-write regression tests for the NDFS object-file (the volume-wide directory listing
 * every file across every user) growing past its plain-Indexed capacity of 512 directory
 * pages (16,384 files total, or 64 users at 8 reserved index-pointer slots each).
 *
 * Previously ensureObjectDirPage's Indexed branch had no bounds check at all: once a write
 * needed pageIdx >= MAX_OBJECT_FILE_POINTERS (512), BlockPointer.fromBytes() would read past
 * the end of the 2048-byte index page buffer. Fixed by growing the object file into a
 * SubIndexed structure (sub-index block -> group index blocks -> directory pages), mirroring
 * a single file's own SubIndexed layout: the existing single Indexed block is converted into
 * "group 0" with no data migration.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ImageTemplate } from '../src/types.js';

// 30,000 empty files each cost exactly 1 real page (their own index block -- empty content
// is a sparse hole, so no data block), plus ~938 object-directory pages (30000/32), plus a
// handful of group-index/sub-index/user-file pages. 90,000 pages leaves comfortable headroom.
function createBigFS(directoryName: string, customPages: number = 90000): NdfsFileSystem {
  return NdfsFileSystem.createImage({
    template: ImageTemplate.Custom,
    customPages,
    directoryName,
  });
}

describe('ObjectFile SubIndexed growth (past 512-page Indexed capacity)', () => {
  it('creating files for users past the old 64-user ceiling does not throw', () => {
    // The old plain-Indexed object file capped out at 64 users (512 index-pointer slots / 8
    // reserved per user) -- well before any file-count limit is even reached. This alone used
    // to corrupt/throw for user #64 onward.
    const fs = createBigFS('MANYUSERS');

    for (let i = 0; i < 80; i++) {
      const name = `USER${String(i).padStart(3, '0')}`;
      expect(fs.addUser(name, 10)).toBe(true);
      expect(() => fs.writeFile(`${name}/FILE:DAT`, new Uint8Array([1]))).not.toThrow();
    }

    // SYSTEM (index 0, created by the image template) + 80 new users.
    expect(fs.getUsers().length).toBe(81);
  });

  it(
    'writes 30,000 files across 120 users, all survive a fresh reopen with correct ownership',
    () => {
      // 30,000 files spread across 120 users (250 each -- well under the 256-file-per-user
      // cap) forces the object file well past its old 16,384-file/64-user ceiling: pageIdx
      // reaches roughly 120*8=960, requiring 2 SubIndexed groups (each covering 512 pages).
      const userCount = 120;
      const filesPerUser = 250;
      const totalFiles = userCount * filesPerUser;

      const fs1 = createBigFS('THIRTYK');

      for (let u = 0; u < userCount; u++) {
        const userName = `USER${u}`;
        expect(fs1.addUser(userName, filesPerUser)).toBe(true);
        for (let f = 0; f < filesPerUser; f++) {
          fs1.writeFile(`${userName}/F${f}:DAT`, new Uint8Array([u % 251, f % 251]));
        }
      }

      // Remount fresh (not reusing in-memory state) and verify everything survived,
      // including that reading a SubIndexed object-file directory correctly resolves
      // non-colliding ObjectIndex/ownership for files in groups beyond the first.
      const fs2 = new NdfsFileSystem(fs1.toBuffer());
      const objects = fs2.getObjectEntries();
      expect(objects.length).toBe(totalFiles);

      // Spot-check users from the FIRST group (pageIdx < 512, e.g. user 0), the group
      // BOUNDARY (around pageIdx 512, i.e. user ~64), and the SECOND group (e.g. user 119)
      // -- exactly the scenario a subindex read-side bug would corrupt.
      const users2 = fs2.getUsers();
      for (const userToCheck of [0, 64, 119]) {
        const userName = `USER${userToCheck}`;
        const user = users2.find((u) => u.userName === userName);
        expect(user, `${userName} must exist`).toBeDefined();

        let filesForThisUser = 0;
        for (const o of objects) {
          if (o.userIndex === user!.userIndex) {
            filesForThisUser++;
            // Ownership must decode correctly from the objectIndex's high byte -- this is
            // exactly what a broken subindex read would corrupt (colliding with another
            // user's region).
            const ownerFromObjectIndex = o.objectIndex >> 8;
            expect(
              ownerFromObjectIndex,
              `${userName}'s file objectIndex high byte must equal their userIndex`,
            ).toBe(user!.userIndex);
          }
        }
        expect(filesForThisUser, `${userName} must have exactly ${filesPerUser} files`).toBe(
          filesPerUser,
        );
      }

      // Byte-level content check on one file from each of the 3 spot-checked users.
      expect(Array.from(fs2.readFile('USER0/F0:DAT'))).toEqual([0, 0]);
      expect(Array.from(fs2.readFile('USER64/F100:DAT'))).toEqual([64 % 251, 100]);
      expect(Array.from(fs2.readFile('USER119/F249:DAT'))).toEqual([119, 249 % 251]);
    },
    60000,
  );

  it('delete and recreate a file in the second SubIndexed group works correctly', () => {
    // Exercises delete/recreate for a user whose directory page lives in the second
    // SubIndexed group (not just create-only). USER69's pageIdx (69*8=552..559) is past the
    // first group's 512-page cap.
    const userCount = 70;
    const fs = createBigFS('DELSECOND', 10000);

    for (let i = 0; i < userCount; i++) {
      expect(fs.addUser(`USER${i}`, 10)).toBe(true);
    }

    fs.writeFile('USER69/FILE:DAT', new Uint8Array([1, 2, 3]));
    fs.deleteFile('USER69/FILE:DAT');
    expect(() => fs.writeFile('USER69/FILE:DAT', new Uint8Array([4, 5, 6]))).not.toThrow();
    expect(Array.from(fs.readFile('USER69/FILE:DAT'))).toEqual([4, 5, 6]);
  });
});
