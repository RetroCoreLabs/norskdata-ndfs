"""
Shared pytest fixtures for NDFS tests.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import os
import pytest

from ndfs import (
    NDFS_PAGE_SIZE,
    NDFS_NAME_TERMINATOR,
    BlockPointer,
    write_uint32_be,
)
from ndfs.types import PointerType


FIXTURE_DIR = os.path.join(os.path.dirname(__file__), "fixtures")


def create_test_image(total_pages: int = 50, dir_name: str = "TESTDISK") -> bytearray:
    """Create a minimal valid NDFS image in memory.

    Layout:
        Page 0:  Master block
        Page 7:  Object file index block
        Page 8:  Object file data block (32 entries)
        Page 9:  User file index block
        Page 10: User file data block (32 entries)
        Page 11: Bit file (bitmap)
    """
    image_size = total_pages * NDFS_PAGE_SIZE
    image = bytearray(image_size)

    # Page 0: Master block at offset 2016
    mb_off = 2016
    # Directory name
    for i in range(len(dir_name)):
        image[mb_off + i] = ord(dir_name[i])
    if len(dir_name) < 16:
        image[mb_off + len(dir_name)] = NDFS_NAME_TERMINATOR

    # Object file pointer at page 7 (Indexed)
    obj_ptr = BlockPointer(7, PointerType.Indexed)
    obj_ptr.to_bytes(image, mb_off + 0x10)

    # User file pointer at page 9 (Indexed)
    usr_ptr = BlockPointer(9, PointerType.Indexed)
    usr_ptr.to_bytes(image, mb_off + 0x14)

    # Bit file pointer at page 11 (Contiguous)
    bit_ptr = BlockPointer(11, PointerType.Contiguous)
    bit_ptr.to_bytes(image, mb_off + 0x18)

    # Unreserved pages
    write_uint32_be(image, mb_off + 0x1C, total_pages - 12)

    # Page 7: Object file index block - pointer to page 8
    obj_idx_off = 7 * NDFS_PAGE_SIZE
    obj_data_ptr = BlockPointer(8, PointerType.Contiguous)
    obj_data_ptr.to_bytes(image, obj_idx_off)

    # Page 8: Object file data block (empty - no files yet)

    # Page 9: User file index block - pointer to page 10
    usr_idx_off = 9 * NDFS_PAGE_SIZE
    usr_data_ptr = BlockPointer(10, PointerType.Contiguous)
    usr_data_ptr.to_bytes(image, usr_idx_off)

    # Page 10: User file data block - create SYSTEM user
    usr_data_off = 10 * NDFS_PAGE_SIZE
    image[usr_data_off + 0] = 0x81  # valid user flag
    # User name "SYSTEM"
    sys_name = "SYSTEM"
    for i in range(len(sys_name)):
        image[usr_data_off + 2 + i] = ord(sys_name[i])
    image[usr_data_off + 2 + len(sys_name)] = NDFS_NAME_TERMINATOR
    # Pages reserved = 1000
    write_uint32_be(image, usr_data_off + 28, 1000)
    # Pages used = 0
    write_uint32_be(image, usr_data_off + 32, 0)
    # User index = 0
    image[usr_data_off + 37] = 0

    # Page 11: Bit file (bitmap) - mark pages 0-11 as used
    bitmap_off = 11 * NDFS_PAGE_SIZE
    # Pages 0-7: first byte = 0xFF (bits 0-7)
    image[bitmap_off] = 0xFF
    # Pages 8-11: second byte = bits 0-3
    image[bitmap_off + 1] = 0x0F

    return image


@pytest.fixture
def test_image():
    """Fixture returning a 50-page test image."""
    return create_test_image()


@pytest.fixture
def large_test_image():
    """Fixture returning a 200-page test image."""
    return create_test_image(200)


@pytest.fixture
def empty_fixture_path():
    """Path to the empty.ndfs fixture file."""
    return os.path.join(FIXTURE_DIR, "empty.ndfs")


@pytest.fixture
def withfiles_fixture_path():
    """Path to the withfiles.ndfs fixture file."""
    return os.path.join(FIXTURE_DIR, "withfiles.ndfs")
