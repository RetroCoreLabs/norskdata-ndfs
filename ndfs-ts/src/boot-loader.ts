/**
 * NDFS boot loader detection and extraction.
 *
 * Boot formats:
 *   - FLOMON: '!' delimiter followed by address=0 and count=0
 *   - BPUN: '!' delimiter followed by valid address and count, then data + checksum
 *   - Binary: Non-zero/non-uniform data in first 1024 bytes without '!' delimiter
 *   - None: Empty or uniform page 0
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { NDFS_PAGE_SIZE, MASTER_BLOCK_OFFSET } from './constants.js';
import { readUint16BE } from './endian.js';
import { BootFormat, BootControllerType, BootCode } from './types.js';

/** Maximum scan range for '!' delimiter (stay within boot area, before master block). */
const BOOT_SCAN_LIMIT = Math.min(1024, MASTER_BLOCK_OFFSET);

// ND-100 CPU opcodes (16-bit, big-endian on disk). A real bootstrap always begins by
// disabling interrupts (and usually paging) before touching hardware - these are the
// ONLY two words a genuine boot sector can start with. Verified against the assembler
// source (norsk_data/nd100-as/instr.h: PIOF=0150405, IOF=0150401) and cross-checked
// against nd100x/asm/DISK-IMAGE-INVENTORY.md, which uses the same two values to
// classify real disk images.
const OPCODE_PIOF = 0xd105; // octal 150405 - disable interrupts + paging
const OPCODE_IOF = 0xd101; // octal 150401 - disable interrupts only

// Literal IOX instruction word = base opcode 0164000 (octal) OR'd with an 11-bit device
// address in bits 10-0 (norsk_data/nd100-as/instr.h: OPC("iox",A_IOX,0164000)). IOXT
// (device address taken from the T register at runtime - used by SCSI/NCR-5386) is the
// single fixed word 0150415 octal; no device address is visible in the instruction
// stream for it.
const IOX_OPCODE_BASE = 0xe800; // octal 164000
const IOX_OPCODE_MASK = 0xf800; // top 5 bits select the IOX instruction class
const IOX_DEVICE_MASK = 0x07ff; // low 11 bits carry the literal device address
const IOXT_OPCODE = 0xd10d; // octal 150415 (SCSI/NCR-5386, indirect)

// Hard-disk controller IOX device bases (octal, converted to decimal), each covering an
// 8-word register window. Source: nd100-dis/DEVICE-BASES.md (thumbwheel-selectable base
// addresses extracted from the RetroCore NDBus controller drivers).
const SMD_ECC_BASES = [0x360, 0x368, 0x160, 0x168]; // 1540, 1550, 540, 550 octal
const WINCHESTER_BASES = [0x140, 0x148]; // 500, 510 octal
const FLOPPY_BASES = [0x370, 0x378]; // 1560, 1570 octal

/** Checks whether a 16-bit word is the genuine ND-100 bootstrap prologue. */
function isPrologueOpcode(word: number): boolean {
  return word === OPCODE_PIOF || word === OPCODE_IOF;
}

/** Checks whether a device address falls within any of the given controllers'
 *  8-word register windows (base .. base+7). */
function isInDeviceWindow(address: number, bases: number[]): boolean {
  for (const base of bases) {
    if (address >= base && address <= base + 7) return true;
  }
  return false;
}

/**
 * Scans page 0 for a literal IOX (SMD/ECC, Winchester, Floppy) or indirect IOXT
 * (SCSI/NCR-5386) instruction to classify which hard-disk controller the bootstrap
 * targets. Only meaningful once isPrologueOpcode() has already confirmed the page
 * is a genuine bootstrap. (Exported as detectControllerType() below.)
 */
function scanControllerType(page0: Uint8Array): BootControllerType {
  const wordCount = Math.floor(page0.length / 2);

  for (let i = 0; i < wordCount; i++) {
    const word = readUint16BE(page0, i * 2);

    if (word === IOXT_OPCODE) {
      return BootControllerType.Scsi;
    }

    if ((word & IOX_OPCODE_MASK) === IOX_OPCODE_BASE) {
      const deviceAddress = word & IOX_DEVICE_MASK;

      if (isInDeviceWindow(deviceAddress, SMD_ECC_BASES)) return BootControllerType.SmdEcc;
      if (isInDeviceWindow(deviceAddress, WINCHESTER_BASES)) return BootControllerType.Winchester;
      if (isInDeviceWindow(deviceAddress, FLOPPY_BASES)) return BootControllerType.Floppy;
    }
  }

  return BootControllerType.Unknown;
}

/**
 * Detect the boot format of an NDFS page 0.
 */
