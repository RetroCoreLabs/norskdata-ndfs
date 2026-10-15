"""
Tests for XAT (Extended Attribute) copy-across scenarios.

Validates that XAT metadata and sparse data are preserved when
copying files across NDFS filesystem images.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs.filesystem import NdfsFileSystem
from ndfs.types import ImageTemplate, ImageCreationOptions, UserCreationInfo
from ndfs.constants import NDFS_PAGE_SIZE
from ndfs.xat import (
    object_entry_to_xat,
    xat_to_object_entry,
    serialize_xat,
    deserialize_xat,
    get_xat_filename,
    is_xat_file,
    XAT_OBJECT_NAME,
    XAT_TYPE,
    XAT_USER_NAME,
    XAT_USER_INDEX,
    XAT_ACCESS_BITS,
    XAT_FILE_TYPE_FLAGS,
    XAT_FILE_TYPE,
    XAT_PAGES_IN_FILE,
    XAT_BYTES_IN_FILE,
    XAT_DATE_CREATED,
    XAT_LAST_READ_DATE,
    XAT_LAST_WRITE_DATE,
)


def create_fs(dir_name="TESTDISK"):
    """Helper: create a writable NDFS image via the image creator API."""
    opts = ImageCreationOptions(
        template=ImageTemplate.Floppy360KB,
        directory_name=dir_name,
    )
    return NdfsFileSystem.create_image(opts)


class TestBasicXatRoundTrip:
    """1. Basic XAT round-trip."""

    def test_write_extract_serialize_deserialize_write_verify(self):
        fs1 = create_fs("DISK1")
        content = bytes([10, 20, 30, 40, 50])
        fs1.write_file("SYSTEM/HELLO:TEXT", content)

        data, properties = fs1.read_file_with_properties("SYSTEM/HELLO:TEXT")

        json_str = serialize_xat(properties)
        restored = deserialize_xat(json_str)

        fs2 = create_fs("DISK2")
        fs2.write_file_with_properties("SYSTEM/HELLO:TEXT", data, restored)

        props2 = fs2.get_file_properties("SYSTEM/HELLO:TEXT")
        assert props2 is not None
        assert props2[XAT_OBJECT_NAME] == properties[XAT_OBJECT_NAME]
        assert props2[XAT_TYPE] == properties[XAT_TYPE]
        assert props2[XAT_USER_NAME] == properties[XAT_USER_NAME]
        assert props2[XAT_FILE_TYPE] == properties[XAT_FILE_TYPE]

        read_back = fs2.read_file("SYSTEM/HELLO:TEXT")
        assert len(read_back) == 5
        for i in range(5):
            assert read_back[i] == content[i]


class TestAccessBitsPreservation:
    """2. Access bits preservation."""

    def test_preserves_access_bits(self):
        fs1 = create_fs("DISK1")
        fs1.write_file("SYSTEM/SECURE:DATA", bytes([1, 2, 3]))
        fs1.write_file_with_properties("SYSTEM/SECURE:DATA", bytes([1, 2, 3]), {
            XAT_ACCESS_BITS: 0x7FFF,
        })

        data, properties = fs1.read_file_with_properties("SYSTEM/SECURE:DATA")
        assert properties[XAT_ACCESS_BITS] == 0x7FFF

        json_str = serialize_xat(properties)
        restored = deserialize_xat(json_str)

        fs2 = create_fs("DISK2")
        fs2.write_file_with_properties("SYSTEM/SECURE:DATA", data, restored)

        props2 = fs2.get_file_properties("SYSTEM/SECURE:DATA")
        assert props2 is not None
        assert props2[XAT_ACCESS_BITS] == 0x7FFF


class TestFileTypeFlagsPreservation:
    """3. File type flags preservation."""

    def test_preserves_file_type_flags(self):
        fs1 = create_fs()
        content = bytes([0xAA, 0xBB])
        fs1.write_file("SYSTEM/FLAGS:DATA", content)

        data, properties = fs1.read_file_with_properties("SYSTEM/FLAGS:DATA")
        properties[XAT_FILE_TYPE_FLAGS] = 0x28

        json_str = serialize_xat(properties)
        restored = deserialize_xat(json_str)
        assert restored[XAT_FILE_TYPE_FLAGS] == 0x28


class TestFileTypeCodePreservation:
    """4. File type code preservation."""

    def test_preserves_file_type_symb(self):
        fs1 = create_fs()
        fs1.write_file("SYSTEM/SYMBOLS:SYMB", bytes([1, 2, 3, 4]))
        fs1.write_file_with_properties("SYSTEM/SYMBOLS:SYMB", bytes([1, 2, 3, 4]), {
            XAT_FILE_TYPE: 2,
        })

        data, properties = fs1.read_file_with_properties("SYSTEM/SYMBOLS:SYMB")
        assert properties[XAT_FILE_TYPE] == 2

        json_str = serialize_xat(properties)
        restored = deserialize_xat(json_str)

        fs2 = create_fs()
        fs2.write_file_with_properties("SYSTEM/SYMBOLS:SYMB", data, restored)

        props2 = fs2.get_file_properties("SYSTEM/SYMBOLS:SYMB")
        assert props2 is not None
        assert props2[XAT_FILE_TYPE] == 2


class TestUserAssociationPreservation:
    """5. User association preservation."""

    def test_preserves_user_index_and_name(self):
        opts = ImageCreationOptions(
            template=ImageTemplate.Floppy360KB,
            directory_name="USRDISK",
            users=[
                UserCreationInfo(name="SYSTEM", reserved_pages=100),
                UserCreationInfo(name="GUEST", reserved_pages=50),
            ],
        )
        fs1 = NdfsFileSystem.create_image(opts)

        fs1.write_file("GUEST/MYFILE:TEXT", bytes([65, 66, 67]))

        data, properties = fs1.read_file_with_properties("GUEST/MYFILE:TEXT")
        assert properties[XAT_USER_NAME] == "GUEST"
        guest_index = properties[XAT_USER_INDEX]
        assert isinstance(guest_index, int)
        assert guest_index > 0  # GUEST is not the first user

        json_str = serialize_xat(properties)
        restored = deserialize_xat(json_str)
        assert restored[XAT_USER_NAME] == "GUEST"
        assert restored[XAT_USER_INDEX] == guest_index


class TestDateFieldsPreservation:
    """6. Date fields preservation."""

    def test_preserves_dates(self):
        fs1 = create_fs()
        fs1.write_file("SYSTEM/DATED:DATA", bytes([1]))
        fs1.write_file_with_properties("SYSTEM/DATED:DATA", bytes([1]), {
            XAT_DATE_CREATED: 12345,
            XAT_LAST_WRITE_DATE: 67890,
        })

        data, properties = fs1.read_file_with_properties("SYSTEM/DATED:DATA")
        assert properties[XAT_DATE_CREATED] == 12345
        assert properties[XAT_LAST_WRITE_DATE] == 67890

        json_str = serialize_xat(properties)
        restored = deserialize_xat(json_str)

        fs2 = create_fs()
        fs2.write_file_with_properties("SYSTEM/DATED:DATA", data, restored)

        props2 = fs2.get_file_properties("SYSTEM/DATED:DATA")
        assert props2 is not None
        assert props2[XAT_DATE_CREATED] == 12345
        assert props2[XAT_LAST_WRITE_DATE] == 67890


class TestSparseFileWithXat:
    """7. Sparse file with XAT (4 pages, pages 1 and 3 all zeros)."""

    def test_sparse_zero_pages_preserved(self):
        fs1 = create_fs()

        file_size = 4 * NDFS_PAGE_SIZE
        content = bytearray(file_size)
        # Page 0: 0xAA
        for i in range(NDFS_PAGE_SIZE):
            content[i] = 0xAA
        # Page 1: zeros
        # Page 2: 0xBB
        for i in range(2 * NDFS_PAGE_SIZE, 3 * NDFS_PAGE_SIZE):
            content[i] = 0xBB
        # Page 3: zeros

        # Pages 1 and 3 are DECLARED holes -- being zero would not make them so.
        fs1.write_file("SYSTEM/SPARSE:DATA", bytes(content), holes=[1, 3])

        data, properties = fs1.read_file_with_properties("SYSTEM/SPARSE:DATA")
        # pages_in_file counts the pages ACTUALLY ALLOCATED, not the logical extent:
        # pages 0 and 2 hold data, pages 1 and 3 are sparse holes occupying no disk.
        # Was 4 before the 2026-07-30 correction, which matched SINTRAN -- a real pack
        # records 89 for a file whose index spans 99 slots with 10 holes. The logical
        # extent is not lost, it is ceil(bytes_in_file / NDFS_PAGE_SIZE).
        assert properties[XAT_PAGES_IN_FILE] == 2
        assert properties[XAT_BYTES_IN_FILE] == file_size

        fs2 = create_fs()
        fs2.write_file_with_properties("SYSTEM/SPARSE:DATA", data, properties)

        read_back = fs2.read_file("SYSTEM/SPARSE:DATA")
        assert len(read_back) == file_size

        for i in range(NDFS_PAGE_SIZE):
            assert read_back[i] == 0xAA
        for i in range(NDFS_PAGE_SIZE, 2 * NDFS_PAGE_SIZE):
            assert read_back[i] == 0
        for i in range(2 * NDFS_PAGE_SIZE, 3 * NDFS_PAGE_SIZE):
            assert read_back[i] == 0xBB
        for i in range(3 * NDFS_PAGE_SIZE, 4 * NDFS_PAGE_SIZE):
            assert read_back[i] == 0


class TestLargeSparseFileXat:
    """8. Large sparse file XAT (10 pages, only first and last have data)."""

    def test_large_sparse_data_only_first_and_last(self):
        fs1 = create_fs()

        file_size = 10 * NDFS_PAGE_SIZE
        content = bytearray(file_size)
        for i in range(NDFS_PAGE_SIZE):
            content[i] = 0x11
        for i in range(9 * NDFS_PAGE_SIZE, 10 * NDFS_PAGE_SIZE):
            content[i] = 0xFF

        # Pages 1..8 are DECLARED holes; only 0 and 9 carry data.
        fs1.write_file("SYSTEM/BIGSPARSE:DATA", bytes(content),
                       holes=range(1, 9))

        data, properties = fs1.read_file_with_properties("SYSTEM/BIGSPARSE:DATA")
        # Only the first and last pages hold data; the 8 in between are sparse holes
        # occupying no disk. pages_in_file counts allocated pages, so 2 -- see the note
        # in the sparse round-trip test above.
        assert properties[XAT_PAGES_IN_FILE] == 2

        read_back = fs1.read_file("SYSTEM/BIGSPARSE:DATA")
        assert len(read_back) == file_size

        for i in range(NDFS_PAGE_SIZE):
            assert read_back[i] == 0x11
        for i in range(NDFS_PAGE_SIZE, 9 * NDFS_PAGE_SIZE):
            assert read_back[i] == 0
        for i in range(9 * NDFS_PAGE_SIZE, 10 * NDFS_PAGE_SIZE):
            assert read_back[i] == 0xFF


class TestMixedContentSparse:
    """9. Mixed content sparse: alternating data/zero pages."""

    def test_alternating_data_zero_pages(self):
        fs1 = create_fs()

        num_pages = 6
        file_size = num_pages * NDFS_PAGE_SIZE
        content = bytearray(file_size)
        for p in range(num_pages):
            if p % 2 == 0:
                fill = ((p + 1) * 0x10) & 0xFF
                for i in range(NDFS_PAGE_SIZE):
                    content[p * NDFS_PAGE_SIZE + i] = fill

        fs1.write_file("SYSTEM/MIXED:DATA", bytes(content))
        data, properties = fs1.read_file_with_properties("SYSTEM/MIXED:DATA")

        json_str = serialize_xat(properties)
        restored = deserialize_xat(json_str)

        fs2 = create_fs()
        fs2.write_file_with_properties("SYSTEM/MIXED:DATA", data, restored)

        read_back = fs2.read_file("SYSTEM/MIXED:DATA")
        assert len(read_back) == file_size

        for i in range(file_size):
            assert read_back[i] == content[i]


class TestXatWithOverwrite:
    """10. XAT with overwrite: apply file A's XAT to file B."""

    def test_apply_file_a_xat_to_file_b(self):
        fs1 = create_fs()
        data_a = bytes([1, 2, 3])
        fs1.write_file("SYSTEM/FILEA:TEXT", data_a)
        fs1.write_file_with_properties("SYSTEM/FILEA:TEXT", data_a, {
            XAT_ACCESS_BITS: 0x1234,
            XAT_FILE_TYPE: 3,
        })

        props_a = fs1.get_file_properties("SYSTEM/FILEA:TEXT")
        assert props_a is not None

        data_b = bytes([10, 20, 30, 40, 50])
        fs1.write_file("SYSTEM/FILEB:TEXT", data_b)

        fs1.write_file_with_properties("SYSTEM/FILEB:TEXT", data_b, props_a)

        props_b = fs1.get_file_properties("SYSTEM/FILEB:TEXT")
        assert props_b is not None
        assert props_b[XAT_ACCESS_BITS] == 0x1234
        assert props_b[XAT_FILE_TYPE] == 3

        read_back = fs1.read_file("SYSTEM/FILEB:TEXT")
        assert len(read_back) == 5
        assert read_back[0] == 10
        assert read_back[4] == 50


