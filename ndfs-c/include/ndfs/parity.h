/**
 * ND-100 even parity helpers.
 *
 * The ND-100 uses even parity on text files: bit 7 is set so that the
 * total number of 1-bits in each byte is even.
 *
 * Applies to: :MODE, :SYMB, :TEXT, :C, :BATC, :OUT, :LOG, :LIST, etc.
 * Does NOT apply to: :PROG, :BPUN, :DATA, :VTM (binary files)
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_PARITY_H
#define NDFS_PARITY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Strip even parity: clear bit 7 on every byte (in-place). */
void ndfs_strip_parity(uint8_t *data, size_t len);

/** Set even parity: set bit 7 so total 1-bits per byte is even (in-place). */
void ndfs_set_parity(uint8_t *data, size_t len);

/** Check if a file type string is a text type (parity applies). */
bool ndfs_is_text_type(const char *file_type);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_PARITY_H */
