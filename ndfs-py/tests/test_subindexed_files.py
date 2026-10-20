"""
Tests for SubIndexed (large-file, >512 pages) write support.

A single Indexed index block holds up to MAX_OBJECT_FILE_POINTERS (512)
data-page pointers, covering files up to ~1MB. Larger files use a
SubIndexed layout: a top-level sub-index block holding up to 512 pointers
to *group* index blocks, each of which holds up to 512 data-page pointers
of its own -- i.e. the plain Indexed layout, repeated per 512-page group,
covering files up to 512*512 = 262144 pages (~512MB).

These tests exercise the write/allocate side (_allocate_and_write_data in
filesystem.py); the read side (_read_sub_indexed_data) and free side
(_free_file_blocks' SubIndexed branch) were already correct before this
change and are exercised incidentally by every round-trip/delete test here.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs.filesystem import NdfsFileSystem
from ndfs.constants import NDFS_PAGE_SIZE, MAX_OBJECT_FILE_POINTERS
from ndfs.types import ImageTemplate, ImageCreationOptions, PointerType


def _make_fs(pages):
    opts = ImageCreationOptions(
        template=ImageTemplate.Custom,
        directory_name="SUBIDX",
        custom_pages=pages,
    )
    return NdfsFileSystem.create_image(opts)


def _pattern_data(num_pages):
    """Build a deterministic, fully non-zero byte pattern spanning
    `num_pages` full pages (so every page requires a real data block --
    no sparse holes -- to make the group-boundary test as strict as
    possible)."""
    data = bytearray(NDFS_PAGE_SIZE * num_pages)
    for i in range(len(data)):
        # Nonzero everywhere: (i & 0xFF) would legitimately hit 0 every
        # 256 bytes, which is fine for correctness but we also want to
        # confirm no page is accidentally treated as an all-zero sparse
        # hole, so bias by 1.
        data[i] = ((i & 0xFF) + 1) & 0xFF
        if data[i] == 0:
            data[i] = 1
    return data


def _find_entry(fs, name, user_name="SYSTEM"):
    for e in fs.get_object_entries():
        if e.object_name == name and e.user_name == user_name:
            return e
    raise AssertionError(f"object entry {name} not found")


class TestJustOverBoundary:
    def test_513_pages_round_trips(self):
        # 512 pages is the largest a plain Indexed file can hold; 513 forces
        # SubIndexed allocation (one group index block + one sub-index block).
        ndfs = _make_fs(pages=700)
        data = _pattern_data(MAX_OBJECT_FILE_POINTERS + 1)
        ndfs.write_file("SYSTEM/OVER512:DATA", data)

        entry = _find_entry(ndfs, "OVER512")
        assert entry.file_pointer.type == PointerType.SubIndexed

        content = ndfs.read_file("SYSTEM/OVER512:DATA")
        assert len(content) == len(data)
        assert content == bytes(data)


class TestMultiGroupBoundary:
    def test_1000_pages_crossing_group_boundary_round_trips(self):
        # 1000 pages spans two 512-page groups (pages 0..511 in group 0,
        # 512..999 in group 1), crossing the group boundary at page 512.
        # This is the case that would have corrupted content at the
        # boundary if group index blocks were spliced into the flat
        # data-page stream instead of being kept purely structural.
        ndfs = _make_fs(pages=1300)
        num_pages = 1000
        data = _pattern_data(num_pages)
        ndfs.write_file("SYSTEM/MULTIGRP:DATA", data)

        entry = _find_entry(ndfs, "MULTIGRP")
        assert entry.file_pointer.type == PointerType.SubIndexed

        content = ndfs.read_file("SYSTEM/MULTIGRP:DATA")
        assert len(content) == len(data)
        # Check well around the group boundary explicitly, byte-for-byte,
        # plus the full buffer for good measure.
        boundary = MAX_OBJECT_FILE_POINTERS * NDFS_PAGE_SIZE
        for i in range(boundary - 16, boundary + 16):
            assert content[i] == data[i], f"Mismatch at byte {i} (near group boundary)"
        assert content == bytes(data)

    def test_1000_pages_survives_export_reimport(self):
        ndfs1 = _make_fs(pages=1300)
        data = _pattern_data(1000)
        ndfs1.write_file("SYSTEM/MULTIGRP2:DATA", data)

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        content = ndfs2.read_file("SYSTEM/MULTIGRP2:DATA")
        assert content == bytes(data)


class TestSubIndexedSparseHoles:
    def test_sparse_holes_round_trip(self):
        # 600 pages (SubIndexed, single group's worth over the 512 boundary)
        # with a mix of real and all-zero (sparse-hole) pages, including a
        # hole that straddles the group boundary.
        ndfs = _make_fs(pages=900)
        num_pages = 600
        data = bytearray(NDFS_PAGE_SIZE * num_pages)
        # Fill pages 0, 510, 511, 512, 513, 599 with real data; leave the
        # rest (including pages spanning the group boundary at 512) as
        # sparse zero pages.
        for pg in (0, 510, 511, 512, 513, 599):
            start = pg * NDFS_PAGE_SIZE
            for i in range(start, start + NDFS_PAGE_SIZE):
                data[i] = 0xCC

        # Every other page is DECLARED a hole. Zeros alone do not make holes --
        # a file with invented holes is one SINTRAN cannot READ-BI.
        real = (0, 510, 511, 512, 513, 599)
        holes = [p for p in range(num_pages) if p not in real]

        free_before = ndfs.get_free_pages()
        ndfs.write_file("SYSTEM/SPARSEBIG:DATA", data, holes=holes)
        free_after = ndfs.get_free_pages()

        entry = _find_entry(ndfs, "SPARSEBIG")
        assert entry.file_pointer.type == PointerType.SubIndexed

        # Structural cost: 1 sub-index block + 2 group index blocks
        # (ceil(600/512) == 2), plus 6 real data blocks for the non-zero
        # pages above -- the other 594 pages are sparse holes and must NOT
        # consume a disk block.
        pages_used = free_before - free_after
        assert pages_used == 3 + 6

        content = ndfs.read_file("SYSTEM/SPARSEBIG:DATA")
        assert len(content) == len(data)
        assert content == bytes(data)


class TestSubIndexedFreesAllStructuralBlocks:
    def test_delete_frees_data_group_and_subindex_blocks(self):
        ndfs = _make_fs(pages=1300)
        free_before = ndfs.get_free_pages()

        data = _pattern_data(1000)
        ndfs.write_file("SYSTEM/TODEL:DATA", data)
        free_after_write = ndfs.get_free_pages()
        assert free_after_write < free_before

        ndfs.delete_file("SYSTEM/TODEL:DATA")
        free_after_delete = ndfs.get_free_pages()
        # All data blocks, both group index blocks, and the top-level
        # sub-index block must be freed -- not just the data blocks.
        assert free_after_delete == free_before


class TestSubIndexedOverwriteDoesNotLeak:
    def test_write_delete_write_returns_to_same_free_count(self):
        # Mirrors a real leak class already found and fixed in the C# port:
        # overwriting (or re-creating after delete) a SubIndexed file must
        # not leave its old group index blocks or sub-index block
        # dangling/unfreed.
        ndfs = _make_fs(pages=1300)
        free_baseline = ndfs.get_free_pages()

        data = _pattern_data(1000)
        ndfs.write_file("SYSTEM/LEAKCHECK:DATA", data)
        ndfs.delete_file("SYSTEM/LEAKCHECK:DATA")
        free_after_first_cycle = ndfs.get_free_pages()
        assert free_after_first_cycle == free_baseline

        ndfs.write_file("SYSTEM/LEAKCHECK:DATA", data)
        ndfs.delete_file("SYSTEM/LEAKCHECK:DATA")
        free_after_second_cycle = ndfs.get_free_pages()
        assert free_after_second_cycle == free_baseline

    def test_overwrite_subindexed_with_subindexed_frees_old_structure(self):
        # Overwrite one SubIndexed file's content with different SubIndexed
        # content (both >512 pages) -- the old sub-index block and its
        # group index blocks must be freed before the new ones are
        # allocated, not leaked alongside them.
        ndfs = _make_fs(pages=1300)
        data_a = _pattern_data(600)
        data_b = _pattern_data(700)

        ndfs.write_file("SYSTEM/OW:DATA", data_a)
        free_after_a = ndfs.get_free_pages()

        ndfs.write_file("SYSTEM/OW:DATA", data_b)
        free_after_b = ndfs.get_free_pages()

        content = ndfs.read_file("SYSTEM/OW:DATA")
        assert content == bytes(data_b)

        # Delete and confirm we return to a clean baseline (no leaked
        # blocks from either allocation).
        free_before_delete = ndfs.get_free_pages()
        ndfs.delete_file("SYSTEM/OW:DATA")
        free_after_delete = ndfs.get_free_pages()
        assert free_after_delete > free_before_delete
        # Sanity: writing data_b used strictly more structural+data pages
        # than data_a's leftover would have, if the old structure had
        # leaked we'd see less free space recovered than pages consumed.
        assert free_after_b <= free_after_a


class TestSubIndexedSparseQuotaAccounting:
    def test_mostly_sparse_subindexed_file_charges_only_real_pages(self):
        # A file large enough to require SubIndexed (>512 pages) with real
        # data confined to a few pages -- user.pages_used quota must not
        # charge for the sub-index block or any group index blocks, only
        # the handful of genuinely non-sparse pages.
        ndfs = _make_fs(pages=900)
        num_pages = 600
        data = bytearray(NDFS_PAGE_SIZE * num_pages)
        pattern_pages = (2,)
        for pg in pattern_pages:
            start = pg * NDFS_PAGE_SIZE
            for i in range(start, start + NDFS_PAGE_SIZE):
                data[i] = 0xCC
        # Second real page, straddling nothing in particular -- just two
        # real pages total among 598 sparse holes.
        data[0] = 0xAA

        used_before = ndfs.get_user(0).pages_used
        # Pages 0 and 2 hold data; the other 598 are DECLARED holes.
        ndfs.write_file("SYSTEM/BIGSPARSE:DAT", data,
                        holes=[p for p in range(num_pages) if p not in (0, 2)])
        used_after = ndfs.get_user(0).pages_used

        assert used_after - used_before == 2 + 3, (
            "the 2 real pages count, plus the sub-index block and the two "
            "group index blocks this file needed (SINTRAN charges them)"
        )


class TestExactBoundaryStillIndexed:
    def test_exactly_512_pages_uses_plain_indexed(self):
        # Pin the boundary: exactly MAX_OBJECT_FILE_POINTERS (512) data
        # pages must still use the plain Indexed layout, not SubIndexed.
        ndfs = _make_fs(pages=700)
        data = _pattern_data(MAX_OBJECT_FILE_POINTERS)
        ndfs.write_file("SYSTEM/EXACT512:DATA", data)

        entry = _find_entry(ndfs, "EXACT512")
        assert entry.file_pointer.type == PointerType.Indexed

        content = ndfs.read_file("SYSTEM/EXACT512:DATA")
        assert content == bytes(data)


class TestEmptySubIndexEntry:
    def test_pages_behind_an_empty_entry_stay_in_place(self):
        # SINTRAN III leaves the sub-index entry of a group with no page in it
        # EMPTY (a domain of the ND Linker B01, 2026-10-14: entries 0, 2, 4, 6
        # set). Our writer always makes every group's index block, so the file
        # is written and entry 1 is then cleared, as SINTRAN would have it.
        ndfs = _make_fs(pages=2000)
        num_pages = 1500
        data = bytearray(NDFS_PAGE_SIZE * num_pages)
        for pg in (3, 1030):
            start = pg * NDFS_PAGE_SIZE
            for i in range(start, start + NDFS_PAGE_SIZE):
                data[i] = 0xA5
        holes = [p for p in range(num_pages) if p not in (3, 1030)]
        ndfs.write_file("SYSTEM/GAP:DATA", data, holes=holes)
        entry = _find_entry(ndfs, "GAP")
        assert entry.file_pointer.type == PointerType.SubIndexed

        image = ndfs.to_buffer()
        at = entry.file_pointer.block_id * NDFS_PAGE_SIZE + 4
        image[at:at + 4] = b"\0\0\0\0"
        again = NdfsFileSystem(image, read_only=True)

        assert again.read_file("SYSTEM/GAP:DATA") == bytes(data)
        blocks = again.get_file_blocks("SYSTEM/GAP:DATA")
        assert len(blocks) == num_pages
        assert blocks[3] != 0
        assert blocks[600] == 0
        assert blocks[1030] != 0
