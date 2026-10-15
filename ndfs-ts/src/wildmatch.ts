/**
 * Portable shell-style wildcard matching for NDFS names.
 *
 * Supports two wildcards:
 *   '*'  matches any sequence of characters (including the empty sequence)
 *   '?'  matches exactly one character
 *
 * No character classes ('[...]') or path-separator awareness -- matching is
 * over the whole candidate string. Callers split NDFS paths
 * ("USER/NAME:TYPE") into the parts they want to match. Mirrors the C and
 * Python ports so all three libraries behave identically.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

function charEq(a: string, b: string, ci: boolean): boolean {
  if (a === b) return true;
  if (ci) return a.toLowerCase() === b.toLowerCase();
  return false;
}

/**
 * Match `name` against a shell-style wildcard `pattern` using '*' and '?'.
 *
 * @param pattern         Pattern with '*' and '?' wildcards.
 * @param name            Candidate string to test.
 * @param caseInsensitive When true, ASCII letters compare case-insensitively
 *                        (NDFS names are conventionally uppercase).
 * @returns true if `name` matches `pattern`. An empty pattern matches only an
 *          empty name.
 */
export function wildmatch(
  pattern: string,
  name: string,
  caseInsensitive = false,
): boolean {
  let p = 0;
  let n = 0;
  let starP = -1;
  let starN = -1;

  while (n < name.length) {
    if (p < pattern.length &&
        (pattern[p] === '?' || charEq(pattern[p], name[n], caseInsensitive))) {
      p++;
      n++;
    } else if (p < pattern.length && pattern[p] === '*') {
      starP = ++p;
      starN = n;
    } else if (starP >= 0) {
      p = starP;
      n = ++starN;
    } else {
      return false;
    }
  }

  while (p < pattern.length && pattern[p] === '*') p++;
  return p === pattern.length;
}
