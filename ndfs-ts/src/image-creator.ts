/**
 * NDFS image creation: builds new disk images from templates.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * HackerCorp Labs - https://github.com/HackerCorpLabs
 */

import {
  NDFS_PAGE_SIZE,
  MASTER_BLOCK_OFFSET,
  EXTENDED_INFO_OFFSET,
  NDFS_NAME_TERMINATOR,
  NDFS_NAME_MAX,
  ENTRIES_PER_PAGE,
  ENTRY_SIZE,
  USER_ENTRY_FLAG,
} from './constants.js';
import { writeUint16BE, writeUint32BE } from './endian.js';
import { writeNdfsName } from './ndfs-name.js';
import { MasterBlock } from './master-block.js';
import { BlockPointer } from './block-pointer.js';
import { PointerType, ImageTemplate, ImageCreationOptions } from './types.js';

/** Internal specification for a disk template. */
interface TemplateSpec {
  ndfsPages: number;
  fileBlocks: number;
  objectFileBlock: number;
  userFileBlock: number;
  bitFileBlock: number;
  unreservedPages: number;
  isFloppy: boolean;
  includeExtendedInfo: boolean;
}

function getTemplateSpec(template: ImageTemplate, customPages?: number): TemplateSpec {
  switch (template) {
    case ImageTemplate.Floppy360KB:
      return {
        ndfsPages: 154,
        fileBlocks: 154,
        objectFileBlock: 149,
        userFileBlock: 151,
        bitFileBlock: 153,
        unreservedPages: 1,
        isFloppy: true,
        includeExtendedInfo: false,
      };
    case ImageTemplate.Floppy12MB:
      return {
        ndfsPages: 616,
        fileBlocks: 616,
        objectFileBlock: 611,
        userFileBlock: 613,
        bitFileBlock: 615,
        unreservedPages: 1,
        isFloppy: true,
        includeExtendedInfo: false,
      };
    // Drive geometries below are MEASURED from real ND disk images. Every real drive
    // reserves a spare (bad-sector remap) region, so fileBlocks (the physical device) is
    // always LARGER than ndfsPages (the declared, usable capacity) — never smaller.
    // The spare size is a property of the DRIVE, not a percentage of capacity: the same
    // 36396-page capacity has 468 spare on the Winchester and a different spare elsewhere.
    case ImageTemplate.Smd75MB:
      return {
        ndfsPages: 36945, // DOCUMENTED: ND-30.003.007, "75 MB disk pack giving 36,945 pages"
        fileBlocks: 38400, // real device (PACK-ONE / SMD0.IMG); spare = 1455
        objectFileBlock: 18684,
        userFileBlock: 18686,
        // KERNEL-CORRECTED (ALBIT 137526B): floor(36945/2)=18472 rounded DOWN to a multiple
        // of 9 = 18468 — the true PACK-ONE bit_file_ptr. Plain pages/2 gave 18472, off by 4.
        bitFileBlock: 18468,
        unreservedPages: 36945,
        isFloppy: false,
        includeExtendedInfo: true,
      };
    case ImageTemplate.Winchester74MB:
      return {
        ndfsPages: 36396, // DOCUMENTED: ND-30.003.007 DEVICE-COPY, "Pages to copy: 36396"
        // Real drive: a Micropolis 1325 (5.25" ST-506/MFM), measured across 7 real images —
        // device 36864 pages = exactly 72.0 MiB, spare 468.
        // fileBlocks used to be 36360, which was SMALLER than the declared capacity: the
        // image's last 36 pages did not exist at all.
        fileBlocks: 36864,
        objectFileBlock: 18428, // real disk (1325.img)
        userFileBlock: 18430, // = object + 2
        bitFileBlock: 18198, // 9*floor(floor(36396/2)/9) = 18198 (ALBIT)
        unreservedPages: 36396,
        isFloppy: false,
        includeExtendedInfo: true,
      };
    case ImageTemplate.Custom: {
      if (!customPages || customPages < 20) {
        throw new Error('Custom template requires at least 20 pages');
      }
      const pages = customPages;
      // Floppies carry no valid extended-info block; that is what distinguishes them here.
      const isFloppy = pages <= 1000;

      // Placement — KERNEL-DERIVED from ALBIT (137517B).
      //
      // The old "small disk vs big disk" branch was WRONG: SINTRAN does not switch layout
      // on device size. It branches on whether @CREATE-DIRECTORY was given an EXPLICIT
      // bit-file address. A small floppy created on the DEFAULT path still lands mid-disk.
      // (Two same-size 616-page floppies sit in opposite layouts for exactly this reason.)
      //
      // DEFAULT path:  bit_file = 9 * floor(floor(pages / 2) / 9)
      //   i.e. floor(pages/2) rounded DOWN to a multiple of 9.
      //   ALBIT 137526B-137532B: /2 -> /9 -> *9.
      //   Verified on every real disk: 36945 -> 18468, 616 -> 306, 61036 -> 30510.
      //   Plain pages/2 (18472 for PACK-ONE) is simply wrong.
      //
      // The "9" is PAGES PER TRACK (SMD: 18 sectors x 1024 B = 18432 B = 9 pages of 2048),
      // which is why @CREATE-DIRECTORY says the bit file starts "at a track boundary".
      //
      // The object/user base is APPROXIMATE — it comes from the CRDIR scan loop
      // (137173B-137352B) and has no clean closed form (empirically bit+216 on SMD,
      // bit+206 on SCSI, bit+202 on floppy). We place object/user clear of the bitmap's own
      // page span. Only bit-exactness with a SINTRAN-created image is affected; the reader
      // is pointer-driven and reads any placement correctly.
      const bitmapBytes = (Math.ceil(pages / 8) + 1) & ~1;
      const bitmapPages = Math.ceil(bitmapBytes / NDFS_PAGE_SIZE);

      const bitBlock = Math.floor(Math.floor(pages / 2) / 9) * 9;
      const objBlock = bitBlock + bitmapPages;
      const usrBlock = objBlock + 2;

      return {
        ndfsPages: pages,
        fileBlocks: pages,
        objectFileBlock: objBlock,
        userFileBlock: usrBlock,
        bitFileBlock: bitBlock,
        unreservedPages: isFloppy ? 1 : pages,
        isFloppy,
        includeExtendedInfo: !isFloppy,
      };
    }
    default:
      throw new Error(`Unknown template: ${template}`);
  }
}

