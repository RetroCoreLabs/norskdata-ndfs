"""
Tests for ndfs.object_entry -- ObjectEntry class.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest
from ndfs.object_entry import ObjectEntry
from ndfs.block_pointer import BlockPointer
from ndfs.types import PointerType
from ndfs.constants import ENTRY_SIZE, OBJECT_ENTRY_IN_USE, NDFS_NAME_TERMINATOR
from ndfs.endian import write_uint32_be


def build_object_bytes(
    name: str,
    file_type: str,
    user_index: int,
    pages: int,
    byte_count: int,
    pointer_native: int,
) -> bytearray:
    """Build a raw 64-byte object entry."""
    buf = bytearray(ENTRY_SIZE)
    buf[0] = OBJECT_ENTRY_IN_USE  # 0x80

    # Name at offset 2
    for i in range(min(len(name), 16)):
        buf[2 + i] = ord(name[i])
    if len(name) < 16:
        buf[2 + len(name)] = NDFS_NAME_TERMINATOR

    # Type at offset 18
    for i in range(min(len(file_type), 4)):
        buf[18 + i] = ord(file_type[i])
    if len(file_type) < 4:
        buf[18 + len(file_type)] = NDFS_NAME_TERMINATOR

    buf[34] = user_index
    write_uint32_be(buf, 52, pages)
    write_uint32_be(buf, 56, byte_count - 1 if byte_count > 0 else 0)  # stored as bytes-1
    write_uint32_be(buf, 60, pointer_native)

    return buf


class TestObjectEntryFromBytes:
    def test_parses_valid_object_entry(self):
        data = build_object_bytes("TESTFILE", "DATA", 0, 5, 10240, 0x40000064)
        entry = ObjectEntry.from_bytes(data, 0)
        assert entry is not None
        assert entry.object_name == "TESTFILE"
        assert entry.type == "DATA"
        assert entry.user_index == 0
        assert entry.pages_in_file == 5
        assert entry.bytes_in_file == 10240
        assert entry.file_pointer is not None
        assert entry.file_pointer.block_id == 100
        assert entry.file_pointer.type == PointerType.Indexed

    def test_returns_none_for_unused_entry(self):
        data = bytearray(ENTRY_SIZE)
        data[0] = 0x00  # not in use
        assert ObjectEntry.from_bytes(data, 0) is None

    def test_handles_1_byte_file_correctly(self):
        data = build_object_bytes("TINY", "DATA", 0, 1, 1, 0x00000010)
        entry = ObjectEntry.from_bytes(data, 0)
        assert entry.bytes_in_file == 1


class TestObjectEntryFullName:
    def test_returns_name_type_format(self):
        entry = ObjectEntry()
        entry.object_name = "README"
        entry.type = "TEXT"
        assert entry.full_name == "README:TEXT"


class TestObjectEntryFileTypeAsText:
    def test_returns_DATA_for_type_0(self):
        entry = ObjectEntry()
        entry.file_type = 0
        assert entry.file_type_as_text == "DATA"

    def test_returns_PROG_for_type_1(self):
        entry = ObjectEntry()
        entry.file_type = 1
        assert entry.file_type_as_text == "PROG"


class TestObjectEntryToBytesRoundTrip:
    def test_round_trips_all_fields(self):
        entry = ObjectEntry()
        entry.object_name = "MYFILE"
        entry.type = "PROG"
        entry.user_index = 3
        entry.pages_in_file = 10
        entry.bytes_in_file = 20000
        entry.file_pointer = BlockPointer(50, PointerType.Indexed)

        buf = bytearray(ENTRY_SIZE)
        entry.to_bytes(buf, 0)

        parsed = ObjectEntry.from_bytes(buf, 0)
        assert parsed is not None
        assert parsed.object_name == "MYFILE"
        assert parsed.type == "PROG"
        assert parsed.user_index == 3
        assert parsed.pages_in_file == 10
        assert parsed.bytes_in_file == 20000
        assert parsed.file_pointer.block_id == 50
        assert parsed.file_pointer.type == PointerType.Indexed


def test_extended_fields_roundtrip():
    """Versioning, access, flags, open counts and dates survive a round trip."""
    from ndfs.object_entry import FT_INDEXED
    entry = ObjectEntry()
    entry.object_name = "LOAD"
    entry.type = "MODE"
    entry.next_version = 20
    entry.prev_version = 20
    entry.access_bits = 0x00FF
    entry.file_type_flags = FT_INDEXED
    entry.total_open_count = 145
    entry.date_created = 0x01020304
    entry.last_date_read = 0x05060708
    entry.last_date_written = 0x090A0B0C
    entry.pages_in_file = 5
    entry.bytes_in_file = 4621
    entry.file_pointer = BlockPointer(18419, PointerType.Indexed)

    buf = bytearray(64)
    entry.to_bytes(buf, 0)
    p = ObjectEntry.from_bytes(buf, 0)
    assert p.next_version == 20
    assert p.prev_version == 20
    assert p.access_bits == 0x00FF
    assert p.file_type_flags == FT_INDEXED
    assert p.total_open_count == 145
    assert p.date_created == 0x01020304
    assert p.last_date_read == 0x05060708
    assert p.last_date_written == 0x090A0B0C

    buf2 = bytearray(64)
    p.to_bytes(buf2, 0)
    assert bytes(buf2) == bytes(buf)


def test_preserves_unmodelled_bytes():
    """Bytes 33 and 35 (unmodelled) survive re-serialization via raw."""
    buf = bytearray(range(1, 65))
    buf[0] = 0x80
    p = ObjectEntry.from_bytes(buf, 0)
    p.access_bits = 0x03FF
    out = bytearray(64)
    p.to_bytes(out, 0)
    assert out[33] == buf[33]
    assert out[35] == buf[35]
    assert out[26] == 0x03
    assert out[27] == 0xFF
