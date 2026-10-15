/**
 * NDFS user friend entry: 16-bit packed permission value.
 *
 * Bit 15:     Entry used
 * Bits 14-13: Reserved
 * Bit 12:     Directory access
 * Bit 11:     Common access
 * Bit 10:     Append access
 * Bit 9:      Write access
 * Bit 8:      Read access
 * Bits 7-0:   Friend user index (0-255)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_USER_FRIEND_H
#define NDFS_USER_FRIEND_H

#include "types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 16-bit packed friend permission entry. */
typedef struct {
    uint16_t bits;
} ndfs_user_friend_t;

/** Create from raw 16-bit value. */
static inline ndfs_user_friend_t ndfs_uf_from_bits(uint16_t bits)
{
    ndfs_user_friend_t uf;
    uf.bits = bits;
    return uf;
}

/** Create with explicit permissions. */
ndfs_user_friend_t ndfs_uf_create(uint8_t friend_user_id,
                                  bool read, bool write,
                                  bool append, bool common,
                                  bool directory);

/** Parse from big-endian bytes at offset. */
ndfs_user_friend_t ndfs_uf_from_bytes(const uint8_t *data, size_t offset);

/** Write to big-endian bytes at offset. */
void ndfs_uf_to_bytes(const ndfs_user_friend_t *uf, uint8_t *data, size_t offset);

static inline bool ndfs_uf_is_active(const ndfs_user_friend_t *uf)
{
    return (uf->bits & (1u << 15)) != 0;
}

static inline bool ndfs_uf_read_access(const ndfs_user_friend_t *uf)
{
    return (uf->bits & (1u << 8)) != 0;
}

static inline bool ndfs_uf_write_access(const ndfs_user_friend_t *uf)
{
    return (uf->bits & (1u << 9)) != 0;
}

static inline bool ndfs_uf_append_access(const ndfs_user_friend_t *uf)
{
    return (uf->bits & (1u << 10)) != 0;
}

static inline bool ndfs_uf_common_access(const ndfs_user_friend_t *uf)
{
    return (uf->bits & (1u << 11)) != 0;
}

static inline bool ndfs_uf_directory_access(const ndfs_user_friend_t *uf)
{
    return (uf->bits & (1u << 12)) != 0;
}

static inline uint8_t ndfs_uf_friend_index(const ndfs_user_friend_t *uf)
{
    return (uint8_t)(uf->bits & 0xFF);
}

/** Set friend with permission bits. */
void ndfs_uf_set_friend(ndfs_user_friend_t *uf,
                        uint8_t friend_user_id,
                        uint8_t permissions);

/** Clear this friend slot. */
static inline void ndfs_uf_clear(ndfs_user_friend_t *uf)
{
    uf->bits = 0;
}

/**
 * Get permission string: "RWACD" or "-----".
 * @param buf  Must be at least 6 bytes.
 */
void ndfs_uf_permission_string(const ndfs_user_friend_t *uf, char *buf);

/**
 * Parse a permission letters string (any case) into the 5-bit permission
 * value used by ndfs_uf_set_friend: R=read, W=write, A=append, C=common,
 * D=directory. '-' is ignored. Returns NDFS_OK, or NDFS_ERR_INVALID_ARG on
 * an unrecognised letter. A NULL or empty string yields 0 permissions.
 */
ndfs_error_t ndfs_uf_parse_permissions(const char *s, uint8_t *out_permissions);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_USER_FRIEND_H */
