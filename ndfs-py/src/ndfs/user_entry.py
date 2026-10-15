"""
NDFS user entry: 64-byte record in the user file.

Byte offsets:
  0:     Flag (0x81 = valid user)
  1:     Enter count
  2-17:  User name (16 bytes, terminated by 0x27)
  18-19: Password (16-bit, big-endian)
  20-23: Date created (ND time, big-endian)
  24-27: Last date entered (ND time, big-endian)
  28-31: Pages reserved (32-bit, big-endian)
  32-35: Pages used (32-bit, big-endian)
  36:    Directory index
  37:    User index
  38-39: Reserved
  40-41: Default file access (16-bit, big-endian)
  42-46: Reserved / tracking (42-43 and 44-45 both hold the user index)
  47:    MXOBL/ACOBL - object block counts, two ZERO-BASED nibbles:
             high = MXOBL - 1   maximum object blocks this user may have
             low  = ACOBL - 1   object blocks actually allocated so far
         A SINTRAN user area holds 256 files per object block, 1 block by
         default and up to 16 (4096 files) on version K. The operator raises
         the ceiling with @GIVE-OBJECT-BLOCKS and reads it back from
         @USER-STATISTICS ("MAXIMUM NUMBER OF FILES"); blocks are allocated
         on demand as files are created.
         A default user therefore stores 0x00 = 1 max, 1 allocated = 256 files.
         VERIFIED 2026-08-01 on a live SINTRAN III K pack with a control group:
         user BIGMAN, given 3 extra blocks and holding 482 files, stores 0x31
         -> MXOBL 4 (matching its reported maximum of 1024 files) and ACOBL 2
         (matching ceil(482/256)); every other user on the pack stores 0x00.
         Doc: NDInsight SINTRAN/XMSG/DOC/NDFS-OBJECT-BLOCKS-DECODED-2026-08-01.md
  48-63: Friends (8 x 2-byte entries, big-endian)

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from typing import List, Optional, Union

from ndfs.constants import (
    ENTRY_SIZE,
    USER_ENTRY_FLAG,
    NDFS_NAME_MAX,
    MAX_FRIENDS,
)
from ndfs.endian import read_uint16_be, read_uint32_be, write_uint16_be, write_uint32_be
from ndfs.ndfs_name import read_ndfs_name, write_ndfs_name
from ndfs.user_friend import UserFriend

_BufType = Union[bytes, bytearray, memoryview]


class UserEntry:
    """A single NDFS user record (64 bytes on disk)."""

    __slots__ = (
        "uf",
        "user_index",
        "user_name",
        "password",
        "enter_count",
        "date_created",
        "last_date_entered",
        "pages_reserved",
        "pages_used",
        "directory_index",
        "default_file_access",
        "max_object_blocks",
        "allocated_object_blocks",
        "friends",
        "raw",
    )

    def __init__(self) -> None:
        # Byte 0, the flags byte, kept whole. from_bytes only requires the
        # USER_ENTRY_FLAG bits (bit 7 "entry used" + bit 0 "user entry"), so a
        # real pack may carry other bits here that we do not model; to_bytes
        # writes this back verbatim rather than re-hardcoding 0x81, which would
        # destroy them on every read/modify/write. A fresh entry must keep it
        # set, or the next from_bytes reads the slot as free.
        self.uf: int = USER_ENTRY_FLAG
        self.user_index: int = 0
        self.user_name: str = ""
        self.password: int = 0
        self.enter_count: int = 0
        self.date_created: int = 0
        self.last_date_entered: int = 0
        self.pages_reserved: int = 0
        self.pages_used: int = 0
        self.directory_index: int = 0
        self.default_file_access: int = 0x4FF
        # Byte 47, MXOBL/ACOBL. A brand-new user area gets ONE object block =
        # 256 files, which is what SINTRAN itself does; @GIVE-OBJECT-BLOCKS
        # raises the maximum later and blocks are allocated on demand.
        self.max_object_blocks: int = 1
        self.allocated_object_blocks: int = 1
        self.friends: List[UserFriend] = []
        for _ in range(MAX_FRIENDS):
            self.friends.append(UserFriend())
        # Verbatim on-disk 64 bytes; base for re-serialization so unmodelled
        # bytes (38-39, 42-46) survive. Byte 47 IS modelled, above.
        # None for freshly-built entries.
        self.raw: Optional[bytes] = None

    @classmethod
    def from_bytes(cls, data: _BufType, offset: int) -> Optional[UserEntry]:
        """Parse a user entry from 64 bytes at *offset*.

        Returns None if the entry is not a valid user (flag != 0x81).
        """
        if len(data) < offset + ENTRY_SIZE:
            raise ValueError("Insufficient data for user entry")

        # Check valid user flag
        if (data[offset] & USER_ENTRY_FLAG) != USER_ENTRY_FLAG:
            return None

        entry = cls()
        entry.raw = bytes(data[offset:offset + ENTRY_SIZE])
        # Keep the WHOLE flags byte, not just the 0x81 bits tested above.
        # Anything else set here is a flag we do not model yet, and to_bytes
        # writes it straight back out.
        entry.uf = data[offset]
        entry.enter_count = data[offset + 1]
        entry.user_name = read_ndfs_name(data, offset + 2, NDFS_NAME_MAX)
        entry.password = read_uint16_be(data, offset + 18)
        entry.date_created = read_uint32_be(data, offset + 20)
        entry.last_date_entered = read_uint32_be(data, offset + 24)
        entry.pages_reserved = read_uint32_be(data, offset + 28)
        entry.pages_used = read_uint32_be(data, offset + 32)
        entry.directory_index = data[offset + 36]
        entry.user_index = data[offset + 37]
        # Default file access is at offset 40 (verified on-disk); 38-39 unused.
        entry.default_file_access = read_uint16_be(data, offset + 40)

        # Byte 47: MXOBL/ACOBL, two ZERO-BASED nibbles. A default user stores
        # 0x00 meaning 1 block max and 1 allocated = 256 files, so +1 on both
        # halves is what turns the raw byte into usable counts.
        b47 = data[offset + 47]
        entry.max_object_blocks = (b47 >> 4) + 1
        entry.allocated_object_blocks = (b47 & 0x0F) + 1

        # Parse friends (8 x 2 bytes starting at offset 48)
        for i in range(MAX_FRIENDS):
            entry.friends[i] = UserFriend.from_bytes(data, offset + 48 + i * 2)

        return entry

    def to_bytes(self) -> bytearray:
        """Serialize to a 64-byte bytearray."""
        buf = bytearray(ENTRY_SIZE)
        if self.raw is not None and len(self.raw) == ENTRY_SIZE:
            buf[0:ENTRY_SIZE] = self.raw

        # Flags byte written verbatim so unmodelled bits survive a read/modify/
        # write. This used to be a hardcoded USER_ENTRY_FLAG, which silently
        # cleared every other bit in byte 0 of every user in the page - the
        # user-file writer re-serializes all slots, not just the edited one.
        buf[0] = self.uf & 0xFF
        buf[1] = self.enter_count & 0xFF

        write_ndfs_name(buf, 2, self.user_name, NDFS_NAME_MAX)
        write_uint16_be(buf, 18, self.password)
        write_uint32_be(buf, 20, self.date_created)
        write_uint32_be(buf, 24, self.last_date_entered)
        write_uint32_be(buf, 28, self.pages_reserved)
        write_uint32_be(buf, 32, self.pages_used)
        buf[36] = self.directory_index
        buf[37] = self.user_index & 0xFF
        write_uint16_be(buf, 40, self.default_file_access)

        # Byte 47: MXOBL/ACOBL back to zero-based nibbles. Clamped to 1..16 so
        # a caller cannot write a count SINTRAN could not express - 16 blocks
        # of 256 files is the version-K ceiling of 4096.
        mx = min(16, max(1, self.max_object_blocks))
        ac = min(mx, max(1, self.allocated_object_blocks))
        buf[47] = ((mx - 1) << 4) | (ac - 1)

        for i in range(MAX_FRIENDS):
            self.friends[i].to_bytes(buf, 48 + i * 2)

        return buf

    def is_over_quota(self) -> bool:
        """Check if user has exceeded quota."""
        return self.pages_used > self.pages_reserved

    def get_free_pages(self) -> int:
        """Get remaining free pages in quota."""
        return self.pages_reserved - self.pages_used

    def set_name(self, name: str) -> None:
        """Set user name (max 16 chars, uppercased)."""
        if not name or len(name.strip()) == 0:
            raise ValueError("User name cannot be empty")
        self.user_name = name.upper().strip()[:NDFS_NAME_MAX]

    def is_friend(self, user_id: int) -> bool:
        """Check if *user_id* is in this user's friend list."""
        for i in range(len(self.friends)):
            if self.friends[i].entry_used and self.friends[i].friend_user_index == user_id:
                return True
        return False

    def add_friend(self, friend_id: int, permissions: int) -> bool:
        """Add a friend. Returns False if no empty slot."""
        for i in range(len(self.friends)):
            if not self.friends[i].entry_used:
                self.friends[i].set_friend(friend_id, permissions)
                return True
        return False

    def remove_friend(self, friend_id: int) -> bool:
        """Remove a friend. Returns False if not found."""
        for i in range(len(self.friends)):
            if self.friends[i].entry_used and self.friends[i].friend_user_index == friend_id:
                self.friends[i].clear()
                return True
        return False

    def get_friend(self, friend_id: int) -> Optional[UserFriend]:
        """Get friend entry for a user."""
        for i in range(len(self.friends)):
            if self.friends[i].entry_used and self.friends[i].friend_user_index == friend_id:
                return self.friends[i]
        return None
