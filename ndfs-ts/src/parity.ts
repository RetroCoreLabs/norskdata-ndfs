/**
 * ND-100 even parity helpers.
 *
 * The ND-100 uses even parity on text files: bit 7 is set so that the
 * total number of 1-bits in each byte is even. This applies to text file
 * types (:MODE, :SYMB, :TEXT, :C, etc.) but NOT binary files (:PROG, :BPUN).
 *
 * Example:
 *   'H' = 0x48 (01001000) -> 2 ones (even) -> bit 7 = 0 -> 0x48
 *   's' = 0x73 (01110011) -> 5 ones (odd)  -> bit 7 = 1 -> 0xF3
 *   ' ' = 0x20 (00100000) -> 1 one  (odd)  -> bit 7 = 1 -> 0xA0
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

/** Text file types where parity applies. */
const TEXT_TYPES = new Set([
  'MODE', 'SYMB', 'TEXT', 'C', 'BATC', 'OUT',
  'LOG', 'LIST', 'FADM', 'BASM', 'FORT', 'NPL',
  'COBO', 'PASC', 'PLAN', 'BAS', 'MAC', 'EDIT',
]);

/** Count 1-bits in the low 7 bits. */
function popcount7(b: number): number {
  let v = b & 0x7f;
  let count = 0;
  while (v) {
    count += v & 1;
    v >>>= 1;
  }
  return count;
}

/**
 * Strip even parity: clear bit 7 on every byte.
 * Converts ND-100 text (with parity) to standard 7-bit ASCII.
 */
export function stripParity(data: Uint8Array): Uint8Array {
  const result = new Uint8Array(data.length);
  for (let i = 0; i < data.length; i++) {
    result[i] = data[i] & 0x7f;
  }
  return result;
}

/**
 * Set even parity: set bit 7 so total 1-bits per byte is even.
 * Converts standard ASCII text to ND-100 text format with even parity.
 */
export function setParity(data: Uint8Array): Uint8Array {
  const result = new Uint8Array(data.length);
  for (let i = 0; i < data.length; i++) {
    const lo7 = data[i] & 0x7f;
    const ones = popcount7(lo7);
    result[i] = ones % 2 !== 0 ? lo7 | 0x80 : lo7;
  }
  return result;
}

/**
 * Check if a file type is a text type (parity applies).
 */
export function isTextType(fileType: string): boolean {
  return TEXT_TYPES.has(fileType.toUpperCase());
}
