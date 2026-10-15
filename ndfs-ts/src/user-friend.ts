/**
 * NDFS user friend entry: 16-bit packed permission value.
 *
 * Bit 15:   Entry used
 * Bits 14-13: Reserved
 * Bit 12:   Directory access
 * Bit 11:   Common access
 * Bit 10:   Append access
 * Bit 9:    Write access
 * Bit 8:    Read access
 * Bits 7-0: Friend user index (0-255)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { readUint16BE, writeUint16BE } from './endian.js';

export class UserFriend {
  bits: number;

  constructor(bits: number = 0) {
    this.bits = bits & 0xffff;
  }

  /** Create with explicit permissions. */
  static create(
    friendUserId: number,
    read: boolean = false,
    write: boolean = false,
    append: boolean = false,
    common: boolean = false,
    directory: boolean = false,
  ): UserFriend {
    let bits = (friendUserId & 0xff) | (1 << 15); // mark used
    if (read) bits |= 1 << 8;
    if (write) bits |= 1 << 9;
    if (append) bits |= 1 << 10;
    if (common) bits |= 1 << 11;
    if (directory) bits |= 1 << 12;
    return new UserFriend(bits);
  }

  /** Parse from big-endian bytes. */
  static fromBytes(data: Uint8Array, offset: number): UserFriend {
    return new UserFriend(readUint16BE(data, offset));
  }

  get entryUsed(): boolean {
    return (this.bits & (1 << 15)) !== 0;
  }
  get directoryAccess(): boolean {
    return (this.bits & (1 << 12)) !== 0;
  }
  get commonAccess(): boolean {
    return (this.bits & (1 << 11)) !== 0;
  }
  get appendAccess(): boolean {
    return (this.bits & (1 << 10)) !== 0;
  }
  get writeAccess(): boolean {
    return (this.bits & (1 << 9)) !== 0;
  }
  get readAccess(): boolean {
    return (this.bits & (1 << 8)) !== 0;
  }
  get friendUserIndex(): number {
    return this.bits & 0xff;
  }

  /** Set friend with permission bits. */
  setFriend(friendUserId: number, permissions: number): void {
    this.bits = (friendUserId & 0xff) | (1 << 15) | ((permissions & 0x1f) << 8);
  }

  /**
   * Parse a permission letters string into the 5-bit value used by setFriend:
   * R=read, W=write, A=append, C=common, D=directory. '-' and spaces are
   * ignored; null/empty yields 0. Throws on an unrecognised letter.
   */
  static parsePermissions(s: string | null | undefined): number {
    if (!s) return 0;
    let bits = 0;
    const table: Record<string, number> = { R: 0x01, W: 0x02, A: 0x04, C: 0x08, D: 0x10 };
    for (const ch of s) {
      if (ch === '-' || ch === ' ') continue;
      const up = ch.toUpperCase();
      if (!(up in table)) throw new Error(`Invalid permission letter: '${ch}'`);
      bits |= table[up];
    }
    return bits;
  }

  /** Clear this friend slot. */
  clear(): void {
    this.bits = 0;
  }

  /** Write to big-endian bytes. */
  toBytes(data: Uint8Array, offset: number): void {
    writeUint16BE(data, offset, this.bits);
  }

  getPermissionString(): string {
    if (!this.entryUsed) return '-----';
    return (
      (this.readAccess ? 'R' : '-') +
      (this.writeAccess ? 'W' : '-') +
      (this.appendAccess ? 'A' : '-') +
      (this.commonAccess ? 'C' : '-') +
      (this.directoryAccess ? 'D' : '-')
    );
  }

  toString(): string {
    if (!this.entryUsed) return '[Empty]';
    return `Friend[${this.friendUserIndex}] ${this.getPermissionString()}`;
  }
}
