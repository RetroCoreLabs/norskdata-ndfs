"""
NDFS block pointer: 32-bit value with type in top 2 bits, block ID in bottom 30 bits.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from typing import Union

from ndfs.types import PointerType
from ndfs.endian import read_uint32_be, write_uint32_be

_BufType = Union[bytes, bytearray, memoryview]


class BlockPointer:
    """A 32-bit NDFS block pointer: 2-bit type + 30-bit block address."""

    __slots__ = ("block_id", "type")

    def __init__(
        self,
        block_id: int = 0,
        pointer_type: PointerType = PointerType.Contiguous,
    ) -> None:
        self.block_id: int = block_id & 0x3FFFFFFF
        self.type: PointerType = pointer_type

    # ── Factory methods ──────────────────────────────────────────────

    @classmethod
    def from_native(cls, value: int) -> BlockPointer:
        """Create from a 32-bit native value."""
        pointer_type = PointerType((value >> 30) & 0x03)
        block_id = value & 0x3FFFFFFF
        return cls(block_id, pointer_type)

    @classmethod
    def from_bytes(cls, data: _BufType, offset: int) -> BlockPointer:
        """Create from big-endian bytes at *offset*."""
        return cls.from_native(read_uint32_be(data, offset))

    # ── Properties ───────────────────────────────────────────────────

    @property
    def native(self) -> int:
        """Get the 32-bit native representation."""
        return (((self.type & 0x03) << 30) | (self.block_id & 0x3FFFFFFF)) & 0xFFFFFFFF

    # ── Methods ──────────────────────────────────────────────────────

    def is_valid(self) -> bool:
        """Check if this pointer is valid (non-zero block_id, non-reserved type)."""
        return self.block_id > 0 and self.type != PointerType.Reserved

    def to_bytes(self, data: bytearray, offset: int) -> None:
        """Serialize to big-endian bytes at *offset*."""
        write_uint32_be(data, offset, self.native)

    def to_bytes_array(self) -> bytes:
        """Serialize to a new 4-byte bytes object."""
        buf = bytearray(4)
        self.to_bytes(buf, 0)
        return bytes(buf)

    def __repr__(self) -> str:
        return f"BlockPointer({self.block_id}, {self.type.name})"

    def __str__(self) -> str:
        return f"{self.block_id} ({self.type.name})"
