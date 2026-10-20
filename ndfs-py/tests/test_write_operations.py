"""
Tests for write operations -- small, 1-page, multi-page, overwrite, auto quota, disk full, delete, rename.

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
        directory_name="WRITETEST",
        custom_pages=pages,
    )
    return NdfsFileSystem.create_image(opts)


class TestSmallFileWrite:
    def test_write_and_read_small_file(self):
        ndfs = _make_fs()
        ndfs.write_file("SYSTEM/HELLO:DATA", b"Hello NDFS!")
        content = ndfs.read_file("SYSTEM/HELLO:DATA")
        assert content == b"Hello NDFS!"

    def test_write_single_byte(self):
        ndfs = _make_fs()
        ndfs.write_file("SYSTEM/BYTE:DATA", b"X")
        content = ndfs.read_file("SYSTEM/BYTE:DATA")
        assert content == b"X"

    def test_write_empty_data(self):
        ndfs = _make_fs()
        ndfs.write_file("SYSTEM/EMPTY:DATA", b"")
        # File exists but with minimal data
        assert ndfs.file_exists("SYSTEM/EMPTY:DATA") is True


class TestOnePageFile:
    def test_exact_page_size(self):
        ndfs = _make_fs()
        data = bytearray(NDFS_PAGE_SIZE)
        for i in range(len(data)):
            data[i] = 0xAA
        ndfs.write_file("SYSTEM/ONEPAGE:DATA", data)
        content = ndfs.read_file("SYSTEM/ONEPAGE:DATA")
        assert len(content) == NDFS_PAGE_SIZE
        assert content[0] == 0xAA
        assert content[NDFS_PAGE_SIZE - 1] == 0xAA


class TestMultiPageFile:
    def test_multi_page_data_integrity(self):
        ndfs = _make_fs()
        data = bytearray(5000)
        for i in range(len(data)):
            data[i] = i & 0xFF
        ndfs.write_file("SYSTEM/BIGFILE:DATA", data)
        content = ndfs.read_file("SYSTEM/BIGFILE:DATA")
        assert len(content) == 5000
        for i in range(len(data)):
            assert content[i] == data[i], f"Mismatch at byte {i}"

    def test_exact_two_pages(self):
        ndfs = _make_fs()
        data = bytearray(NDFS_PAGE_SIZE * 2)
        for i in range(len(data)):
            data[i] = (i % 251) & 0xFF
        ndfs.write_file("SYSTEM/TWOPG:DATA", data)
        content = ndfs.read_file("SYSTEM/TWOPG:DATA")
        assert len(content) == NDFS_PAGE_SIZE * 2
        for i in range(len(data)):
            assert content[i] == data[i]


class TestOverwrite:
    def test_overwrite_with_larger_data(self):
        ndfs = _make_fs()
        ndfs.write_file("SYSTEM/FILE:DATA", b"version 1")
        ndfs.write_file("SYSTEM/FILE:DATA", b"version 2 is longer")
        content = ndfs.read_file("SYSTEM/FILE:DATA")
        assert content == b"version 2 is longer"

    def test_overwrite_with_smaller_data(self):
        ndfs = _make_fs()
        ndfs.write_file("SYSTEM/FILE:DATA", b"long version one data here")
        ndfs.write_file("SYSTEM/FILE:DATA", b"short")
        content = ndfs.read_file("SYSTEM/FILE:DATA")
        assert content == b"short"

    def test_overwrite_preserves_other_files(self):
        ndfs = _make_fs()
        ndfs.write_file("SYSTEM/A:DATA", b"aaa")
        ndfs.write_file("SYSTEM/B:DATA", b"bbb")
        ndfs.write_file("SYSTEM/A:DATA", b"AAA")
        assert ndfs.read_file("SYSTEM/A:DATA") == b"AAA"
        assert ndfs.read_file("SYSTEM/B:DATA") == b"bbb"


class TestAutoQuota:
    def test_quota_expands_automatically(self):
        ndfs = _make_fs(200)
        # Write a large file that exceeds initial quota
        data = bytearray(NDFS_PAGE_SIZE * 10)
        for i in range(len(data)):
            data[i] = i & 0xFF
        ndfs.write_file("SYSTEM/BIG:DATA", data)
        content = ndfs.read_file("SYSTEM/BIG:DATA")
        assert len(content) == len(data)


class TestDiskFull:
    def test_raises_on_disk_full(self):
        # Create a tiny filesystem and fill it up
        ndfs = _make_fs(50)
        free = ndfs.get_free_pages()
        # Write NON-ZERO data requiring more pages than available
        # (zero pages would be sparse and not allocate blocks)
        need_pages = free + 10
        data = bytearray(NDFS_PAGE_SIZE * need_pages)
        for i in range(len(data)):
            data[i] = 0xFF
        with pytest.raises(IOError):
            ndfs.write_file("SYSTEM/TOOBIG:DATA", data)


class TestDeleteFreesBlocks:
    def test_delete_frees_blocks(self):
        ndfs = _make_fs()
        free_before = ndfs.get_free_pages()
        ndfs.write_file("SYSTEM/TODEL:DATA", b"delete me please")
        free_after_write = ndfs.get_free_pages()
        assert free_after_write < free_before

        ndfs.delete_file("SYSTEM/TODEL:DATA")
        free_after_delete = ndfs.get_free_pages()
        assert free_after_delete == free_before

    def test_delete_nonexistent_raises(self):
        ndfs = _make_fs()
        with pytest.raises(FileNotFoundError):
            ndfs.delete_file("SYSTEM/NOPE:DATA")

    def test_delete_updates_user_usage(self):
        ndfs = _make_fs()
        user = ndfs.get_user(0)
        used_before = user.pages_used

        ndfs.write_file("SYSTEM/TEMP:DATA", b"temporary data")
        used_after_write = ndfs.get_user(0).pages_used
        assert used_after_write > used_before

        ndfs.delete_file("SYSTEM/TEMP:DATA")
        used_after_delete = ndfs.get_user(0).pages_used
        assert used_after_delete == used_before


class TestRename:
    def test_rename_file(self):
        ndfs = _make_fs()
        ndfs.write_file("SYSTEM/OLD:DATA", b"content")
        ndfs.rename("SYSTEM/OLD:DATA", "NEW:TEXT")
        assert ndfs.file_exists("SYSTEM/OLD:DATA") is False
        assert ndfs.file_exists("SYSTEM/NEW:TEXT") is True
        assert ndfs.read_file("SYSTEM/NEW:TEXT") == b"content"


class TestSparseQuotaAccounting:
    """Regression tests for user.pages_used quota tracking.

    Previously pages_used was charged using the file's LOGICAL page count
    (which wrongly counts sparse holes as consuming real disk space).
    Quota tracks the real, non-sparse data pages actually allocated on disk
    plus the file's index and sub-index blocks, which real SINTRAN also
    charges to the owner (measured, docs/NDFS-ALLOCATION-RULES.md section 7).
    """

    def test_fully_sparse_charges_zero_pages_used(self):
        ndfs = _make_fs()
        used_before = ndfs.get_user(0).pages_used

        data = bytearray(NDFS_PAGE_SIZE * 5)  # every page DECLARED a hole
        ndfs.write_file("SYSTEM/ZERO:DAT", data, holes=range(5))

        used_after = ndfs.get_user(0).pages_used
        assert used_after - used_before == 1, (
            "a fully-sparse file allocates no real data blocks; only its "
            "index block is charged to quota"
        )

    def test_mixed_sparse_charges_only_real_pages(self):
        ndfs = _make_fs()
        used_before = ndfs.get_user(0).pages_used

        data = bytearray(NDFS_PAGE_SIZE * 10)
        for i in range(NDFS_PAGE_SIZE * 3):  # pages 0-2: real
            data[i] = 0xAA
        # pages 3-9 DECLARED as holes (their being zero would not suffice)
        ndfs.write_file("SYSTEM/MIXED:DAT", data, holes=range(3, 10))

        used_after = ndfs.get_user(0).pages_used
        assert used_after - used_before == 3 + 1, (
            "the 3 real (non-zero) pages and the index block count toward "
            "quota -- not the 7 holes"
        )

    def test_fully_real_charges_exact_page_count_no_index_overhead(self):
        ndfs = _make_fs()
        used_before = ndfs.get_user(0).pages_used

        data = bytearray(NDFS_PAGE_SIZE * 6)
        for i in range(len(data)):
            data[i] = 0xFF  # no zero pages at all
        ndfs.write_file("SYSTEM/FULL:DAT", data)

        used_after = ndfs.get_user(0).pages_used
        assert used_after - used_before == 6 + 1, (
            "the 6 real data pages plus the index block"
        )

    def test_delete_refunds_exactly_what_create_charged(self):
        ndfs = _make_fs()
        used_before = ndfs.get_user(0).pages_used

        data = bytearray(NDFS_PAGE_SIZE * 10)
        for i in range(NDFS_PAGE_SIZE * 4):  # 4 real, 6 sparse
            data[i] = 0xAA
        ndfs.write_file("SYSTEM/MIXED2:DAT", data)
        ndfs.delete_file("SYSTEM/MIXED2:DAT")

        used_after = ndfs.get_user(0).pages_used
        assert used_after == used_before, "delete must refund exactly what create charged"

    def test_overwrite_grow_then_shrink_tracks_real_page_count(self):
        # Previously the update-file path never adjusted pages_used at all
        # on overwrite -- a separate bug fixed alongside the sparse
        # over-charge one.
        ndfs = _make_fs()
        used_baseline = ndfs.get_user(0).pages_used

        small = bytearray(NDFS_PAGE_SIZE * 2)
        for i in range(len(small)):
            small[i] = 0xAA
        ndfs.write_file("SYSTEM/GROW:DAT", small)
        used_after_first_write = ndfs.get_user(0).pages_used
        assert used_after_first_write - used_baseline == 2 + 1

        big = bytearray(NDFS_PAGE_SIZE * 8)
        for i in range(len(big)):
            big[i] = 0xBB
        ndfs.write_file("SYSTEM/GROW:DAT", big)
        used_after_overwrite = ndfs.get_user(0).pages_used
        assert used_after_overwrite - used_baseline == 8 + 1, (
            "overwriting with a larger file must charge the NEW real page "
            "count, replacing the old"
        )

        tiny = bytearray(NDFS_PAGE_SIZE * 1)
        for i in range(len(tiny)):
            tiny[i] = 0xCC
        ndfs.write_file("SYSTEM/GROW:DAT", tiny)
        used_after_shrink = ndfs.get_user(0).pages_used
        assert used_after_shrink - used_baseline == 1 + 1, (
            "overwriting with a smaller file must refund down to the new "
            "(smaller) real page count"
        )

    def test_rename_nonexistent_raises(self):
        ndfs = _make_fs()
        with pytest.raises(FileNotFoundError):
            ndfs.rename("SYSTEM/NOPE:DATA", "NEW:DATA")
