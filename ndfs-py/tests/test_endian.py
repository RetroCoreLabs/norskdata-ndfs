"""
Tests for ndfs.endian -- big-endian read/write helpers.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest
from ndfs.endian import read_uint16_be, read_uint32_be, write_uint16_be, write_uint32_be


# ── read_uint16_be ──────────────────────────────────────────────────


class TestReadUint16BE:
    def test_reads_0x0000(self):
        assert read_uint16_be(bytes([0x00, 0x00]), 0) == 0

    def test_reads_0xFFFF(self):
        assert read_uint16_be(bytes([0xFF, 0xFF]), 0) == 0xFFFF

    def test_reads_0x1234(self):
        assert read_uint16_be(bytes([0x12, 0x34]), 0) == 0x1234

    def test_reads_at_offset(self):
        assert read_uint16_be(bytes([0x00, 0x00, 0xAB, 0xCD]), 2) == 0xABCD


# ── read_uint32_be ──────────────────────────────────────────────────


class TestReadUint32BE:
    def test_reads_0x00000000(self):
        assert read_uint32_be(bytes([0, 0, 0, 0]), 0) == 0

    def test_reads_0xFFFFFFFF(self):
        assert read_uint32_be(bytes([0xFF, 0xFF, 0xFF, 0xFF]), 0) == 0xFFFFFFFF

    def test_reads_0x12345678(self):
        assert read_uint32_be(bytes([0x12, 0x34, 0x56, 0x78]), 0) == 0x12345678

    def test_reads_at_offset(self):
        data = bytes([0x00, 0x00, 0xDE, 0xAD, 0xBE, 0xEF])
        assert read_uint32_be(data, 2) == 0xDEADBEEF


# ── write_uint16_be ─────────────────────────────────────────────────


class TestWriteUint16BE:
    def test_writes_0x1234(self):
        buf = bytearray(2)
        write_uint16_be(buf, 0, 0x1234)
        assert buf[0] == 0x12
        assert buf[1] == 0x34

    def test_round_trips_with_read(self):
        buf = bytearray(4)
        write_uint16_be(buf, 1, 0xABCD)
        assert read_uint16_be(buf, 1) == 0xABCD


# ── write_uint32_be ─────────────────────────────────────────────────


class TestWriteUint32BE:
    def test_writes_0xDEADBEEF(self):
        buf = bytearray(4)
        write_uint32_be(buf, 0, 0xDEADBEEF)
        assert buf[0] == 0xDE
        assert buf[1] == 0xAD
        assert buf[2] == 0xBE
        assert buf[3] == 0xEF

    def test_round_trips_with_read(self):
        buf = bytearray(8)
        write_uint32_be(buf, 2, 0x12345678)
        assert read_uint32_be(buf, 2) == 0x12345678

    def test_handles_high_bit_correctly_unsigned(self):
        buf = bytearray(4)
        write_uint32_be(buf, 0, 0x80000000)
        assert read_uint32_be(buf, 0) == 0x80000000
