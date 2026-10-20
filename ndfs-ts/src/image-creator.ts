/**
 * NDFS image creation: builds new disk images.
 *
 * The layout follows what real SINTRAN M writes with @CREATE-DIRECTORY, measured in the nd100x
 * emulator (docs/CREATE-DIRECTORY-ORACLE.md, docs/CREATE-DIRECTORY-PLACEMENT.md):
 *
 * - the bit file goes at the address of SINTRAN's prompt "OCTAL BIT FILE ADDRESS (-1 END OF DISC)":
 *   0 gives 9 * floor(floor(capacity / 2) / 9), -1 gives capacity minus the bit file pages;
 * - the bit file is ceil(capacity / 16384) pages and marks page 0, itself, and the pages from the
 *   capacity up to the next multiple of 256 (not past the end of the device);
 * - the user data page, user index block, object data page and object index block are the first four
 *   free pages going down from `(BA - 1) OR 15` (routine RSBPG);
 * - hard disks get an Extended Info Block with the declared capacity, floppies none.
 *
 * Templates: Floppy360KB, Floppy12MB, Smd75MB, Winchester74MB, Scsi126MB and Custom. Any SINTRAN disk
 * type name can be made with {@link optionsForDiskType}.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import {
  NDFS_PAGE_SIZE,
  MASTER_BLOCK_OFFSET,
  EXTENDED_INFO_OFFSET,
  NDFS_NAME_MAX,
  ENTRIES_PER_PAGE,
  ENTRY_SIZE,
  USER_ENTRY_FLAG,
} from './constants.js';
import { writeUint16BE, writeUint32BE } from './endian.js';
import { writeNdfsName } from './ndfs-name.js';
import { MasterBlock } from './master-block.js';
import { BlockPointer } from './block-pointer.js';
import { PointerType, ImageTemplate, BootChoice, ImageCreationOptions } from './types.js';
import { bootBlob, scsiLastPage } from './boot-blobs.js';
import { DiskFamily, findDiskType } from './disk-types.js';

/** What a template fixes. Everything else is worked out from these numbers. */
interface TemplateSpec {
  /** Declared directory size in pages (Pages Available). */
  capacity: number;
  /** Length of the image in pages; never less than capacity. */
  devicePages: number;
  /** Hard disk: write the Extended Info Block (floppies have none). */
  extValid: boolean;
  /** Boot area written when the caller says Auto. */
  boot: BootChoice;
  /** Copy the real SCSI last page (SINTRAN needs it). */
  scsiLastPage: boolean;
}

function getTemplateSpec(options: ImageCreationOptions): TemplateSpec {
  switch (options.template) {
    case ImageTemplate.Floppy360KB:
      return { capacity: 154, devicePages: 154, extValid: false, boot: BootChoice.Floppy360, scsiLastPage: false };
    case ImageTemplate.Floppy12MB:
      return { capacity: 616, devicePages: 616, extValid: false, boot: BootChoice.Floppy616, scsiLastPage: false };
    // capacity 36945 is DISC-75MB in SINTRAN's name table; 38400 pages is the file size of the real SMD0.IMG.
    case ImageTemplate.Smd75MB:
      return { capacity: 36945, devicePages: 38400, extValid: true, boot: BootChoice.Smd, scsiLastPage: false };
    // Real Micropolis 1325 image: file 36864 pages, declared capacity 36396 (DISC-74MB).
    case ImageTemplate.Winchester74MB:
      return { capacity: 36396, devicePages: 36864, extValid: true, boot: BootChoice.Winchester, scsiLastPage: false };
    // Real SCSI image: SINTRAN accepts a SCSI disk only with exactly this length and the real last page.
    case ImageTemplate.Scsi126MB:
      return { capacity: 61036, devicePages: 64656, extValid: true, boot: BootChoice.Scsi, scsiLastPage: true };
    case ImageTemplate.Custom: {
      const pages = options.customPages;
      if (pages === undefined || pages < 32) {
        throw new Error('customPages must be specified (>= 32) for Custom template');
      }
      const boot =
        pages === 154 ? BootChoice.Floppy360 : pages === 616 ? BootChoice.Floppy616 : BootChoice.None;
      let devicePages = pages;
      if (options.devicePages !== undefined && options.devicePages > devicePages) {
        devicePages = options.devicePages;
      }
      // Floppy sized images carry no Extended Info Block.
      return { capacity: pages, devicePages, extValid: pages > 1000, boot, scsiLastPage: false };
    }
    default:
      throw new Error(`Unknown template: ${options.template}`);
  }
}

/**
 * Image options for a SINTRAN disk type name such as `DISC-288MB-R`.
 *
 * The image has the directory size SINTRAN gives the type and the full disk layout length where that
 * is known (the emulator's disk controllers pick their geometry from the file length). The boot area is
 * the one of the controller family. Throws for an unknown name or one whose size is not known.
 */
