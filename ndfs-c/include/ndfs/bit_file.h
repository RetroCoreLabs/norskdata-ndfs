/**
 * NDFS bit file (allocation bitmap): one bit per page, 0=free, 1=used.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_BIT_FILE_H
#define NDFS_BIT_FILE_H

#include "block_pointer.h"
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Allocation bitmap. */
typedef struct {
    ndfs_block_pointer_t index_pointer;
    /** Pages the bitmap covers. SINTRAN sizes the bitmap to the PHYSICAL DEVICE
     *  (e.g. 38400 on a 75MB SMD pack), not to the declared capacity. */
    uint32_t             total_pages;
    /** Highest allocatable page + 1 - the descending allocator's ceiling.
     *
     *  SINTRAN bounds the allocatable window by the directory's DECLARED CAPACITY
     *  (ext_pages_available, words 1756B-1757B), not by the device size. On PACK-ONE the
     *  capacity is 36945, so the highest allocatable page is 36944; pages 36945..38399 are
     *  a deliberate free-but-unreachable gap (drive spare / bad-sector remap area) - they
     *  stay 0 in the bitmap yet are never handed out. Verified on the real disk: page 36944
     *  is used, 36945+ are all free.
     *
     *  Set to total_pages when the capacity is unknown (e.g. FLOMON floppies, which carry
     *  no valid extended-info block). MUST be clamped to total_pages: the real Winchester
     *  WD0.img declares a capacity 36 pages LARGER than the file, and allocating there
     *  would run off the end. */
    uint32_t             alloc_ceiling;
    /** Page where the bit file itself starts (BA). 0 = not known; the SINTRAN placement rules that
     *  start from BA then fall back to the top-down search. */
    uint32_t             bit_address;
    /** The 256 page block of the last page given by ndfs_bf_find_free_block(); valid only if
     *  cursor_valid. A fresh bit file has no cursor (SINTRAN's "cold start"). */
    uint32_t             cursor_block;
    bool                 cursor_valid;
    uint8_t             *bitmap;      /**< Owned by the struct. */
    size_t               bitmap_size; /**< Bytes in bitmap.     */
} ndfs_bit_file_t;

/** Initialize an empty bitmap for the given total page count. */
ndfs_error_t ndfs_bf_init(ndfs_bit_file_t *bf, uint32_t total_pages);

/** Release bitmap memory. Safe to call on zeroed or already-destroyed struct. */
void ndfs_bf_destroy(ndfs_bit_file_t *bf);

/** Load bitmap data from raw bytes (copies the data). */
ndfs_error_t ndfs_bf_load(ndfs_bit_file_t *bf, const uint8_t *data, size_t len);

/** Check if a block is marked as used. */
bool ndfs_bf_is_used(const ndfs_bit_file_t *bf, uint32_t block_id);

/** Mark a block as used. */
ndfs_error_t ndfs_bf_mark_used(ndfs_bit_file_t *bf, uint32_t block_id);

/** Mark a block as free. */
ndfs_error_t ndfs_bf_mark_free(ndfs_bit_file_t *bf, uint32_t block_id);

/** Count total used pages. */
uint32_t ndfs_bf_count_used(const ndfs_bit_file_t *bf);

/** Get number of free pages. */
uint32_t ndfs_bf_count_free(const ndfs_bit_file_t *bf);

/**
 * Find the first free block (starting from block 7).
 * @param out_block  Receives the block ID.
 * @return NDFS_OK or NDFS_ERR_NO_SPACE.
 */
ndfs_error_t ndfs_bf_find_free(const ndfs_bit_file_t *bf, uint32_t *out_block);

/**
 * Find a free page by SINTRAN's RSBPG rule (index blocks, user and object pages).
 *
 * Start at page (hint OR 15), the top bit of the bitmap word that holds the hint, and take the first
 * free page going down; if the word is full go to word+1 (lowest bit first), word-1 (highest bit
 * first), word+2, word-2 and so on. Pages below the first allocatable block and at or above
 * alloc_ceiling are never given. See docs/CREATE-DIRECTORY-PLACEMENT.md section 2.2.
 *
 * @param hint       Hint page (SINTRAN uses the bit file address minus 1).
 * @param out_block  Receives the page; it is NOT marked used.
 */
ndfs_error_t ndfs_bf_find_free_near(const ndfs_bit_file_t *bf, uint32_t hint, uint32_t *out_block);

/**
 * RSBPG rule with the hint taken from the bit file: hint = bit_address - 1. Falls back to
 * ndfs_bf_find_free() when bit_address is 0. The page is NOT marked used.
 */
ndfs_error_t ndfs_bf_find_free_hint(const ndfs_bit_file_t *bf, uint32_t *out_block);

/**
 * First 256 page block RSPAG looks in when nothing was allocated yet ("cold start"):
 * lastPage = (alloc_ceiling-1) div 16384, BP = (bit_address+1) div 16384;
 * lastPage > BP: block 0 of lastPage; lastPage = BP: the block of bit_address+1; lastPage < BP:
 * block 63 of lastPage. Returns the block number counted from page 0 (page div 256).
 */
uint32_t ndfs_bf_cold_start_block(const ndfs_bit_file_t *bf);

/**
 * Find a free page by SINTRAN's RSPAG rule (data pages of indexed files, files of 1 page).
 *
 * Search 256 page blocks: the cursor block (or the cold start block), then +1, -1, +2, -2 ...; in a
 * block the LOWEST free page wins. On success the cursor moves to the block of the page found.
 * See docs/NDFS-ALLOCATION-RULES.md section 3. Falls back to ndfs_bf_find_free() when bit_address
 * is 0. The page is NOT marked used.
 */
ndfs_error_t ndfs_bf_find_free_block(ndfs_bit_file_t *bf, uint32_t *out_block);

/**
 * Find a contiguous range of free blocks.
 * @param count      Number of contiguous blocks needed.
 * @param out_start  Receives the starting block ID.
 * @return NDFS_OK or NDFS_ERR_NO_SPACE.
 */
ndfs_error_t ndfs_bf_find_free_range(const ndfs_bit_file_t *bf,
                                     uint32_t count,
                                     uint32_t *out_start);

/**
 * Allocate (mark as used) a range of blocks.
 * Blocks 0-6 cannot be allocated. Fails if any block is already used.
 */
ndfs_error_t ndfs_bf_allocate(ndfs_bit_file_t *bf,
                              uint32_t start_block,
                              uint32_t count);

/** Free a range of blocks. */
void ndfs_bf_free_range(ndfs_bit_file_t *bf,
                        uint32_t start_block,
                        uint32_t count);

/**
 * Get a pointer to the raw bitmap data (not a copy).
 * @param out_data  Receives pointer to internal bitmap.
 * @param out_len   Receives bitmap length in bytes.
 */
ndfs_error_t ndfs_bf_get_data(const ndfs_bit_file_t *bf,
                              const uint8_t **out_data,
                              size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_BIT_FILE_H */
