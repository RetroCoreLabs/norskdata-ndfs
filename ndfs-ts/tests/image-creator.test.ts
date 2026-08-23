import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ImageTemplate } from '../src/types.js';
import { NDFS_PAGE_SIZE } from '../src/constants.js';

describe('ImageCreator', () => {
  describe('Floppy360KB', () => {
    it('creates a valid 360KB floppy image', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Floppy360KB,
        directoryName: 'FLOPPY360',
      });
      expect(fs.getDirectoryName()).toBe('FLOPPY360');
    });

    it('has correct master block pointers', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Floppy360KB,
        directoryName: 'TEST360',
      });
      const mb = fs.getMasterBlock();
      expect(mb.objectFilePointer!.blockId).toBe(149);
      expect(mb.userFilePointer!.blockId).toBe(151);
      expect(mb.bitFilePointer!.blockId).toBe(153);
      expect(mb.unreservedPages).toBe(1);
    });

    it('has SYSTEM user after creation', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Floppy360KB,
      });
      const users = fs.getUsers();
      expect(users.length).toBe(1);
      expect(users[0].userName).toBe('SYSTEM');
    });

    // Regression: the image creator wrote defaultFileAccess to offset+38
    // instead of +40 (38-39 is an unused gap -- see UserEntry.fromBytes),
    // so a freshly created image's SYSTEM read defaultFileAccess as 0 and
    // every new file fell through to the restrictive ACCESS_DEFAULT
    // (0x03FF, PUBLIC nothing). Matches ndfs-c's ndfs_ue_init, which stamps
    // a fresh user 0x04FF.
    it('SYSTEM gets defaultFileAccess 0x04FF, granting PUBLIC READ', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Floppy360KB,
      });
      expect(fs.getUser(0)!.defaultFileAccess).toBe(0x04ff);
    });

    it('marks system blocks as used in bitmap', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Floppy360KB,
      });
      expect(fs.isBlockUsed(0)).toBe(true);       // master block
      expect(fs.isBlockUsed(149)).toBe(true);      // obj index
      expect(fs.isBlockUsed(150)).toBe(true);      // obj data
      expect(fs.isBlockUsed(151)).toBe(true);      // usr index
      expect(fs.isBlockUsed(152)).toBe(true);      // usr data
      expect(fs.isBlockUsed(153)).toBe(true);      // bit file
    });

    it('pointers are near end of disk', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Floppy360KB,
      });
      const mb = fs.getMasterBlock();
      // Floppy pointers should be near the end (page 154 total)
      expect(mb.objectFilePointer!.blockId).toBeGreaterThan(140);
      expect(mb.userFilePointer!.blockId).toBeGreaterThan(140);
      expect(mb.bitFilePointer!.blockId).toBeGreaterThan(140);
    });

    it('can write files to the created image', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Floppy360KB,
        directoryName: 'WRITABLE',
      });
      const data = new TextEncoder().encode('Hello from a new floppy!');
      fs.writeFile('SYSTEM/HELLO:DATA', data);
      const read = fs.readFile('SYSTEM/HELLO:DATA');
      expect(new TextDecoder().decode(read)).toBe('Hello from a new floppy!');
    });
  });

  describe('Floppy12MB', () => {
    it('creates a valid 1.2MB floppy image', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Floppy12MB,
        directoryName: 'FLOPPY12',
      });
      expect(fs.getDirectoryName()).toBe('FLOPPY12');
      const mb = fs.getMasterBlock();
      expect(mb.objectFilePointer!.blockId).toBe(611);
      expect(mb.userFilePointer!.blockId).toBe(613);
      expect(mb.bitFilePointer!.blockId).toBe(615);
    });

    it('has correct free page count', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Floppy12MB,
      });
      const usedPages = fs.getUsedPages();
      // 6 system blocks used: 0, 611, 612, 613, 614, 615
      expect(usedPages).toBe(6);
      expect(fs.getFreePages()).toBe(616 - 6);
    });
  });

  describe('Smd75MB', () => {
    it('creates a valid 75MB SMD image', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Smd75MB,
        directoryName: 'SMD75',
      });
      expect(fs.getDirectoryName()).toBe('SMD75');
    });

    it('has extended info with valid checksum', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Smd75MB,
        directoryName: 'SMDEXT',
        systemNumber: 42,
        flagWord: 0x0001,
      });
      const mb = fs.getMasterBlock();
      expect(mb.extValid).toBe(true);
      expect(mb.extLastSystemNumber).toBe(42);
      expect(mb.extFlagWord).toBe(0x0001);
    });

    it('has correct pointers', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Smd75MB,
      });
      const mb = fs.getMasterBlock();
      expect(mb.objectFilePointer!.blockId).toBe(18684);
      expect(mb.userFilePointer!.blockId).toBe(18686);
      // ALBIT 137526B: floor(36945/2)=18472 rounded DOWN to a multiple of 9 = 18468 — the
      // true PACK-ONE bit_file_ptr. This previously asserted 18472 (plain pages/2), off by 4.
      expect(mb.bitFilePointer!.blockId).toBe(18468);
      expect(mb.unreservedPages).toBe(36945);
    });

    it('pointers are spread out on hard disk', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Smd75MB,
      });
      const mb = fs.getMasterBlock();
      // Hard disk pointers should be in the middle-to-upper range
      expect(mb.bitFilePointer!.blockId).toBeLessThan(mb.objectFilePointer!.blockId);
    });
  });

  describe('Winchester74MB', () => {
    it('creates a valid 74MB Winchester image', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Winchester74MB,
        directoryName: 'WINCH74',
      });
      expect(fs.getDirectoryName()).toBe('WINCH74');
      const mb = fs.getMasterBlock();
      // Real Winchester drive = a Micropolis 1325 (5.25" ST-506/MFM), measured across 7
      // real images: device 36864 pages (72.0 MiB), capacity 36396, spare 468.
      expect(mb.objectFilePointer!.blockId).toBe(18428);
      expect(mb.userFilePointer!.blockId).toBe(18430);
      expect(mb.bitFilePointer!.blockId).toBe(18198); // 9*floor(floor(36396/2)/9)
    });
  });

  describe('Custom', () => {
    it('creates a custom-sized floppy image', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Custom,
        customPages: 200,
        directoryName: 'CUSTOM',
      });
      expect(fs.getDirectoryName()).toBe('CUSTOM');
      const mb = fs.getMasterBlock();
      // KERNEL-CORRECTED: there is no "small disk goes near the end" layout. SINTRAN
      // branches on whether an explicit bit-file address was given, NOT on device size, so
      // a small volume on the DEFAULT path still lands mid-disk:
      //   bit = 9*floor(floor(200/2)/9) = 9*floor(100/9) = 9*11 = 99
      //   obj = bit + bitmapPages(1) = 100, usr = obj + 2 = 102
      expect(mb.bitFilePointer!.blockId).toBe(99);
      expect(mb.objectFilePointer!.blockId).toBe(100);
      expect(mb.userFilePointer!.blockId).toBe(102);
    });

    it('creates a custom-sized hard disk image', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Custom,
        customPages: 5000,
        directoryName: 'BIGDISK',
      });
      expect(fs.getDirectoryName()).toBe('BIGDISK');
      const mb = fs.getMasterBlock();
      // ALBIT default path: bit = 9*floor(floor(5000/2)/9) = 9*floor(2500/9) = 9*277 = 2493.
      // (Plain pages/2 gave 2500 — the old, wrong formula.)
      // obj = bit + bitmapPages(1) = 2494, usr = obj + 2 = 2496.
      expect(mb.bitFilePointer!.blockId).toBe(2493);
      expect(mb.objectFilePointer!.blockId).toBe(2494);
      expect(mb.userFilePointer!.blockId).toBe(2496);
    });

    it('verifies all fields on custom image', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Custom,
        customPages: 300,
        directoryName: 'ALLFIELDS',
      });
      expect(fs.getDirectoryName()).toBe('ALLFIELDS');
      expect(fs.getUsers().length).toBe(1);
      expect(fs.getUsers()[0].userName).toBe('SYSTEM');
      expect(fs.verifyIntegrity()).toBe(true);
    });

    it('throws on too-small custom size', () => {
      expect(() =>
        NdfsFileSystem.createImage({
          template: ImageTemplate.Custom,
          customPages: 5,
        }),
      ).toThrow();
    });

    it('supports extra users during creation', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Custom,
        customPages: 200,
        directoryName: 'MULTIUSER',
        users: [
          { name: 'ALICE', reservedPages: 50 },
          { name: 'BOB', reservedPages: 30 },
        ],
      });
      const users = fs.getUsers();
      expect(users.length).toBe(3); // SYSTEM + ALICE + BOB
    });

    // Same offset+38-vs-40 regression as SYSTEM's, for a user created via
    // options.users at image-creation time rather than addUser() afterward.
    it('extra users created at image-creation time also get defaultFileAccess 0x04FF', () => {
      const fs = NdfsFileSystem.createImage({
        template: ImageTemplate.Custom,
        customPages: 200,
        directoryName: 'MULTIUSER',
        users: [{ name: 'ALICE', reservedPages: 50 }],
      });
      const alice = fs.getUsers().find((u) => u.userName === 'ALICE')!;
      expect(alice.defaultFileAccess).toBe(0x04ff);
    });
  });
});
