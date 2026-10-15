"""
NDFS name encoding: strings terminated by 0x27 (single quote).

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from typing import Union

from ndfs.constants import NDFS_NAME_TERMINATOR

_BufType = Union[bytes, bytearray, memoryview]


def read_ndfs_name(data: _BufType, offset: int, max_len: int) -> str:
    """Read an NDFS name from raw bytes.

    Names are 7-bit ASCII, terminated by 0x27 (') or 0x00, up to *max_len* bytes.
    """
    end = 0
    for i in range(max_len):
        b = data[offset + i]
        if b == NDFS_NAME_TERMINATOR or b == 0x00:
            break
        end += 1

    result = []
    for i in range(end):
        result.append(chr(data[offset + i]))
    return "".join(result)


def write_ndfs_name(data: bytearray, offset: int, name: str, max_len: int) -> None:
    """Write an NDFS name into a byte buffer.

    Writes a single 0x27 terminator after the name (when it fits) followed by
    NULs -- matching SINTRAN. A field full of terminators is rejected by
    SINTRAN's directory scan. Zero-fill also clears a longer previous name.
    """
    upper = name.upper()
    length = min(len(upper), max_len)
    for i in range(length):
        data[offset + i] = ord(upper[i]) & 0x7F
    if length < max_len:
        data[offset + length] = NDFS_NAME_TERMINATOR
        for i in range(length + 1, max_len):
            data[offset + i] = 0x00
