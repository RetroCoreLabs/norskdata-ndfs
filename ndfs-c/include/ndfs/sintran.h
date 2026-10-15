/**
 * SINTRAN III initial-command buffer: locate / read / write / repair.
 *
 * The initial commands are the list SINTRAN runs first at every restart
 * (@LIST-INITIAL-COMMANDS). The buffer has no file form: it lives at the kernel
 * symbol INIBU inside the SINTRAN command segment, whose disk image is stored in
 * the (SYSTEM)SEGFIL0:DATA object file. It is located WITHOUT byte-searching,
 * from the INIBU symbol value plus the segment table read off SEGFIL0 itself.
 *
 * Full derivation + evidence: docs/SINTRAN-INITIAL-COMMANDS-SPEC.md.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_SINTRAN_H
#define NDFS_SINTRAN_H

#include "types.h"
#include "filesystem.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Region layout (identical in K03/L07/M06):
 *   INIBU+0..+129  command text  (260 bytes)
 *   INIBU+130      length cell    (byte count of the text)
 *   INIBU+131      INCOM          (executable code - never written)               */
#define NDFS_INITCMD_CAP_WORDS        131
#define NDFS_INITCMD_CAP_BYTES        262
#define NDFS_INITCMD_LEN_CELL_BYTES   260
/* Max text SINTRAN itself accepts (literal 252 in the NEXIN handler -> (252+2)&~1). */
#define NDFS_INITCMD_MAX_TEXT_BYTES   254

/** Decoded initial-command buffer and where it was found in SEGFIL0. */
typedef struct {
    char      version;             /**< SINTRAN version letter: 'K', 'L' or 'M'. */
    int       segment_number;      /**< Segment-table entry holding the live command segment. */
    uint32_t  madr;                /**< MADR (page offset of the segment in SEGFIL0). */
    long      byte_offset;         /**< Byte offset of INIBU within SEGFIL0:DATA. */
    int       segment_table_page;  /**< SEGFIL0 page where the segment table was found. */
    int       text_length;         /**< Length-cell value (text bytes, excl. terminator). */
    long      length_cell_offset;  /**< byte_offset + 260. */
    char    **commands;            /**< Malloc'd array of malloc'd command strings. */
    size_t    command_count;
} ndfs_initial_commands_t;

/** Free the contents of a result filled by ndfs_read_initial_commands(). */
void ndfs_initial_commands_free(ndfs_initial_commands_t *ic);

/**
 * Locate and decode the SINTRAN initial-command buffer on this pack.
 * @return NDFS_OK, or NDFS_ERR_NOT_FOUND when the pack is not a SINTRAN system
 *         disk (no SEGFIL0) or the buffer cannot be located.
 */
ndfs_error_t ndfs_read_initial_commands(const ndfs_filesystem_t *fs,
                                        ndfs_initial_commands_t *out);

/**
 * Replace the entire initial-command buffer with @p commands and write it back
 * to SEGFIL0 in place. Commands are upper-cased and validated (printable ASCII,
 * no embedded '); the encoded text must fit NDFS_INITCMD_MAX_TEXT_BYTES.
 * @return NDFS_OK, NDFS_ERR_NOT_FOUND (no locatable buffer), NDFS_ERR_READ_ONLY,
 *         NDFS_ERR_INVALID_ARG (bad command), NDFS_ERR_OUT_OF_RANGE (overflow).
 */
ndfs_error_t ndfs_write_initial_commands(ndfs_filesystem_t *fs,
                                         const char *const *commands,
                                         size_t count);

/**
 * Repair a buffer whose header (length cell / first bytes) was clobbered so the
 * normal locator can no longer see it. Finds the buffer by matching the surviving
 * command text (requires a single unambiguous winner) and writes @p commands
 * there. Refuses (NDFS_ERR_NOT_FOUND) when no clear target is found rather than
 * risk patching SINTRAN code.
 * @param out_byte_offset  Optional; receives the SEGFIL0 offset written.
 * @param out_score        Optional; receives the winning keyword match score.
 */
ndfs_error_t ndfs_repair_initial_commands(ndfs_filesystem_t *fs,
                                          const char *const *commands,
                                          size_t count,
                                          long *out_byte_offset,
                                          int *out_score);

/** One INIBU-containing segment candidate (diagnostic). */
typedef struct {
    char      version;
    int       segment_number;
    uint32_t  madr;
    long      byte_offset;
    int       length_cell;
    bool      length_plausible;
    bool      consistent;          /**< Self-consistent (re-encode == stored length) = a REAL buffer. */
    char    **commands;            /**< Parsed commands, or NULL when it does not decode. */
    size_t    command_count;
} ndfs_initcmd_candidate_t;

/**
 * Enumerate EVERY INIBU-containing segment candidate in SEGFIL0 (all versions,
 * all matching segments), in the order the reader scans them. Diagnostic aid:
 * the reader picks the first `consistent` candidate.
 */
ndfs_error_t ndfs_diagnose_initial_commands(const ndfs_filesystem_t *fs,
                                            ndfs_initcmd_candidate_t **out,
                                            size_t *out_count);

/** Free an array returned by ndfs_diagnose_initial_commands(). */
void ndfs_free_initcmd_candidates(ndfs_initcmd_candidate_t *candidates, size_t count);

/* ── Pure helpers (operate on a SEGFIL0 byte buffer; exposed for testing) ── */

/**
 * Locate the self-describing segment-table page inside a SEGFIL0 buffer.
 * @return the page index, or -1 if not found.
 */
int ndfs_sintran_find_segment_table_page(const uint8_t *seg, size_t size);

/**
 * Parse a buffer region: text + 0x27, word-aligned; a lone 0x27 ends it.
 * @param out_commands  Malloc'd array of malloc'd strings (free with
 *                      ndfs_sintran_free_commands). NULL/0 on a non-buffer.
 * @return NDFS_OK (parsed, possibly zero commands) or NDFS_ERR_CORRUPT
 *         (non-printable payload -> not a real buffer).
 */
ndfs_error_t ndfs_sintran_parse_buffer(const uint8_t *seg, size_t size,
                                        size_t off, size_t cap_bytes,
                                        char ***out_commands, size_t *out_count);

/**
 * Encode a command list into buffer bytes. @p out_bytes is malloc'd (free with
 * ndfs_free_data). @p out_text_len receives the length-cell value (text bytes,
 * EXCLUDING the terminator).
 * @return NDFS_OK, NDFS_ERR_INVALID_ARG (bad command),
 *         NDFS_ERR_OUT_OF_RANGE (text exceeds @p max_text_bytes).
 */
ndfs_error_t ndfs_sintran_encode_buffer(const char *const *commands, size_t count,
                                        int max_text_bytes,
                                        uint8_t **out_bytes, size_t *out_len,
                                        int *out_text_len);

/**
 * True when re-encoding @p commands reproduces exactly @p stored_text_len bytes,
 * i.e. the parsed commands are a self-consistent round trip (a REAL buffer), not
 * a coincidental parse of unrelated bytes.
 */
bool ndfs_sintran_reencode_matches(const char *const *commands, size_t count,
                                   int stored_text_len);

/** Free a command-string array returned by the helpers above. */
void ndfs_sintran_free_commands(char **commands, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_SINTRAN_H */
