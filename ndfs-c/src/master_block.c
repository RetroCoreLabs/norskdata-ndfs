/**
 * NDFS master block parse/write implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/master_block.h>
#include "endian_util.h"
#include "ndfs_name.h"
#include <string.h>

/* ── Detect FLOMON boot format ───────────────────────────────────── */

static bool detect_flomon(const uint8_t *page_data)
{
    int excl_pos = -1;
    int i;
    int limit = 256;

    for (i = 0; i < limit; i++) {
        if (page_data[i] == 0x21) {
            excl_pos = i;
            break;
        }
    }
    if (excl_pos < 0) return false;

    {
        int after = excl_pos + 1;
        uint16_t addr, count;
        if (after + 4 > (int)NDFS_PAGE_SIZE) return false;
        addr  = ndfs_read_u16be(page_data, (size_t)after);
        count = ndfs_read_u16be(page_data, (size_t)(after + 2));
        return addr == 0 && count == 0;
    }
}

/* ── Public API ──────────────────────────────────────────────────── */

uint16_t ndfs_mb_ext_checksum(uint16_t reserved1, uint16_t reserved2, uint16_t reserved3,
                              uint16_t flag_word, uint16_t system_number,
                              uint16_t pages_hi, uint16_t pages_lo)
{
    /* Plain 16-bit additive sum of the seven words FOLLOWING the checksum slot
     * (1751B-1757B). Kernel: WXDIR 37702B (writer) and CHDSI 37763B (validator) both run
     * the identical `ADD ,X 0` loop. Overflow past bit 15 is discarded, exactly as the
     * ND-100's two's-complement ADD does. */
    uint32_t sum = (uint32_t)reserved1 + reserved2 + reserved3 +
                   flag_word + system_number + pages_hi + pages_lo;
    return (uint16_t)(sum & 0xFFFF);
}

ndfs_error_t ndfs_mb_parse(const uint8_t *page_data, ndfs_master_block_t *out)
{
    size_t off, ext;
    uint16_t pages_lo, pages_hi, calculated;
    bool checksum_non_zero, checksum_ok;

    if (!page_data || !out) return NDFS_ERR_NULL_PTR;

    memset(out, 0, sizeof(*out));
    off = NDFS_MASTER_BLOCK_OFFSET;

    /* Directory name (16 bytes, terminated by 0x27) */
    ndfs_read_name(page_data, off, NDFS_NAME_MAX, out->directory_name);

    /* Block pointers */
    out->object_file_ptr = ndfs_bp_from_bytes(page_data, off + 0x10);
    out->user_file_ptr   = ndfs_bp_from_bytes(page_data, off + 0x14);
    out->bit_file_ptr    = ndfs_bp_from_bytes(page_data, off + 0x18);

    /* Unreserved pages */
    out->unreserved_pages = ndfs_read_u32be(page_data, off + 0x1C);

    /* ── Extended info (bytes 2000-2015) ── */
    ext = NDFS_EXTENDED_INFO_OFFSET;
    out->ext_checksum           = ndfs_read_u16be(page_data, ext);
    out->ext_reserved1          = ndfs_read_u16be(page_data, ext + 2);
    out->ext_reserved2          = ndfs_read_u16be(page_data, ext + 4);
    out->ext_reserved3          = ndfs_read_u16be(page_data, ext + 6);
    out->ext_flag_word          = ndfs_read_u16be(page_data, ext + 8);
    out->ext_last_system_number = ndfs_read_u16be(page_data, ext + 10);
    out->ext_pages_available    = ndfs_read_u32be(page_data, ext + 12);

    /* Calculate checksum.
     *
     * KERNEL-CORRECTED: the checksum is a plain 16-bit ADDITIVE SUM of the seven words
     * that FOLLOW the checksum slot (words 1751B-1757B) - NOT "XOR six words, then add
     * the system number". Proven from BOTH sides of the real SINTRAN III kernel (carved
     * 006-S3FS): the writer WXDIR (37702B) and the enter-directory validator CHDSI
     * (37763B) run the identical `ADD ,X 0` accumulation loop over these words. There is
     * no XOR anywhere, and the system number is simply one of the seven summed words.
     *
     * The old XOR-then-add form matched PACK-ONE only by coincidence: its only two
     * summed words sharing a set bit are flag=0x8000 and pages_lo=0x9051 (both bit 15);
     * under ADD the two bit-15s carry out past bit 15 and are lost, under XOR they
     * cancel - identical low 16 bits. It breaks as soon as any two words share a LOWER
     * set bit. Verified against three real disks (0x10B7, 0x1051, 0xC162). */
    pages_lo = (uint16_t)(out->ext_pages_available & 0xFFFF);
    pages_hi = (uint16_t)((out->ext_pages_available >> 16) & 0xFFFF);
    calculated = ndfs_mb_ext_checksum(out->ext_reserved1, out->ext_reserved2,
                                      out->ext_reserved3, out->ext_flag_word,
                                      out->ext_last_system_number, pages_hi, pages_lo);
    out->ext_calculated_checksum = calculated;

    /* Determine checksum validation state. The kernel writes and compares the FULL 16
     * bits (WXDIR stores it, CHDSI compares it), so there is no "low byte only" accept
     * state - that was a reader-side heuristic SINTRAN never produces. */
    out->checksum_state = (out->ext_checksum == calculated)
                              ? NDFS_CHECKSUM_VALID
                              : NDFS_CHECKSUM_INVALID;

    /* Detect FLOMON */
    out->has_flomon = detect_flomon(page_data);

    /* Extended info validity */
    if (out->has_flomon) {
        out->ext_valid = false;
    } else {
        checksum_non_zero = out->ext_checksum != 0;
        checksum_ok = (out->checksum_state == NDFS_CHECKSUM_VALID);
        out->ext_valid = checksum_non_zero && checksum_ok;
    }

    return NDFS_OK;
}

