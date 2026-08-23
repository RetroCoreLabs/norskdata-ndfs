"""
Reading a floppy whose pages will not read.

A real ND floppy comes off the drive with individual pages unreadable. The file
listing is what an archive is kept for, so a lost page must cost only what was
written ON that page:

  - an object-file data page  -> its own 32 entries, nothing else
  - an object-file index page -> the entries the pages under it name
  - a user-file data page     -> the names of the users on it
  - a bit-file page           -> the allocation state it covered

A page that cannot be read is modelled here the way it happens on the media:
the pointer names a page outside the image, so _read_page() fails. Every case
has the same name in tests/damaged-media.test.ts, tests/test_damaged_media.py
and tests/test_damaged_media.c, and asserts the same numbers.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
HackerCorp Labs - https://github.com/HackerCorpLabs
"""
import pytest

from ndfs.filesystem import NdfsFileSystem
from ndfs.block_pointer import BlockPointer
from ndfs.types import ImageTemplate, ImageCreationOptions, PointerType
from ndfs.constants import NDFS_PAGE_SIZE, MASTER_BLOCK_OFFSET

# A page number no image in these tests is long enough to hold.
UNREADABLE = 9000

OBJECT_PTR = MASTER_BLOCK_OFFSET + 0x10
USER_PTR = MASTER_BLOCK_OFFSET + 0x14
BIT_PTR = MASTER_BLOCK_OFFSET + 0x18

# 40 files: 32 in the first object-file data page, 8 in the second.
FILE_COUNT = 40


def make_floppy(file_count: int = FILE_COUNT) -> bytearray:
    fs = NdfsFileSystem.create_image(
        ImageCreationOptions(template=ImageTemplate.Floppy360KB, directory_name="DAMAGED")
    )
    for i in range(file_count):
        fs.write_file("SYSTEM/FILE-%d:DATA" % i, bytes([i & 0xFF]))
    return fs.to_buffer()


def point_away(image, offset, ptr_type=PointerType.Contiguous):
    BlockPointer(UNREADABLE, ptr_type).to_bytes(image, offset)


def break_object_data_page(image, slot):
    """Break one pointer slot of the object file's index page."""
    index_page = BlockPointer.from_bytes(image, OBJECT_PTR).block_id
    point_away(image, index_page * NDFS_PAGE_SIZE + slot * 4)


def break_user_data_page(image, slot=0):
    """Break the single pointer slot of the user file's index page."""
    index_page = BlockPointer.from_bytes(image, USER_PTR).block_id
    point_away(image, index_page * NDFS_PAGE_SIZE + slot * 4)


def names(fs):
    return sorted(o.full_name for o in fs.get_object_entries())


class TestIntactFloppyIsTheBaseline:
    def test_lists_every_file_and_reports_no_damage(self):
        fs = NdfsFileSystem(make_floppy())
        assert len(fs.get_object_entries()) == FILE_COUNT
        assert fs.get_damage_report() == {
            "object_pages": 0,
            "user_pages": 0,
            "bit_file_pages": 0,
        }
        assert [u.user_name for u in fs.get_users()] == ["SYSTEM"]


class TestObjectFileDataPageThatWillNotRead:
    def test_costs_its_own_32_entries_and_no_others(self):
        image = make_floppy()
        intact = names(NdfsFileSystem(bytearray(image)))
        break_object_data_page(image, 0)
        fs = NdfsFileSystem(image)

        assert len(fs.get_object_entries()) == FILE_COUNT - 32
        assert fs.get_damage_report()["object_pages"] == 1
        for n in names(fs):
            assert n in intact

    def test_leaves_surviving_entries_at_their_object_index(self):
        image = make_floppy()
        before = {
            o.full_name: o.object_index
            for o in NdfsFileSystem(bytearray(image)).get_object_entries()
        }
        break_object_data_page(image, 0)
        for o in NdfsFileSystem(image).get_object_entries():
            assert o.object_index == before[o.full_name]

    def test_two_lost_pages_are_counted_separately(self):
        image = make_floppy()
        break_object_data_page(image, 0)
        break_object_data_page(image, 1)
        fs = NdfsFileSystem(image)
        assert len(fs.get_object_entries()) == 0
        assert fs.get_damage_report()["object_pages"] == 2


class TestObjectFileIndexPageItself:
    def test_cannot_be_worked_around(self):
        image = make_floppy()
        point_away(image, OBJECT_PTR, PointerType.Indexed)
        with pytest.raises(Exception):
            NdfsFileSystem(image)


class TestUserFileDataPageThatWillNotRead:
    def test_costs_the_user_names_and_not_one_file(self):
        image = make_floppy()
        break_user_data_page(image)
        fs = NdfsFileSystem(image)

        assert len(fs.get_object_entries()) == FILE_COUNT
        assert len(fs.get_users()) == 0
        assert fs.get_damage_report()["user_pages"] == 1
        assert fs.get_damage_report()["object_pages"] == 0
        for o in fs.get_object_entries():
            assert o.user_index == 0


class TestBitFilePageThatWillNotRead:
    def test_costs_the_allocation_state_and_not_the_listing(self):
        image = make_floppy()
        point_away(image, BIT_PTR)
        fs = NdfsFileSystem(image)

        assert len(fs.get_object_entries()) == FILE_COUNT
        assert fs.get_damage_report()["bit_file_pages"] == 1
        assert fs.get_damage_report()["object_pages"] == 0


class TestAllThreeAtOnce:
    def test_reports_each_count_and_still_lists_what_survived(self):
        image = make_floppy()
        break_object_data_page(image, 0)
        break_user_data_page(image)
        point_away(image, BIT_PTR)
        fs = NdfsFileSystem(image)

        assert len(fs.get_object_entries()) == FILE_COUNT - 32
        assert fs.get_damage_report() == {
            "object_pages": 1,
            "user_pages": 1,
            "bit_file_pages": 1,
        }
        assert fs.get_directory_name() == "DAMAGED"