export function optionsForDiskType(name: string, directoryName?: string): ImageCreationOptions {
  const t = findDiskType(name);
  if (!t) throw new Error(`Unknown SINTRAN disk type: ${name}`);
  if (t.pages === 0) throw new Error(`${t.name} is a valid SINTRAN disk type name but its size is not known`);
  const boot =
    t.family === DiskFamily.Smd
      ? BootChoice.Smd
      : t.family === DiskFamily.Winchester
        ? BootChoice.Winchester
        : BootChoice.Auto;
  return {
    template: ImageTemplate.Custom,
    directoryName,
    customPages: t.pages,
    devicePages: t.layoutPages > t.pages ? t.layoutPages : undefined,
    includeExtendedInfo: t.pages > 1000,
    bootChoice: boot,
  };
}

function markPageUsed(bitmap: Uint8Array, page: number): void {
  // Same addressing as BitFile: page N is bit N%16 of the 16-bit big-endian word N/16, which on a
  // byte array is byte (N>>3)^1, bit N&7.
  bitmap[(page >>> 3) ^ 1] |= 1 << (page & 7);
}

function pageIsUsed(bitmap: Uint8Array, page: number): boolean {
  return ((bitmap[(page >>> 3) ^ 1] >>> (page & 7)) & 1) === 1;
}

/**
 * RSBPG rule (see BitFile.findFreeNear): the first free page going down from `hint OR 15`, then word+1
 * up, word-1 down, ... Marks the page used. Returns -1 if none.
 */
function allocNearHint(bitmap: Uint8Array, capacity: number, hint: number): number {
  const word = hint >>> 4;
  const nwords = (capacity + 15) >>> 4;
  const ok = (p: number): boolean => p < capacity && !pageIsUsed(bitmap, p);
  for (let bit = 15; bit >= 0; bit--) {
    const p = word * 16 + bit;
    if (ok(p)) { markPageUsed(bitmap, p); return p; }
  }
  for (let step = 1; step <= nwords; step++) {
    let w = word + step;
    if (w < nwords) {
      for (let bit = 0; bit < 16; bit++) {
        const p = w * 16 + bit;
        if (ok(p)) { markPageUsed(bitmap, p); return p; }
      }
    }
    if (word >= step) {
      w = word - step;
      for (let bit = 15; bit >= 0; bit--) {
        const p = w * 16 + bit;
        if (ok(p)) { markPageUsed(bitmap, p); return p; }
      }
    }
  }
  return -1;
}

/**
 * Create a new NDFS disk image from the given options.
 * Returns the raw image buffer.
 */