bool ndfs_mb_is_valid(const ndfs_master_block_t *mb)
{
    size_t i;
    bool has_valid_ptr;

    if (!mb) return false;

    /* Check directory name is printable ASCII */
    if (mb->directory_name[0] != '\0') {
        for (i = 0; mb->directory_name[i] != '\0'; i++) {
            unsigned char c = (unsigned char)mb->directory_name[i];
            if (c < 0x20 || c > 0x7E) return false;
        }
    }

    /* At least one pointer must be valid, or a directory name must exist */
    has_valid_ptr = false;
    if (ndfs_bp_is_valid(&mb->object_file_ptr)) has_valid_ptr = true;
    if (ndfs_bp_is_valid(&mb->user_file_ptr))   has_valid_ptr = true;
    if (ndfs_bp_is_valid(&mb->bit_file_ptr))     has_valid_ptr = true;

    return has_valid_ptr || mb->directory_name[0] != '\0';
}

ndfs_error_t ndfs_mb_write(const ndfs_master_block_t *mb, uint8_t *page_data)
{
    size_t off;

    if (!mb || !page_data) return NDFS_ERR_NULL_PTR;

    off = NDFS_MASTER_BLOCK_OFFSET;

    /* Clear the master block area */
    memset(page_data + off, 0, NDFS_MASTER_BLOCK_SIZE);

    /* Directory name */
    ndfs_write_name(page_data, off, mb->directory_name, NDFS_NAME_MAX);

    /* Block pointers */
    ndfs_bp_to_bytes(&mb->object_file_ptr, page_data, off + 0x10);
    ndfs_bp_to_bytes(&mb->user_file_ptr,   page_data, off + 0x14);
    ndfs_bp_to_bytes(&mb->bit_file_ptr,    page_data, off + 0x18);

    /* Unreserved pages */
    ndfs_write_u32be(page_data, off + 0x1C, mb->unreserved_pages);

    return NDFS_OK;
}

ndfs_error_t ndfs_mb_write_extended(const ndfs_master_block_t *mb, uint8_t *page_data)
{
    size_t ext;
    uint16_t pages_lo, pages_hi, checksum;

    if (!mb || !page_data) return NDFS_ERR_NULL_PTR;

    ext = NDFS_EXTENDED_INFO_OFFSET;

    /* Recompute the checksum from the current fields, exactly as the kernel writer WXDIR
     * (37702B) does: a 16-bit additive sum of words 1751B-1757B. Every write of the
     * extended-info block is followed by a fresh checksum. */
    pages_lo = (uint16_t)(mb->ext_pages_available & 0xFFFF);
    pages_hi = (uint16_t)((mb->ext_pages_available >> 16) & 0xFFFF);
    checksum = ndfs_mb_ext_checksum(mb->ext_reserved1, mb->ext_reserved2, mb->ext_reserved3,
                                    mb->ext_flag_word, mb->ext_last_system_number,
                                    pages_hi, pages_lo);

    ndfs_write_u16be(page_data, ext,      checksum);
    ndfs_write_u16be(page_data, ext + 2,  mb->ext_reserved1);
    ndfs_write_u16be(page_data, ext + 4,  mb->ext_reserved2);
    ndfs_write_u16be(page_data, ext + 6,  mb->ext_reserved3);
    ndfs_write_u16be(page_data, ext + 8,  mb->ext_flag_word);
    ndfs_write_u16be(page_data, ext + 10, mb->ext_last_system_number);
    ndfs_write_u32be(page_data, ext + 12, mb->ext_pages_available);

    return NDFS_OK;
}
