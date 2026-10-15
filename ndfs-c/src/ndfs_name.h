/**
 * Internal NDFS name read/write helpers.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_NAME_INTERNAL_H
#define NDFS_NAME_INTERNAL_H

#include <stdint.h>
#include <stddef.h>

/**
 * Read an NDFS name from raw bytes.
 * Names are 7-bit ASCII, terminated by 0x27 (') or 0x00, up to max_len bytes.
 * @param data     Source bytes.
 * @param offset   Byte offset.
 * @param max_len  Maximum name length in bytes.
 * @param out      Destination buffer (must be at least max_len+1).
 */
void ndfs_read_name(const uint8_t *data, size_t offset,
                    size_t max_len, char *out);

/**
 * Write an NDFS name into a byte buffer.
 * Uppercases the name and pads with 0x27 terminator.
 */
void ndfs_write_name(uint8_t *data, size_t offset,
                     const char *name, size_t max_len);

#endif /* NDFS_NAME_INTERNAL_H */
