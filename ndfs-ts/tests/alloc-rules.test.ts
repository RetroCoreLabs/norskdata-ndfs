/**
 * Tests for SINTRAN's page allocation rules and image layout (docs/NDFS-ALLOCATION-RULES.md,
 * docs/CREATE-DIRECTORY-PLACEMENT.md).
 *
 * The expected numbers come from real SINTRAN M runs in the nd100x emulator: bit file address,
 * first single page of 12 disk formats, contiguous stacks, index block placement and quota.
 */
import { describe, it, expect } from 'vitest';
import { BitFile } from '../src/bit-file.js';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { createNdfsImage, optionsForDiskType } from '../src/image-creator.js';
import { ImageTemplate, BootChoice, ImageCreationOptions } from '../src/types.js';
import { NDFS_PAGE_SIZE } from '../src/constants.js';
import { DISK_TYPES, DiskFamily, diskTypeCount, findDiskType, getDiskType } from '../src/disk-types.js';
import { bootBlob, scsiLastPage } from '../src/boot-blobs.js';

/** A bit file for a disk of `cap` pages with the bit file at `ba`: page 0 and the bit file pages used. */
function makeBf(cap: number, ba: number): BitFile {
  const bf = new BitFile();
  bf.initialize(cap);
  bf.allocCeiling = cap;
  bf.bitAddress = ba;
  bf.markBlockUsed(0);
  for (let i = 0; i < Math.floor((cap + 16383) / 16384); i++) bf.markBlockUsed(ba + i);
  return bf;
}

function takeHint(bf: BitFile): number {
  const p = bf.findFreeHint();
  bf.markBlockUsed(p);
  return p;
}

function takeBlock(bf: BitFile): number {
  const p = bf.findFreeDataBlock();
  bf.markBlockUsed(p);
  return p;
}

describe('RSBPG hint rule: first free page going down from (BA - 1) OR 15', () => {
  it('SMD 75 MB gives the four pages SINTRAN gave', () => {
    const bf = makeBf(36945, 18468);
    expect([takeHint(bf), takeHint(bf), takeHint(bf), takeHint(bf)]).toEqual([18479, 18478, 18477, 18476]);
  });

  it('a bit file that holds the top of the home word (DISC-288MB)', () => {
    const bf = makeBf(140391, 70191);
    expect([takeHint(bf), takeHint(bf), takeHint(bf), takeHint(bf)]).toEqual([70190, 70189, 70188, 70187]);
  });

  it('small Winchester (DISC-14MB)', () => {
    const bf = makeBf(6912, 3456);
    expect([takeHint(bf), takeHint(bf), takeHint(bf), takeHint(bf)]).toEqual([3455, 3454, 3453, 3452]);
  });

  it('spirals to the neighbouring words when the home word is full (read in code, not measured)', () => {
    const bf = makeBf(36945, 18468);
    for (let p = 18464; p < 18480; p++) bf.markBlockUsed(p);
    expect(takeHint(bf)).toBe(18480);
    expect(takeHint(bf)).toBe(18481);
    for (let p = 18482; p < 18496; p++) bf.markBlockUsed(p);
    expect(takeHint(bf)).toBe(18463);
  });

  it('never gives a page below block 7 or at or above the directory size', () => {
    const bf = makeBf(100, 50);
    for (let p = 7; p < 100; p++) if (!bf.isBlockUsed(p)) bf.markBlockUsed(p);
    expect(bf.findFreeHint()).toBe(-1);
    bf.markBlockFree(99);
    expect(bf.findFreeHint()).toBe(99);
  });

  it('falls back to the top-down search without a bit file address', () => {
    const bf = makeBf(1000, 500);
    bf.bitAddress = 0;
    const top = bf.findFirstFreeBlock();
    expect(bf.findFreeHint()).toBe(top);
    expect(bf.findFreeDataBlock()).toBe(top);
  });
});

