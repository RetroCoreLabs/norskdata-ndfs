/**
 * Internal big-endian read/write helpers.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_ENDIAN_UTIL_H
#define NDFS_ENDIAN_UTIL_H

#include <stdint.h>
#include <stddef.h>

static inline uint16_t ndfs_read_u16be(const uint8_t *data, size_t offset)
{
    return (uint16_t)((data[offset] << 8) | data[offset + 1]);
}

static inline uint32_t ndfs_read_u32be(const uint8_t *data, size_t offset)
{
    return ((uint32_t)data[offset]     << 24) |
           ((uint32_t)data[offset + 1] << 16) |
           ((uint32_t)data[offset + 2] <<  8) |
           ((uint32_t)data[offset + 3]);
}

static inline void ndfs_write_u16be(uint8_t *data, size_t offset, uint16_t value)
{
    data[offset]     = (uint8_t)((value >> 8) & 0xFF);
    data[offset + 1] = (uint8_t)(value & 0xFF);
}

static inline void ndfs_write_u32be(uint8_t *data, size_t offset, uint32_t value)
{
    data[offset]     = (uint8_t)((value >> 24) & 0xFF);
    data[offset + 1] = (uint8_t)((value >> 16) & 0xFF);
    data[offset + 2] = (uint8_t)((value >>  8) & 0xFF);
    data[offset + 3] = (uint8_t)(value & 0xFF);
}

#endif /* NDFS_ENDIAN_UTIL_H */
