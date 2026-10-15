/**
 * NDFS object (file) entry: 64-byte record in the object file.
 *
 * Byte offsets:
 *   0:     Header (bit 7 = in use, i.e. 0x80)
 *   1:     Reserved
 *   2-17:  Object name (16 bytes, terminated by 0x27)
 *   18-21: File type (4 bytes, terminated by 0x27)
 *   22-31: Reserved / versioning / access
 *   32:    File type code (0=DATA, 1=PROG, 2=SYMB, 3=TEXT)
 *   33:    Reserved
 *   34:    User index (owner)
 *   35-51: Reserved / tracking
 *   52-55: Pages in file (32-bit, big-endian)
 *   56-59: Bytes in file - 1 (32-bit, big-endian; actual = stored + 1)
 *   60-63: File pointer (BlockPointer, big-endian)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_OBJECT_ENTRY_H
#define NDFS_OBJECT_ENTRY_H

#include "types.h"
#include "block_pointer.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Object file_type_flags bits (offset 28): "L M A C I B P T". */
#define NDFS_FT_TERMINAL    (1u << 0)
#define NDFS_FT_PERIPHERAL  (1u << 1)
#define NDFS_FT_SPOOLING    (1u << 2)
#define NDFS_FT_INDEXED     (1u << 3)
#define NDFS_FT_CONTIGUOUS  (1u << 4)
#define NDFS_FT_ALLOCATED   (1u << 5)
#define NDFS_FT_MAGTAPE     (1u << 6)
#define NDFS_FT_LIBRARY     (1u << 7)

/* Access rights within a 5-bit tier (access_bits, offset 26). */
#define NDFS_ACC_READ       (1u << 0)
#define NDFS_ACC_WRITE      (1u << 1)
#define NDFS_ACC_APPEND     (1u << 2)
#define NDFS_ACC_COMMON     (1u << 3)  /* common / execute */
#define NDFS_ACC_DIRECTORY  (1u << 4)  /* directory / delete */
#define NDFS_ACC_TIER_MASK  0x1Fu
/* Tier shifts: OWN bits 0-4, FRIEND 5-9, PUBLIC 10-14. */
#define NDFS_ACC_OWN_SHIFT     0
#define NDFS_ACC_FRIEND_SHIFT  5
#define NDFS_ACC_PUBLIC_SHIFT  10

/* Default access for a newly-created file: OWN + FRIEND all five rights,
 * PUBLIC none (0x3FF). Matches the RetroCore reference implementation. */
#define NDFS_ACCESS_DEFAULT 0x03FFu

/* What a real ND PRODUCT FLOPPY carries on every file: the same OWN/FRIEND
 * rights as NDFS_ACCESS_DEFAULT plus PUBLIC READ.
 *
 * MEASURED on 210580B01-XX-01D (genuine ND Ethernet distribution floppy): all
 * 12 files read 0x07FF. This is not cosmetic -- PUBLIC is the tier every user
 * who is neither the owner nor a friend falls into, SYSTEM included, so with
 * PUBLIC = 0 the files are invisible from any other user and @LIST-FILES
 * prints nothing while the pack itself verifies clean. Proved by adding one
 * 0x03FF file beside those 12: SINTRAN listed the original twelve and
 * skipped the new one; flipping that single field to 0x07FF made it appear.
 *
 * Pass this as ndfs_write_file_full()'s access_bits when building
 * distribution media; NDFS_ACCESS_DEFAULT (or the owning user's own default)
 * is right for a file a user creates for itself on a live machine. */
#define NDFS_ACCESS_PUBLIC_READ 0x07FFu