describe('RSPAG block rule: lowest free page of a 256 page block, then block+1, block-1, ...', () => {
  // capacity, bit file address, first page given on a cold start (measured with SINTRAN M)
  const cold: Array<[number, number, number]> = [
    [18486, 9243, 16384],     // DISC-38MB
    [34765, 17379, 32768],    // DISC-70MB
    [36945, 18468, 32768],    // DISC-75MB
    [69530, 34758, 65536],    // DISC-140MB-F
    [140391, 70191, 131072],  // DISC-288MB
    [220584, 110286, 212992], // DISC-450MB
    [22032, 11016, 16384],    // DISC-45MB
    [36396, 18198, 32768],    // DISC-74MB
    [6912, 3456, 3328],       // DISC-14MB
    [10728, 5364, 5120],      // DISC-21MB
    [11016, 5508, 5376],      // DISC-23MB
    [13648, 6822, 6656],      // DISC-28MB
  ];

  for (const [cap, ba, first] of cold) {
    it(`cold start, capacity ${cap}: first page ${first}`, () => {
      const bf = makeBf(cap, ba);
      expect(takeBlock(bf)).toBe(first);
      expect(takeBlock(bf)).toBe(first + 1); // the next call stays in the block
    });
  }

  it('spirals when a block is full', () => {
    const bf = makeBf(36945, 18468);
    for (let p = 32768; p < 33024; p++) bf.markBlockUsed(p); // cold start block full
    expect(takeBlock(bf)).toBe(33024);                       // block + 1
    for (let p = 33025; p < 33280; p++) bf.markBlockUsed(p); // block + 1 full
    expect(takeBlock(bf)).toBe(33280);                       // cursor moved to block + 1; its next block up
    bf.cursorBlock = 128;                                    // back to the first block: up is full, so down
    expect(takeBlock(bf)).toBe(32512);
  });

  it('the cursor follows the last page given', () => {
    const bf = makeBf(36945, 18468);
    expect(takeBlock(bf)).toBe(32768);
    bf.cursorBlock = 72;                                     // 18432..18687, the block of BA + 1
    expect(takeBlock(bf)).toBe(18432);
    for (let p = 18433; p < 18468; p++) bf.markBlockUsed(p);
    expect(takeBlock(bf)).toBe(18471);                       // lowest free page of the block
  });
});

function newSmd75(): NdfsFileSystem {
  return NdfsFileSystem.createImage({ template: ImageTemplate.Smd75MB, directoryName: 'ALLOC' });
}

function usedOf(fs: NdfsFileSystem, name: string): number {
  return fs.getUsers().find((u) => u.userName === name)!.pagesUsed;
}

function entryOf(fs: NdfsFileSystem, name: string) {
  return fs.getObjectEntries().find((e) => e.objectName === name.split(':')[0])!;
}

