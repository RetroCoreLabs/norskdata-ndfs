/**
 * NDFS object entry implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/object_entry.h>
#include "endian_util.h"
#include "ndfs_name.h"
#include <string.h>
#include <stdio.h>

void ndfs_oe_init(ndfs_object_entry_t *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->header = NDFS_OBJECT_IN_USE;
    entry->type[0] = 'D'; entry->type[1] = 'A';
    entry->type[2] = 'T'; entry->type[3] = 'A';
    entry->type[4] = '\0';
}

int32_t ndfs_oe_compute_file_number(uint32_t object_index, uint32_t user_index)
{
    /* One object block = 8 pages x 32 entries = 256 files. Successive blocks of
     * the same user are one whole index block (512 pages) apart. */
    const uint32_t entries_per_page       = 32u;
    const uint32_t pages_per_object_block = 8u;
    const uint32_t pages_per_index_block  = 512u;
    const uint32_t files_per_object_block = pages_per_object_block * entries_per_page;

    uint32_t page             = object_index / entries_per_page;
    uint32_t slot_in_page     = object_index % entries_per_page;
    uint32_t block            = page / pages_per_index_block;
    uint32_t page_within_row  = page % pages_per_index_block;
    uint32_t first_page_of_u  = user_index * pages_per_object_block;
    uint32_t offset_pages;
    uint32_t slot;

    /* Unsigned subtraction would wrap, so compare before subtracting. */
    if (page_within_row < first_page_of_u) {
        return -1;
    }

    offset_pages = page_within_row - first_page_of_u;
    if (offset_pages >= pages_per_object_block) {
        return -1;
    }

    slot = offset_pages * entries_per_page + slot_in_page;
    return (int32_t)(block * files_per_object_block + slot);
}

ndfs_error_t ndfs_oe_from_bytes(const uint8_t *data, size_t data_len,
                                size_t offset, ndfs_object_entry_t *out)
{
    uint32_t bytes_minus_one;

    if (!data || !out) return NDFS_ERR_NULL_PTR;
    if (data_len < offset + NDFS_ENTRY_SIZE) return NDFS_ERR_TOO_SMALL;

    /* Check in-use bit */
    if ((data[offset] & NDFS_OBJECT_IN_USE) == 0)
        return NDFS_ERR_NOT_FOUND;

    ndfs_oe_init(out);

    /* Preserve the verbatim 64 bytes so re-serialization never loses fields
     * we do not explicitly model. */
    memcpy(out->raw, data + offset, NDFS_ENTRY_SIZE);
    out->has_raw = true;

    out->header = data[offset];
    out->header_word = ndfs_read_u16be(data, offset + 0);

    /* Object name (16 bytes at offset+2) */
    ndfs_read_name(data, offset + 2, NDFS_NAME_MAX, out->object_name);

    /* File type (4 bytes at offset+18). Preserve an empty type as-is — do NOT
     * default to "DATA". A parse must faithfully represent what is on disk;
     * defaulting here corrupts files whose type is intentionally empty (e.g.
     * TERMINAL: 27 00 00 00) on the next write-back. (Matches RetroFS.NDFS.) */
    ndfs_read_name(data, offset + 18, NDFS_TYPE_MAX, out->type);

    /* Versioning, access, flags, device (offsets 22-31). */
    out->next_version    = ndfs_read_u16be(data, offset + 22);
    out->prev_version    = ndfs_read_u16be(data, offset + 24);
    out->access_bits     = ndfs_read_u16be(data, offset + 26);
    out->file_type_flags = ndfs_read_u16be(data, offset + 28);
    out->device_number   = ndfs_read_u16be(data, offset + 30);

    /* File type code (byte 32) */
    out->file_type = data[offset + 32];

    /* User index (byte 34, high byte of the object-index word) */
    out->user_index = data[offset + 34];
    out->disk_object_index = ndfs_read_u16be(data, offset + 34);
    /* file_number is filled in by the caller once the physical position is
     * known - see ndfs_oe_compute_file_number. It cannot be derived from the
     * 64 bytes alone, because the position is what encodes the object block. */

    /* Open counts and timestamps (offsets 36-51). */
    out->current_open_count = ndfs_read_u16be(data, offset + 36);
    out->total_open_count   = ndfs_read_u16be(data, offset + 38);
    out->date_created    = ndfs_read_u32be(data, offset + 40);
    out->last_read_date  = ndfs_read_u32be(data, offset + 44);
    out->last_write_date = ndfs_read_u32be(data, offset + 48);

    /* Pages in file (bytes 52-55, big-endian) */
    out->pages_in_file = ndfs_read_u32be(data, offset + 52);

    /* Bytes in file (bytes 56-59, big-endian) + 1 */
    bytes_minus_one = ndfs_read_u32be(data, offset + 56);
    out->bytes_in_file = bytes_minus_one + 1;

    /* File pointer (bytes 60-63) */
    out->file_pointer = ndfs_bp_from_bytes(data, offset + 60);

    out->user_name[0] = '\0';

    return NDFS_OK;
}

