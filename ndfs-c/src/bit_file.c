/**
 * NDFS bit file (allocation bitmap) implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/bit_file.h>
#include <string.h>

ndfs_error_t ndfs_bf_init(ndfs_bit_file_t *bf, uint32_t total_pages)
{
    size_t bytes;

    if (!bf) return NDFS_ERR_NULL_PTR;

    ndfs_bf_destroy(bf);

    bf->total_pages = total_pages;
    /* Until the caller supplies the directory's declared capacity, the whole device is
     * allocatable. ndfs_fs_* narrows this to ext_pages_available on mount. */
    bf->alloc_ceiling = total_pages;
    /* Rounded up to a whole number of 16-bit WORDS: the bit file is word addressed
     * (see bf_byte_index), so an odd byte count would leave the last word half present
     * and its high-byte pages past the end of the buffer. A 616-page ND floppy is the
     * real case - ceil(616/8) = 77 bytes, but page 615 needs byte 77 to exist. */
    bytes = (((total_pages + 7) / 8) + 1) & ~1u;
    bf->bitmap = (uint8_t *)calloc(bytes, 1);
    if (!bf->bitmap) return NDFS_ERR_ALLOC;
    bf->bitmap_size = bytes;

    memset(&bf->index_pointer, 0, sizeof(bf->index_pointer));
    return NDFS_OK;
}

void ndfs_bf_destroy(ndfs_bit_file_t *bf)
{
    if (!bf) return;
    if (bf->bitmap) {
        free(bf->bitmap);
        bf->bitmap = NULL;
    }
    bf->bitmap_size = 0;
    bf->total_pages = 0;
}

ndfs_error_t ndfs_bf_load(ndfs_bit_file_t *bf, const uint8_t *data, size_t len)
{
    if (!bf || !data) return NDFS_ERR_NULL_PTR;
    if (!bf->bitmap) return NDFS_ERR_INVALID_ARG;

    {
        size_t copy_len = len < bf->bitmap_size ? len : bf->bitmap_size;
        memcpy(bf->bitmap, data, copy_len);
    }
    return NDFS_OK;
}

/*
 * Bit-file addressing: page N lives in 16-bit WORD N/16, at bit N%16 counting from
 * the LSB.
 *
 * AUTHORITY: SINTRAN III System Supervisor, ND-30.003.007 EN, appendix F.2 "Bit-File":
 *
 *     PAGE = BLOCK*400B + WORD*20B + BIT
 *
 * 20B is 16 decimal, so ONE 16-BIT WORD MAPS 16 PAGES - not one byte mapping 8. And
 * because the formula ADDS the bit number, page x sits at bit 0, the least significant
 * bit. The Norwegian edition (ND-30.003.7 NO) carries the identical formula, and
 * SINTRAN's own allocator agrees: TPAGF at 51043B forms the word index with
 * "SHA ZIN SHR 4", i.e. page/16.
 *
 * The image is big-endian, so a 16-bit word at word index w occupies bytes w*2 (high)
 * and w*2+1 (low). Page N therefore lands in the OTHER byte of the pair from where a
 * naive "byte N/8, bit N%8" scheme would put it - which reduces to flipping the low
 * bit of the byte index:
 *
 *     byte = (N >> 3) ^ 1        bit = N & 7
 *
 * WHY THIS MATTERS: this library previously used the naive byte scheme. Being wrong in
 * both the reader and the writer, it was self-consistent and every round-trip test
 * passed - but on a pack written by real SINTRAN it reads the allocation map with every
 * byte pair swapped. Measured on three genuine ND media (a 78 MB pack and two ND
 * distribution floppies), the naive scheme reports 32, 4 and 3 pages that demonstrably
 * hold file data as FREE; this scheme reports none. Handing those pages out would have
 * overwritten live files.
 */
static size_t bf_byte_index(uint32_t block_id)
{
    return (size_t)((block_id >> 3) ^ 1u);
}

static uint8_t bf_bit_mask(uint32_t block_id)
{
    return (uint8_t)(1u << (block_id & 7u));
}

bool ndfs_bf_is_used(const ndfs_bit_file_t *bf, uint32_t block_id)
{
    if (!bf || !bf->bitmap || block_id >= bf->total_pages) return false;
    return (bf->bitmap[bf_byte_index(block_id)] & bf_bit_mask(block_id)) != 0;
}

ndfs_error_t ndfs_bf_mark_used(ndfs_bit_file_t *bf, uint32_t block_id)
{
    if (!bf) return NDFS_ERR_NULL_PTR;
    if (!bf->bitmap || block_id >= bf->total_pages) return NDFS_ERR_OUT_OF_RANGE;

    bf->bitmap[bf_byte_index(block_id)] |= bf_bit_mask(block_id);
    return NDFS_OK;
}

ndfs_error_t ndfs_bf_mark_free(ndfs_bit_file_t *bf, uint32_t block_id)
{
    if (!bf) return NDFS_ERR_NULL_PTR;
    if (!bf->bitmap || block_id >= bf->total_pages) return NDFS_ERR_OUT_OF_RANGE;

    bf->bitmap[bf_byte_index(block_id)] &= (uint8_t)~bf_bit_mask(block_id);
    return NDFS_OK;
}

