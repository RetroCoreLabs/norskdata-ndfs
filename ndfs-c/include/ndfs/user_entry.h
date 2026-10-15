/**
 * NDFS user entry: 64-byte record in the user file.
 *
 * Byte offsets:
 *   0:     Flags (bit 7 = entry used, bit 0 = user entry; 0x81 = valid user)
 *   1:     Enter count
 *   2-17:  User name (16 bytes, terminated by 0x27)
 *   18-19: Password (16-bit, big-endian)
 *   20-23: Date created (ND time, big-endian)
 *   24-27: Last date entered (ND time, big-endian)
 *   28-31: Pages reserved (32-bit, big-endian)
 *   32-35: Pages used (32-bit, big-endian)
 *   36:    Directory index
 *   37:    User index
 *   38-39: Reserved
 *   40-41: Default file access (16-bit, big-endian)
 *   42-47: Reserved / tracking
 *   48-63: Friends (8 x 2-byte entries, big-endian)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_USER_ENTRY_H
#define NDFS_USER_ENTRY_H

#include "types.h"
#include "user_friend.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 64-byte user entry. */
typedef struct {
    /* Byte 0, the flags byte, kept whole. from_bytes only requires the
     * NDFS_USER_ENTRY_FLAG bits (bit 7 "entry used" + bit 0 "user entry"), so a
     * real pack may carry other bits here that we do not model; to_bytes writes
     * this field back verbatim rather than re-hardcoding 0x81, which would
     * destroy them on every read/modify/write. Set it to NDFS_USER_ENTRY_FLAG
     * when building an entry from scratch - a zero byte 0 makes from_bytes
     * reject the slot and the user disappears. */
    uint8_t            uf;
    uint8_t            user_index;
    char               user_name[NDFS_NAME_MAX + 1];
    uint16_t           password;
    uint8_t            enter_count;
    uint32_t           date_created;
    uint32_t           last_date_entered;
    uint32_t           pages_reserved;
    uint32_t           pages_used;
    uint8_t            directory_index;
    uint16_t           default_file_access;
    ndfs_user_friend_t friends[NDFS_MAX_FRIENDS];

    /* --- Object blocks: byte 47, decoded 2026-08-01 -------------------------
     *
     * A SINTRAN user area holds 256 files by default and up to 4096 on version
     * K, structured as up to 16 "object blocks" of 256 files each. Blocks are
     * allocated on demand; the operator raises the ceiling with
     * @GIVE-OBJECT-BLOCKS and reads it back with @USER-STATISTICS
     * ("MAXIMUM NUMBER OF FILES").
     *
     * Byte 47 holds two ZERO-BASED nibbles:
     *   high nibble = MXOBL - 1   maximum blocks this user may have
     *   low  nibble = ACOBL - 1   blocks actually allocated so far
     *
     * So a default user reads 0x00 = 1 max, 1 allocated = 256 files, which is
     * why every untouched user on a real pack has byte 47 == 0.
     *
     * VERIFIED on a live SINTRAN III K pack with a control group: user BIGMAN,
     * given 3 extra blocks and 482 files, reads 0x31 -> MXOBL 4 (matching the
     * reported maximum of 1024 files) and ACOBL 2 (matching ceil(482/256));
     * every other user on the same pack reads 0x00.
     *
     * Doc: NDInsight SINTRAN/XMSG/DOC/NDFS-OBJECT-BLOCKS-DECODED-2026-08-01.md
     */
    uint8_t            max_object_blocks;        /* MXOBL, 1..16 (decoded)  */
    uint8_t            allocated_object_blocks;  /* ACOBL, 1..MXOBL         */

    /* Verbatim on-disk 64 bytes, used as the base when re-serializing so
     * unmodelled bytes (38-39, 42-46) survive. Byte 47 is now modelled above
     * and is rewritten from max/allocated_object_blocks.
     * has_raw is false for freshly-built entries. */
    uint8_t            raw[NDFS_ENTRY_SIZE];
    bool               has_raw;
} ndfs_user_entry_t;

/**
 * Parse a user entry from 64 bytes at offset.
 * @param data    Buffer.
 * @param offset  Byte offset into buffer.
 * @param out     Receives the parsed entry.
 * @return NDFS_OK, NDFS_ERR_TOO_SMALL, or NDFS_ERR_NOT_FOUND
 *         (if flag byte != 0x81).
 */
ndfs_error_t ndfs_ue_from_bytes(const uint8_t *data, size_t data_len,
                                size_t offset, ndfs_user_entry_t *out);

/**
 * Serialize a user entry to a 64-byte buffer.
 * @param entry  The entry to serialize.
 * @param buf    Must be at least NDFS_ENTRY_SIZE bytes.
 */
void ndfs_ue_to_bytes(const ndfs_user_entry_t *entry, uint8_t *buf);

/** Check if user has exceeded quota. */
static inline bool ndfs_ue_is_over_quota(const ndfs_user_entry_t *e)
{
    return e->pages_used > e->pages_reserved;
}

/** Get remaining free pages in quota. */
static inline int32_t ndfs_ue_free_pages(const ndfs_user_entry_t *e)
{
    return (int32_t)e->pages_reserved - (int32_t)e->pages_used;
}

/** Initialize a new empty user entry with defaults. */
void ndfs_ue_init(ndfs_user_entry_t *entry);

/** True if friend_index is an active friend of this user. */
bool ndfs_ue_is_friend(const ndfs_user_entry_t *entry, uint8_t friend_index);

/**
 * Add a friend in the first free slot with the given 5-bit permissions
 * (see ndfs_uf_set_friend). Returns false if all NDFS_MAX_FRIENDS slots are
 * in use. Does not dedupe — callers should check ndfs_ue_is_friend first.
 */
bool ndfs_ue_add_friend(ndfs_user_entry_t *entry, uint8_t friend_index,
                        uint8_t permissions);

/** Remove a friend by index. Returns false if not present. */
bool ndfs_ue_remove_friend(ndfs_user_entry_t *entry, uint8_t friend_index);

/** Get a pointer to a friend's entry, or NULL if not present. */
const ndfs_user_friend_t *ndfs_ue_get_friend(const ndfs_user_entry_t *entry,
                                             uint8_t friend_index);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_USER_ENTRY_H */
