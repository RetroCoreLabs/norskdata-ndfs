/**
 * SINTRAN III segments on a system pack: list, export, import.
 *
 * SINTRAN keeps its swappable code and data in SEGMENTS, numbered 1-255, each
 * described by an 8-word entry of the segment table (itself the segment pair
 * S3ISGT / S3SSGT inside SEGFIL0:DATA): SEGLI (link), PRESE, LOGAD (logical base
 * page in the low 6 bits), SEGLE (length in 2 KB pages), MADR (page offset inside
 * its SEGMENT FILE), FLAG (segment-file number in bits 13-15, 5OK in bit 0),
 * SGSTA (ring and protection) and BPAGL (core-map link). A segment with SEGLE 0
 * has no pages and lives in memory only (S3DPIT, S3RPIT, S3MPIT); MADR 0 is a real
 * offset, the first page of the segment file (S3IMAGE on file 0, S3SAVE on file 4).
 *
 * The NAMES are not in the table. They are "DEFINED SEGMENT NAME" records of the
 * RT-loader (type 25B) in (SYSTEM)RTFIL:DATA and in its segment copy inside
 * SEGFIL0 (see sintran_identity.h, chapter 22 section 6.5 of NDInsight).
 *
 * A SEGMENT FILE is a contiguous NDFS file (`@DEFINE-SEGMENT-FILE <memory?>
 * <save?> <number 0-3> <file>`). SINTRAN does not address it by name: the kernel
 * keeps, per segment file N, the disk address DBLST(N) in disk blocks and
 * ABLPAGE(N) blocks per page, plus MASSNO(N), the logical device of the disk it
 * is on. The disk page of page P of a segment is
 *
 *     DBLST(N) / ABLPAGE(N) + MADR + P
 *
 * (IP-P2-SEGADM.NPL RWSEGM: `DBLST(X)=:BLSTX`, `ABLPAGE(X)=:CABLPAGE`). Those
 * tables sit in the resident data page, so they are read here from the S3IDPIT
 * copy through the pack's own symbol list (BLST, ABLPA, MASSN). Measured on K and
 * L packs: DBLST(0) = 256 blocks, ABLPAGE(0) = 2, so SEGFIL0 starts at disk page
 * 128, where SEGFIL0:DATA begins. Segment file 4 is the "absolute save" file for
 * segments 52-60 and 63; its base is set at start-up to ABLPAGE(0) blocks = one
 * page (PH-P2-OPPSTART.NPL:751), measured as disk page 1 + MADR on the K packs,
 * which is page MADR of SINTRAN:DATA. Segment files 1-3 are defined by the
 * installer; on the packs measured so far they were unused (DBLST 0).
 *
 * Nothing here is a fixed number: every value is read from the pack. When the
 * symbol list is missing, segment file 0 falls back to the first disk page of
 * SEGFIL0:DATA and the others are reported as unknown.
 *
 * Export copies the pages of one segment to a host buffer; import writes a host
 * buffer over them (same length or shorter). Both refuse a memory-only segment,
 * an undefined segment file, one on another disk unit, or pages past the image.
 * Import does not touch the segment table, so a shorter import leaves the tail
 * pages as they were. Importing only an image copy (S3Ixxx) is undone by the next
 * cold start; a save copy (S3Sxxx) is what a cold start loads.
 *
 * Derivation: NDInsight chapters SINTRAN/OS/11, 22 and 28 (sibling repository).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_SINTRAN_SEGMENTS_H
#define NDFS_SINTRAN_SEGMENTS_H

#include "types.h"
#include "filesystem.h"
#include "sintran_identity.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Segment files 0-4 (the kernel tables have five entries). */
#define NDFS_SEGFILE_COUNT 5