describe('file system placement', () => {
  it('contiguous files stack downward from the directory size', () => {
    const fs = newSmd75();
    fs.createContiguousFile('SYSTEM/C1:DATA', 10);
    fs.createContiguousFile('SYSTEM/C2:DATA', 20);
    expect(entryOf(fs, 'C1:DATA').filePointer!.blockId).toBe(36935);
    expect(entryOf(fs, 'C2:DATA').filePointer!.blockId).toBe(36915);
  });

  it('a contiguous file of one page uses the data page rule', () => {
    const fs = newSmd75();
    fs.createContiguousFile('SYSTEM/N1:DATA', 1);
    fs.createContiguousFile('SYSTEM/N2:DATA', 1);
    expect(entryOf(fs, 'N1:DATA').filePointer!.blockId).toBe(32768);
    expect(entryOf(fs, 'N2:DATA').filePointer!.blockId).toBe(32769);
  });

  it('an indexed file: index block by the hint rule, data pages by the data page rule', () => {
    const fs = newSmd75();
    fs.createContiguousFile('SYSTEM/N1:DATA', 1);
    fs.createContiguousFile('SYSTEM/N2:DATA', 1);
    fs.writeFile('SYSTEM/N3:DATA', new Uint8Array(3 * NDFS_PAGE_SIZE).fill(0x5a));
    expect(entryOf(fs, 'N3:DATA').filePointer!.blockId).toBe(18475);
    expect(fs.getFileBlocks('SYSTEM/N3:DATA')).toEqual([32770, 32771, 32772]);
  });

  it('quota charges data pages, index blocks and the owner\'s object data page', () => {
    const fs = newSmd75();
    expect(usedOf(fs, 'SYSTEM')).toBe(1);                       // its object data page
    fs.writeFile('SYSTEM/Q1:DATA', new Uint8Array(3 * NDFS_PAGE_SIZE).fill(0x11));
    expect(usedOf(fs, 'SYSTEM')).toBe(1 + 3 + 1);               // + 3 data + 1 index
    expect(fs.addUser('ALPHA', 50)).toBe(true);
    expect(usedOf(fs, 'ALPHA')).toBe(0);
    fs.writeFile('ALPHA/A1:DATA', new Uint8Array(NDFS_PAGE_SIZE).fill(0x22));
    expect(usedOf(fs, 'ALPHA')).toBe(1 + 1 + 1);                // data + index + object page
    fs.deleteFile('ALPHA/A1:DATA');
    expect(usedOf(fs, 'ALPHA')).toBe(1);                        // the object page stays charged
    fs.deleteFile('SYSTEM/Q1:DATA');
    expect(usedOf(fs, 'SYSTEM')).toBe(1);
  });

  it('a 600 page file costs a top block and two sub-index blocks', () => {
    const fs = newSmd75();
    const data = new Uint8Array(600 * NDFS_PAGE_SIZE);
    for (let i = 0; i < data.length; i++) data[i] = (i * 7 + 1) & 0xff;
    fs.writeFile('SYSTEM/BIGF:DATA', data);
    expect(usedOf(fs, 'SYSTEM')).toBe(1 + 600 + 3);
    expect(fs.verifyIntegrity()).toBe(true);
    expect(fs.readFile('SYSTEM/BIGF:DATA')).toEqual(data);
  });

  it('the bit file is sized from the directory size, not the device length', () => {
    // Device 153576 pages, directory 140391 (DISC-288MB-F): 9 bit file pages, not 10.
    const image = createNdfsImage({
      template: ImageTemplate.Custom,
      directoryName: 'BIGDEV',
      customPages: 140391,
      devicePages: 153576,
      includeExtendedInfo: true,
    });
    expect(image.length).toBe(153576 * NDFS_PAGE_SIZE);
    const fs = new NdfsFileSystem(image);
    const after = fs.getMasterBlock().bitFilePointer!.blockId + 9;
    fs.writeFile('SYSTEM/BIG:DATA', Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8]));
    const buf = fs.toBuffer();
    expect(buf.subarray(after * NDFS_PAGE_SIZE, (after + 1) * NDFS_PAGE_SIZE).some((b) => b !== 0)).toBe(false);
    expect(fs.verifyIntegrity()).toBe(true);
    expect(Array.from(fs.readFile('SYSTEM/BIG:DATA'))).toEqual([1, 2, 3, 4, 5, 6, 7, 8]);
  });
});

function ptr(buf: Uint8Array, off: number): number {
  const o = 0x7e0 + off;
  return (((buf[o] << 24) | (buf[o + 1] << 16) | (buf[o + 2] << 8) | buf[o + 3]) & 0x3fffffff) >>> 0;
}

function rawBit(buf: Uint8Array, bitFile: number, page: number): number {
  return (buf[bitFile * NDFS_PAGE_SIZE + ((page >>> 3) ^ 1)] >>> (page & 7)) & 1;
}

