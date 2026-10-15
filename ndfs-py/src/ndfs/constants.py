"""
NDFS (Norsk Data File System) constants.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""

# Page size in bytes (1024 x 16-bit words).
NDFS_PAGE_SIZE: int = 2048

# Name terminator character (ASCII single quote).
NDFS_NAME_TERMINATOR: int = 0x27

# Maximum length of a directory/user/object name.
NDFS_NAME_MAX: int = 16

# Maximum length of a file type string.
NDFS_TYPE_MAX: int = 4

# Number of entries (user or object) per page.
ENTRIES_PER_PAGE: int = 32

# --- Object blocks (decoded 2026-08-01) -------------------------------------
#
# A SINTRAN user area holds 256 files per "object block": one block by default
# and up to 16 (4096 files) on version K. @GIVE-OBJECT-BLOCKS raises the
# ceiling, @USER-STATISTICS reports it as MAXIMUM NUMBER OF FILES, and blocks
# are allocated on demand as files are created. The counts live in user-entry
# byte 47 as two zero-based nibbles (high = MXOBL-1, low = ACOBL-1).
#
# Placement: object block n (0-based) for user U occupies object-file pages
#   n*PAGES_PER_INDEX_BLOCK + U*PAGES_PER_OBJECT_BLOCK  ..  +7
# i.e. successive blocks of the same user are one whole index block apart.
#
# Doc: NDInsight SINTRAN/XMSG/DOC/NDFS-OBJECT-BLOCKS-DECODED-2026-08-01.md

#: Pages of object entries a single object block occupies (8 x 32 = 256 files).
PAGES_PER_OBJECT_BLOCK: int = 8

#: Files addressable by one object block.
FILES_PER_OBJECT_BLOCK: int = PAGES_PER_OBJECT_BLOCK * ENTRIES_PER_PAGE

#: Page pointers in one index block; also the page stride between successive
#: object blocks of the same user.
PAGES_PER_INDEX_BLOCK: int = 512

#: Hardest limit SINTRAN III K can express (16 blocks x 256 files).
MAX_OBJECT_BLOCKS: int = 16

#: Highest user index for which the block placement rule is VERIFIED.
#: PAGES_PER_INDEX_BLOCK / PAGES_PER_OBJECT_BLOCK = 64 users fit in one index
#: block, but the user file holds 256 users - so for user 64 the naive formula
#: puts block 0 on the same page as user 0's block 1. All measured evidence is
#: user 8, blocks 0 and 1. Callers must refuse rather than compute a colliding
#: page above this. See the doc, section "users above 63".
MAX_VERIFIED_MULTIBLOCK_USER: int = 63

# Size of a single entry (user or object) in bytes.
ENTRY_SIZE: int = 64

# Maximum number of pointers in the user file index block.
MAX_USER_FILE_POINTERS: int = 8

# Maximum number of pointers in an object file index block.
MAX_OBJECT_FILE_POINTERS: int = 512

# Maximum number of users.
MAX_USERS: int = 256

# Maximum number of friends per user.
MAX_FRIENDS: int = 8

# Byte offset of the master block within page 0.
MASTER_BLOCK_OFFSET: int = 2016

# Byte offset of the extended info block within page 0.
EXTENDED_INFO_OFFSET: int = 2000

# Size of the master block in bytes.
MASTER_BLOCK_SIZE: int = 32

# Size of the extended info block in bytes.
EXTENDED_INFO_SIZE: int = 16

# First block ID available for allocation (0-6 are system).
FIRST_ALLOCATABLE_BLOCK: int = 7

# Valid user entry flag byte.
USER_ENTRY_FLAG: int = 0x81

# Valid object entry header bit (bit 7).
OBJECT_ENTRY_IN_USE: int = 0x80

#: Users whose object-block slot fits in one 512-page index block: 512/8 = 64.
#: A user's slot within a group is (user % USERS_PER_INDEX_BLOCK).
USERS_PER_INDEX_BLOCK: int = PAGES_PER_INDEX_BLOCK // PAGES_PER_OBJECT_BLOCK
