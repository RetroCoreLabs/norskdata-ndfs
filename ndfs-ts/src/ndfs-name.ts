/**
 * NDFS name encoding: strings terminated by 0x27 (single quote).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { NDFS_NAME_TERMINATOR } from './constants.js';

/**
 * Read an NDFS name from raw bytes.
 * Names are 7-bit ASCII, terminated by 0x27 (') or 0x00, up to maxLen bytes.
 */
export function readNdfsName(
  data: Uint8Array,
  offset: number,
  maxLen: number,
): string {
  let end = 0;
  for (let i = 0; i < maxLen; i++) {
    const b = data[offset + i];
    if (b === NDFS_NAME_TERMINATOR || b === 0x00) break;
    end++;
  }
  let result = '';
  for (let i = 0; i < end; i++) {
    result += String.fromCharCode(data[offset + i]);
  }
  return result;
}

/**
 * Write an NDFS name into a byte buffer.
 * Pads with the terminator byte (0x27) after the name.
 */
export function writeNdfsName(
  data: Uint8Array,
  offset: number,
  name: string,
  maxLen: number,
): void {
  const upper = name.toUpperCase();
  const len = Math.min(upper.length, maxLen);
  for (let i = 0; i < len; i++) {
    data[offset + i] = upper.charCodeAt(i) & 0x7f;
  }
  // SINTRAN writes a single 0x27 terminator after the name (when it fits)
  // followed by NULs -- not a field full of terminators (which SINTRAN's
  // directory scan rejects). Zero-fill also clears a longer previous name.
  if (len < maxLen) {
    data[offset + len] = NDFS_NAME_TERMINATOR;
    for (let i = len + 1; i < maxLen; i++) {
      data[offset + i] = 0x00;
    }
  }
}
