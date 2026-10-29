/**
 * SINTRAN III CPU number and CPU type on a system pack: read / change / verify.
 *
 * A SINTRAN pack holds its CPU number twice (kernel symbols SYSNO and FCPUN) and
 * its CPU type once (HWINF+2 = HWINFO(2)) in the resident data page. That page is
 * stored in two disk copies, the segments named S3IDPIT (the image a warm start
 * loads) and S3SDPIT (the save copy a cold start loads and copies over the image).
 * The live copy S3DPIT exists only in memory. ND's own install run patches S3SDPIT
 * with DMAC and asks for a cold start; this module patches both disk copies so
 * either kind of start sees the new value.
 *
 * NOTHING HERE IS A FIXED ADDRESS OR A FIXED SEGMENT NUMBER. Every value comes from
 * the pack itself, so the same code serves every SINTRAN generation:
 *  - the word addresses SYSNO, HWINF, REVLE and FCPUN from the pack's own
 *    (SYSTEM)SYMBOL-1-LIST:SYMB and SYMBOL-2-LIST:SYMB (FCPUN moves with every
 *    generation: 105034B on one K pack, 004414B on L07/M06, 107017B on another L);
 *  - the segment numbers of S3IDPIT and S3SDPIT from the RT-loader's "DEFINED
 *    SEGMENT NAME" records (type 25B) in (SYSTEM)RTFIL:DATA, or from the copies
 *    of that table inside SEGFIL0:DATA when the file is missing;
 *  - the segment's logical base, length, disk page and segment file from its
 *    segment-table entry in SEGFIL0:DATA (the self-validating table page of
 *    sintran.h); segment file 0 is SEGFIL0:DATA, segment file 4 is SINTRAN:DATA
 *    (the K release information: segments 52-60 and 63 live there).
 *
 * The generation symbols 33CPN / 33CPU in SYMBOL-2-LIST are updated too, so a
 * later ND patch run (`SYSNO/ 33CPN`) keeps the new number.
 *
 * The directory keeps the system number of the machine that last entered it in
 * page 0 (Extended Info Block word 5, flag word bit 15). The kernel routine CHDSI
 * compares it with SYSNO and refuses the enter with error 3203 "DIRECTORY
 * ENTERED BY ANOTHER SYSTEM" when they differ, so a renamed pack has to have the
 * stamp cleared as well (ndfs_release_directory).
 *
 * Full derivation, measurements and sources: NDInsight chapter
 * SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md and chapter 22 section 6.5 (the
 * name records), sibling repository.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_SINTRAN_IDENTITY_H
#define NDFS_SINTRAN_IDENTITY_H

#include "types.h"
#include "filesystem.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The two disk copies of the resident data page, by their RT-loader names. */
#define NDFS_SEGNAME_IMAGE_DPIT "S3IDPIT"
#define NDFS_SEGNAME_SAVE_DPIT  "S3SDPIT"

/** Symbol type of a "DEFINED SEGMENT NAME" record in the RT-loader table (octal 25). */
#define NDFS_RTFIL_TYPE_SEGMENT_NAME 025

/** Longest packed segment name: 8 SIXBIT characters in 48 bits. */
#define NDFS_SEGNAME_MAX 8

/** "USER/NAME:TYPE" with room for the separators. */
#define NDFS_IDENTITY_PATH_MAX (NDFS_NAME_MAX * 2 + 8)

/** Kernel symbols read from the pack's symbol lists. -1 = not defined there. */
typedef struct {
    int sysno;      /**< The CPU number word. */
    int hwinf;      /**< The HWINFO table; the CPU type is HWINF+2. */
    int revle;      /**< The patch level word. */
    int fcpun;      /**< The second CPU number word (SYMBOL-2-LIST on K, SYMBOL-1-LIST on L and M). */
    int cpn33;      /**< 33CPN: the CPU number NEW-SYSTEM was given (SYMBOL-2-LIST). */
    int cpu33;      /**< 33CPU: the CPU type NEW-SYSTEM was given (SYMBOL-2-LIST). */
    int inibu;      /**< INIBU, used only to name the version letter. */
} ndfs_sintran_symbols_t;

/** One disk copy of the resident data page. */
typedef struct {
    char     name[NDFS_SEGNAME_MAX + 1]; /**< "S3IDPIT" or "S3SDPIT". */
    const char *role;           /**< "image" (warm start) or "save" (cold start). */
    int      segment_number;    /**< From the name record; -1 when the name is not on the pack. */
    bool     present;           /**< Segment-table entry found, with a disk address, covering SYSNO. */
    int      logad, segle, madr, flag;
    int      segment_file;      /**< (flag >> 13) & 7: 0 = SEGFIL0:DATA, 4 = SINTRAN:DATA. */
    char     file_path[NDFS_IDENTITY_PATH_MAX]; /**< The NDFS file that holds the copy. */
    bool     file_found;        /**< That file exists and is long enough for the copy. */
    long     sysno_offset;      /**< Byte offsets inside file_path; -1 when outside the segment. */
    long     fcpun_offset;
    long     type_offset;
    long     revle_offset;
    int      sysno;             /**< Words read at those offsets; -1 when unreadable. */
    int      fcpun;
    int      cpu_type;
    int      revle;
} ndfs_dpit_copy_t;