describe('created image layout', () => {
  const make = (template: ImageTemplate, answer = 0, boot: BootChoice = BootChoice.None): Uint8Array =>
    createNdfsImage({ template, directoryName: 'RULES', bitFileAnswer: answer, bootChoice: boot });

  it('SCSI 126 MB', () => {
    const img = make(ImageTemplate.Scsi126MB);
    expect(img.length).toBe(64656 * NDFS_PAGE_SIZE);
    expect([ptr(img, 0x10), ptr(img, 0x14), ptr(img, 0x18)]).toEqual([30506, 30508, 30510]);
    expect(new NdfsFileSystem(img).getMasterBlock().extPagesAvailable).toBe(61036);
  });

  it('writes the real SCSI last page', () => {
    const img = make(ImageTemplate.Scsi126MB);
    expect(img.subarray(64655 * NDFS_PAGE_SIZE)).toEqual(scsiLastPage());
  });

  it('bit file address answers on SMD 75 MB', () => {
    let img = make(ImageTemplate.Smd75MB, -1); // end of disc: capacity - 3 bit file pages
    expect([ptr(img, 0x18), ptr(img, 0x14), ptr(img, 0x10)]).toEqual([36942, 36940, 36938]);
    img = make(ImageTemplate.Smd75MB, 16384);  // a typed address
    expect([ptr(img, 0x18), ptr(img, 0x14), ptr(img, 0x10)]).toEqual([16384, 16382, 16380]);
  });

  it('default pointers of every template', () => {
    const expected: Array<[ImageTemplate, number, number, number]> = [
      [ImageTemplate.Floppy360KB, 76, 78, 72],
      [ImageTemplate.Floppy12MB, 316, 318, 306],
      [ImageTemplate.Smd75MB, 18476, 18478, 18468],
      [ImageTemplate.Winchester74MB, 18204, 18206, 18198],
      [ImageTemplate.Scsi126MB, 30506, 30508, 30510],
    ];
    for (const [tmpl, obj, usr, bit] of expected) {
      const img = make(tmpl);
      expect([ptr(img, 0x10), ptr(img, 0x14), ptr(img, 0x18)]).toEqual([obj, usr, bit]);
    }
  });

  it('Unreserved Pages = capacity - 1 - bitmap pages - 2 - 1 - the SYSTEM quota', () => {
    const img = make(ImageTemplate.Smd75MB); // 36945-1-3-2-1-18472
    expect(ptr(img, 0x1c)).toBe(18466);
  });

  it('marks the pages above the capacity up to the next multiple of 256', () => {
    const img = make(ImageTemplate.Smd75MB);
    const ba = ptr(img, 0x18);
    expect(rawBit(img, ba, 36945)).toBe(1);
    expect(rawBit(img, ba, 37119)).toBe(1);
    expect(rawBit(img, ba, 37120)).toBe(0);
    expect(rawBit(img, ba, 0)).toBe(1);
    expect(rawBit(img, ba, 1)).toBe(0);
    expect(rawBit(img, ba, 6)).toBe(0);
  });

  it('marks every bit file page as used', () => {
    const img = make(ImageTemplate.Scsi126MB);
    const ba = ptr(img, 0x18);
    for (let i = 0; i < 4; i++) expect(rawBit(img, ba, ba + i)).toBe(1);
  });

  it('a floppy has no Extended Info Block', () => {
    const img = make(ImageTemplate.Floppy12MB);
    expect(img.subarray(0x7d0, 0x7e0).some((b) => b !== 0)).toBe(false);
  });

  it('writes the boot area of the template, or none, or your own bytes', () => {
    let img = make(ImageTemplate.Smd75MB, 0, BootChoice.Auto);
    expect(img.subarray(0, 2000)).toEqual(bootBlob('smd'));
    expect(img[0]).toBe(0xd1);
    expect(img[1]).toBe(0x05); // PIOF, the first word of every hard disk boot area
    img = make(ImageTemplate.Smd75MB, 0, BootChoice.None);
    expect(img.subarray(0, 2000).some((b) => b !== 0)).toBe(false);
    const own = Uint8Array.from([1, 2, 3, 4, 5, 6, 7, 8]);
    img = createNdfsImage({ template: ImageTemplate.Floppy360KB, directoryName: 'OWNBOOT', bootData: own });
    expect(img.subarray(0, 8)).toEqual(own);
    expect(img[8]).toBe(0);
  });

  it('SYSTEM and extra users get the default file access 0x04FF', () => {
    const fs = NdfsFileSystem.createImage({
      template: ImageTemplate.Custom,
      customPages: 200,
      users: [{ name: 'ALICE', reservedPages: 50 }],
    });
    expect(fs.getUser(0)!.defaultFileAccess).toBe(0x04ff);
    expect(fs.getUsers().find((u) => u.userName === 'ALICE')!.defaultFileAccess).toBe(0x04ff);
  });
});

