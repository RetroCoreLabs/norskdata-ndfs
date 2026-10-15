/**
 * NDFS block pointer implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/block_pointer.h>
#include "endian_util.h"

ndfs_block_pointer_t ndfs_bp_from_native(uint32_t value)
{
    ndfs_block_pointer_t bp;
    bp.type     = (ndfs_pointer_type_t)((value >> 30) & 0x03);
    bp.block_id = value & 0x3FFFFFFFU;
    return bp;
}

uint32_t ndfs_bp_to_native(const ndfs_block_pointer_t *bp)
{
    return (((uint32_t)(bp->type & 0x03) << 30) |
            (bp->block_id & 0x3FFFFFFFU));
}

ndfs_block_pointer_t ndfs_bp_from_bytes(const uint8_t *data, size_t offset)
{
    uint32_t raw = ndfs_read_u32be(data, offset);
    return ndfs_bp_from_native(raw);
}

void ndfs_bp_to_bytes(const ndfs_block_pointer_t *bp, uint8_t *data, size_t offset)
{
    ndfs_write_u32be(data, offset, ndfs_bp_to_native(bp));
}

bool ndfs_bp_is_valid(const ndfs_block_pointer_t *bp)
{
    return bp->block_id > 0 && bp->type != NDFS_PTR_RESERVED;
}
