"""
NDFS user file: manages the collection of user entries.

The user file is an indexed structure with up to 8 index pointers,
each pointing to a data page containing 32 user entries (64 bytes each).
Maximum 256 users (8 pages x 32 entries).

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
HackerCorp Labs - https://github.com/HackerCorpLabs
"""
from __future__ import annotations

import math
from typing import Callable, Dict, List, Optional, Tuple, Union

from ndfs.constants import (
    NDFS_PAGE_SIZE,
    ENTRIES_PER_PAGE,
    ENTRY_SIZE,
    MAX_USER_FILE_POINTERS,
    MAX_USERS,
)
from ndfs.block_pointer import BlockPointer
from ndfs.user_entry import UserEntry

_BufType = Union[bytes, bytearray, memoryview]


class UserFile:
    """Manages the collection of NDFS user entries."""

    __slots__ = ("index_pointer", "_entries", "unreadable_pages")

    def __init__(self) -> None:
        self.index_pointer: Optional[BlockPointer] = None
        self._entries: Dict[int, UserEntry] = {}
        # User-file pages that could not be read on the last load. Greater
        # than zero means some users on this floppy have no name here.
        self.unreadable_pages: int = 0

    def get_users(self) -> List[UserEntry]:
        """Get all user entries as a list."""
        result: List[UserEntry] = []
        for v in self._entries.values():
            result.append(v)
        return result

    def get_user(self, index: int) -> Optional[UserEntry]:
        """Get a user by index."""
        return self._entries.get(index, None)

    def find_user(self, user_name: str) -> Optional[UserEntry]:
        """Find a user by name (case-insensitive)."""
        upper = user_name.upper()
        for entry in self._entries.values():
            if entry.user_name.upper() == upper:
                return entry
        return None

    def add_user(self, entry: UserEntry) -> None:
        """Add or update a user entry."""
        self._entries[entry.user_index] = entry

    def remove_user(self, index: int) -> bool:
        """Remove a user entry."""
        if index in self._entries:
            del self._entries[index]
            return True
        return False

    def update_user_quota(self, user_index: int, new_reserved_pages: int) -> bool:
        """Update a user's quota."""
        user = self._entries.get(user_index)
        if user is None:
            return False
        user.pages_reserved = new_reserved_pages
        return True

    def update_user_usage(self, user_index: int, pages_used: int) -> bool:
        """Update a user's usage."""
        user = self._entries.get(user_index)
        if user is None:
            return False
        user.pages_used = pages_used
        return True

    def get_next_available_index(self) -> int:
        """Get the next available user index."""
        for i in range(MAX_USERS):
            if i not in self._entries:
                return i
        return -1

    def get_total_pages_reserved(self) -> int:
        """Get total pages reserved across all users."""
        total = 0
        for u in self._entries.values():
            total += u.pages_reserved
        return total

    def get_total_pages_used(self) -> int:
        """Get total pages used across all users."""
        total = 0
        for u in self._entries.values():
            total += u.pages_used
        return total

    def clear(self) -> None:
        """Clear all entries."""
        self._entries.clear()

    def load_from_pages(
        self,
        index_page: _BufType,
        read_page: Callable[[int], _BufType],
    ) -> None:
        """Load user entries from index block and data pages.

        Args:
            index_page: The 2048-byte index block (contains up to 8 block pointers).
            read_page: Callback to read a data page by block ID.
        """
        self._entries.clear()
        self.unreadable_pages = 0

        # Read up to 8 pointers from the index block
        for i in range(MAX_USER_FILE_POINTERS):
            ptr = BlockPointer.from_bytes(index_page, i * 4)
            if not ptr.is_valid():
                continue

            # One unreadable user page costs only the users on it. The page
            # reads fail on a damaged floppy, and the users named by the other
            # pages are still there to resolve object entries against.
            try:
                data_page = read_page(ptr.block_id)
            except Exception:
                self.unreadable_pages += 1
                continue

            # Parse up to 32 user entries per page
            for j in range(ENTRIES_PER_PAGE):
                entry_offset = j * ENTRY_SIZE
                user = UserEntry.from_bytes(data_page, entry_offset)
                if user is not None:
                    self._entries[user.user_index] = user

    def to_page_buffers(self) -> Tuple[bytearray, List[bytearray]]:
        """Serialize all user entries into page-aligned buffers.

        Returns:
            A tuple of (index_page, data_pages) where data_pages is a list
            of bytearrays.
        """
        index_page = bytearray(NDFS_PAGE_SIZE)
        page_map: Dict[int, bytearray] = {}

        for user in self._entries.values():
            page_index = user.user_index // ENTRIES_PER_PAGE
            if page_index not in page_map:
                page_map[page_index] = bytearray(NDFS_PAGE_SIZE)
            page = page_map[page_index]
            slot_in_page = user.user_index % ENTRIES_PER_PAGE
            user_bytes = user.to_bytes()
            offset = slot_in_page * ENTRY_SIZE
            page[offset:offset + ENTRY_SIZE] = user_bytes

        # Build data pages array in order
        data_pages: List[bytearray] = []
        for i in range(MAX_USER_FILE_POINTERS):
            page = page_map.get(i)
            if page is not None:
                data_pages.append(page)
            else:
                data_pages.append(bytearray(NDFS_PAGE_SIZE))

        return index_page, data_pages

    def to_data_page(self, page_index: int) -> bytearray:
        """Serialize the single user-file data page `page_index`, zero-filled.

        Zero-fill clears the slot of any removed user.
        """
        page = bytearray(NDFS_PAGE_SIZE)
        for user in self._entries.values():
            if user.user_index // ENTRIES_PER_PAGE == page_index:
                slot_in_page = user.user_index % ENTRIES_PER_PAGE
                offset = slot_in_page * ENTRY_SIZE
                page[offset:offset + ENTRY_SIZE] = user.to_bytes()
        return page