describe('disk types', () => {
  it('has the table', () => {
    expect(diskTypeCount()).toBe(40);
    expect(findDiskType('DISC-75MB')!.pages).toBe(36945);
    expect(findDiskType('disc-74mb')!.pages).toBe(36396); // case-insensitive
    expect(findDiskType('DISC-450MB-F')!.pages).toBe(220584);
    expect(findDiskType('DISC-14MB')!.pages).toBe(6912);
    expect(findDiskType('DISC-90MB')!.pages).toBe(7389);
    expect(findDiskType('DISC-SCSI-1')).toBeUndefined();
    expect(findDiskType('DISC-75MB-1')).toBeUndefined();
    expect(findDiskType('')).toBeUndefined();
    expect(findDiskType('DISC-74MB')!.family).toBe(DiskFamily.Winchester);
    expect(findDiskType('DISC-74MB')!.tested).toBe(true);
    expect(findDiskType('DISC-75MB')!.family).toBe(DiskFamily.Smd);
    expect(findDiskType('DISC-30MB')!.tested).toBe(false);
    expect(findDiskType('DISC-10MB')!.pages).toBe(0); // valid name, size not known
    expect(getDiskType(0)).toBeDefined();
    expect(getDiskType(40)).toBeUndefined();
    for (const t of DISK_TYPES) {
      expect(findDiskType(t.name)).toBe(t);
      expect(t.pages === 0 || t.pages >= 32).toBe(true);
    }
  });

  it('knows the layout lengths', () => {
    expect(findDiskType('DISC-288MB-R')!.layoutPages).toBe(140733);
    expect(findDiskType('DISC-288MB-F')!.layoutPages).toBe(153576);
    expect(findDiskType('DISC-450MB-F')!.layoutPages).toBe(230364);
    expect(findDiskType('DISC-450MB-N')!.layoutPages).toBe(223820);
    expect(findDiskType('DISC-75MB')!.layoutPages).toBe(37035);
    expect(findDiskType('DISC-30MB')!.layoutPages).toBe(0);
    expect(findDiskType('DISC-140MB-F')!.layoutPages).toBe(0);
  });

  it('creates an image from a disk type name', () => {
    const img = createNdfsImage(optionsForDiskType('DISC-14MB', 'DISC14'));
    expect(img.length).toBe(6912 * NDFS_PAGE_SIZE);
    expect(new NdfsFileSystem(img).getMasterBlock().extPagesAvailable).toBe(6912);
  });

  it('DISC-288MB-F gets the full layout length', () => {
    const img = createNdfsImage(optionsForDiskType('DISC-288MB-F', 'BIG'));
    expect(img.length).toBe(153576 * NDFS_PAGE_SIZE);
    const mb = new NdfsFileSystem(img).getMasterBlock();
    expect([mb.objectFilePointer!.blockId, mb.userFilePointer!.blockId, mb.bitFilePointer!.blockId]).toEqual([70187, 70189, 70191]);
    expect(mb.extPagesAvailable).toBe(140391);
  });

  it('refuses unknown names and names without a size', () => {
    expect(() => optionsForDiskType('DISC-NOPE')).toThrow();
    expect(() => optionsForDiskType('DISC-10MB')).toThrow();
  });

  it('ignores a device length shorter than the capacity', () => {
    const opts: ImageCreationOptions = {
      template: ImageTemplate.Custom,
      customPages: 6912,
      devicePages: 100,
      includeExtendedInfo: true,
    };
    expect(createNdfsImage(opts).length).toBe(6912 * NDFS_PAGE_SIZE);
  });
});
