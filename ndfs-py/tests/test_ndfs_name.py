"""
Tests for ndfs.ndfs_name -- NDFS name encoding/decoding.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest
from ndfs.ndfs_name import read_ndfs_name, write_ndfs_name
from ndfs.constants import NDFS_NAME_TERMINATOR


class TestReadNdfsName:
    def test_reads_name_terminated_by_0x27(self):
        data = bytes([0x54, 0x45, 0x53, 0x54, 0x27, 0x27, 0x27])
        assert read_ndfs_name(data, 0, 7) == "TEST"

    def test_reads_name_terminated_by_null(self):
        data = bytes([0x48, 0x45, 0x4C, 0x4C, 0x4F, 0x00])
        assert read_ndfs_name(data, 0, 6) == "HELLO"

    def test_reads_full_length_name(self):
        data = bytearray(16)
        for i in range(16):
            data[i] = 0x41 + i  # A-P
        assert read_ndfs_name(data, 0, 16) == "ABCDEFGHIJKLMNOP"

    def test_reads_at_offset(self):
        data = bytes([0x00, 0x00, 0x41, 0x42, 0x27])
        assert read_ndfs_name(data, 2, 3) == "AB"

    def test_returns_empty_string_for_immediate_terminator(self):
        data = bytes([0x27, 0x27])
        assert read_ndfs_name(data, 0, 2) == ""


class TestWriteNdfsName:
    def test_writes_name_terminator_then_nuls(self):
        # SINTRAN writes one 0x27 terminator after the name, then NULs (verified
        # against real on-disk entries), NOT a field full of terminators.
        buf = bytearray(8)
        write_ndfs_name(buf, 0, "TEST", 8)
        assert buf[0] == 0x54  # T
        assert buf[1] == 0x45  # E
        assert buf[2] == 0x53  # S
        assert buf[3] == 0x54  # T
        assert buf[4] == NDFS_NAME_TERMINATOR
        assert buf[5] == 0x00
        assert buf[6] == 0x00
        assert buf[7] == 0x00

    def test_uppercases_the_name(self):
        buf = bytearray(8)
        write_ndfs_name(buf, 0, "hello", 8)
        assert buf[0] == 0x48  # H
        assert buf[1] == 0x45  # E

    def test_truncates_to_max_len(self):
        buf = bytearray(4)
        write_ndfs_name(buf, 0, "TOOLONGNAME", 4)
        assert read_ndfs_name(buf, 0, 4) == "TOOL"

    def test_round_trips_with_read_ndfs_name(self):
        buf = bytearray(16)
        write_ndfs_name(buf, 0, "NORDISK", 16)
        assert read_ndfs_name(buf, 0, 16) == "NORDISK"