/** One segment file as the kernel sees it. */
typedef struct {
    bool     defined;           /**< A disk address is known. */
    uint32_t dblst;             /**< DBLST(N): start in disk blocks, as read. */
    int      blocks_per_page;   /**< ABLPAGE(N), or ABLPAGE(0) when N's own word is 0. */
    int      massno;            /**< MASSNO(N): logical device number of its disk. */
    bool     on_this_disk;      /**< MASSNO equals MASSNO(0), or is 0. */
    long     base_page;         /**< Disk page where segment page 0 of MADR 0 would be; -1 unknown. */
    const char *source;         /**< "DBLST", "start-up rule (OPPSTART:751)", "SEGFIL0:DATA file" ... */
    char     file_path[NDFS_IDENTITY_PATH_MAX]; /**< NDFS file whose first page is base_page, or "". */
} ndfs_segfile_t;

/** One segment-table entry with its name and disk place. */
typedef struct {
    int      number;            /**< 1-255. */
    char     name[NDFS_SEGNAME_MAX + 1]; /**< RT-loader name, or "" when no record names it. */
    int      segli, prese, logad, segle, madr, flag, sgsta, bpagl;
    int      segfile;           /**< (flag >> 13) & 7. */
    bool     ok5;               /**< flag bit 0. */
    bool     memory_only;       /**< SEGLE 0: no pages, no disk copy. */
    long     disk_page;         /**< First disk page of the segment (needs 5OK and a known segment file), or -1. */
} ndfs_segment_t;

/** The whole picture of a pack's segments. */
typedef struct {
    char     version;           /**< 'K', 'L', 'M' from INIBU, '?' when unknown. */
    int      segment_table_page;/**< Page inside SEGFIL0:DATA. */
    char     segfil_path[NDFS_IDENTITY_PATH_MAX];
    char     names_path[NDFS_IDENTITY_PATH_MAX];
    bool     have_symbols;      /**< BLST / ABLPA / MASSN were found in the symbol list. */
    int      blst_sym, ablpa_sym, massn_sym;
    ndfs_segfile_t segfiles[NDFS_SEGFILE_COUNT];
    ndfs_segment_t segments[256];
    int      count;             /**< Entries with SEGLE > 0 or a name. */
} ndfs_segment_list_t;

/**
 * Read the segment table, the names and the segment-file tables of a pack.
 * @return NDFS_OK, or NDFS_ERR_NOT_FOUND when there is no SEGFIL0 with a segment table.
 *         What was found is filled in either way.
 */
ndfs_error_t ndfs_list_segments(const ndfs_filesystem_t *fs, ndfs_segment_list_t *out);

/**
 * Find a segment by RT-loader name (case-insensitive) or by octal number with a
 * trailing B ("53B"), or decimal without. @return index into segments[], or -1.
 */
int ndfs_find_segment(const ndfs_segment_list_t *list, const char *name_or_number);

/**
 * Copy a segment's pages (SEGLE * NDFS_PAGE_SIZE bytes) into a malloc'd buffer
 * (free with ndfs_free_data). NDFS_ERR_NOT_FOUND for a memory-only segment or an
 * unknown disk place, NDFS_ERR_OUT_OF_RANGE past the image.
 */
ndfs_error_t ndfs_export_segment(const ndfs_filesystem_t *fs, const ndfs_segment_list_t *list,
                                 int index, uint8_t **out_data, size_t *out_size);

/**
 * Write @p size bytes over a segment's pages, starting at its first page. @p size
 * must not exceed SEGLE * NDFS_PAGE_SIZE; a last partial page keeps its remaining
 * bytes. Same refusals as export, plus NDFS_ERR_OUT_OF_RANGE for a longer buffer
 * and NDFS_ERR_READ_ONLY. Nothing is written when any check fails.
 */
ndfs_error_t ndfs_import_segment(ndfs_filesystem_t *fs, const ndfs_segment_list_t *list,
                                 int index, const uint8_t *data, size_t size);

/**
 * Fill @p names[1..255] from every "DEFINED SEGMENT NAME" record in @p buf
 * (first record wins). Entries with no record are left as "".
 * @return the number of names found.
 */
int ndfs_sintran_collect_segment_names(const uint8_t *buf, size_t size,
                                       char names[256][NDFS_SEGNAME_MAX + 1]);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_SINTRAN_SEGMENTS_H */