ndfs_error_t ndfs_oe_to_bytes(const ndfs_object_entry_t *entry,
                              uint8_t *buffer, size_t buf_len,
                              size_t offset)
{
    uint32_t bytes_minus_one;

    if (!entry || !buffer) return NDFS_ERR_NULL_PTR;
    if (buf_len < offset + NDFS_ENTRY_SIZE) return NDFS_ERR_TOO_SMALL;

    /* Base the output on the original on-disk bytes when available, so any
     * bytes we do not model (e.g. byte 33, the low byte of the object-index
     * word) survive a round trip. Freshly-built entries start from zero. */
    if (entry->has_raw) {
        memcpy(buffer + offset, entry->raw, NDFS_ENTRY_SIZE);
    } else {
        memset(buffer + offset, 0, NDFS_ENTRY_SIZE);
    }

    /* Header: a loaded entry keeps its original header word (used / write-open
     * / modified / terminal bits) via the raw copy; a freshly-built entry just
     * gets the in-use bit. */
    if (!entry->has_raw) {
        buffer[offset] = NDFS_OBJECT_IN_USE;
    }

    /* Object name */
    ndfs_write_name(buffer, offset + 2, entry->object_name, NDFS_NAME_MAX);

    /* File type string */
    ndfs_write_name(buffer, offset + 18, entry->type, NDFS_TYPE_MAX);

    /* Versioning, access, flags, device (offsets 22-31). */
    ndfs_write_u16be(buffer, offset + 22, entry->next_version);
    ndfs_write_u16be(buffer, offset + 24, entry->prev_version);
    ndfs_write_u16be(buffer, offset + 26, entry->access_bits);
    ndfs_write_u16be(buffer, offset + 28, entry->file_type_flags);
    ndfs_write_u16be(buffer, offset + 30, entry->device_number);

    /* File type code */
    buffer[offset + 32] = entry->file_type;

    /* Object index word at 34: high byte = user index, low byte = file slot.
     * Writing both bytes keeps the version pointers (which equal this word)
     * consistent for newly-created files. */
    buffer[offset + 34] = entry->user_index;
    buffer[offset + 35] = (uint8_t)(entry->disk_object_index & 0xFF);

    /* Open counts and timestamps (offsets 36-51). */
    ndfs_write_u16be(buffer, offset + 36, entry->current_open_count);
    ndfs_write_u16be(buffer, offset + 38, entry->total_open_count);
    ndfs_write_u32be(buffer, offset + 40, entry->date_created);
    ndfs_write_u32be(buffer, offset + 44, entry->last_read_date);
    ndfs_write_u32be(buffer, offset + 48, entry->last_write_date);

    /* Pages in file */
    ndfs_write_u32be(buffer, offset + 52, entry->pages_in_file);

    /* Bytes in file - 1 */
    bytes_minus_one = entry->bytes_in_file > 0 ? entry->bytes_in_file - 1 : 0;
    ndfs_write_u32be(buffer, offset + 56, bytes_minus_one);

    /* File pointer */
    ndfs_bp_to_bytes(&entry->file_pointer, buffer, offset + 60);

    return NDFS_OK;
}

void ndfs_oe_full_name(const ndfs_object_entry_t *entry, char *buf, size_t buf_len)
{
    if (entry->type[0] != '\0') {
        snprintf(buf, buf_len, "%s:%s", entry->object_name, entry->type);
    } else {
        snprintf(buf, buf_len, "%s", entry->object_name);
    }
}
