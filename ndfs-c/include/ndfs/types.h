/**
 * NDFS (Norsk Data File System) types and constants.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_TYPES_H
#define NDFS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ─────────────────────────────────────────────────── */

typedef int ndfs_error_t;

#define NDFS_OK                  0
#define NDFS_ERR_NULL_PTR       -1
#define NDFS_ERR_INVALID_ARG    -2
#define NDFS_ERR_TOO_SMALL      -3
#define NDFS_ERR_NOT_ALIGNED    -4
#define NDFS_ERR_INVALID_IMAGE  -5
#define NDFS_ERR_OUT_OF_RANGE   -6
#define NDFS_ERR_NOT_FOUND      -7
#define NDFS_ERR_ALREADY_EXISTS -8
#define NDFS_ERR_NO_SPACE       -9
#define NDFS_ERR_READ_ONLY      -10
#define NDFS_ERR_NO_SLOTS       -11
#define NDFS_ERR_HAS_FILES      -12
#define NDFS_ERR_ALLOC          -13
#define NDFS_ERR_CORRUPT        -14
#define NDFS_ERR_IO             -15   /* block backend read/write failed */

/* Object-block limits. A user holds 256 files per object block, one by default
 * and up to NDFS_MAX_OBJECT_BLOCKS. These two distinguish the limit an operator
 * CAN lift from the one nobody can, so a copy loop can print the right advice:
 *
 *   NDFS_ERR_OBJ_BLOCKS_FULL  every block the user is ALLOWED is full, but the
 *                             maximum is still below 16 - grant more with
 *                             ndfs_fs_give_object_blocks().
 *   NDFS_ERR_OBJ_BLOCKS_MAX   the user already has all 16 blocks (4096 files).
 *                             Nothing can be granted; delete files instead.
 *
 * See docs/NDFS-OBJECT-BLOCKS-SPEC.md.
 */
#define NDFS_ERR_OBJ_BLOCKS_FULL -16
#define NDFS_ERR_OBJ_BLOCKS_MAX  -17

/* Growing a user into another object block would land on another user's pages:
 * user U's block n occupies exactly the pages of user (n*64 + U)'s FIRST block.
 * See docs/NDFS-OBJECT-BLOCKS-SPEC.md section 6. */
#define NDFS_ERR_OBJ_BLOCK_COLLISION -18

/* ── Constants ───────────────────────────────────────────────────── */

/** Page size in bytes (1024 x 16-bit words). */
#define NDFS_PAGE_SIZE            2048

/** Name terminator character (ASCII single quote). */
#define NDFS_NAME_TERMINATOR      0x27

/** Maximum length of a directory/user/object name. */
#define NDFS_NAME_MAX             16

/** Maximum length of a file type string. */
#define NDFS_TYPE_MAX             4

/** Number of entries (user or object) per page. */
#define NDFS_ENTRIES_PER_PAGE     32

/** Size of a single entry (user or object) in bytes. */
#define NDFS_ENTRY_SIZE           64

/** Maximum number of pointers in the user file index block. */
#define NDFS_MAX_USER_FILE_PTRS   8

/** Maximum number of pointers in an object file index block. */
#define NDFS_MAX_OBJECT_FILE_PTRS 512

/** Maximum number of users. */
#define NDFS_MAX_USERS            256

/* ── Object blocks: how a user area holds more than 256 files ──────────────
 *
 * A user's file entries live in OBJECT BLOCKS. One block is 8 object-file pages
 * of 32 entries = 256 files. A user gets one block when created and may be
 * granted up to 16, giving the SINTRAN version-K ceiling of 4096 files. The
 * counts live in the two zero-based nibbles of user-entry byte 47 (MXOBL high,
 * ACOBL low), so a default user stores 0x00.
 *
 * Block n for user U occupies object-file pages n*512 + U*8 .. +7, i.e.
 * successive blocks of the same user are one whole index block apart.
 *
 * VERIFIED 2026-08-01 on a live SINTRAN III K pack. See
 * docs/NDFS-OBJECT-BLOCKS-SPEC.md.
 */

/** Object-file pages in one object block. */
#define NDFS_PAGES_PER_OBJECT_BLOCK 8

/** Files held by one object block - 256. */
#define NDFS_FILES_PER_OBJECT_BLOCK (NDFS_PAGES_PER_OBJECT_BLOCK * NDFS_ENTRIES_PER_PAGE)

/** Page pointers in one index block, and the stride between a user's blocks. */
#define NDFS_PAGES_PER_INDEX_BLOCK  512

/** Users whose FIRST object block fits in a single index block - 64. */
#define NDFS_USERS_PER_INDEX_BLOCK  (NDFS_PAGES_PER_INDEX_BLOCK / NDFS_PAGES_PER_OBJECT_BLOCK)

