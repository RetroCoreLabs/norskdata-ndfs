/**
 * SINTRAN disk types: the fixed SMD and Winchester sizes and the layout lengths.
 *
 * Names and sizes come from the device-name table (segment 101-S3DNAM) of the SINTRAN L and M carves:
 * each disk unit name carries one fixed 32-bit number, the directory size in pages. SCSI disks are not
 * in the table: SINTRAN sets their size from the drive. `layoutPages` is the length of the full SINTRAN
 * disk layout (cylinders x heads x sectors / 2) of the DT table entry whose comment names the type;
 * 0 = unknown.
 *
 * See docs/DISK-GEOMETRY-AND-BOOT-FINDINGS.md and docs/CREATE-DIRECTORY-PLACEMENT.md.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

/** Controller family of a SINTRAN disk type, from the generation source. */
export enum DiskFamily {
  /** Named in the manual only; not in the carved name table. */
  Unknown = 'unknown',
  /** ST506 style controller (IOX 500 family). */
  Winchester = 'winchester',
  /** Big disc controller (IOX 1540 family). */
  Smd = 'smd',
}

/** One SINTRAN disk type name and the directory size SINTRAN gives it. */
export interface DiskType {
  /** Name without unit number, for example "DISC-75MB". */
  readonly name: string;
  /** Directory size in pages (declared capacity); 0 = size not known. */
  readonly pages: number;
  readonly family: DiskFamily;
  /** Real SINTRAN M created a directory on it, or entered an ndfs image of it. */
  readonly tested: boolean;
  /** Drive model from the SINTRAN manual. */
  readonly drive: string;
  /** Pages in the full SINTRAN disk layout; 0 = not known. */
  readonly layoutPages: number;
}