/**
 * Calculate spare blocks for a given template.
 */
function getSpareBlocks(template: ImageTemplate, pages: number): number {
  switch (template) {
    case ImageTemplate.Smd75MB:
      return Math.floor(pages * 0.0394);
    default:
      return 0;
  }
}

/**
 * Create a new NDFS disk image from the given options.
 * Returns the raw image buffer.
 */
export function createNdfsImage(options: ImageCreationOptions): Uint8Array {
  const spec = getTemplateSpec(options.template, options.customPages);
  const dirName = (options.directoryName || 'NDFS').toUpperCase().substring(0, NDFS_NAME_MAX);
  const includeExt = options.includeExtendedInfo !== undefined
    ? options.includeExtendedInfo
    : spec.includeExtendedInfo;

  // Allocate zero-filled buffer
  const imageSize = spec.fileBlocks * NDFS_PAGE_SIZE;
  const image = new Uint8Array(imageSize);

  // ---- Page 0: Master block at offset 2016 ----
  const mbOff = MASTER_BLOCK_OFFSET;

  // Directory name
  writeNdfsName(image, mbOff, dirName, NDFS_NAME_MAX);

  // Object file pointer (Indexed)
  const objPtr = new BlockPointer(spec.objectFileBlock, PointerType.Indexed);
  objPtr.toBytes(image, mbOff + 0x10);

  // User file pointer (Indexed)
  const usrPtr = new BlockPointer(spec.userFileBlock, PointerType.Indexed);
  usrPtr.toBytes(image, mbOff + 0x14);

  // Bit file pointer (Contiguous)
  const bitPtr = new BlockPointer(spec.bitFileBlock, PointerType.Contiguous);
  bitPtr.toBytes(image, mbOff + 0x18);

  // Unreserved pages
  writeUint32BE(image, mbOff + 0x1c, spec.unreservedPages);

  // ---- Extended info at offset 2000 (hard disks only) ----
  if (includeExt) {
    const ext = EXTENDED_INFO_OFFSET;
    const sysNum = options.systemNumber || 0;
    const flagWord = options.flagWord || 0;
    const pagesAvailable = spec.ndfsPages;

    writeUint16BE(image, ext + 2, 0); // reserved1
    writeUint16BE(image, ext + 4, 0); // reserved2
    writeUint16BE(image, ext + 6, 0); // reserved3
    writeUint16BE(image, ext + 8, flagWord);
    writeUint16BE(image, ext + 10, sysNum);
    writeUint32BE(image, ext + 12, pagesAvailable);

    // Kernel-correct checksum: a 16-bit ADDITIVE sum of the seven words following the
    // checksum slot (reserved1-3 = 0, flag, system#, pages-hi, pages-lo). NOT XOR-then-add
    // — see MasterBlock.computeExtChecksum (WXDIR 37702B / CHDSI 37763B).
    const pagesLo = pagesAvailable & 0xffff;
    const pagesHi = (pagesAvailable >>> 16) & 0xffff;
    const checksum = MasterBlock.computeExtChecksum(0, 0, 0, flagWord, sysNum, pagesHi, pagesLo);
    writeUint16BE(image, ext, checksum);
  }

  // ---- Bitmap: mark system blocks as used ----
  const bitmapOff = spec.bitFileBlock * NDFS_PAGE_SIZE;

  // Helper to mark a block used in bitmap
  const markUsed = (blockId: number): void => {
    if (blockId >= spec.fileBlocks) return;
    // Must match BitFile.bitPosition: page N is bit N%16 of the 16-bit big-endian word
    // N/16, i.e. byte (N>>3)^1 on a byte array. This was a second, independent copy of
    // the arithmetic; when the bit order was corrected the copy would otherwise have
    // been left behind, marking a new image's reserved pages in one convention while
    // reading them back in the other.
    const byteIdx = (blockId >>> 3) ^ 1;
    const bitIdx = blockId & 7;
    image[bitmapOff + byteIdx] |= (1 << bitIdx);
  };

  // Mark page 0 (master block)
  markUsed(0);

  // Object file: index block + 1 data block
  markUsed(spec.objectFileBlock);
  const objDataBlock = spec.objectFileBlock + 1;
  markUsed(objDataBlock);

  // User file: index block + 1 data block
  markUsed(spec.userFileBlock);
  const usrDataBlock = spec.userFileBlock + 1;
  markUsed(usrDataBlock);

  // Bit file block
  markUsed(spec.bitFileBlock);

  // ---- Object file index block: one pointer to data block ----
  const objIdxOff = spec.objectFileBlock * NDFS_PAGE_SIZE;
  const objDataPtr = new BlockPointer(objDataBlock, PointerType.Contiguous);
  objDataPtr.toBytes(image, objIdxOff);

  // Object file data block is already zero (no files)

  // ---- User file index block: one pointer to data block ----
  const usrIdxOff = spec.userFileBlock * NDFS_PAGE_SIZE;
  const usrDataPtr = new BlockPointer(usrDataBlock, PointerType.Contiguous);
  usrDataPtr.toBytes(image, usrIdxOff);

  // ---- User file data block: create SYSTEM user (index 0) ----
  const usrDataOff = usrDataBlock * NDFS_PAGE_SIZE;
  image[usrDataOff + 0] = USER_ENTRY_FLAG; // 0x81 = valid user
  image[usrDataOff + 1] = 0; // enter count

  // User name "SYSTEM"
  writeNdfsName(image, usrDataOff + 2, 'SYSTEM', NDFS_NAME_MAX);

  // Password = 0
  writeUint16BE(image, usrDataOff + 18, 0);

  // Default quota: pages reserved
  const defaultQuota = Math.min(spec.ndfsPages, 1000);
  writeUint32BE(image, usrDataOff + 28, defaultQuota);

  // Pages used = 0
  writeUint32BE(image, usrDataOff + 32, 0);

  // Directory index = 0
  image[usrDataOff + 36] = 0;

  // User index = 0
  image[usrDataOff + 37] = 0;

  // Default file access = 0x04FF. Offset 40, NOT 38 -- offsets 38-39 are an
  // unused gap (see UserEntry.fromBytes/toBytes); writing to +38 landed the
  // value where nothing ever reads it, so a freshly created image's SYSTEM
  // user read defaultFileAccess as 0 and every new file fell through to the
  // restrictive ACCESS_DEFAULT (0x03FF, PUBLIC nothing) -- the exact
  // invisible-to-SYSTEM shape ACCESS_PUBLIC_READ exists to work around.
  writeUint16BE(image, usrDataOff + 40, 0x04ff);

  // Add optional extra users
  if (options.users) {
    for (let u = 0; u < options.users.length; u++) {
      const userOpt = options.users[u];
      const slotIndex = u + 1; // 0 is SYSTEM
      if (slotIndex >= ENTRIES_PER_PAGE) break;

      const entryOff = usrDataOff + slotIndex * ENTRY_SIZE;
      image[entryOff] = USER_ENTRY_FLAG;
      writeNdfsName(image, entryOff + 2, userOpt.name.toUpperCase(), NDFS_NAME_MAX);
      writeUint16BE(image, entryOff + 18, 0); // no password
      writeUint32BE(image, entryOff + 28, userOpt.reservedPages);
      writeUint32BE(image, entryOff + 32, 0);
      image[entryOff + 36] = 0;
      image[entryOff + 37] = slotIndex;
      writeUint16BE(image, entryOff + 40, 0x04ff); // offset 40, not 38 -- see SYSTEM's entry above
    }
  }

  return image;
}