/** The SINTRAN version-K ceiling: 16 object blocks = 4096 files per user. */
#define NDFS_MAX_OBJECT_BLOCKS      16

/** Maximum number of friends per user. */
#define NDFS_MAX_FRIENDS          8

/** Byte offset of the master block within page 0. */
#define NDFS_MASTER_BLOCK_OFFSET  2016

/** Byte offset of the extended info block within page 0. */
#define NDFS_EXTENDED_INFO_OFFSET 2000

/** Size of the master block in bytes. */
#define NDFS_MASTER_BLOCK_SIZE    32

/** Size of the extended info block in bytes. */
#define NDFS_EXTENDED_INFO_SIZE   16

/** First block ID available for allocation (0-6 are system). */
#define NDFS_FIRST_ALLOC_BLOCK    7

/** Valid user entry flag byte. */
#define NDFS_USER_ENTRY_FLAG      0x81

/** Valid object entry header bit (bit 7). */
#define NDFS_OBJECT_IN_USE        0x80

/* ── Enums ───────────────────────────────────────────────────────── */

/** Block pointer type encoded in the top 2 bits. */
typedef enum {
    NDFS_PTR_CONTIGUOUS  = 0,
    NDFS_PTR_INDEXED     = 1,
    NDFS_PTR_SUBINDEXED  = 2,
    NDFS_PTR_RESERVED    = 3
} ndfs_pointer_type_t;

/** Boot code format detected in page 0. */
typedef enum {
    NDFS_BOOT_NONE   = 0,
    NDFS_BOOT_BINARY = 1,
    NDFS_BOOT_BPUN   = 2,
    NDFS_BOOT_FLOMON = 3
} ndfs_boot_format_t;

/**
 * Hard-disk controller family identified from the IOX/IOXT instructions in a
 * raw-binary bootstrap. Only meaningful when ndfs_boot_format_t is NDFS_BOOT_BINARY;
 * BPUN/FLOMON boots are always floppy media and are not classified this way.
 */
typedef enum {
    NDFS_CONTROLLER_UNKNOWN    = 0,
    NDFS_CONTROLLER_SMD_ECC    = 1,
    NDFS_CONTROLLER_WINCHESTER = 2,
    NDFS_CONTROLLER_SCSI       = 3,
    NDFS_CONTROLLER_FLOPPY     = 4
} ndfs_boot_controller_type_t;

/** Access level for a user relative to a file. */
typedef enum {
    NDFS_ACCESS_OWN    = 0,
    NDFS_ACCESS_FRIEND = 1,
    NDFS_ACCESS_PUBLIC = 2
} ndfs_file_access_type_t;

/** Checksum validation state for extended info. */
/* Extended-info checksum state.
 *
 * NOTE: there is deliberately NO "valid low byte only" state. The SINTRAN kernel writes
 * and compares the FULL 16-bit checksum (writer WXDIR 37702B, validator CHDSI 37763B);
 * accepting a low-byte-only match was a reader-side heuristic that SINTRAN never
 * produces. See ndfs_mb_ext_checksum(). */
typedef enum {
    NDFS_CHECKSUM_VALID   = 0,
    NDFS_CHECKSUM_INVALID = 1
} ndfs_checksum_validation_t;

/** File type flags (bit field). */
typedef enum {
    NDFS_FTYPE_NONE           = 0,
    NDFS_FTYPE_TERMINAL       = 1 << 0,
    NDFS_FTYPE_PERIPHERAL     = 1 << 1,
    NDFS_FTYPE_SPOOLING       = 1 << 2,
    NDFS_FTYPE_INDEXED        = 1 << 3,
    NDFS_FTYPE_CONTIGUOUS     = 1 << 4,
    NDFS_FTYPE_ALLOCATED      = 1 << 5,
    NDFS_FTYPE_MAGNETIC_TAPE  = 1 << 6,
    NDFS_FTYPE_LIBRARY        = 1 << 7
} ndfs_file_type_flags_t;

/** A file/directory entry returned by ndfs_list_directory(). */
typedef struct {
    char     name[NDFS_NAME_MAX + 1];
    char     type[NDFS_TYPE_MAX + 1];
    char     full_name[NDFS_NAME_MAX + NDFS_TYPE_MAX + 2]; /* "NAME:TYPE" */
    char     user_name[NDFS_NAME_MAX + 1];
    uint32_t size;
    uint32_t pages;
    bool     is_directory;
} ndfs_file_entry_t;

#ifdef __cplusplus
}
#endif

#endif /* NDFS_TYPES_H */