export function createNdfsImage(options: ImageCreationOptions): Uint8Array {
  const spec = getTemplateSpec(options);
  const cap = spec.capacity;
  const dev = spec.devicePages;
  const dirName = (options.directoryName || 'NDFS').toUpperCase().substring(0, NDFS_NAME_MAX);

  // The bit file: one bit per page, 16384 bits per page, sized by the declared capacity.
  const bitmapPages = Math.floor((cap + 16383) / 16384);

  // Bit file address, the answer to SINTRAN's prompt "OCTAL BIT FILE ADDRESS (-1 END OF DISC)".
  const answer = options.bitFileAnswer ?? 0;
  let ba: number;
  if (answer === 0) ba = 9 * Math.floor(Math.floor(cap / 2) / 9);
  else if (answer === -1) ba = cap - bitmapPages;
  else if (answer > 0) ba = answer;
  else throw new Error('bitFileAnswer must be 0, -1 or a page address');
  if (ba < 8 || ba + bitmapPages > cap) throw new Error('bit file address outside the disk');

  // Pages left for the user quota: capacity - page 0 - the bit file - the user file (index block + data
  // page) - the object file index block. Unreserved Pages is this minus the sum of the user quotas.
  if (cap < 40 + bitmapPages) throw new Error('disk too small');
  const avail = cap - 1 - bitmapPages - 2 - 1;
  const quota = cap > 1000 ? Math.floor(cap / 2) : Math.min(avail, 500);

  const image = new Uint8Array(dev * NDFS_PAGE_SIZE);

  // Bitmap, as @CREATE-DIRECTORY leaves it, then the four pages the first user and the first file take.
  const bm = new Uint8Array(bitmapPages * NDFS_PAGE_SIZE);
  markPageUsed(bm, 0);
  for (let i = 0; i < bitmapPages; i++) markPageUsed(bm, ba + i);
  if (spec.extValid && dev > cap) {
    // When the device is longer than the capacity SINTRAN marks the pages from the capacity up to the
    // next multiple of 256 as used (not past the end of the device).
    const end = Math.min(Math.ceil(cap / 256) * 256, dev);
    for (let i = cap; i < end; i++) markPageUsed(bm, i);
  }
  const userData = allocNearHint(bm, cap, ba - 1);
  const userIndex = allocNearHint(bm, cap, ba - 1);
  const objData = allocNearHint(bm, cap, ba - 1);
  const objIndex = allocNearHint(bm, cap, ba - 1);
  if (Math.min(userData, userIndex, objData, objIndex) < 0) throw new Error('no space for the directory files');

  // Boot area: bytes 0..1999 of page 0.
  const boot = options.bootChoice === undefined || options.bootChoice === BootChoice.Auto
    ? spec.boot
    : options.bootChoice;
  if (options.bootData && options.bootData.length > 0) {
    image.set(options.bootData.subarray(0, Math.min(options.bootData.length, 2000)), 0);
  } else if (boot !== BootChoice.None && boot !== BootChoice.Auto) {
    image.set(bootBlob(boot), 0);
  }

  // Page 0: master block (and Extended Info Block on hard disks).
  const mbOff = MASTER_BLOCK_OFFSET;
  writeNdfsName(image, mbOff, dirName, NDFS_NAME_MAX);
  new BlockPointer(objIndex, PointerType.Indexed).toBytes(image, mbOff + 0x10);
  new BlockPointer(userIndex, PointerType.Indexed).toBytes(image, mbOff + 0x14);
  new BlockPointer(ba, PointerType.Contiguous).toBytes(image, mbOff + 0x18);
  writeUint32BE(image, mbOff + 0x1c, avail - quota);

  const writeExt = options.includeExtendedInfo === true || (spec.extValid && options.includeExtendedInfo !== false);
  if (writeExt) {
    const ext = EXTENDED_INFO_OFFSET;
    const sysNum = options.systemNumber || 0;
    const flagWord = options.flagWord || 0;
    writeUint16BE(image, ext + 2, 0); // reserved1
    writeUint16BE(image, ext + 4, 0); // reserved2
    writeUint16BE(image, ext + 6, 0); // reserved3
    writeUint16BE(image, ext + 8, flagWord);
    writeUint16BE(image, ext + 10, sysNum);
    writeUint32BE(image, ext + 12, cap);
    // Kernel-correct checksum: a 16-bit ADDITIVE sum of the seven words following the checksum slot.
    const pagesLo = cap & 0xffff;
    const pagesHi = (cap >>> 16) & 0xffff;
    writeUint16BE(image, ext, MasterBlock.computeExtChecksum(0, 0, 0, flagWord, sysNum, pagesHi, pagesLo));
  }

  // User file: index block -> data page with the SYSTEM entry. SYSTEM is charged the object data page.
  new BlockPointer(userData, PointerType.Contiguous).toBytes(image, userIndex * NDFS_PAGE_SIZE);
  const usrDataOff = userData * NDFS_PAGE_SIZE;
  writeUserEntry(image, usrDataOff, 0, 'SYSTEM', quota, 1);
  if (options.users) {
    for (let u = 0; u < options.users.length; u++) {
      const slotIndex = u + 1; // 0 is SYSTEM
      if (slotIndex >= ENTRIES_PER_PAGE) break;
      const userOpt = options.users[u];
      writeUserEntry(image, usrDataOff + slotIndex * ENTRY_SIZE, slotIndex, userOpt.name.toUpperCase(), userOpt.reservedPages, 0);
    }
  }

  // Object file: index block -> data page (no files yet, all zero).
  new BlockPointer(objData, PointerType.Contiguous).toBytes(image, objIndex * NDFS_PAGE_SIZE);

  // Bit file.
  image.set(bm, ba * NDFS_PAGE_SIZE);

  // SCSI: the last page the real disks carry.
  if (spec.scsiLastPage) image.set(scsiLastPage(), (dev - 1) * NDFS_PAGE_SIZE);

  return image;
}

function writeUserEntry(
  image: Uint8Array,
  off: number,
  userIndex: number,
  name: string,
  reservedPages: number,
  usedPages: number,
): void {
  image[off] = USER_ENTRY_FLAG; // 0x81 = valid user
  image[off + 1] = 0; // enter count
  writeNdfsName(image, off + 2, name, NDFS_NAME_MAX);
  writeUint16BE(image, off + 18, 0); // no password
  writeUint32BE(image, off + 28, reservedPages);
  writeUint32BE(image, off + 32, usedPages);
  image[off + 36] = 0; // directory index
  image[off + 37] = userIndex;
  // Default file access 0x04FF at offset 40, NOT 38 (offsets 38-39 are an unused gap).
  writeUint16BE(image, off + 40, 0x04ff);
}
