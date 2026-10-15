"""
Big-endian read/write helpers for NDFS binary data.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

import struct
from typing import Union

_BufType = Union[bytes, bytearray, memoryview]


def read_uint16_be(data: _BufType, offset: int) -> int:
    """Read a 16-bit unsigned integer in big-endian byte order."""
    return struct.unpack_from(">H", data, offset)[0]


def read_uint32_be(data: _BufType, offset: int) -> int:
    """Read a 32-bit unsigned integer in big-endian byte order."""
    return struct.unpack_from(">I", data, offset)[0]


def write_uint16_be(data: bytearray, offset: int, value: int) -> None:
    """Write a 16-bit unsigned integer in big-endian byte order."""
    struct.pack_into(">H", data, offset, value & 0xFFFF)


def write_uint32_be(data: bytearray, offset: int, value: int) -> None:
    """Write a 32-bit unsigned integer in big-endian byte order."""
    struct.pack_into(">I", data, offset, value & 0xFFFFFFFF)
