/**
 * ND-100 even parity implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "ndfs/parity.h"
#include <string.h>
#include <ctype.h>

static int popcount7(uint8_t b)
{
    int count = 0;
    uint8_t v = b & 0x7F;
    while (v) {
        count += v & 1;
        v >>= 1;
    }
    return count;
}

void ndfs_strip_parity(uint8_t *data, size_t len)
{
    size_t i;
    if (!data) return;
    for (i = 0; i < len; i++) {
        data[i] &= 0x7F;
    }
}

void ndfs_set_parity(uint8_t *data, size_t len)
{
    size_t i;
    if (!data) return;
    for (i = 0; i < len; i++) {
        uint8_t lo7 = data[i] & 0x7F;
        int ones = popcount7(lo7);
        data[i] = (ones % 2 != 0) ? (lo7 | 0x80) : lo7;
    }
}

bool ndfs_is_text_type(const char *file_type)
{
    static const char *text_types[] = {
        "MODE", "SYMB", "TEXT", "C", "BATC", "OUT",
        "LOG", "LIST", "FADM", "BASM", "FORT", "NPL",
        "COBO", "PASC", "PLAN", "BAS", "MAC", "EDIT",
        NULL
    };
    char upper[8];
    size_t i;

    if (!file_type || file_type[0] == '\0') return false;

    for (i = 0; i < sizeof(upper) - 1 && file_type[i]; i++) {
        upper[i] = (char)toupper((unsigned char)file_type[i]);
    }
    upper[i] = '\0';

    for (i = 0; text_types[i] != NULL; i++) {
        if (strcmp(upper, text_types[i]) == 0) return true;
    }
    return false;
}
