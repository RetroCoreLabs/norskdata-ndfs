"""
NDFS user friend entry: 16-bit packed permission value.

Bit 15:   Entry used
Bits 14-13: Reserved
Bit 12:   Directory access
Bit 11:   Common access
Bit 10:   Append access
Bit 9:    Write access
Bit 8:    Read access
Bits 7-0: Friend user index (0-255)

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from typing import Optional, Union

from ndfs.endian import read_uint16_be, write_uint16_be

_BufType = Union[bytes, bytearray, memoryview]


class UserFriend:
    """A single friend entry with packed permission bits."""

    __slots__ = ("bits",)

    def __init__(self, bits: int = 0) -> None:
        self.bits: int = bits & 0xFFFF

    @classmethod
    def create(
        cls,
        friend_user_id: int,
        read: bool = False,
        write: bool = False,
        append: bool = False,
        common: bool = False,
        directory: bool = False,
    ) -> UserFriend:
        """Create with explicit permissions."""
        bits = (friend_user_id & 0xFF) | (1 << 15)  # mark used
        if read:
            bits |= 1 << 8
        if write:
            bits |= 1 << 9
        if append:
            bits |= 1 << 10
        if common:
            bits |= 1 << 11
        if directory:
            bits |= 1 << 12
        return cls(bits)

    @classmethod
    def from_bytes(cls, data: _BufType, offset: int) -> UserFriend:
        """Parse from big-endian bytes."""
        return cls(read_uint16_be(data, offset))

    # ── Properties ───────────────────────────────────────────────────

    @property
    def entry_used(self) -> bool:
        return (self.bits & (1 << 15)) != 0

    @property
    def directory_access(self) -> bool:
        return (self.bits & (1 << 12)) != 0

    @property
    def common_access(self) -> bool:
        return (self.bits & (1 << 11)) != 0

    @property
    def append_access(self) -> bool:
        return (self.bits & (1 << 10)) != 0

    @property
    def write_access(self) -> bool:
        return (self.bits & (1 << 9)) != 0

    @property
    def read_access(self) -> bool:
        return (self.bits & (1 << 8)) != 0

    @property
    def friend_user_index(self) -> int:
        return self.bits & 0xFF

    # ── Methods ──────────────────────────────────────────────────────

    def set_friend(self, friend_user_id: int, permissions: int) -> None:
        """Set friend with permission bits."""
        self.bits = (friend_user_id & 0xFF) | (1 << 15) | ((permissions & 0x1F) << 8)

    def clear(self) -> None:
        """Clear this friend slot."""
        self.bits = 0

    def to_bytes(self, data: bytearray, offset: int) -> None:
        """Write to big-endian bytes."""
        write_uint16_be(data, offset, self.bits)

    @staticmethod
    def parse_permissions(s: Optional[str]) -> int:
        """Parse a permission letters string into the 5-bit value used by
        set_friend: R=read, W=write, A=append, C=common, D=directory. '-' and
        spaces are ignored. None/empty yields 0. Raises ValueError on an
        unrecognised letter."""
        if not s:
            return 0
        bits = 0
        table = {"R": 0x01, "W": 0x02, "A": 0x04, "C": 0x08, "D": 0x10}
        for ch in s:
            if ch in ("-", " "):
                continue
            up = ch.upper()
            if up not in table:
                raise ValueError(f"Invalid permission letter: {ch!r}")
            bits |= table[up]
        return bits

    def get_permission_string(self) -> str:
        """Get a human-readable permission string like 'RWACD'."""
        if not self.entry_used:
            return "-----"
        return (
            ("R" if self.read_access else "-")
            + ("W" if self.write_access else "-")
            + ("A" if self.append_access else "-")
            + ("C" if self.common_access else "-")
            + ("D" if self.directory_access else "-")
        )

    def __repr__(self) -> str:
        if not self.entry_used:
            return "UserFriend([Empty])"
        return f"UserFriend(friend={self.friend_user_index}, perms={self.get_permission_string()})"

    def __str__(self) -> str:
        if not self.entry_used:
            return "[Empty]"
        return f"Friend[{self.friend_user_index}] {self.get_permission_string()}"
