/**
 * NDFS name encoding: strings terminated by 0x27 (single quote).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "ndfs_name.h"
#include <ndfs/types.h>
#include <ctype.h>
#include <string.h>

void ndfs_read_name(const uint8_t *data, size_t offset,
                    size_t max_len, char *out)
{
    size_t end = 0;
    size_t i;
    for (i = 0; i < max_len; i++) {
        uint8_t b = data[offset + i];
        if (b == NDFS_NAME_TERMINATOR || b == 0x00) break;
        end++;
    }
    for (i = 0; i < end; i++) {
        out[i] = (char)data[offset + i];
    }
    out[end] = '\0';
}

void ndfs_write_name(uint8_t *data, size_t offset,
                     const char *name, size_t max_len)
{
    size_t name_len = strlen(name);
    size_t len = name_len < max_len ? name_len : max_len;
    size_t i;

    for (i = 0; i < len; i++) {
        data[offset + i] = (uint8_t)(toupper((unsigned char)name[i]) & 0x7F);
    }
    /* SINTRAN writes a single 0x27 terminator after the name (when it fits)
     * followed by NULs -- NOT a field full of terminators. Writing the whole
     * remainder with 0x27 corrupts the on-disk format and can make SINTRAN's
     * directory scan reject the entry. Zero-filling also clears any longer
     * previous name on a rename/overwrite. */
    if (len < max_len) {
        data[offset + len] = NDFS_NAME_TERMINATOR;
        for (i = len + 1; i < max_len; i++) {
            data[offset + i] = 0x00;
        }
    }
}
