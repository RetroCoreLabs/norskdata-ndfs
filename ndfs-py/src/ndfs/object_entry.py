"""
NDFS object (file) entry: 64-byte record in the object file.

Byte offsets:
  0:     Header (bit 7 = in use, i.e. 0x80)
  1:     Reserved
  2-17:  Object name (16 bytes, terminated by 0x27)
  18-21: File type (4 bytes, terminated by 0x27)
  22-23: Next version (u16)
  24-25: Previous version (u16)
  26-27: Access bits (u16, 3x5-bit OWN/FRIEND/PUBLIC)
  28-29: File type flags (u16: L M A C I B P T)
  30-31: Device number (u16)
  32:    File type code (0=DATA, 1=PROG, 2=SYMB, 3=TEXT)
  34:    User index (owner) / object-index word
  36-37: Current open count (u16)
  38-39: Total open count (u16)
  40-43: Date created (u32, ND timestamp)
  44-47: Last date opened for read (u32, ND timestamp)
  48-51: Last date opened for write (u32, ND timestamp)
  52-55: Pages in file (32-bit, big-endian)
  56-59: Bytes in file - 1 (32-bit, big-endian; actual = stored + 1)
  60-63: File pointer (BlockPointer, big-endian)

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from typing import Optional, Union

from ndfs.constants import (
    ENTRIES_PER_PAGE,
    ENTRY_SIZE,
    FILES_PER_OBJECT_BLOCK,
    OBJECT_ENTRY_IN_USE,
    NDFS_NAME_MAX,
    NDFS_TYPE_MAX,
    PAGES_PER_INDEX_BLOCK,
    PAGES_PER_OBJECT_BLOCK,
)
from ndfs.endian import (
    read_uint16_be,
    write_uint16_be,
    read_uint32_be,
    write_uint32_be,
)
from ndfs.ndfs_name import read_ndfs_name, write_ndfs_name
from ndfs.block_pointer import BlockPointer

_BufType = Union[bytes, bytearray, memoryview]

# Object file_type_flags bits (offset 28): "L M A C I B P T".
FT_TERMINAL = 1 << 0
FT_PERIPHERAL = 1 << 1
FT_SPOOLING = 1 << 2
FT_INDEXED = 1 << 3
FT_CONTIGUOUS = 1 << 4
FT_ALLOCATED = 1 << 5
FT_MAGTAPE = 1 << 6
FT_LIBRARY = 1 << 7

# Default access for a new file: OWN + FRIEND all rights, PUBLIC none.
ACCESS_DEFAULT = 0x03FF


def compute_file_number(object_index: int, user_index: int) -> int:
    """Convert a physical object-file position into the file number SINTRAN reports.

    ``object_index`` is the raw physical position (``page * 32 + slot_in_page``);
    ``user_index`` is the owning user from byte 34.

    A user's files live in up to 16 *object blocks* of 256 files each, and block
    ``n`` occupies pages ``n*512 + user*8 .. +7``. So the raw position encodes
    both the block and the slot, and the number the user sees is::

        block      = page // 512
        slot       = (page % 512 - user*8) * 32 + slot_in_page
        fileNumber = block * 256 + slot

    Using the raw position instead is correct ONLY for a user's first block. For
    anything beyond it both the number and the "owner" derived from its high
    byte are wrong - on a verified pack, file ``F0500`` (SINTRAN: ``FILE 307``,
    owner 8) sits at raw position 18483, whose high byte reads as user 72.

    Returns -1 when the position does not belong to ``user_index`` at all, so a
    caller can spot a mismatch instead of silently getting a plausible number.
    """
    page = object_index // ENTRIES_PER_PAGE
    slot_in_page = object_index % ENTRIES_PER_PAGE

    block = page // PAGES_PER_INDEX_BLOCK
    page_within_block_row = page % PAGES_PER_INDEX_BLOCK
    first_page_of_user = user_index * PAGES_PER_OBJECT_BLOCK

    offset_pages = page_within_block_row - first_page_of_user
    if offset_pages < 0 or offset_pages >= PAGES_PER_OBJECT_BLOCK:
        return -1

    slot = offset_pages * ENTRIES_PER_PAGE + slot_in_page
    return block * FILES_PER_OBJECT_BLOCK + slot


class ObjectEntry:
    """A single NDFS file (object) entry (64 bytes on disk)."""

    __slots__ = (
        "header",
        "header_word",
        "object_index",
        "file_number",
        "object_name",
        "type",
        "user_name",
        "user_index",
        "file_type",
        "pages_in_file",
        "bytes_in_file",
        "file_pointer",
        "access_bits",
        "next_version",
        "prev_version",
        "file_type_flags",
        "device_number",
        "disk_object_index",
        "current_open_count",
        "total_open_count",
        "date_created",
        "last_date_read",
        "last_date_written",
        "raw",
    )

    def __init__(self) -> None:
        self.header: int = OBJECT_ENTRY_IN_USE
        self.header_word: int = OBJECT_ENTRY_IN_USE << 8
        # PHYSICAL position in the object file: page * 32 + slot-in-page. This
        # is the write-back key (see ObjectFile.to_data_pages) and is NOT the
        # number SINTRAN shows the user - see file_number below.
        self.object_index: int = 0
        # The number SINTRAN reports as "FILE n", per user, stable for the life
        # of the file. Set by compute_file_number() during load; 0 on a fresh
        # entry until it is placed.
        self.file_number: int = 0
        self.object_name: str = ""
        self.type: str = "DATA"
        self.user_name: str = ""
        self.user_index: int = 0
        self.file_type: int = 0  # 0=DATA, 1=PROG, 2=SYMB, 3=TEXT
        self.pages_in_file: int = 0
        self.bytes_in_file: int = 0
        self.file_pointer: Optional[BlockPointer] = None
        self.access_bits: int = 0
        self.next_version: int = 0
        self.prev_version: int = 0
        self.file_type_flags: int = 0
        self.device_number: int = 0
        self.disk_object_index: int = 0
        self.current_open_count: int = 0
        self.total_open_count: int = 0
        self.date_created: int = 0
        self.last_date_read: int = 0
        self.last_date_written: int = 0
        # Verbatim on-disk 64 bytes, used as the base when re-serializing so
        # unmodelled bytes survive. None for freshly-built entries.
        self.raw: Optional[bytes] = None

    @property
    def full_name(self) -> str:
        """Full name in 'NAME:TYPE' format."""
        if self.type:
            return f"{self.object_name}:{self.type}"
        return self.object_name

    @property
    def file_type_as_text(self) -> str:
        """File type as text string."""
        if self.file_type == 0:
            return "DATA"
        elif self.file_type == 1:
            return "PROG"
        elif self.file_type == 2:
            return "SYMB"
        elif self.file_type == 3:
            return "TEXT"
        else:
            return f"TYPE{self.file_type}"

    @classmethod
    def from_bytes(cls, data: _BufType, offset: int) -> Optional[ObjectEntry]:
        """Parse an object entry from 64 bytes at *offset*.

        Returns None if the entry is not in use (bit 7 of byte 0 not set).
        """
        if len(data) < offset + ENTRY_SIZE:
            raise ValueError("Insufficient data for object entry")

        # Check in-use bit
        if (data[offset] & OBJECT_ENTRY_IN_USE) == 0:
            return None

        entry = cls()

        # Preserve the verbatim 64 bytes so re-serialization never loses
        # fields we do not explicitly model.
        entry.raw = bytes(data[offset:offset + ENTRY_SIZE])

        entry.header = data[offset]
        entry.header_word = read_uint16_be(data, offset + 0)

        # Object name (16 bytes at offset+2)
        entry.object_name = read_ndfs_name(data, offset + 2, NDFS_NAME_MAX)

        # File type (4 bytes at offset+18). Preserve an empty type as-is — do
        # NOT default to "DATA". A parse must faithfully represent what is on
        # disk; defaulting here corrupts files whose type is intentionally
        # empty (e.g. TERMINAL: 27 00 00 00) on write-back. (Matches RetroFS.)
        entry.type = read_ndfs_name(data, offset + 18, NDFS_TYPE_MAX)

        # Versioning, access, flags, device (offsets 22-31)
        entry.next_version = read_uint16_be(data, offset + 22)
        entry.prev_version = read_uint16_be(data, offset + 24)
        entry.access_bits = read_uint16_be(data, offset + 26)
        entry.file_type_flags = read_uint16_be(data, offset + 28)
        entry.device_number = read_uint16_be(data, offset + 30)

        # File type code (byte 32)
        entry.file_type = data[offset + 32]

        # User index (byte 34) / object-index word
        entry.user_index = data[offset + 34]
        entry.disk_object_index = read_uint16_be(data, offset + 34)

        # Open counts and timestamps (offsets 36-51, big-endian)
        entry.current_open_count = read_uint16_be(data, offset + 36)
        entry.total_open_count = read_uint16_be(data, offset + 38)
        entry.date_created = read_uint32_be(data, offset + 40)
        entry.last_date_read = read_uint32_be(data, offset + 44)
        entry.last_date_written = read_uint32_be(data, offset + 48)

        # Pages in file (bytes 52-55, big-endian)
        entry.pages_in_file = read_uint32_be(data, offset + 52)

        # Bytes in file (bytes 56-59, big-endian) + 1. The field is 32 bits wide, so a stored
        # 0xFFFFFFFF (empty file) wraps to 0; Python ints do not wrap by themselves.
        entry.bytes_in_file = (read_uint32_be(data, offset + 56) + 1) & 0xFFFFFFFF

        # File pointer (bytes 60-63)
        entry.file_pointer = BlockPointer.from_bytes(data, offset + 60)

        return entry

    def to_bytes(self, buffer: bytearray, offset: int) -> None:
        """Serialize to a 64-byte region in a buffer."""
        if len(buffer) < offset + ENTRY_SIZE:
            raise ValueError("Insufficient buffer for object entry")

        # Base on the original on-disk bytes when available so unmodelled
        # bytes (e.g. byte 33, the low byte of the object-index word) survive;
        # otherwise start from zero.
        if self.raw is not None and len(self.raw) == ENTRY_SIZE:
            buffer[offset:offset + ENTRY_SIZE] = self.raw
        else:
            for i in range(ENTRY_SIZE):
                buffer[offset + i] = 0

        # Header: a loaded entry keeps its original header word (used/write/
        # modified/terminal bits) via the raw copy; a fresh entry gets in-use.
        if self.raw is None or len(self.raw) != ENTRY_SIZE:
            buffer[offset] = OBJECT_ENTRY_IN_USE

        # Object name
        write_ndfs_name(buffer, offset + 2, self.object_name, NDFS_NAME_MAX)

        # File type string
        write_ndfs_name(buffer, offset + 18, self.type, NDFS_TYPE_MAX)

        # Versioning, access, flags, device (offsets 22-31)
        write_uint16_be(buffer, offset + 22, self.next_version)
        write_uint16_be(buffer, offset + 24, self.prev_version)
        write_uint16_be(buffer, offset + 26, self.access_bits)
        write_uint16_be(buffer, offset + 28, self.file_type_flags)
        write_uint16_be(buffer, offset + 30, self.device_number)

        # File type code
        buffer[offset + 32] = self.file_type & 0xFF

        # Object index word at 34: high byte = user index, low byte = file slot
        # (keeps the version pointers, which equal this word, consistent).
        buffer[offset + 34] = self.user_index & 0xFF
        buffer[offset + 35] = self.disk_object_index & 0xFF

        # Open counts and timestamps (offsets 36-51)
        write_uint16_be(buffer, offset + 36, self.current_open_count)
        write_uint16_be(buffer, offset + 38, self.total_open_count)
        write_uint32_be(buffer, offset + 40, self.date_created)
        write_uint32_be(buffer, offset + 44, self.last_date_read)
        write_uint32_be(buffer, offset + 48, self.last_date_written)

        # Pages in file
        write_uint32_be(buffer, offset + 52, self.pages_in_file)

        # Bytes in file - 1
        bytes_minus_one = self.bytes_in_file - 1 if self.bytes_in_file > 0 else 0
        write_uint32_be(buffer, offset + 56, bytes_minus_one)

        # File pointer
        if self.file_pointer is not None:
            self.file_pointer.to_bytes(buffer, offset + 60)
