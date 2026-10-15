/**
 * NDFS block pointer: 32-bit value with type in top 2 bits, block ID in bottom 30 bits.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { PointerType } from './types.js';
import { readUint32BE, writeUint32BE } from './endian.js';

export class BlockPointer {
  /** 30-bit page/block address. */
  blockId: number;

  /** 2-bit pointer type. */
  type: PointerType;

  constructor(blockId: number = 0, type: PointerType = PointerType.Contiguous) {
    this.blockId = blockId & 0x3fffffff;
    this.type = type;
  }

  /** Create from a 32-bit native value. */
  static fromNative(value: number): BlockPointer {
    const type = (value >>> 30) as PointerType;
    const blockId = value & 0x3fffffff;
    return new BlockPointer(blockId, type);
  }

  /** Create from big-endian bytes at offset. */
  static fromBytes(data: Uint8Array, offset: number): BlockPointer {
    return BlockPointer.fromNative(readUint32BE(data, offset));
  }

  /** Get the 32-bit native representation. */
  get native(): number {
    return (((this.type & 0x03) << 30) | (this.blockId & 0x3fffffff)) >>> 0;
  }

  /** Check if this pointer is valid (non-zero blockId, non-reserved type). */
  isValid(): boolean {
    return this.blockId > 0 && this.type !== PointerType.Reserved;
  }

  /** Serialize to big-endian bytes at offset. */
  toBytes(data: Uint8Array, offset: number): void {
    writeUint32BE(data, offset, this.native);
  }

  /** Serialize to a new 4-byte array. */
  toBytesArray(): Uint8Array {
    const buf = new Uint8Array(4);
    this.toBytes(buf, 0);
    return buf;
  }

  toString(): string {
    return `${this.blockId} (${PointerType[this.type]})`;
  }
}