class TestCopyBetweenTwoImages:
    """11. Copy between two images."""

    def test_copy_files_between_images(self):
        fs1 = create_fs("SRC")
        fs1.write_file("SYSTEM/FILE1:TEXT", bytes([65, 66]))
        fs1.write_file_with_properties("SYSTEM/FILE1:TEXT", bytes([65, 66]), {
            XAT_ACCESS_BITS: 999,
            XAT_FILE_TYPE: 1,
            XAT_DATE_CREATED: 100,
        })

        fs1.write_file("SYSTEM/FILE2:DATA", bytes([67, 68, 69]))
        fs1.write_file_with_properties("SYSTEM/FILE2:DATA", bytes([67, 68, 69]), {
            XAT_ACCESS_BITS: 511,
            XAT_FILE_TYPE: 2,
            XAT_DATE_CREATED: 200,
        })

        fs2 = create_fs("DST")

        # Copy file1
        data1, props1 = fs1.read_file_with_properties("SYSTEM/FILE1:TEXT")
        fs2.write_file_with_properties("SYSTEM/FILE1:TEXT", data1, props1)

        # Copy file2
        data2, props2 = fs1.read_file_with_properties("SYSTEM/FILE2:DATA")
        fs2.write_file_with_properties("SYSTEM/FILE2:DATA", data2, props2)

        # Verify file1 on fs2
        p1 = fs2.get_file_properties("SYSTEM/FILE1:TEXT")
        assert p1 is not None
        assert p1[XAT_ACCESS_BITS] == 999
        assert p1[XAT_FILE_TYPE] == 1
        assert p1[XAT_DATE_CREATED] == 100

        d1 = fs2.read_file("SYSTEM/FILE1:TEXT")
        assert d1[0] == 65
        assert d1[1] == 66

        # Verify file2 on fs2
        p2 = fs2.get_file_properties("SYSTEM/FILE2:DATA")
        assert p2 is not None
        assert p2[XAT_ACCESS_BITS] == 511
        assert p2[XAT_FILE_TYPE] == 2
        assert p2[XAT_DATE_CREATED] == 200

        d2 = fs2.read_file("SYSTEM/FILE2:DATA")
        assert d2[0] == 67
        assert d2[2] == 69


