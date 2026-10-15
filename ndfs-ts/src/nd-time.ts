/**
 * ND-100 timestamp format: 32-bit packed date/time.
 *
 * Bits 31-26: year (0-63, add 1950 for calendar year)
 * Bits 25-22: month (1-12)
 * Bits 21-17: day (1-31)
 * Bits 16-12: hour (0-23)
 * Bits 11-6:  minute (0-59)
 * Bits 5-0:   second (0-59)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

/** ND epoch year. */
const ND_EPOCH = 1950;

/**
 * Unpack a 32-bit ND timestamp to a Date, or null if the value is 0.
 */
export function ndTimeToDate(value: number): Date | null {
  if (value === 0) return null;

  const year = ((value >>> 26) & 0x3f) + ND_EPOCH;
  const month = (value >>> 22) & 0x0f;
  const day = (value >>> 17) & 0x1f;
  const hour = (value >>> 12) & 0x1f;
  const minute = (value >>> 6) & 0x3f;
  const second = value & 0x3f;

  return new Date(year, month - 1, day, hour, minute, second);
}

/**
 * Pack a Date into a 32-bit ND timestamp.
 * Returns 0 for null input.
 */
export function dateToNdTime(date: Date | null): number {
  if (date === null) return 0;

  const year = date.getFullYear() - ND_EPOCH;
  if (year < 0 || year > 63) return 0;

  return (
    (((year & 0x3f) << 26) |
      (((date.getMonth() + 1) & 0x0f) << 22) |
      ((date.getDate() & 0x1f) << 17) |
      ((date.getHours() & 0x1f) << 12) |
      ((date.getMinutes() & 0x3f) << 6) |
      (date.getSeconds() & 0x3f)) >>>
    0
  );
}