uint32_t ndfs_bf_count_used(const ndfs_bit_file_t *bf)
{
    uint32_t count = 0;
    uint32_t i;

    if (!bf || !bf->bitmap) return 0;
    for (i = 0; i < bf->total_pages; i++) {
        if (ndfs_bf_is_used(bf, i)) count++;
    }
    return count;
}

uint32_t ndfs_bf_count_free(const ndfs_bit_file_t *bf)
{
    if (!bf) return 0;
    return bf->total_pages - ndfs_bf_count_used(bf);
}

/* Highest page index the allocator may consider (inclusive).
 *
 * The window is bounded by the declared capacity (alloc_ceiling), NOT the device size,
 * and is clamped to the bitmap so a stale or over-large ceiling can never index past it.
 * Returns 0 and sets *empty when there is no usable window at all. */
static uint32_t bf_top_index(const ndfs_bit_file_t *bf, bool *empty)
{
    uint32_t ceiling = bf->alloc_ceiling;

    if (ceiling == 0 || ceiling > bf->total_pages) ceiling = bf->total_pages;
    if (ceiling <= NDFS_FIRST_ALLOC_BLOCK) { *empty = true; return 0; }

    *empty = false;
    return ceiling - 1;
}

/* Find a single free page, scanning HIGH -> LOW.
 *
 * KERNEL-CORRECTED: SINTRAN allocates from the TOP of the volume downward. The scanner
 * TESTP (51355B) only ever DECREMENTS its bitmap word index (`AAX -1` at 51372B and
 * 51401B) - it never increments - and is bounded below by the block-7 floor. This matches
 * the @CREATE-FILE rule "contiguous files are positioned in the highest page addresses".
 * The old upward scan produced the opposite layout to genuine SINTRAN.
 *
 * Reading an existing bitmap is direction-agnostic; it is the ALLOCATION CHOICE that must
 * descend. */
ndfs_error_t ndfs_bf_find_free(const ndfs_bit_file_t *bf, uint32_t *out_block)
{
    uint32_t i;
    bool empty;

    if (!bf || !out_block) return NDFS_ERR_NULL_PTR;

    i = bf_top_index(bf, &empty);
    if (empty) return NDFS_ERR_NO_SPACE;

    for (;;) {
        if (!ndfs_bf_is_used(bf, i)) {
            *out_block = i;
            return NDFS_OK;
        }
        if (i == NDFS_FIRST_ALLOC_BLOCK) break;  /* floor reached; avoid unsigned wrap */
        i--;
    }
    return NDFS_ERR_NO_SPACE;
}

/* Find a contiguous run of free pages, scanning HIGH -> LOW so the run lands in the
 * highest available addresses (SINTRAN's downward range reserve, RSPAG 51120B -> TESTP).
 * Returns the LOWEST block of the run (its start). */
ndfs_error_t ndfs_bf_find_free_range(const ndfs_bit_file_t *bf,
                                     uint32_t count,
                                     uint32_t *out_start)
{
    uint32_t consecutive = 0;
    uint32_t i;
    bool empty;

    if (!bf || !out_start) return NDFS_ERR_NULL_PTR;
    if (count == 0 || count > bf->total_pages) return NDFS_ERR_INVALID_ARG;

    i = bf_top_index(bf, &empty);
    if (empty) return NDFS_ERR_NO_SPACE;

    for (;;) {
        if (!ndfs_bf_is_used(bf, i)) {
            consecutive++;
            if (consecutive >= count) {
                *out_start = i;  /* i is the lowest block of the run */
                return NDFS_OK;
            }
        } else {
            consecutive = 0;
        }
        if (i == NDFS_FIRST_ALLOC_BLOCK) break;
        i--;
    }
    return NDFS_ERR_NO_SPACE;
}

ndfs_error_t ndfs_bf_allocate(ndfs_bit_file_t *bf,
                              uint32_t start_block,
                              uint32_t count)
{
    uint32_t i;

    if (!bf) return NDFS_ERR_NULL_PTR;
    if (start_block < NDFS_FIRST_ALLOC_BLOCK) return NDFS_ERR_INVALID_ARG;
    if (start_block + count > bf->total_pages) return NDFS_ERR_OUT_OF_RANGE;

    /* Check all blocks are free */
    for (i = start_block; i < start_block + count; i++) {
        if (ndfs_bf_is_used(bf, i)) return NDFS_ERR_ALREADY_EXISTS;
    }

    /* Mark all blocks as used */
    for (i = start_block; i < start_block + count; i++) {
        ndfs_bf_mark_used(bf, i);
    }
    return NDFS_OK;
}

void ndfs_bf_free_range(ndfs_bit_file_t *bf,
                        uint32_t start_block,
                        uint32_t count)
{
    uint32_t i;

    if (!bf) return;
    for (i = start_block; i < start_block + count && i < bf->total_pages; i++) {
        ndfs_bf_mark_free(bf, i);
    }
}

ndfs_error_t ndfs_bf_get_data(const ndfs_bit_file_t *bf,
                              const uint8_t **out_data,
                              size_t *out_len)
{
    if (!bf || !out_data || !out_len) return NDFS_ERR_NULL_PTR;
    *out_data = bf->bitmap;
    *out_len  = bf->bitmap_size;
    return NDFS_OK;
}