class TestStatusBitsSurviveRewrite:
    """12. Status bits survive re-write."""

    def test_status_bits_preserved_after_rewrite(self):
        fs1 = create_fs()
        fs1.write_file("SYSTEM/PERSIST:DATA", bytes([1, 2, 3]))
        fs1.write_file_with_properties("SYSTEM/PERSIST:DATA", bytes([1, 2, 3]), {
            XAT_ACCESS_BITS: 0x0FFF,
            XAT_DATE_CREATED: 42,
        })

        props_orig = fs1.get_file_properties("SYSTEM/PERSIST:DATA")
        assert props_orig[XAT_ACCESS_BITS] == 0x0FFF

        new_data = bytes([10, 20, 30, 40])
        fs1.delete_file("SYSTEM/PERSIST:DATA")
        fs1.write_file_with_properties("SYSTEM/PERSIST:DATA", new_data, props_orig)

        props_after = fs1.get_file_properties("SYSTEM/PERSIST:DATA")
        assert props_after is not None
        assert props_after[XAT_ACCESS_BITS] == 0x0FFF
        assert props_after[XAT_DATE_CREATED] == 42

        read_back = fs1.read_file("SYSTEM/PERSIST:DATA")
        assert len(read_back) == 4
        assert read_back[0] == 10


class TestXatFilenameConvention:
    """13. XAT filename convention."""

    def test_xat_filename(self):
        assert get_xat_filename("README.TEXT") == "README.TEXT.xat"
        assert get_xat_filename("foo") == "foo.xat"
        assert is_xat_file("README.TEXT.xat") is True
        assert is_xat_file("README.TEXT") is False
