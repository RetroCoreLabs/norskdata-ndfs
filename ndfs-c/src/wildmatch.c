/**
 * Portable shell-style wildcard matching ('*' and '?').
 *
 * Iterative matcher with backtracking on '*' -- O(n*m) worst case, no
 * recursion, no allocation. Mirrors the TypeScript and Python ports so the
 * three libraries match identically.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "ndfs/wildmatch.h"
#include <ctype.h>
#include <stddef.h>

static bool char_eq(char a, char b, bool ci)
{
    if (a == b) return true;
    if (ci) {
        return tolower((unsigned char)a) == tolower((unsigned char)b);
    }
    return false;
}

bool ndfs_wildmatch(const char *pattern, const char *name, bool case_insensitive)
{
    const char *star_pat = NULL;   /* pattern position just after the last '*' */
    const char *star_name = NULL;  /* name position when that '*' was seen */

    if (!pattern || !name) return false;

    while (*name) {
        if (*pattern == '?' || char_eq(*pattern, *name, case_insensitive)) {
            /* Direct (or single-char wildcard) match: advance both. */
            pattern++;
            name++;
        } else if (*pattern == '*') {
            /* Record backtrack point; '*' initially absorbs nothing. */
            star_pat = ++pattern;
            star_name = name;
        } else if (star_pat) {
            /* Mismatch, but an earlier '*' can absorb one more char. */
            pattern = star_pat;
            name = ++star_name;
        } else {
            return false;
        }
    }

    /* Name exhausted: any trailing pattern must be all '*'. */
    while (*pattern == '*') pattern++;
    return *pattern == '\0';
}
