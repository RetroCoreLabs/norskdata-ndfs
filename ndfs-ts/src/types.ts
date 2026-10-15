/**
 * Public type definitions for the NDFS library.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

/** Block pointer type encoded in the top 2 bits. */
export enum PointerType {
  Contiguous = 0,
  Indexed = 1,
  SubIndexed = 2,
  Reserved = 3,
}

/** Boot code format detected in page 0. */
export enum BootFormat {
  None = 'none',
  Binary = 'binary',
  BPUN = 'bpun',
  FloMon = 'flomon',
}

/**
 * Hard-disk controller family identified from the IOX/IOXT instructions in a
 * raw-binary bootstrap. Only meaningful when BootFormat is Binary; BPUN/FloMon
 * boots are always floppy media and are not classified this way.
 */
export enum BootControllerType {
  Unknown = 'unknown',
  SmdEcc = 'smd_ecc',
  Winchester = 'winchester',
  Scsi = 'scsi',
  Floppy = 'floppy',
}

/** Access level for a user relative to a file. */
export enum FileAccessType {
  Own = 0,
  Friend = 1,
  Public = 2,
}

/** File operation types for permission checks. */
export enum FileOperationType {
  Read = 0,
  Write = 1,
  Append = 2,
  Execute = 3,
  Delete = 4,
  List = 5,
}

/** Checksum validation state for extended info. */
/**
 * Extended-info checksum state.
 *
 * NOTE: there is deliberately no `ValidLowByteOnly`. The SINTRAN kernel writes and
 * compares the FULL 16-bit checksum (writer WXDIR 37702B, validator CHDSI 37763B);
 * accepting a low-byte-only match was a reader-side heuristic SINTRAN never produces.
 */
export enum ChecksumValidation {
  Valid = 'valid',
  Invalid = 'invalid',
}

/** Disk image template for image creation. */
export enum ImageTemplate {
  Floppy360KB = 'floppy_360kb',
  Floppy12MB = 'floppy_12mb',
  Smd75MB = 'smd_75mb',
  Winchester74MB = 'winchester_74mb',
  Custom = 'custom',
}

/** File type flags (bit field). */
export enum FileTypeFlags {
  None = 0,
  TerminalFile = 1 << 0,
  PeripheralFile = 1 << 1,
  SpoolingFile = 1 << 2,
  IndexedFile = 1 << 3,
  ContiguousFile = 1 << 4,
  AllocatedFile = 1 << 5,
  MagneticTapeFile = 1 << 6,
  LibraryFile = 1 << 7,
}

/** A file/directory entry returned by listDirectory(). */
export interface FileEntry {
  name: string;
  type: string;
  fullName: string;
  userName: string;
  size: number;
  pages: number;
  isDirectory: boolean;
  lastModified: Date | null;
}

/** Boot code extracted from page 0. */
export interface BootCode {
  format: BootFormat;
  controllerType: BootControllerType;
  startAddress: number;
  bootAddress: number;
  loadAddress: number;
  wordCount: number;
  data: Uint8Array;
  checksumValid: boolean;
}

/** Options for creating a new NDFS disk image. */
export interface ImageCreationOptions {
  template: ImageTemplate;
  directoryName?: string;
  customPages?: number;
  includeExtendedInfo?: boolean;
  systemNumber?: number;
  flagWord?: number;
  users?: Array<{ name: string; reservedPages: number }>;
}