export const DISK_TYPES: readonly DiskType[] = [
  { name: "DISC-14MB", pages: 6912, family: DiskFamily.Winchester, tested: true, drive: "CDC 9410 Finch", layoutPages: 6912 },
  { name: "DISC-16MB", pages: 8000, family: DiskFamily.Winchester, tested: false, drive: "Micropolis 1304 (ST506)", layoutPages: 0 },
  { name: "DISC-21MB", pages: 10728, family: DiskFamily.Winchester, tested: true, drive: "CDC 9410 Finch", layoutPages: 10890 },
  { name: "DISC-23MB", pages: 11016, family: DiskFamily.Winchester, tested: true, drive: "Micropolis 1304 (ST506)", layoutPages: 11205 },
  { name: "DISC-28MB", pages: 13648, family: DiskFamily.Winchester, tested: true, drive: "Micropolis 1325 (ST506)", layoutPages: 13797 },
  { name: "DISC-45MB", pages: 22032, family: DiskFamily.Winchester, tested: true, drive: "Micropolis 1304/1325 (ST506)", layoutPages: 22410 },
  { name: "DISC-74MB", pages: 36396, family: DiskFamily.Winchester, tested: true, drive: "Micropolis 1325 (ST506)", layoutPages: 36792 },
  { name: "DISC-30MB", pages: 7389, family: DiskFamily.Smd, tested: false, drive: "CDC 9448 CMD big cartridge", layoutPages: 0 },
  { name: "DISC-60MB", pages: 7389, family: DiskFamily.Smd, tested: false, drive: "CDC 9762 SMD (ND-IO)", layoutPages: 0 },
  { name: "DISC-90MB", pages: 7389, family: DiskFamily.Smd, tested: false, drive: "CDC 9448 CMD", layoutPages: 0 },
  { name: "DISC-38MB", pages: 18486, family: DiskFamily.Smd, tested: true, drive: "CDC 9762 SMD (ND-IO)", layoutPages: 18495 },
  { name: "DISC-70MB", pages: 34765, family: DiskFamily.Smd, tested: true, drive: "CDC 9710 RSD", layoutPages: 37035 },
  { name: "DISC-75MB", pages: 36945, family: DiskFamily.Smd, tested: true, drive: "CDC 9762 SMD", layoutPages: 37035 },
  { name: "DISC-2-75MB", pages: 36945, family: DiskFamily.Smd, tested: false, drive: "CDC 9730 MMD (150 MB, sub-divided)", layoutPages: 0 },
  { name: "DISC-3-75MB", pages: 36945, family: DiskFamily.Smd, tested: false, drive: "CDC 9766 SMD (225 MB, sub-divided)", layoutPages: 0 },
  { name: "DISC-140MB-F", pages: 69530, family: DiskFamily.Smd, tested: false, drive: "Fujitsu M2322K", layoutPages: 0 },
  { name: "DISC-2-70MB-F", pages: 34765, family: DiskFamily.Smd, tested: false, drive: "Fujitsu M2322K (140 MB, sub-divided)", layoutPages: 0 },
  { name: "DISC-225MB-R", pages: 110292, family: DiskFamily.Smd, tested: false, drive: "CDC 9766 SMD", layoutPages: 0 },
  { name: "DISC-225MB-F", pages: 110292, family: DiskFamily.Smd, tested: false, drive: "not named in the manual", layoutPages: 0 },
  { name: "DISC-225MB-E", pages: 110292, family: DiskFamily.Smd, tested: false, drive: "not named in the manual", layoutPages: 0 },
  { name: "DISC-225MB-N", pages: 110292, family: DiskFamily.Smd, tested: false, drive: "not named in the manual", layoutPages: 0 },
  { name: "DISC-288MB-R", pages: 140391, family: DiskFamily.Smd, tested: true, drive: "CDC 9766 SMD", layoutPages: 140733 },
  { name: "DISC-288MB-F", pages: 140391, family: DiskFamily.Smd, tested: true, drive: "CDC 9715 FSD 340", layoutPages: 153576 },
  { name: "DISC-288MB-E", pages: 140391, family: DiskFamily.Smd, tested: false, drive: "CDC 9720 EMD 368", layoutPages: 158210 },
  { name: "DISC-288MB-N", pages: 140391, family: DiskFamily.Smd, tested: false, drive: "not named in the manual", layoutPages: 144025 },
  { name: "DISC-4-70MB-R", pages: 34765, family: DiskFamily.Smd, tested: false, drive: "CDC 9766 SMD (280 MB, sub-divided)", layoutPages: 0 },
  { name: "DISC-4-70MB-F", pages: 34765, family: DiskFamily.Smd, tested: false, drive: "CDC 9715 FSD 340 (280 MB, sub-divided)", layoutPages: 0 },
  { name: "DISC-4-70MB-E", pages: 34765, family: DiskFamily.Smd, tested: false, drive: "CDC 9720 EMD 368 (280 MB, sub-divided)", layoutPages: 0 },
  { name: "DISC-4-70MB-N", pages: 34765, family: DiskFamily.Smd, tested: false, drive: "not named in the manual", layoutPages: 0 },
  { name: "DISC-6-70MB-F", pages: 34765, family: DiskFamily.Smd, tested: false, drive: "CDC 9715 FSD 515 (450 MB, sub-divided)", layoutPages: 0 },
  { name: "DISC-6-70MB-N", pages: 34765, family: DiskFamily.Smd, tested: false, drive: "not named in the manual", layoutPages: 0 },
  { name: "DISC-450MB-F", pages: 220584, family: DiskFamily.Smd, tested: true, drive: "CDC 9715 FSD 515", layoutPages: 230364 },
  { name: "DISC-450MB-N", pages: 220584, family: DiskFamily.Smd, tested: true, drive: "not named in the manual", layoutPages: 223820 },
  { name: "DISC-10MB", pages: 0, family: DiskFamily.Unknown, tested: false, drive: "CDC 9427 Hawk cartridge (manual only)", layoutPages: 0 },
  { name: "DISC-33MB", pages: 0, family: DiskFamily.Unknown, tested: false, drive: "CDC 9762 SMD, ND-IO (manual only)", layoutPages: 0 },
  { name: "DISC-50MB", pages: 0, family: DiskFamily.Unknown, tested: false, drive: "CDC 9448 CMD (manual only)", layoutPages: 0 },
  { name: "DISC-2-225MB-F", pages: 0, family: DiskFamily.Unknown, tested: false, drive: "CDC 9715 FSD 515 (450 MB, sub-divided; manual only)", layoutPages: 0 },
  { name: "DISC-2-225MB-E", pages: 0, family: DiskFamily.Unknown, tested: false, drive: "NEC D3525-H (450 MB, sub-divided; manual only)", layoutPages: 0 },
  { name: "DISC-6-70MB-E", pages: 0, family: DiskFamily.Unknown, tested: false, drive: "NEC D3525-H (450 MB, sub-divided; manual only)", layoutPages: 0 },
  { name: "DISC-450MB-E", pages: 0, family: DiskFamily.Unknown, tested: false, drive: "NEC D3525-H (manual only)", layoutPages: 0 },
];

export function diskTypeCount(): number {
  return DISK_TYPES.length;
}

/** Disk type number `index`, or undefined if out of range. */
export function getDiskType(index: number): DiskType | undefined {
  return DISK_TYPES[index];
}

/** Look a disk type up by name (case-insensitive, for example "disc-75mb"). */
export function findDiskType(name: string): DiskType | undefined {
  const upper = name.toUpperCase();
  return DISK_TYPES.find((t) => t.name === upper);
}
