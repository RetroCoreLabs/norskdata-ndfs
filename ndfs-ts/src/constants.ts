/**
 * NDFS (Norsk Data File System) constants.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

/** Page size in bytes (1024 x 16-bit words). */
export const NDFS_PAGE_SIZE = 2048;

/** Name terminator character (ASCII single quote). */
export const NDFS_NAME_TERMINATOR = 0x27;

/** Maximum length of a directory/user/object name. */
export const NDFS_NAME_MAX = 16;

/** Maximum length of a file type string. */
export const NDFS_TYPE_MAX = 4;

/** Number of entries (user or object) per page. */
export const ENTRIES_PER_PAGE = 32;

/** Size of a single entry (user or object) in bytes. */
export const ENTRY_SIZE = 64;

/** Maximum number of pointers in the user file index block. */
export const MAX_USER_FILE_POINTERS = 8;

/** Maximum number of pointers in an object file index block. */
export const MAX_OBJECT_FILE_POINTERS = 512;

/*
 * Object blocks: how a user area holds more than 256 files.
 *
 * A user's file entries live in OBJECT BLOCKS. One block is 8 object-file pages
 * of 32 entries = 256 files. A user gets one block when created and may be
 * granted up to 16, giving the SINTRAN version-K ceiling of 4096 files. The
 * counts live in the two zero-based nibbles of user-entry byte 47 (MXOBL high,
 * ACOBL low), so a default user stores 0x00 - which is why the byte looked
 * unused on every pack examined before 2026-08-01.
 *
 * Block n for user U occupies object-file pages n*512 + U*8 .. +7, so successive
 * blocks of the same user are one whole index block apart.
 *
 * VERIFIED 2026-08-01 on a live SINTRAN III K pack.
 * See docs/NDFS-OBJECT-BLOCKS-SPEC.md.
 */

/** Object-file pages in one object block. */
export const PAGES_PER_OBJECT_BLOCK = 8;

/** Files held by one object block - 256. */
export const FILES_PER_OBJECT_BLOCK = PAGES_PER_OBJECT_BLOCK * ENTRIES_PER_PAGE;

/** Page pointers in one index block, and the stride between a user's blocks. */
export const PAGES_PER_INDEX_BLOCK = 512;

/** Users whose FIRST object block fits in a single index block - 64. */
export const USERS_PER_INDEX_BLOCK = PAGES_PER_INDEX_BLOCK / PAGES_PER_OBJECT_BLOCK;

/** The SINTRAN version-K ceiling: 16 object blocks = 4096 files per user. */
export const MAX_OBJECT_BLOCKS = 16;

/** Maximum number of users. */
export const MAX_USERS = 256;

/** Maximum number of friends per user. */
export const MAX_FRIENDS = 8;

/** Byte offset of the master block within page 0. */
export const MASTER_BLOCK_OFFSET = 2016;

/** Byte offset of the extended info block within page 0. */
export const EXTENDED_INFO_OFFSET = 2000;

/** Size of the master block in bytes. */
export const MASTER_BLOCK_SIZE = 32;

/** Size of the extended info block in bytes. */
export const EXTENDED_INFO_SIZE = 16;

/** First block ID available for allocation (0-6 are system). */
export const FIRST_ALLOCATABLE_BLOCK = 7;

/** Valid user entry flag byte. */
export const USER_ENTRY_FLAG = 0x81;

/** Valid object entry header bit (bit 7). */
export const OBJECT_ENTRY_IN_USE = 0x80;
