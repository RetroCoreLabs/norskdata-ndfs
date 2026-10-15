/**
 * Portable shell-style wildcard matching for NDFS names.
 *
 * Supports two wildcards:
 *   '*'  matches any sequence of characters (including the empty sequence)
 *   '?'  matches exactly one character
 *
 * There is no support for character classes ('[...]') or path-separator
 * awareness -- matching is performed over the whole candidate string. The
 * caller is responsible for splitting NDFS paths (e.g. "USER/NAME:TYPE") into
 * the components it wants to match separately.
 *
 * This is implemented in-library (rather than relying on POSIX fnmatch, which
 * is unavailable on MinGW) so all platforms and all language ports behave
 * identically.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_WILDMATCH_H
#define NDFS_WILDMATCH_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Match a string against a shell-style wildcard pattern.
 *
 * @param pattern          Pattern using '*' and '?' wildcards.
 * @param name             Candidate string to test.
 * @param case_insensitive When true, ASCII letters compare without regard
 *                         to case (NDFS names are conventionally uppercase).
 * @return true if @p name matches @p pattern. A NULL pattern or name returns
 *         false. An empty pattern matches only an empty name.
 */
bool ndfs_wildmatch(const char *pattern, const char *name, bool case_insensitive);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_WILDMATCH_H */
