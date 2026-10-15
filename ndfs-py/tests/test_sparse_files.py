"""
Tests for sparse file support -- all-zero, mixed, correct read-back, fewer blocks used.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs.filesystem import NdfsFileSystem
from ndfs.constants import NDFS_PAGE_SIZE
from ndfs.types import ImageTemplate, ImageCreationOptions


def _make_fs(pages=200):
    opts = ImageCreationOptions(
        template=ImageTemplate.Custom,
        directory_name="SPARSE",
        custom_pages=pages,
    )
    return NdfsFileSystem.create_image(opts)


class TestAllZeroSparse:
    def test_all_zero_file_uses_fewer_blocks(self):
        ndfs = _make_fs()
        free_before = ndfs.get_free_pages()

        # A multi-page file whose every page is DECLARED a hole. The holes have
        # to be stated: an all-zero page is not a hole on its own, because a
        # file with invented holes is one SINTRAN cannot READ-BI.
        data = bytearray(NDFS_PAGE_SIZE * 5)
        ndfs.write_file("SYSTEM/ZEROS:DATA", data, holes=range(5))

        free_after = ndfs.get_free_pages()
        # Only the index block: a declared hole allocates no data block.
        pages_used = free_before - free_after
        assert pages_used == 1

    def test_all_zero_file_without_declared_holes_allocates_every_page(self):
        """THE REGRESSION THAT MATTERS: zeros alone must not make holes.

        Deliberately asymmetric -- it fails against the old inference-based
        writer, which is the point; a test that passes under both conventions
        proves nothing. Measured consequence of that writer on a SINTRAN III
        VSX-K pack (2026-08-18): (TCP-IP)TCP-SER-B1..B3-B05:BPUN were copied in
        with 48/62/53 invented holes and the machine refused all three with
        NO SUCH PAGE, while bank 0 (which had no all-zero page) loaded.
        """
        ndfs = _make_fs()
        free_before = ndfs.get_free_pages()

        data = bytearray(NDFS_PAGE_SIZE * 5)
        ndfs.write_file("SYSTEM/ZEROS2:DATA", data)

        pages_used = free_before - ndfs.get_free_pages()
        assert pages_used == 6  # 1 index block + 5 real data pages
        assert ndfs.get_file_blocks("SYSTEM/ZEROS2:DATA").count(0) == 0

    def test_all_zero_reads_back_correctly(self):
        ndfs = _make_fs()
        data = bytearray(NDFS_PAGE_SIZE * 3)
        ndfs.write_file("SYSTEM/ZEROS:DATA", data)

        content = ndfs.read_file("SYSTEM/ZEROS:DATA")
        assert len(content) == NDFS_PAGE_SIZE * 3
        for i in range(len(content)):
            assert content[i] == 0


class TestMixedSparse:
    def test_mixed_sparse_and_data(self):
        ndfs = _make_fs()
        free_before = ndfs.get_free_pages()

        # Create a file with one data page and two zero pages
        data = bytearray(NDFS_PAGE_SIZE * 3)
        # Fill only the first page with data
        for i in range(NDFS_PAGE_SIZE):
            data[i] = 0xAA

        ndfs.write_file("SYSTEM/MIXED:DATA", data, holes=[1, 2])

        free_after = ndfs.get_free_pages()
        pages_used = free_before - free_after
        # 1 index + 1 data: pages 1 and 2 were declared holes.
        assert pages_used == 2

    def test_mixed_reads_back_correctly(self):
        ndfs = _make_fs()
        data = bytearray(NDFS_PAGE_SIZE * 3)
        # Fill first page with 0xBB
        for i in range(NDFS_PAGE_SIZE):
            data[i] = 0xBB
        # Second page all zeros (sparse)
        # Third page with 0xCC
        for i in range(NDFS_PAGE_SIZE * 2, NDFS_PAGE_SIZE * 3):
            data[i] = 0xCC

        ndfs.write_file("SYSTEM/MIXED:DATA", data)
        content = ndfs.read_file("SYSTEM/MIXED:DATA")

        assert len(content) == NDFS_PAGE_SIZE * 3
        # First page
        assert content[0] == 0xBB
        assert content[NDFS_PAGE_SIZE - 1] == 0xBB
        # Second page (sparse zeros)
        assert content[NDFS_PAGE_SIZE] == 0x00
        assert content[NDFS_PAGE_SIZE * 2 - 1] == 0x00
        # Third page
        assert content[NDFS_PAGE_SIZE * 2] == 0xCC
        assert content[NDFS_PAGE_SIZE * 3 - 1] == 0xCC


class TestSparseBlockCount:
    def test_fully_nonzero_uses_all_blocks(self):
        ndfs = _make_fs()
        free_before = ndfs.get_free_pages()

        data = bytearray(NDFS_PAGE_SIZE * 3)
        for i in range(len(data)):
            data[i] = 0xFF
        ndfs.write_file("SYSTEM/FULL:DATA", data)

        free_after = ndfs.get_free_pages()
        pages_used = free_before - free_after
        # Should use 4 blocks: 1 index + 3 data
        assert pages_used == 4

    def test_sparse_file_integrity_after_round_trip(self):
        ndfs1 = _make_fs()
        data = bytearray(NDFS_PAGE_SIZE * 4)
        # Only third page has data
        for i in range(NDFS_PAGE_SIZE * 2, NDFS_PAGE_SIZE * 3):
            data[i] = (i & 0xFF)

        ndfs1.write_file("SYSTEM/SPARSE:DATA", data)

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        content = ndfs2.read_file("SYSTEM/SPARSE:DATA")
        assert len(content) == NDFS_PAGE_SIZE * 4
        # First two pages should be zero
        for i in range(NDFS_PAGE_SIZE * 2):
            assert content[i] == 0
        # Third page should have data
        for i in range(NDFS_PAGE_SIZE * 2, NDFS_PAGE_SIZE * 3):
            assert content[i] == (i & 0xFF)
        # Fourth page should be zero
        for i in range(NDFS_PAGE_SIZE * 3, NDFS_PAGE_SIZE * 4):
            assert content[i] == 0
