/**
 * NDFS block pointer: 32-bit value with type in top 2 bits,
 * block ID in bottom 30 bits.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_BLOCK_POINTER_H
#define NDFS_BLOCK_POINTER_H

#include "types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 4-byte block pointer: 2-bit type + 30-bit block ID. */
typedef struct {
    uint32_t            block_id; /**< 30-bit page/block address. */
    ndfs_pointer_type_t type;     /**< 2-bit pointer type.        */
} ndfs_block_pointer_t;

/** Create a block pointer from a 32-bit native (big-endian decoded) value. */
ndfs_block_pointer_t ndfs_bp_from_native(uint32_t value);

/** Get the 32-bit native representation. */
uint32_t ndfs_bp_to_native(const ndfs_block_pointer_t *bp);

/** Create from big-endian bytes at offset. */
ndfs_block_pointer_t ndfs_bp_from_bytes(const uint8_t *data, size_t offset);

/** Serialize to big-endian bytes at offset. */
void ndfs_bp_to_bytes(const ndfs_block_pointer_t *bp, uint8_t *data, size_t offset);

/** Check if this pointer is valid (non-zero blockId, non-reserved type). */
bool ndfs_bp_is_valid(const ndfs_block_pointer_t *bp);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_BLOCK_POINTER_H */
