/**
 * ND-100 timestamp format: 32-bit packed date/time, used for
 * ndfs_object_entry_t.date_created/last_read_date/last_write_date and the
 * equivalent UserEntry fields.
 *
 * Bits 31-26: year (0-63, add 1950 for calendar year; range 1950-2013)
 * Bits 25-22: month (1-12)
 * Bits 21-17: day (1-31)
 * Bits 16-12: hour (0-23; the source system encodes 24-31, which are
 *             clamped to 0 here rather than treated as invalid)
 * Bits 11-6:  minute (0-59)
 * Bits 5-0:   second (0-59)
 *
 * A raw value of 0 means "not set" (ndfs_nd_time_to_calendar returns false).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_ND_TIME_H
#define NDFS_ND_TIME_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Decoded ND-100 calendar date/time. */
typedef struct {
    unsigned year;   /**< 1950-2013. */
    unsigned month;  /**< 1-12. */
    unsigned day;    /**< 1-31. */
    unsigned hour;   /**< 0-23. */
    unsigned minute; /**< 0-59. */
    unsigned second; /**< 0-59. */
} ndfs_calendar_t;

/**
 * Decode a raw packed ND-100 timestamp into calendar fields.
 * @param raw  The packed 32-bit value.
 * @param out  Receives the decoded fields; untouched on failure.
 * @return true if raw is nonzero and decodes to a field combination that
 *         passes range validation (this does NOT check day-per-month, e.g.
 *         day 31 in April passes here); false for raw == 0 or an
 *         out-of-range field.
 */
bool ndfs_nd_time_to_calendar(uint32_t raw, ndfs_calendar_t *out);

/**
 * Encode calendar fields into a raw packed ND-100 timestamp.
 * @return the packed value, or 0 if year is outside 1950-2013.
 */
uint32_t ndfs_calendar_to_nd_time(const ndfs_calendar_t *cal);

/**
 * Format a raw packed ND-100 timestamp as "YYYY-MM-DD HH:MM:SS".
 * @param raw  The packed 32-bit value.
 * @param out  Destination buffer.
 * @param len  Size of out; must be at least 20 bytes.
 * @return true if raw decoded successfully and was formatted into out;
 *         false if raw == 0 or invalid, in which case out is set to
 *         "(not set)" instead.
 */
bool ndfs_nd_time_format(uint32_t raw, char *out, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_ND_TIME_H */
