"""
NDFS master block: the 32-byte structure at offset 2016 of page 0.
Also handles the 16-byte extended info block at offset 2000.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from typing import Optional, Union

from ndfs.block_pointer import BlockPointer
from ndfs.constants import (
    MASTER_BLOCK_OFFSET,
    EXTENDED_INFO_OFFSET,
    NDFS_PAGE_SIZE,
    NDFS_NAME_MAX,
)
from ndfs.types import ChecksumValidation
from ndfs.endian import read_uint16_be, read_uint32_be, write_uint16_be, write_uint32_be
from ndfs.ndfs_name import read_ndfs_name, write_ndfs_name

_BufType = Union[bytes, bytearray, memoryview]


class MasterBlock:
    """The NDFS master block and extended info parsed from page 0."""

    __slots__ = (
        "directory_name",
        "object_file_pointer",
        "user_file_pointer",
        "bit_file_pointer",
        "unreserved_pages",
        "image_size",
        "ext_checksum",
        "ext_reserved1",
        "ext_reserved2",
        "ext_reserved3",
        "ext_flag_word",
        "ext_last_system_number",
        "ext_pages_available",
        "ext_calculated_checksum",
        "ext_valid",
        "checksum_state",
        "has_flomon",
    )

    def __init__(self) -> None:
        self.directory_name: str = ""
        self.object_file_pointer: Optional[BlockPointer] = None
        self.user_file_pointer: Optional[BlockPointer] = None
        self.bit_file_pointer: Optional[BlockPointer] = None
        self.unreserved_pages: int = 0
        self.image_size: int = 0

        # Extended info fields
        self.ext_checksum: int = 0
        self.ext_reserved1: int = 0
        self.ext_reserved2: int = 0
        self.ext_reserved3: int = 0
        self.ext_flag_word: int = 0
        self.ext_last_system_number: int = 0
        self.ext_pages_available: int = 0
        self.ext_calculated_checksum: int = 0
        self.ext_valid: bool = False
        self.checksum_state: ChecksumValidation = ChecksumValidation.Invalid
        self.has_flomon: bool = False

    #: Extended-info flag word (1754B) bit 15: "directory entered / in use".
    #:
    #: The kernel sets it on enter (CHDSI 37763B, ``BSET ONE 170``) and clears it on
    #: release (REENB 40162B, ``BSET ZRO 170``). A stored 0x8000 means the volume was left
    #: entered by the system in ``ext_last_system_number``. Bits 0-14 have no known
    #: meaning -- preserve them verbatim.
    #:
    #: NOTE: system number 0 means NO OWNER recorded, not "system zero".
    EXT_FLAG_ENTERED = 0x8000

    @property
    def is_directory_entered(self) -> bool:
        """True when the volume was left 'entered' (flag bit 15) and not cleanly released."""
        return (self.ext_flag_word & MasterBlock.EXT_FLAG_ENTERED) != 0

    @staticmethod
    def compute_ext_checksum(
        reserved1: int, reserved2: int, reserved3: int,
        flag_word: int, system_number: int,
        pages_hi: int, pages_lo: int,
    ) -> int:
        """Compute the extended-info checksum exactly as the SINTRAN kernel does.

        A plain 16-bit ADDITIVE SUM of the seven words that FOLLOW the checksum slot
        (words 1751B-1757B), truncated to 16 bits.

        Kernel-proven from both sides of the carved 006-S3FS segment: the writer WXDIR
        (37702B) and the enter-directory validator CHDSI (37763B) run the identical
        ``ADD ,X 0`` accumulation loop. There is no XOR, and the system number is simply
        one of the seven summed words -- not a separate addend.
        """
        total = (reserved1 + reserved2 + reserved3
                 + flag_word + system_number + pages_hi + pages_lo)
        return total & 0xFFFF

    @classmethod
    def from_bytes(cls, page_data: _BufType) -> MasterBlock:
        """Parse a master block (and extended info) from a full page 0 buffer.

        The buffer must be at least NDFS_PAGE_SIZE bytes.
        """
        if len(page_data) < NDFS_PAGE_SIZE:
            raise ValueError("Page data too small for master block")

        mb = cls()
        off = MASTER_BLOCK_OFFSET

        # Directory name (16 bytes, terminated by 0x27)
        mb.directory_name = read_ndfs_name(page_data, off, NDFS_NAME_MAX)

        # Block pointers
        mb.object_file_pointer = BlockPointer.from_bytes(page_data, off + 0x10)
        mb.user_file_pointer = BlockPointer.from_bytes(page_data, off + 0x14)
        mb.bit_file_pointer = BlockPointer.from_bytes(page_data, off + 0x18)

        # Unreserved pages
        mb.unreserved_pages = read_uint32_be(page_data, off + 0x1C)

        # --- Extended info (bytes 2000-2015) ---
        ext = EXTENDED_INFO_OFFSET
        mb.ext_checksum = read_uint16_be(page_data, ext)
        mb.ext_reserved1 = read_uint16_be(page_data, ext + 2)
        mb.ext_reserved2 = read_uint16_be(page_data, ext + 4)
        mb.ext_reserved3 = read_uint16_be(page_data, ext + 6)
        mb.ext_flag_word = read_uint16_be(page_data, ext + 8)
        mb.ext_last_system_number = read_uint16_be(page_data, ext + 10)
        mb.ext_pages_available = read_uint32_be(page_data, ext + 12)

        # KERNEL-CORRECTED: the checksum is a plain 16-bit ADDITIVE SUM of the seven words
        # FOLLOWING it (1751B-1757B) -- NOT "XOR six words, then add the system number".
        # Proven from both sides of the real SINTRAN kernel (carved 006-S3FS): the writer
        # WXDIR (37702B) and the enter-directory validator CHDSI (37763B) run the identical
        # `ADD ,X 0` accumulation loop. The XOR form matched PACK-ONE only by coincidence
        # (its only two words sharing a set bit are flag=0x8000 and pages_lo=0x9051, both
        # bit 15 -- they cancel under XOR and carry out under ADD, giving the same low 16
        # bits). Verified against three real disks: 0x10B7, 0x1051, 0xC162.
        pages_lo = mb.ext_pages_available & 0xFFFF
        pages_hi = (mb.ext_pages_available >> 16) & 0xFFFF
        calculated = MasterBlock.compute_ext_checksum(
            mb.ext_reserved1, mb.ext_reserved2, mb.ext_reserved3,
            mb.ext_flag_word, mb.ext_last_system_number, pages_hi, pages_lo,
        )
        mb.ext_calculated_checksum = calculated

        # The kernel writes and compares the FULL 16 bits, so there is no "low byte only"
        # accept state -- that was a reader-side heuristic SINTRAN never produces.
        mb.checksum_state = (
            ChecksumValidation.Valid
            if mb.ext_checksum == calculated
            else ChecksumValidation.Invalid
        )

        # Detect FLOMON
        mb.has_flomon = MasterBlock._detect_flomon(page_data)

        # Extended info validity
        if mb.has_flomon:
            mb.ext_valid = False
        else:
            checksum_non_zero = mb.ext_checksum != 0
            checksum_ok = mb.checksum_state == ChecksumValidation.Valid
            mb.ext_valid = checksum_non_zero and checksum_ok

        return mb

    @staticmethod
    def _detect_flomon(page_data: _BufType) -> bool:
        """Detect FLOMON boot format.

        FLOMON disks have a simplified boot loader where the BPUN binary section
        has address=0, count=0, and checksum=0.
        """
        # Look for '!' marker in the first part of the page
        exclamation_pos = -1
        limit = min(len(page_data), 256)
        for i in range(limit):
            if page_data[i] == 0x21:
                exclamation_pos = i
                break
        if exclamation_pos < 0:
            return False

        # After '!', BPUN has: address(2) + count(2) bytes
        after_excl = exclamation_pos + 1
        if after_excl + 4 > len(page_data):
            return False

        addr = read_uint16_be(page_data, after_excl)
        count = read_uint16_be(page_data, after_excl + 2)

        return addr == 0 and count == 0

    def is_valid(self) -> bool:
        """Check if the master block is valid."""
        # Check directory name is printable ASCII
        if len(self.directory_name) > 0:
            for ch in self.directory_name:
                c = ord(ch)
                if c < 0x20 or c > 0x7E:
                    return False

        # At least one pointer must be valid, or a directory name must exist
        has_valid_pointer = False
        if self.object_file_pointer is not None and self.object_file_pointer.is_valid():
            has_valid_pointer = True
        if self.user_file_pointer is not None and self.user_file_pointer.is_valid():
            has_valid_pointer = True
        if self.bit_file_pointer is not None and self.bit_file_pointer.is_valid():
            has_valid_pointer = True

        return has_valid_pointer or len(self.directory_name) > 0

    def write_to_bytes(self, page_data: bytearray) -> None:
        """Write the master block to page data at the standard offset."""
        if len(page_data) < NDFS_PAGE_SIZE:
            raise ValueError("Page buffer too small for master block")

        off = MASTER_BLOCK_OFFSET

        # Clear the master block area
        for i in range(32):
            page_data[off + i] = 0

        # Directory name
        write_ndfs_name(page_data, off, self.directory_name, NDFS_NAME_MAX)

        # Block pointers
        if self.object_file_pointer is not None:
            self.object_file_pointer.to_bytes(page_data, off + 0x10)
        if self.user_file_pointer is not None:
            self.user_file_pointer.to_bytes(page_data, off + 0x14)
        if self.bit_file_pointer is not None:
            self.bit_file_pointer.to_bytes(page_data, off + 0x18)

        # Unreserved pages
        write_uint32_be(page_data, off + 0x1C, self.unreserved_pages)

    def write_extended_info(self, page_data: bytearray) -> None:
        """Write extended info to page data."""
        if len(page_data) < NDFS_PAGE_SIZE:
            raise ValueError("Page buffer too small for extended info")

        ext = EXTENDED_INFO_OFFSET

        # Calculate checksum
        pages_lo = self.ext_pages_available & 0xFFFF
        pages_hi = (self.ext_pages_available >> 16) & 0xFFFF
        # Recompute the checksum from the current fields, exactly as the kernel writer
        # WXDIR (37702B) does: a 16-bit additive sum of words 1751B-1757B. Every write of
        # the extended-info block is followed by a fresh checksum.
        checksum = MasterBlock.compute_ext_checksum(
            self.ext_reserved1, self.ext_reserved2, self.ext_reserved3,
            self.ext_flag_word, self.ext_last_system_number, pages_hi, pages_lo,
        )

        write_uint16_be(page_data, ext, checksum)
        write_uint16_be(page_data, ext + 2, self.ext_reserved1)
        write_uint16_be(page_data, ext + 4, self.ext_reserved2)
        write_uint16_be(page_data, ext + 6, self.ext_reserved3)
        write_uint16_be(page_data, ext + 8, self.ext_flag_word)
        write_uint16_be(page_data, ext + 10, self.ext_last_system_number)
        write_uint32_be(page_data, ext + 12, self.ext_pages_available)
