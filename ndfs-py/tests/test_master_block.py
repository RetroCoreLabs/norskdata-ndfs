"""
Tests for ndfs.master_block -- MasterBlock class.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import os
import pytest

from ndfs.master_block import MasterBlock
from ndfs.block_pointer import BlockPointer
from ndfs.types import PointerType, ChecksumValidation
from ndfs.constants import (
    MASTER_BLOCK_OFFSET,
    NDFS_PAGE_SIZE,
    NDFS_NAME_TERMINATOR,
)
from ndfs.endian import write_uint32_be, write_uint16_be

FIXTURE_DIR = os.path.join(os.path.dirname(__file__), "fixtures")


def build_page0(
    name: str,
    obj_ptr: int,
    usr_ptr: int,
    bit_ptr: int,
    unreserved: int,
) -> bytearray:
    """Build a minimal valid page 0 buffer with a master block."""
    page = bytearray(NDFS_PAGE_SIZE)
    off = MASTER_BLOCK_OFFSET

    for i in range(min(len(name), 16)):
        page[off + i] = ord(name[i])
    if len(name) < 16:
        page[off + len(name)] = NDFS_NAME_TERMINATOR

    write_uint32_be(page, off + 0x10, obj_ptr)
    write_uint32_be(page, off + 0x14, usr_ptr)
    write_uint32_be(page, off + 0x18, bit_ptr)
    write_uint32_be(page, off + 0x1C, unreserved)

    return page


class TestMasterBlockFromBytes:
    def test_parses_directory_name(self):
        page = build_page0("TESTDISK", 0x40000002, 0x40000005, 0x00000008, 100)
        mb = MasterBlock.from_bytes(page)
        assert mb.directory_name == "TESTDISK"

    def test_parses_block_pointers(self):
        page = build_page0("DISK", 0x40000002, 0x40000005, 0x00000008, 50)
        mb = MasterBlock.from_bytes(page)

        assert mb.object_file_pointer.block_id == 2
        assert mb.object_file_pointer.type == PointerType.Indexed

        assert mb.user_file_pointer.block_id == 5
        assert mb.user_file_pointer.type == PointerType.Indexed

        assert mb.bit_file_pointer.block_id == 8
        assert mb.bit_file_pointer.type == PointerType.Contiguous

    def test_parses_unreserved_pages(self):
        page = build_page0("DISK", 0x40000002, 0x40000005, 0x00000008, 12345)
        mb = MasterBlock.from_bytes(page)
        assert mb.unreserved_pages == 12345

    def test_throws_on_too_small_buffer(self):
        with pytest.raises(ValueError):
            MasterBlock.from_bytes(bytearray(100))


class TestMasterBlockIsValid:
    def test_returns_true_for_valid_master_block(self):
        page = build_page0("VALID", 0x40000002, 0x40000005, 0x00000008, 100)
        mb = MasterBlock.from_bytes(page)
        assert mb.is_valid() is True

    def test_returns_true_for_name_only(self):
        page = build_page0("NAMEONLY", 0, 0, 0, 0)
        mb = MasterBlock.from_bytes(page)
        assert mb.is_valid() is True

    def test_returns_false_for_non_printable_name_and_no_pointers(self):
        page = bytearray(NDFS_PAGE_SIZE)
        page[MASTER_BLOCK_OFFSET] = 0x01  # control char
        page[MASTER_BLOCK_OFFSET + 1] = NDFS_NAME_TERMINATOR
        mb = MasterBlock.from_bytes(page)
        assert mb.is_valid() is False


class TestMasterBlockWriteRoundTrip:
    def test_round_trips_name_and_pointers(self):
        page = build_page0("ROUNDTRIP", 0x40000010, 0x40000020, 0x00000030, 999)
        mb = MasterBlock.from_bytes(page)

        page2 = bytearray(NDFS_PAGE_SIZE)
        mb.write_to_bytes(page2)

        mb2 = MasterBlock.from_bytes(page2)
        assert mb2.directory_name == "ROUNDTRIP"
        assert mb2.object_file_pointer.block_id == 0x10
        assert mb2.user_file_pointer.block_id == 0x20
        assert mb2.bit_file_pointer.block_id == 0x30
        assert mb2.unreserved_pages == 999


class TestMasterBlockExtendedInfo:
    def test_calculates_checksum_correctly(self):
        page = build_page0("HARDDISK", 0x40000002, 0x40000005, 0x00000008, 100)

        ext = 2000
        system_number = 100
        flag_word = 0x0051
        pages_available = 38400

        pages_lo = pages_available & 0xFFFF
        pages_hi = (pages_available >> 16) & 0xFFFF
        checksum = ((pages_lo ^ pages_hi ^ flag_word ^ 0 ^ 0 ^ 0) + system_number) & 0xFFFF

        write_uint16_be(page, ext, checksum)
        write_uint16_be(page, ext + 2, 0)   # reserved1
        write_uint16_be(page, ext + 4, 0)   # reserved2
        write_uint16_be(page, ext + 6, 0)   # reserved3
        write_uint16_be(page, ext + 8, flag_word)
        write_uint16_be(page, ext + 10, system_number)
        write_uint32_be(page, ext + 12, pages_available)

        mb = MasterBlock.from_bytes(page)
        assert mb.checksum_state == ChecksumValidation.Valid
        assert mb.ext_valid is True
        assert mb.ext_pages_available == 38400
        assert mb.ext_last_system_number == 100

    def test_detects_invalid_checksum(self):
        page = build_page0("BADCRC", 0x40000002, 0x40000005, 0x00000008, 100)

        ext = 2000
        write_uint16_be(page, ext, 0xDEAD)  # bad checksum
        write_uint16_be(page, ext + 8, 0x0051)
        write_uint16_be(page, ext + 10, 100)
        write_uint32_be(page, ext + 12, 38400)

        mb = MasterBlock.from_bytes(page)
        assert mb.checksum_state == ChecksumValidation.Invalid
        assert mb.ext_valid is False


class TestMasterBlockFixtures:
    def test_parses_empty_ndfs(self):
        filepath = os.path.join(FIXTURE_DIR, "empty.ndfs")
        with open(filepath, "rb") as f:
            data = f.read()
        page0 = data[:NDFS_PAGE_SIZE]
        mb = MasterBlock.from_bytes(page0)
        assert mb.is_valid() is True
        assert len(mb.directory_name) > 0

    def test_parses_withfiles_ndfs(self):
        filepath = os.path.join(FIXTURE_DIR, "withfiles.ndfs")
        with open(filepath, "rb") as f:
            data = f.read()
        page0 = data[:NDFS_PAGE_SIZE]
        mb = MasterBlock.from_bytes(page0)
        assert mb.is_valid() is True
        assert len(mb.directory_name) > 0
        assert mb.object_file_pointer is not None
        assert mb.object_file_pointer.is_valid() is True
        assert mb.user_file_pointer is not None
        assert mb.user_file_pointer.is_valid() is True
        assert mb.bit_file_pointer is not None
        assert mb.bit_file_pointer.is_valid() is True
