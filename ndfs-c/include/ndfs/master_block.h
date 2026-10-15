/**
 * NDFS master block: the 32-byte structure at offset 2016 of page 0.
 * Also handles the 16-byte extended info block at offset 2000.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_MASTER_BLOCK_H
#define NDFS_MASTER_BLOCK_H

#include "block_pointer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Parsed master block + extended info from page 0. */
typedef struct {
    char                       directory_name[NDFS_NAME_MAX + 1];
    ndfs_block_pointer_t       object_file_ptr;
    ndfs_block_pointer_t       user_file_ptr;
    ndfs_block_pointer_t       bit_file_ptr;
    uint32_t                   unreserved_pages;
    uint32_t                   image_size; /**< total pages */

    /* Extended info fields */
    uint16_t                   ext_checksum;
    uint16_t                   ext_reserved1;
    uint16_t                   ext_reserved2;
    uint16_t                   ext_reserved3;
    uint16_t                   ext_flag_word;
    uint16_t                   ext_last_system_number;
    uint32_t                   ext_pages_available;
    uint16_t                   ext_calculated_checksum;
    bool                       ext_valid;
    ndfs_checksum_validation_t checksum_state;
    bool                       has_flomon;
} ndfs_master_block_t;

/**
 * Compute the extended-info checksum (word 1750B) exactly as the SINTRAN kernel does:
 * a plain 16-bit ADDITIVE SUM of the seven words that FOLLOW it (words 1751B-1757B),
 * truncated to 16 bits.
 *
 * Kernel-proven from both sides of the carved 006-S3FS segment: the writer WXDIR
 * (37702B) and the enter-directory validator CHDSI (37763B) run the identical
 * `ADD ,X 0` accumulation loop over these words. There is no XOR, and the system number
 * is simply one of the seven summed words - NOT added separately.
 *
 * (The previous "XOR six words, then add system_number" model reproduced the PACK-ONE
 * sample only by coincidence and is wrong; see master_block.c.)
 */
uint16_t ndfs_mb_ext_checksum(uint16_t reserved1, uint16_t reserved2, uint16_t reserved3,
                              uint16_t flag_word, uint16_t system_number,
                              uint16_t pages_hi, uint16_t pages_lo);

/** Extended-info flag word (1754B) bit 15: "directory entered / in use".
 *  Set by the kernel on enter (CHDSI 37763B, BSET ONE 170) and cleared on release
 *  (REENB 40162B, BSET ZRO 170). A stored 0x8000 means the volume was left entered by
 *  the system in ext_last_system_number. Bits 0-14 have no known meaning - preserve them.
 *  NOTE: system number 0 means NO OWNER recorded, not "system zero". */
#define NDFS_EXT_FLAG_ENTERED 0x8000u

/**
 * Parse a master block (and extended info) from a full page 0 buffer.
 * @param page_data  Buffer of at least NDFS_PAGE_SIZE bytes.
 * @param out        Receives the parsed master block.
 * @return NDFS_OK or error code.
 */
ndfs_error_t ndfs_mb_parse(const uint8_t *page_data, ndfs_master_block_t *out);

/**
 * Write master block fields into a page 0 buffer at the standard offset.
 * @param mb         The master block to serialize.
 * @param page_data  Buffer of at least NDFS_PAGE_SIZE bytes.
 * @return NDFS_OK or error code.
 */
ndfs_error_t ndfs_mb_write(const ndfs_master_block_t *mb, uint8_t *page_data);

/**
 * Write extended info into a page 0 buffer.
 * Recalculates the checksum.
 */
ndfs_error_t ndfs_mb_write_extended(const ndfs_master_block_t *mb, uint8_t *page_data);

/**
 * Check if the master block is valid.
 */
bool ndfs_mb_is_valid(const ndfs_master_block_t *mb);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_MASTER_BLOCK_H */
