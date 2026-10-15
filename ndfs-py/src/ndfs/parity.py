"""
ND-100 even parity helpers.

The ND-100 uses even parity on text files: bit 7 is set so that the
total number of 1-bits in each byte is even. This applies to text file
types (:MODE, :SYMB, :TEXT, :C, etc.) but NOT binary files (:PROG, :BPUN).

Example:
    'H' = 0x48 (01001000) -> 2 ones (even) -> bit 7 = 0 -> 0x48
    's' = 0x73 (01110011) -> 5 ones (odd)  -> bit 7 = 1 -> 0xF3
    ' ' = 0x20 (00100000) -> 1 one  (odd)  -> bit 7 = 1 -> 0xA0

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""

# Text file types where parity applies
TEXT_TYPES = frozenset({
    "MODE", "SYMB", "TEXT", "C", "BATC", "OUT",
    "LOG", "LIST", "FADM", "BASM", "FORT", "NPL",
    "COBO", "PASC", "PLAN", "BAS", "MAC", "EDIT",
})


def _popcount(b: int) -> int:
    """Count 1-bits in the low 7 bits."""
    v = b & 0x7F
    count = 0
    while v:
        count += v & 1
        v >>= 1
    return count


def strip_parity(data: bytes) -> bytes:
    """Strip even parity: clear bit 7 on every byte.

    Converts ND-100 text (with parity) to standard 7-bit ASCII.

    Args:
        data: Raw bytes from NDFS text file.

    Returns:
        New bytes object with bit 7 cleared on every byte.
    """
    return bytes(b & 0x7F for b in data)


def set_parity(data: bytes) -> bytes:
    """Set even parity: set bit 7 so total 1-bits per byte is even.

    Converts standard ASCII text to ND-100 text format with even parity.

    Args:
        data: Standard ASCII bytes to convert.

    Returns:
        New bytes object with even parity applied.
    """
    result = bytearray(len(data))
    for i in range(len(data)):
        lo7 = data[i] & 0x7F
        ones = _popcount(lo7)
        if ones % 2 != 0:
            result[i] = lo7 | 0x80
        else:
            result[i] = lo7
    return bytes(result)


def is_text_type(file_type: str) -> bool:
    """Check if a file type is a text type (parity applies).

    Args:
        file_type: The NDFS file type string (e.g., "MODE", "PROG").

    Returns:
        True if parity should be applied to this file type.
    """
    return file_type.upper() in TEXT_TYPES
