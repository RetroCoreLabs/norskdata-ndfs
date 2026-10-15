"""
NDFS access permissions: 15-bit encoding with 3 tiers.

Bits 14-10: Public permissions (5 bits)
Bits 9-5:   Friend permissions (5 bits)
Bits 4-0:   Own permissions (5 bits)

Per tier (5 bits):
  Bit 4: Delete
  Bit 3: Execute/Common
  Bit 2: Append
  Bit 1: Write
  Bit 0: Read

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from ndfs.types import FileAccessType

# Permission bit positions within a 5-bit tier.
PERM_READ: int = 0
PERM_WRITE: int = 1
PERM_APPEND: int = 2
PERM_EXECUTE: int = 3
PERM_DELETE: int = 4


class AccessPermissions:
    """15-bit NDFS access permission set with Own/Friend/Public tiers."""

    __slots__ = ("_bits",)

    def __init__(self, access_bits: int = 0) -> None:
        self._bits: int = access_bits & 0x7FFF

    @classmethod
    def from_tiers(cls, own_perms: int, friend_perms: int, public_perms: int) -> AccessPermissions:
        """Create from separate tier values."""
        bits = ((public_perms & 0x1F) << 10) | ((friend_perms & 0x1F) << 5) | (own_perms & 0x1F)
        return cls(bits)

    @classmethod
    def default(cls) -> AccessPermissions:
        """Default permissions: owner full, friends RW, public R."""
        return cls(0x4FF)

    @classmethod
    def owner_only(cls) -> AccessPermissions:
        """Owner-only full access."""
        return cls.from_tiers(0x1F, 0, 0)

    @property
    def raw_bits(self) -> int:
        """Get raw bits."""
        return self._bits

    def _get_tier_bits(self, tier: FileAccessType) -> int:
        """Get permission bits for a specific tier."""
        if tier == FileAccessType.Own:
            return self._bits & 0x1F
        elif tier == FileAccessType.Friend:
            return (self._bits >> 5) & 0x1F
        else:  # Public
            return (self._bits >> 10) & 0x1F

    def has_permission(self, tier: FileAccessType, perm_bit: int) -> bool:
        """Check a specific permission for a tier."""
        return (self._get_tier_bits(tier) & (1 << perm_bit)) != 0

    def can_read(self, tier: FileAccessType) -> bool:
        return self.has_permission(tier, PERM_READ)

    def can_write(self, tier: FileAccessType) -> bool:
        return self.has_permission(tier, PERM_WRITE)

    def can_append(self, tier: FileAccessType) -> bool:
        return self.has_permission(tier, PERM_APPEND)

    def can_execute(self, tier: FileAccessType) -> bool:
        return self.has_permission(tier, PERM_EXECUTE)

    def can_delete(self, tier: FileAccessType) -> bool:
        return self.has_permission(tier, PERM_DELETE)

    def set_permission(self, tier: FileAccessType, perm_bit: int, value: bool) -> None:
        """Set a specific permission for a tier."""
        if tier == FileAccessType.Own:
            shift = 0
        elif tier == FileAccessType.Friend:
            shift = 5
        else:
            shift = 10
        mask = 1 << (shift + perm_bit)
        if value:
            self._bits |= mask
        else:
            self._bits &= ~mask

    def get_permission_string(self, tier: FileAccessType) -> str:
        """Get permission string for a tier (e.g., 'DXAWR')."""
        t = self._get_tier_bits(tier)
        return (
            ("D" if (t & (1 << PERM_DELETE)) != 0 else "-")
            + ("X" if (t & (1 << PERM_EXECUTE)) != 0 else "-")
            + ("A" if (t & (1 << PERM_APPEND)) != 0 else "-")
            + ("W" if (t & (1 << PERM_WRITE)) != 0 else "-")
            + ("R" if (t & (1 << PERM_READ)) != 0 else "-")
        )

    def __str__(self) -> str:
        return (
            f"Own:{self.get_permission_string(FileAccessType.Own)} "
            f"Friend:{self.get_permission_string(FileAccessType.Friend)} "
            f"Public:{self.get_permission_string(FileAccessType.Public)}"
        )

    def __repr__(self) -> str:
        return f"AccessPermissions(0x{self._bits:04X})"
