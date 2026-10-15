"""
Tests for contiguous file creation.

Added 2026-07-30. Until then every create path in the library produced an indexed
file, so SINTRAN's ``@CREATE-FILE name pages`` -- the form that makes the machine
report ``CONTINUOUS FILE`` -- had nothing behind it, and the COSMOS file-access
server's Create-file operation could not be answered when it carried a page count.

A contiguous file allocates all of its pages up front, consecutive on the pack, and
cannot grow: the pages after the run belong to other files. A write past the
reservation therefore fails rather than silently converting the file to an indexed
one, which is what SINTRAN itself does.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs.constants import NDFS_PAGE_SIZE
from ndfs.filesystem import NdfsFileSystem
from ndfs.object_entry import FT_CONTIGUOUS, FT_INDEXED
from ndfs.types import ImageCreationOptions, ImageTemplate, PointerType


def _make_fs(pages=200):
    """Build an empty in-memory pack with a single SYSTEM user."""
    opts = ImageCreationOptions(
        template=ImageTemplate.Custom,
        directory_name="CONTIG",
        custom_pages=pages,
    )
    return NdfsFileSystem.create_image(opts)


def _entry(ndfs, object_name):
    """Find a file's object entry by name."""
    for obj in ndfs.get_object_entries():
        if obj.object_name.upper() == object_name.upper():
            return obj
    return None


class TestCreateContiguousFile:
    def test_entry_is_flagged_contiguous_and_sized_to_the_reservation(self):
        """The three fields a SINTRAN client reads back off the object entry."""
        ndfs = _make_fs()
        ndfs.create_contiguous_file("SYSTEM/RESERVED:DATA", 8)

        obj = _entry(ndfs, "RESERVED")
        assert obj is not None, "the file was not created"
        assert obj.file_type_flags & FT_CONTIGUOUS, "must carry the contiguous flag"
        assert not (obj.file_type_flags & FT_INDEXED), "must not also claim to be indexed"
        assert obj.pages_in_file == 8, "the whole reservation is allocated"
        assert obj.bytes_in_file == 0, "a fresh contiguous file is empty"

    def test_file_pointer_is_contiguous_and_points_at_the_first_page(self):
        """There is no index block: page N of the file is simply block start+N."""
        ndfs = _make_fs()
        ndfs.create_contiguous_file("SYSTEM/RUN:DATA", 6)

        obj = _entry(ndfs, "RUN")
        assert obj.file_pointer.type == PointerType.Contiguous
        assert obj.file_pointer.block_id > 0

    def test_the_run_is_one_unbroken_allocated_range(self):
        """The property the whole file kind exists for, checked against the bitmap."""
        ndfs = _make_fs()
        ndfs.create_contiguous_file("SYSTEM/RUN:DATA", 6)

        start = _entry(ndfs, "RUN").file_pointer.block_id
        for i in range(6):
            assert ndfs.is_block_used(start + i), f"block {start + i} must be allocated"

    def test_creation_costs_exactly_the_reservation_and_no_index_block(self):
        """A contiguous file has no structural overhead -- pages reserved is all it takes."""
        ndfs = _make_fs()
        free_before = ndfs.get_free_pages()

        ndfs.create_contiguous_file("SYSTEM/COST:DATA", 7)

        assert free_before - ndfs.get_free_pages() == 7

    def test_zero_pages_is_rejected(self):
        """A file with no pages has no run to point at."""
        ndfs = _make_fs()
        with pytest.raises(ValueError):
            ndfs.create_contiguous_file("SYSTEM/EMPTY:DATA", 0)

    def test_creating_over_an_existing_name_is_rejected(self):
        ndfs = _make_fs()
        ndfs.create_contiguous_file("SYSTEM/ONCE:DATA", 2)
        with pytest.raises(FileExistsError):
            ndfs.create_contiguous_file("SYSTEM/ONCE:DATA", 2)

    def test_a_run_larger_than_the_pack_is_rejected(self):
        """The allocator must refuse rather than hand back a truncated run."""
        ndfs = _make_fs(pages=50)
        with pytest.raises(IOError):
            ndfs.create_contiguous_file("SYSTEM/HUGE:DATA", 10000)


class TestWritingIntoAContiguousFile:
    def test_written_data_reads_back_byte_for_byte(self):
        ndfs = _make_fs()
        ndfs.create_contiguous_file("SYSTEM/PAYLOAD:DATA", 4)

        payload = b"contiguous payload written in place"
        ndfs.write_file("SYSTEM/PAYLOAD:DATA", payload)

        assert ndfs.read_file("SYSTEM/PAYLOAD:DATA") == payload

    def test_a_write_does_not_convert_the_file_or_move_its_run(self):
        ndfs = _make_fs()
        ndfs.create_contiguous_file("SYSTEM/STABLE:DATA", 4)
        start_before = _entry(ndfs, "STABLE").file_pointer.block_id

        ndfs.write_file("SYSTEM/STABLE:DATA", b"some data")

        obj = _entry(ndfs, "STABLE")
        assert obj.file_type_flags & FT_CONTIGUOUS, "must still be contiguous after a write"
        assert obj.file_pointer.block_id == start_before, "the run must not move"
        assert obj.pages_in_file == 4, "the reservation must not shrink to fit the data"

    def test_a_write_that_exactly_fills_the_reservation_is_accepted(self):
        """Guards the boundary from the side that must always fit."""
        ndfs = _make_fs()
        ndfs.create_contiguous_file("SYSTEM/EXACT:DATA", 3)

        exact = bytes((i & 0xFF) for i in range(3 * NDFS_PAGE_SIZE))
        ndfs.write_file("SYSTEM/EXACT:DATA", exact)

        assert ndfs.read_file("SYSTEM/EXACT:DATA") == exact

    def test_a_write_beyond_the_reservation_fails(self):
        """The deliberate design choice: fail, never grow and never convert."""
        ndfs = _make_fs()
        ndfs.create_contiguous_file("SYSTEM/SMALL:DATA", 2)

        with pytest.raises(IOError):
            ndfs.write_file("SYSTEM/SMALL:DATA", bytes(2 * NDFS_PAGE_SIZE + 1))

        obj = _entry(ndfs, "SMALL")
        assert obj.bytes_in_file == 0, "a refused write must not update the length"
        assert obj.pages_in_file == 2, "a refused write must not change the reservation"

    def test_a_shorter_rewrite_does_not_leave_the_old_tail_behind(self):
        """The run is not reallocated, so the write path has to clear the rest."""
        ndfs = _make_fs()
        ndfs.create_contiguous_file("SYSTEM/SHRINK:DATA", 3)

        ndfs.write_file("SYSTEM/SHRINK:DATA", b"\xAB" * (3 * NDFS_PAGE_SIZE))
        ndfs.write_file("SYSTEM/SHRINK:DATA", b"short")

        assert ndfs.read_file("SYSTEM/SHRINK:DATA") == b"short"

    def test_writing_does_not_allocate_or_free_any_page(self):
        """The pages were charged at creation; a write must not move disk usage at all."""
        ndfs = _make_fs()
        ndfs.create_contiguous_file("SYSTEM/FIXED:DATA", 5)
        free_after_create = ndfs.get_free_pages()

        ndfs.write_file("SYSTEM/FIXED:DATA", b"x" * 100)

        assert ndfs.get_free_pages() == free_after_create
