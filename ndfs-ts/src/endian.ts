/**
 * Big-endian read/write helpers for NDFS binary data.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

/** Read a 16-bit unsigned integer in big-endian byte order. */
export function readUint16BE(data: Uint8Array, offset: number): number {
  return (data[offset] << 8) | data[offset + 1];
}

/** Read a 32-bit unsigned integer in big-endian byte order. */
export function readUint32BE(data: Uint8Array, offset: number): number {
  return (
    ((data[offset] << 24) |
      (data[offset + 1] << 16) |
      (data[offset + 2] << 8) |
      data[offset + 3]) >>>
    0
  );
}

/** Write a 16-bit unsigned integer in big-endian byte order. */
export function writeUint16BE(
  data: Uint8Array,
  offset: number,
  value: number,
): void {
  data[offset] = (value >>> 8) & 0xff;
  data[offset + 1] = value & 0xff;
}

/** Write a 32-bit unsigned integer in big-endian byte order. */
export function writeUint32BE(
  data: Uint8Array,
  offset: number,
  value: number,
): void {
  data[offset] = (value >>> 24) & 0xff;
  data[offset + 1] = (value >>> 16) & 0xff;
  data[offset + 2] = (value >>> 8) & 0xff;
  data[offset + 3] = value & 0xff;
}
