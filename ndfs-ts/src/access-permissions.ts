/**
 * NDFS access permissions: 15-bit encoding with 3 tiers.
 *
 * Bits 14-10: Public permissions (5 bits)
 * Bits 9-5:   Friend permissions (5 bits)
 * Bits 4-0:   Own permissions (5 bits)
 *
 * Per tier (5 bits):
 *   Bit 4: Delete
 *   Bit 3: Execute/Common
 *   Bit 2: Append
 *   Bit 1: Write
 *   Bit 0: Read
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { FileAccessType } from './types.js';

/** Permission bit positions within a 5-bit tier. */
export const PERM_READ = 0;
export const PERM_WRITE = 1;
export const PERM_APPEND = 2;
export const PERM_EXECUTE = 3;
export const PERM_DELETE = 4;

export class AccessPermissions {
  private bits: number;

  constructor(accessBits: number = 0) {
    this.bits = accessBits & 0x7fff;
  }

  /** Create from separate tier values. */
  static fromTiers(ownPerms: number, friendPerms: number, publicPerms: number): AccessPermissions {
    const bits = ((publicPerms & 0x1f) << 10) | ((friendPerms & 0x1f) << 5) | (ownPerms & 0x1f);
    return new AccessPermissions(bits);
  }

  /** Default permissions: owner full, friends RW, public R. */
  static default(): AccessPermissions {
    return new AccessPermissions(0x4ff);
  }

  /** Owner-only full access. */
  static ownerOnly(): AccessPermissions {
    return AccessPermissions.fromTiers(0x1f, 0, 0);
  }

  /** Get raw bits. */
  get rawBits(): number {
    return this.bits;
  }

  /** Get permission bits for a specific tier. */
  private getTierBits(tier: FileAccessType): number {
    switch (tier) {
      case FileAccessType.Own:
        return this.bits & 0x1f;
      case FileAccessType.Friend:
        return (this.bits >>> 5) & 0x1f;
      case FileAccessType.Public:
        return (this.bits >>> 10) & 0x1f;
    }
  }

  /** Check a specific permission for a tier. */
  hasPermission(tier: FileAccessType, permBit: number): boolean {
    return (this.getTierBits(tier) & (1 << permBit)) !== 0;
  }

  canRead(tier: FileAccessType): boolean {
    return this.hasPermission(tier, PERM_READ);
  }
  canWrite(tier: FileAccessType): boolean {
    return this.hasPermission(tier, PERM_WRITE);
  }
  canAppend(tier: FileAccessType): boolean {
    return this.hasPermission(tier, PERM_APPEND);
  }
  canExecute(tier: FileAccessType): boolean {
    return this.hasPermission(tier, PERM_EXECUTE);
  }
  canDelete(tier: FileAccessType): boolean {
    return this.hasPermission(tier, PERM_DELETE);
  }

  /** Set a specific permission for a tier. */
  setPermission(tier: FileAccessType, permBit: number, value: boolean): void {
    const shift = tier === FileAccessType.Own ? 0 : tier === FileAccessType.Friend ? 5 : 10;
    const mask = 1 << (shift + permBit);
    if (value) {
      this.bits |= mask;
    } else {
      this.bits &= ~mask;
    }
  }

  /** Get permission string for a tier (e.g., "DXAWR"). */
  getPermissionString(tier: FileAccessType): string {
    const t = this.getTierBits(tier);
    return (
      ((t & (1 << PERM_DELETE)) !== 0 ? 'D' : '-') +
      ((t & (1 << PERM_EXECUTE)) !== 0 ? 'X' : '-') +
      ((t & (1 << PERM_APPEND)) !== 0 ? 'A' : '-') +
      ((t & (1 << PERM_WRITE)) !== 0 ? 'W' : '-') +
      ((t & (1 << PERM_READ)) !== 0 ? 'R' : '-')
    );
  }

  toString(): string {
    return (
      `Own:${this.getPermissionString(FileAccessType.Own)} ` +
      `Friend:${this.getPermissionString(FileAccessType.Friend)} ` +
      `Public:${this.getPermissionString(FileAccessType.Public)}`
    );
  }
}

// ── Access rights in SINTRAN's own form ─────────────────────────────
//
// SINTRAN writes access as one field per tier: "N" for no access, or any
// combination of R read, W write, A append, C common, D directory; "N" must
// stand alone (@SET-FILE-ACCESS rule 2, SINTRAN III Reference Manual
// ND-60.128.5). The commands that set all three tiers take them in the order
// PUBLIC, FRIEND, OWN, and an empty field means "no change":
//   @SET-FILE-ACCESS F-1:DATA N,N,RWACD
//   @SET-DEFAULT-FILE-ACCESS N,RWA,RWACD
// Mirrors ndfs_parse_access_rights / ndfs_apply_access_triple in ndfs-c.

const RIGHT_BITS: Record<string, number> = { R: 0x01, W: 0x02, A: 0x04, C: 0x08, D: 0x10 };

/** SINTRAN's field order PUBLIC, FRIEND, OWN as shifts in the 15-bit word. */
const TRIPLE_SHIFTS = [10, 5, 0];

/**
 * Parse one SINTRAN access field into a 5-bit tier value. Letters in any
 * order and case; "N" gives 0. Throws for an empty field, an unknown letter,
 * or N together with other letters.
 */
export function parseAccessRights(s: string): number {
  if (!s) throw new Error('empty access field');
  let rights = 0;
  let none = false;
  let letters = false;
  const up = s.toUpperCase();
  for (let i = 0; i < up.length; i++) {
    const ch = up[i];
    if (ch === 'N') {
      none = true;
    } else if (ch in RIGHT_BITS) {
      rights |= RIGHT_BITS[ch];
      letters = true;
    } else {
      throw new Error(`bad access letter '${ch}' in '${s}'`);
    }
  }
  if (none && letters) throw new Error(`N must stand alone: '${s}'`);
  return rights;
}

/**
 * Apply "<public>,<friend>,<own>" to a 15-bit access word. A missing or empty
 * field keeps that tier as it is in `old` ("N,,RWACD" changes PUBLIC and OWN
 * only). Spaces are ignored. Throws for a bad field or more than three.
 */
export function applyAccessTriple(old: number, triple: string): number {
  const fields = triple.replace(/ /g, '').split(',');
  if (fields.length > 3) throw new Error(`more than three access fields: '${triple}'`);
  let bits = old & 0x7fff;
  for (let k = 0; k < fields.length; k++) {
    if (fields[k]) {
      const shift = TRIPLE_SHIFTS[k];
      bits = (bits & ~(0x1f << shift)) | (parseAccessRights(fields[k]) << shift);
    }
  }
  return bits;
}