/** 64-byte object (file) entry. */
typedef struct {
    uint8_t              header;        /**< Byte 0: bit7 = in use. */
    uint16_t             header_word;   /**< Bytes 0-1: full 16-bit header. */
    uint32_t             object_index;  /**< Physical slot in the object file. */
    /**
     * The file number SINTRAN reports ("FILE n"), per user and stable for the
     * life of the file. NOT the same as object_index.
     *
     * object_index is the raw PHYSICAL position (page*32 + slot) and is the
     * write-back key. The two coincide only inside a user's FIRST object block:
     * a user may own up to 16 blocks of 256 files, and block n sits at object
     * file pages n*512 + user*8, so past the first block the physical position
     * encodes both block and slot.
     *
     * Worked example from a verified pack: F0500 is FILE 307 owned by user 8,
     * at physical position 18483. The old model reads that as file 18483&0xFF
     * = 51 owned by 18483>>8 = 72 - a user that does not exist. Both wrong.
     *
     * Set by ndfs_oe_compute_file_number(); -1 (as 0xFFFFFFFF) is never stored
     * here - callers get -1 from the function itself for a position outside the
     * user's region.
     *
     * Doc: NDInsight SINTRAN/XMSG/DOC/NDFS-OBJECT-BLOCKS-DECODED-2026-08-01.md
     */
    uint32_t             file_number;
    char                 object_name[NDFS_NAME_MAX + 1];
    char                 type[NDFS_TYPE_MAX + 1];
    char                 user_name[NDFS_NAME_MAX + 1]; /**< Resolved at load time. */
    uint8_t              user_index;
    uint8_t              file_type;    /**< 0=DATA, 1=PROG, 2=SYMB, 3=TEXT (byte 32) */
    uint32_t             pages_in_file;
    uint32_t             bytes_in_file;
    ndfs_block_pointer_t file_pointer;
    uint16_t             access_bits;  /**< Offset 26: 3x5-bit OWN/FRIEND/PUBLIC. */
    /* Fields at offsets 22-51 (previously unparsed "reserved / tracking"). */
    uint16_t             next_version;        /**< Offset 22. */
    uint16_t             prev_version;        /**< Offset 24. */
    uint16_t             file_type_flags;     /**< Offset 28: L M A C I B P T. */
    uint16_t             device_number;       /**< Offset 30. */
    uint16_t             disk_object_index;   /**< Offset 34 word: [user|file]. */
    uint16_t             current_open_count;  /**< Offset 36. */
    uint16_t             total_open_count;    /**< Offset 38. */
    uint32_t             date_created;        /**< Offset 40 (ND timestamp). */
    uint32_t             last_read_date;      /**< Offset 44 (ND timestamp). */
    uint32_t             last_write_date;     /**< Offset 48 (ND timestamp). */
    /* Verbatim copy of the on-disk 64 bytes, used as the base when
     * re-serializing so bytes we do not model are never lost. has_raw is
     * false for freshly-built entries (ndfs_oe_init). */
    uint8_t              raw[NDFS_ENTRY_SIZE];
    bool                 has_raw;
} ndfs_object_entry_t;

/**
 * Parse an object entry from 64 bytes at offset.
 * @return NDFS_OK, NDFS_ERR_TOO_SMALL, or NDFS_ERR_NOT_FOUND
 *         (if the in-use bit is not set).
 */
ndfs_error_t ndfs_oe_from_bytes(const uint8_t *data, size_t data_len,
                                size_t offset, ndfs_object_entry_t *out);

/**
 * Serialize an object entry to 64 bytes at offset in buffer.
 * @param entry   The entry to serialize.
 * @param buffer  Destination buffer.
 * @param offset  Byte offset into buffer.
 */
ndfs_error_t ndfs_oe_to_bytes(const ndfs_object_entry_t *entry,
                              uint8_t *buffer, size_t buf_len,
                              size_t offset);

/**
 * Get full name in "NAME:TYPE" format.
 * @param buf  Must be at least NDFS_NAME_MAX + NDFS_TYPE_MAX + 2 bytes.
 */
void ndfs_oe_full_name(const ndfs_object_entry_t *entry, char *buf, size_t buf_len);

/** Initialize a new empty object entry. */
void ndfs_oe_init(ndfs_object_entry_t *entry);

/**
 * Convert a physical object-file position into the file number SINTRAN reports.
 *
 * A user's files live in up to 16 object blocks of 256 files each, and block n
 * occupies object-file pages n*512 + user*8 .. +7. The number the user sees is
 * therefore:
 *
 *     block      = page / 512
 *     slot       = (page % 512 - user*8) * 32 + slot_in_page
 *     fileNumber = block * 256 + slot
 *
 * Using the raw position instead is correct only for a user's first block.
 *
 * @param object_index  Physical position, page*32 + slot_in_page.
 * @param user_index    Owning user, from byte 34 of the entry.
 * @return The per-user file number, or -1 when the position lies outside any
 *         object block belonging to user_index. Returning -1 is deliberate: a
 *         caller spotting a mismatch beats silently getting a plausible but
 *         wrong number.
 */
int32_t ndfs_oe_compute_file_number(uint32_t object_index, uint32_t user_index);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_OBJECT_ENTRY_H */
