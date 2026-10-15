"""
Portable shell-style wildcard matching for NDFS names.

Supports two wildcards:
    '*'  matches any sequence of characters (including the empty sequence)
    '?'  matches exactly one character

No character classes ('[...]') or path-separator awareness -- matching is
over the whole candidate string. Callers split NDFS paths ("USER/NAME:TYPE")
into the parts they want to match. Mirrors the C and TypeScript ports so all
three libraries behave identically.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""


def _char_eq(a: str, b: str, ci: bool) -> bool:
    if a == b:
        return True
    if ci:
        return a.lower() == b.lower()
    return False


def wildmatch(pattern: str, name: str, case_insensitive: bool = False) -> bool:
    """Match ``name`` against a shell-style wildcard ``pattern``.

    Args:
        pattern: Pattern using '*' and '?' wildcards.
        name: Candidate string to test.
        case_insensitive: When True, ASCII letters compare without regard to
            case (NDFS names are conventionally uppercase).

    Returns:
        True if ``name`` matches ``pattern``. An empty pattern matches only an
        empty name.
    """
    p = 0
    n = 0
    star_p = -1
    star_n = -1
    plen = len(pattern)
    nlen = len(name)

    while n < nlen:
        if p < plen and (pattern[p] == "?" or _char_eq(pattern[p], name[n], case_insensitive)):
            p += 1
            n += 1
        elif p < plen and pattern[p] == "*":
            p += 1
            star_p = p
            star_n = n
        elif star_p >= 0:
            p = star_p
            star_n += 1
            n = star_n
        else:
            return False

    while p < plen and pattern[p] == "*":
        p += 1
    return p == plen
