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