export function detectBootFormat(page0: Uint8Array): BootFormat {
  if (page0.length < NDFS_PAGE_SIZE) return BootFormat.None;

  // Scan for '!' (0x21) delimiter in boot area
  let exclamationPos = -1;
  for (let i = 0; i < BOOT_SCAN_LIMIT; i++) {
    if (page0[i] === 0x21) {
      exclamationPos = i;
      break;
    }
  }

  if (exclamationPos >= 0) {
    const afterExcl = exclamationPos + 1;
    if (afterExcl + 4 <= page0.length) {
      const address = readUint16BE(page0, afterExcl);
      const count = readUint16BE(page0, afterExcl + 2);

      if (address === 0 && count === 0) {
        return BootFormat.FloMon;
      }

      // Valid address and count -> BPUN
      if (count > 0) {
        return BootFormat.BPUN;
      }
    }
  }

  // Check for a raw hard-disk bootstrap (SMD/ECC, Winchester, SCSI - not BPUN/FloMon).
  // Unlike BPUN/FloMon, raw bootstraps carry no ASCII preamble or delimiter, so the
  // only reliable signature is the CPU opcode itself: real boot code always starts
  // with the PIOF/IOF prologue. Anything else - including data that merely "looks
  // non-uniform" - is not bootable.
  if (isPrologueOpcode(readUint16BE(page0, 0))) {
    return BootFormat.Binary;
  }

  return BootFormat.None;
}

/**
 * Detect the hard-disk controller family (SMD/ECC, Winchester, SCSI) targeted by a
 * raw-binary bootstrap. Returns BootControllerType.Unknown for BPUN/FloMon (floppy)
 * boots or non-bootable disks.
 */
export function detectControllerType(page0: Uint8Array): BootControllerType {
  const boot = loadBootCode(page0);
  return boot?.controllerType ?? BootControllerType.Unknown;
}

/**
 * Load boot code from an NDFS page 0.
 * Returns null if no boot format is detected.
 */
export function loadBootCode(page0: Uint8Array): BootCode | null {
  const format = detectBootFormat(page0);
  if (format === BootFormat.None) return null;

  if (format === BootFormat.Binary) {
    // Extract the first 1024 bytes as raw binary boot code
    const data = new Uint8Array(BOOT_SCAN_LIMIT);
    data.set(page0.subarray(0, BOOT_SCAN_LIMIT));
    return {
      format,
      controllerType: scanControllerType(page0),
      startAddress: 0,
      bootAddress: 0,
      loadAddress: 0,
      wordCount: 0,
      data,
      checksumValid: false,
    };
  }

  // Find '!' delimiter
  let exclamationPos = -1;
  for (let i = 0; i < BOOT_SCAN_LIMIT; i++) {
    if (page0[i] === 0x21) {
      exclamationPos = i;
      break;
    }
  }

  if (exclamationPos < 0) return null;

  const afterExcl = exclamationPos + 1;
  const address = readUint16BE(page0, afterExcl);
  const count = readUint16BE(page0, afterExcl + 2);

  if (format === BootFormat.FloMon) {
    // FLOMON: data before '!'
    const data = new Uint8Array(exclamationPos);
    data.set(page0.subarray(0, exclamationPos));
    return {
      format,
      controllerType: BootControllerType.Unknown,
      startAddress: 0,
      bootAddress: 0,
      loadAddress: 0,
      wordCount: 0,
      data,
      checksumValid: true,
    };
  }

  // BPUN: address(2) + count(2) + data(count*2) + checksum(2) + action(2)
  const dataStart = afterExcl + 4;
  const dataByteCount = count * 2;
  const checksumOffset = dataStart + dataByteCount;

  // Verify we have enough data
  if (checksumOffset + 2 > MASTER_BLOCK_OFFSET) {
    // Truncated BPUN - extract what we can
    const available = Math.min(dataByteCount, MASTER_BLOCK_OFFSET - dataStart);
    const data = new Uint8Array(available);
    data.set(page0.subarray(dataStart, dataStart + available));
    return {
      format,
      controllerType: BootControllerType.Unknown,
      startAddress: address,
      bootAddress: address,
      loadAddress: address,
      wordCount: count,
      data,
      checksumValid: false,
    };
  }

  const data = new Uint8Array(dataByteCount);
  data.set(page0.subarray(dataStart, dataStart + dataByteCount));

  // Validate checksum: sum of all words (address + count + data words) should match
  let checksum = address + count;
  for (let i = 0; i < count; i++) {
    checksum += readUint16BE(page0, dataStart + i * 2);
  }
  checksum = checksum & 0xffff;

  const storedChecksum = readUint16BE(page0, checksumOffset);
  const checksumValid = (checksum === storedChecksum);

  // Action byte (after checksum)
  let bootAddress = address;
  if (checksumOffset + 4 <= page0.length) {
    const action = readUint16BE(page0, checksumOffset + 2);
    if (action > 0) bootAddress = action;
  }

  return {
    format,
    controllerType: BootControllerType.Unknown,
    startAddress: address,
    bootAddress,
    loadAddress: address,
    wordCount: count,
    data,
    checksumValid,
  };
}
