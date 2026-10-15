"""
Tests for ndfs.block_pointer -- BlockPointer class.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest
from ndfs.block_pointer import BlockPointer
from ndfs.types import PointerType


class TestBlockPointerConstructor:
    def test_defaults_to_block_id_0_contiguous(self):
        bp = BlockPointer()
        assert bp.block_id == 0
        assert bp.type == PointerType.Contiguous

    def test_creates_with_explicit_values(self):
        bp = BlockPointer(100, PointerType.Indexed)
        assert bp.block_id == 100
        assert bp.type == PointerType.Indexed

    def test_masks_block_id_to_30_bits(self):
        bp = BlockPointer(0xFFFFFFFF, PointerType.Contiguous)
        assert bp.block_id == 0x3FFFFFFF


class TestBlockPointerFromNative:
    def test_decodes_contiguous_pointer(self):
        bp = BlockPointer.from_native(0x00000005)
        assert bp.block_id == 5
        assert bp.type == PointerType.Contiguous

    def test_decodes_indexed_pointer(self):
        bp = BlockPointer.from_native(0x40000005)
        assert bp.block_id == 5
        assert bp.type == PointerType.Indexed

    def test_decodes_sub_indexed_pointer(self):
        bp = BlockPointer.from_native(0x80000005)
        assert bp.block_id == 5
        assert bp.type == PointerType.SubIndexed

    def test_decodes_reserved_pointer(self):
        bp = BlockPointer.from_native(0xC0000005)
        assert bp.block_id == 5
        assert bp.type == PointerType.Reserved


class TestBlockPointerNativeProperty:
    def test_encodes_contiguous(self):
        assert BlockPointer(5, PointerType.Contiguous).native == 0x00000005

    def test_encodes_indexed(self):
        assert BlockPointer(5, PointerType.Indexed).native == 0x40000005

    def test_encodes_sub_indexed(self):
        assert BlockPointer(5, PointerType.SubIndexed).native == 0x80000005

    def test_encodes_large_block_id_with_indexed(self):
        assert BlockPointer(100, PointerType.Indexed).native == 0x40000064

    def test_round_trips_through_from_native(self):
        values = [0x00000005, 0x40000064, 0x80001000, 0xC0FFFFFF]
        for v in values:
            assert BlockPointer.from_native(v).native == v


class TestBlockPointerIsValid:
    def test_returns_false_for_block_id_0(self):
        assert BlockPointer(0, PointerType.Indexed).is_valid() is False

    def test_returns_false_for_reserved_type(self):
        assert BlockPointer(5, PointerType.Reserved).is_valid() is False

    def test_returns_true_for_valid_contiguous(self):
        assert BlockPointer(5, PointerType.Contiguous).is_valid() is True

    def test_returns_true_for_valid_indexed(self):
        assert BlockPointer(100, PointerType.Indexed).is_valid() is True


class TestBlockPointerBytes:
    def test_parses_big_endian_bytes(self):
        data = bytes([0x40, 0x00, 0x00, 0x64])
        bp = BlockPointer.from_bytes(data, 0)
        assert bp.block_id == 100
        assert bp.type == PointerType.Indexed

    def test_parses_at_offset(self):
        data = bytes([0xFF, 0xFF, 0x80, 0x00, 0x10, 0x00])
        bp = BlockPointer.from_bytes(data, 2)
        assert bp.block_id == 0x1000
        assert bp.type == PointerType.SubIndexed

    def test_round_trips_through_bytes(self):
        bp = BlockPointer(12345, PointerType.Indexed)
        raw = bp.to_bytes_array()
        bp2 = BlockPointer.from_bytes(raw, 0)
        assert bp2.block_id == bp.block_id
        assert bp2.type == bp.type

    def test_writes_to_buffer_at_offset(self):
        buf = bytearray(8)
        bp = BlockPointer(5, PointerType.Contiguous)
        bp.to_bytes(buf, 4)
        assert buf[4] == 0x00
        assert buf[5] == 0x00
        assert buf[6] == 0x00
        assert buf[7] == 0x05