/** Everything the reader found. */
typedef struct {
    char     version;           /**< 'K', 'L', 'M' from INIBU, '?' when unknown. */
    ndfs_sintran_symbols_t sym;
    char     symbol1_path[NDFS_IDENTITY_PATH_MAX];
    char     symbol2_path[NDFS_IDENTITY_PATH_MAX];
    char     names_path[NDFS_IDENTITY_PATH_MAX]; /**< Where the segment names were read. */
    long     cpn33_value_offset; /**< Byte offset of the 33CPN octal digits in symbol2_path; -1 if none. */
    long     cpu33_value_offset; /**< Same for 33CPU. */
    int      cpn33_value_width;  /**< Digits available to rewrite in place. */
    int      cpu33_value_width;
    bool     symbol2_has_parity; /**< The list is stored with bit 7 set (even parity). */
    int      segment_table_page; /**< Page of the segment table inside SEGFIL0:DATA. */
    ndfs_dpit_copy_t copies[2]; /**< [0] = image copy, [1] = save copy. */
    bool     dir_ext_valid;      /**< Extended Info Block present with a good checksum. */
    bool     dir_entered;        /**< Flag word bit 15. */
    uint16_t dir_system_number;  /**< Extended Info Block word 5. */
} ndfs_cpu_identity_t;

/**
 * Read the CPU identity of a SINTRAN system pack.
 * @return NDFS_OK when the symbol lists, the segment names and SEGFIL0 were found and
 *         SYSNO and FCPUN are defined; a copy may still be marked not present or its
 *         file not found, and the caller must look at those flags.
 *         NDFS_ERR_NOT_FOUND when the pack has no symbol lists, no FCPUN symbol, no
 *         segment name records or no SEGFIL0 (not a SINTRAN system pack, or one this
 *         module cannot address safely). What was found so far is still filled in.
 */
ndfs_error_t ndfs_read_cpu_identity(const ndfs_filesystem_t *fs, ndfs_cpu_identity_t *out);

/**
 * Change the CPU number, and optionally the CPU type, of a SINTRAN system pack.
 *
 * Writes @p cpu_number at SYSNO and FCPUN and @p cpu_type (when >= 0) at HWINF+2 in
 * BOTH disk copies of the resident data page, rewrites 33CPN (and 33CPU) in
 * SYMBOL-2-LIST in place, and unless @p keep_stamp clears the page-0 "entered by
 * system N" stamp. Then reads everything back and compares.
 *
 * Refuses (NDFS_ERR_NOT_FOUND) when a copy is missing, its file is too short or a
 * word falls outside the segment, so a pack is never left half changed.
 * NDFS_ERR_INVALID_ARG for values outside 0..65535. NDFS_ERR_READ_ONLY on a
 * read-only image. NDFS_ERR_CORRUPT when the read-back does not show the values
 * written.
 *
 * @param out_after  Optional; receives the identity as read back after the write.
 */
ndfs_error_t ndfs_write_cpu_identity(ndfs_filesystem_t *fs, int cpu_number, int cpu_type,
                                     bool keep_stamp, ndfs_cpu_identity_t *out_after);

/* ── Pure helpers (exposed for tests) ────────────────────────────────── */

/**
 * Find `NAME=OOOOOO` in a symbol list (parity already stripped). Leading blanks
 * before NAME are allowed; NAME must be at the start of a line.
 * @param out_value         The octal value.
 * @param out_value_offset  Byte offset of the first digit, or NULL.
 * @param out_value_width   Number of digits, or NULL.
 * @return true when found.
 */
bool ndfs_sintran_find_symbol(const uint8_t *text, size_t len, const char *name,
                              int *out_value, size_t *out_value_offset, int *out_value_width);

/**
 * Decode a packed RT-loader name: 48 bits, SIXBIT (ASCII & 0x3F), right-justified
 * base-64, at most 8 characters. @p out must hold NDFS_SEGNAME_MAX + 1 bytes.
 * @return true when every character is A-Z or 0-9 and the name is 1..8 long.
 */
bool ndfs_sintran_decode_packed_name(uint16_t w0, uint16_t w1, uint16_t w2,
                                     char *out, size_t out_size);

/**
 * Scan a buffer (RTFIL:DATA, or SEGFIL0:DATA) for the "DEFINED SEGMENT NAME" record
 * of @p name: 8 big-endian words, word 5 = 25B, word 3 = word 6 = the segment
 * number (1..255), words 4 and 7 = 0, words 0-2 the packed name.
 * @return the segment number, or -1 when not found.
 */
int ndfs_sintran_find_segment_by_name(const uint8_t *buf, size_t size, const char *name);

/**
 * Byte offset of logical word address @p addr inside a segment's file.
 * `madr * 2048 + 2 * (addr - (logad & 077) * 1024)`. Returns -1 when the address
 * is outside the segment (segle pages from its base).
 */
long ndfs_sintran_segment_word_offset(int logad, int segle, int madr, int addr);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_SINTRAN_IDENTITY_H */
