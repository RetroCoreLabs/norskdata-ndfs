/**
 * NDFS user entry implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/user_entry.h>
#include "endian_util.h"
#include "ndfs_name.h"
#include <string.h>

void ndfs_ue_init(ndfs_user_entry_t *entry)
{
    int i;
    memset(entry, 0, sizeof(*entry));
    /* A fresh entry must still be marked as a used user entry, or the next
     * ndfs_ue_from_bytes reads the slot as free. */
    entry->uf = NDFS_USER_ENTRY_FLAG;
    entry->default_file_access = 0x04FF;
    /* A brand-new user area gets one object block = 256 files, which is what
     * SINTRAN itself does; @GIVE-OBJECT-BLOCKS raises the maximum later. */
    entry->max_object_blocks       = 1;
    entry->allocated_object_blocks = 1;
    for (i = 0; i < NDFS_MAX_FRIENDS; i++) {
        entry->friends[i].bits = 0;
    }
}

ndfs_error_t ndfs_ue_from_bytes(const uint8_t *data, size_t data_len,
                                size_t offset, ndfs_user_entry_t *out)
{
    int i;

    if (!data || !out) return NDFS_ERR_NULL_PTR;
    if (data_len < offset + NDFS_ENTRY_SIZE) return NDFS_ERR_TOO_SMALL;

    /* Check valid user flag */
    if ((data[offset] & NDFS_USER_ENTRY_FLAG) != NDFS_USER_ENTRY_FLAG)
        return NDFS_ERR_NOT_FOUND;

    ndfs_ue_init(out);

    /* Preserve the verbatim 64 bytes so re-serialization never loses fields
     * we do not model (offsets 38-39 and 42-46). Byte 47 IS modelled - see
     * max_object_blocks / allocated_object_blocks below. */
    memcpy(out->raw, data + offset, NDFS_ENTRY_SIZE);
    out->has_raw = true;

    /* Keep the WHOLE flags byte, not just the 0x81 bits tested above. Anything
     * else set here is a flag we do not model yet, and to_bytes writes it
     * straight back out. */
    out->uf                = data[offset];
    out->enter_count       = data[offset + 1];
    ndfs_read_name(data, offset + 2, NDFS_NAME_MAX, out->user_name);
    out->password          = ndfs_read_u16be(data, offset + 18);
    out->date_created      = ndfs_read_u32be(data, offset + 20);
    out->last_date_entered = ndfs_read_u32be(data, offset + 24);
    out->pages_reserved    = ndfs_read_u32be(data, offset + 28);
    out->pages_used        = ndfs_read_u32be(data, offset + 32);
    out->directory_index   = data[offset + 36];
    out->user_index        = data[offset + 37];
    /* Default file access is at offset 40 (verified against on-disk images
     * and the reference ndfs/RetroCore tools); offset 38-39 is unused. */
    out->default_file_access = ndfs_read_u16be(data, offset + 40);

    /* Byte 47: MXOBL/ACOBL, two ZERO-BASED nibbles (see user_entry.h).
     * A default user stores 0x00, meaning 1 block max and 1 allocated = 256
     * files, so +1 on both halves is what turns the raw byte into counts. */
    out->max_object_blocks       = (uint8_t)((data[offset + 47] >> 4) + 1);
    out->allocated_object_blocks = (uint8_t)((data[offset + 47] & 0x0F) + 1);

    /* Parse friends (8 x 2 bytes starting at offset 48) */
    for (i = 0; i < NDFS_MAX_FRIENDS; i++) {
        out->friends[i] = ndfs_uf_from_bytes(data, offset + 48 + (size_t)i * 2);
    }

    return NDFS_OK;
}

void ndfs_ue_to_bytes(const ndfs_user_entry_t *entry, uint8_t *buf)
{
    int i;

    /* Base on the original on-disk bytes when available so unmodelled bytes
     * survive; otherwise start from zero. */
    if (entry->has_raw) {
        memcpy(buf, entry->raw, NDFS_ENTRY_SIZE);
    } else {
        memset(buf, 0, NDFS_ENTRY_SIZE);
    }

    /* Flags byte written verbatim so unmodelled bits survive a read/modify/
     * write. This used to be a hardcoded NDFS_USER_ENTRY_FLAG, which silently
     * cleared every other bit in byte 0 of every user in the page - the
     * user-file writer re-serializes all slots, not just the edited one. */
    buf[0] = entry->uf;
    buf[1] = entry->enter_count;

    ndfs_write_name(buf, 2, entry->user_name, NDFS_NAME_MAX);
    ndfs_write_u16be(buf, 18, entry->password);
    ndfs_write_u32be(buf, 20, entry->date_created);
    ndfs_write_u32be(buf, 24, entry->last_date_entered);
    ndfs_write_u32be(buf, 28, entry->pages_reserved);
    ndfs_write_u32be(buf, 32, entry->pages_used);
    buf[36] = entry->directory_index;
    buf[37] = entry->user_index;
    ndfs_write_u16be(buf, 40, entry->default_file_access);

    /* Byte 47: MXOBL/ACOBL back to zero-based nibbles. Clamped to 1..16 so a
     * caller cannot write a count SINTRAN could not express - 16 blocks x 256
     * files is the version-K ceiling of 4096. */
    {
        uint8_t mx = entry->max_object_blocks;
        uint8_t ac = entry->allocated_object_blocks;
        if (mx < 1) mx = 1;
        if (mx > 16) mx = 16;
        if (ac < 1) ac = 1;
        if (ac > mx) ac = mx;
        buf[47] = (uint8_t)(((mx - 1) << 4) | (ac - 1));
    }

    for (i = 0; i < NDFS_MAX_FRIENDS; i++) {
        ndfs_uf_to_bytes(&entry->friends[i], buf, 48 + (size_t)i * 2);
    }
}

bool ndfs_ue_is_friend(const ndfs_user_entry_t *entry, uint8_t friend_index)
{
    size_t i;
    for (i = 0; i < NDFS_MAX_FRIENDS; i++) {
        if (ndfs_uf_is_active(&entry->friends[i]) &&
            ndfs_uf_friend_index(&entry->friends[i]) == friend_index) {
            return true;
        }
    }
    return false;
}

bool ndfs_ue_add_friend(ndfs_user_entry_t *entry, uint8_t friend_index,
                        uint8_t permissions)
{
    size_t i;
    for (i = 0; i < NDFS_MAX_FRIENDS; i++) {
        if (!ndfs_uf_is_active(&entry->friends[i])) {
            ndfs_uf_set_friend(&entry->friends[i], friend_index, permissions);
            return true;
        }
    }
    return false;
}

bool ndfs_ue_remove_friend(ndfs_user_entry_t *entry, uint8_t friend_index)
{
    size_t i;
    for (i = 0; i < NDFS_MAX_FRIENDS; i++) {
        if (ndfs_uf_is_active(&entry->friends[i]) &&
            ndfs_uf_friend_index(&entry->friends[i]) == friend_index) {
            ndfs_uf_clear(&entry->friends[i]);
            return true;
        }
    }
    return false;
}

const ndfs_user_friend_t *ndfs_ue_get_friend(const ndfs_user_entry_t *entry,
                                             uint8_t friend_index)
{
    size_t i;
    for (i = 0; i < NDFS_MAX_FRIENDS; i++) {
        if (ndfs_uf_is_active(&entry->friends[i]) &&
            ndfs_uf_friend_index(&entry->friends[i]) == friend_index) {
            return &entry->friends[i];
        }
    }
    return NULL;
}
