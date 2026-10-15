/**
 * NDFS user friend entry implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/user_friend.h>
#include "endian_util.h"

ndfs_user_friend_t ndfs_uf_create(uint8_t friend_user_id,
                                  bool read, bool write,
                                  bool append, bool common,
                                  bool directory)
{
    ndfs_user_friend_t uf;
    uint16_t bits = (uint16_t)((friend_user_id & 0xFF) | (1u << 15));
    if (read)      bits |= (1u << 8);
    if (write)     bits |= (1u << 9);
    if (append)    bits |= (1u << 10);
    if (common)    bits |= (1u << 11);
    if (directory) bits |= (1u << 12);
    uf.bits = bits;
    return uf;
}

ndfs_user_friend_t ndfs_uf_from_bytes(const uint8_t *data, size_t offset)
{
    ndfs_user_friend_t uf;
    uf.bits = ndfs_read_u16be(data, offset);
    return uf;
}

void ndfs_uf_to_bytes(const ndfs_user_friend_t *uf, uint8_t *data, size_t offset)
{
    ndfs_write_u16be(data, offset, uf->bits);
}

void ndfs_uf_set_friend(ndfs_user_friend_t *uf,
                        uint8_t friend_user_id,
                        uint8_t permissions)
{
    uf->bits = (uint16_t)((friend_user_id & 0xFF) |
                          (1u << 15) |
                          ((permissions & 0x1F) << 8));
}

void ndfs_uf_permission_string(const ndfs_user_friend_t *uf, char *buf)
{
    if (!ndfs_uf_is_active(uf)) {
        buf[0] = '-'; buf[1] = '-'; buf[2] = '-';
        buf[3] = '-'; buf[4] = '-'; buf[5] = '\0';
        return;
    }
    buf[0] = ndfs_uf_read_access(uf)      ? 'R' : '-';
    buf[1] = ndfs_uf_write_access(uf)     ? 'W' : '-';
    buf[2] = ndfs_uf_append_access(uf)    ? 'A' : '-';
    buf[3] = ndfs_uf_common_access(uf)    ? 'C' : '-';
    buf[4] = ndfs_uf_directory_access(uf) ? 'D' : '-';
    buf[5] = '\0';
}

ndfs_error_t ndfs_uf_parse_permissions(const char *s, uint8_t *out_permissions)
{
    uint8_t p = 0;
    if (!out_permissions) return NDFS_ERR_NULL_PTR;
    *out_permissions = 0;
    if (!s) return NDFS_OK;

    for (; *s; s++) {
        switch (*s) {
        case 'R': case 'r': p |= 0x01; break; /* -> bit 8 read      */
        case 'W': case 'w': p |= 0x02; break; /* -> bit 9 write     */
        case 'A': case 'a': p |= 0x04; break; /* -> bit 10 append   */
        case 'C': case 'c': p |= 0x08; break; /* -> bit 11 common   */
        case 'D': case 'd': p |= 0x10; break; /* -> bit 12 directory*/
        case '-': case ' ': break;            /* ignore separators  */
        default: return NDFS_ERR_INVALID_ARG;
        }
    }
    *out_permissions = p;
    return NDFS_OK;
}
