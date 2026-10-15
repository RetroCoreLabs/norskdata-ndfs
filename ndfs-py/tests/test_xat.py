"""
Tests for XAT (Extended Attribute) sidecar file support.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import json
import pytest

from ndfs.object_entry import ObjectEntry
from ndfs.xat import (
    object_entry_to_xat,
    xat_to_object_entry,
    serialize_xat,
    deserialize_xat,
    get_xat_filename,
    is_xat_file,
    ALL_XAT_KEYS,
    XAT_OBJECT_NAME,
    XAT_TYPE,
    XAT_USER_NAME,
    XAT_USER_INDEX,
    XAT_ACCESS_BITS,
    XAT_FILE_TYPE_FLAGS,
    XAT_FILE_TYPE,
    XAT_DEVICE_NUMBER,
    XAT_NEXT_VERSION,
    XAT_PREV_VERSION,
    XAT_PAGES_IN_FILE,
    XAT_BYTES_IN_FILE,
    XAT_DATE_CREATED,
    XAT_LAST_READ_DATE,
    XAT_LAST_WRITE_DATE,
)
from ndfs.filesystem import NdfsFileSystem
from ndfs.constants import NDFS_PAGE_SIZE, NDFS_NAME_TERMINATOR
from ndfs.endian import write_uint32_be
from ndfs.block_pointer import BlockPointer
from ndfs.types import PointerType


def create_test_image(total_pages=50, dir_name="TESTDISK"):
    """Create a minimal valid NDFS test image."""
    image_size = total_pages * NDFS_PAGE_SIZE
    image = bytearray(image_size)

    mb_off = 2016
    for i in range(len(dir_name)):
        image[mb_off + i] = ord(dir_name[i])
    if len(dir_name) < 16:
        image[mb_off + len(dir_name)] = NDFS_NAME_TERMINATOR

    obj_ptr = BlockPointer(7, PointerType.Indexed)
    obj_ptr.to_bytes(image, mb_off + 0x10)

    usr_ptr = BlockPointer(9, PointerType.Indexed)
    usr_ptr.to_bytes(image, mb_off + 0x14)

    bit_ptr = BlockPointer(11, PointerType.Contiguous)
    bit_ptr.to_bytes(image, mb_off + 0x18)

    write_uint32_be(image, mb_off + 0x1C, total_pages - 12)

    obj_idx_off = 7 * NDFS_PAGE_SIZE
    obj_data_ptr = BlockPointer(8, PointerType.Contiguous)
    obj_data_ptr.to_bytes(image, obj_idx_off)

    usr_idx_off = 9 * NDFS_PAGE_SIZE
    usr_data_ptr = BlockPointer(10, PointerType.Contiguous)
    usr_data_ptr.to_bytes(image, usr_idx_off)

    usr_data_off = 10 * NDFS_PAGE_SIZE
    image[usr_data_off + 0] = 0x81
    sys_name = "SYSTEM"
    for i in range(len(sys_name)):
        image[usr_data_off + 2 + i] = ord(sys_name[i])
    image[usr_data_off + 2 + len(sys_name)] = NDFS_NAME_TERMINATOR
    write_uint32_be(image, usr_data_off + 28, 1000)
    write_uint32_be(image, usr_data_off + 32, 0)
    image[usr_data_off + 37] = 0

    bitmap_off = 11 * NDFS_PAGE_SIZE
    image[bitmap_off] = 0xFF
    image[bitmap_off + 1] = 0x0F

    return bytes(image)


class TestObjectEntryToXat:
    def test_serializes_all_fields(self):
        entry = ObjectEntry()
        entry.object_name = "README"
        entry.type = "TEXT"
        entry.user_name = "SYSTEM"
        entry.user_index = 0
        entry.access_bits = 1279
        entry.file_type = 3
        entry.pages_in_file = 1
        entry.bytes_in_file = 1024
        entry.date_created = 12345
        entry.last_date_read = 67890
        entry.last_date_written = 11111
        entry.device_number = 5
        entry.next_version = 42
        entry.prev_version = 43

        xat = object_entry_to_xat(entry)

        assert xat[XAT_OBJECT_NAME] == "README"
        assert xat[XAT_TYPE] == "TEXT"
        assert xat[XAT_USER_NAME] == "SYSTEM"
        assert xat[XAT_USER_INDEX] == 0
        assert xat[XAT_ACCESS_BITS] == 1279
        assert xat[XAT_FILE_TYPE_FLAGS] == 0
        assert xat[XAT_FILE_TYPE] == 3
        assert xat[XAT_DEVICE_NUMBER] == 5
        assert xat[XAT_NEXT_VERSION] == 42
        assert xat[XAT_PREV_VERSION] == 43
        assert xat[XAT_PAGES_IN_FILE] == 1
        assert xat[XAT_BYTES_IN_FILE] == 1024
        assert xat[XAT_DATE_CREATED] == 12345
        assert xat[XAT_LAST_READ_DATE] == 67890
        assert xat[XAT_LAST_WRITE_DATE] == 11111

    def test_includes_all_keys(self):
        entry = ObjectEntry()
        xat = object_entry_to_xat(entry)
        for key in ALL_XAT_KEYS:
            assert key in xat


class TestXatToObjectEntry:
    def test_applies_properties(self):
        xat = {
            XAT_OBJECT_NAME: "HELLO",
            XAT_TYPE: "PROG",
            XAT_USER_NAME: "ADMIN",
            XAT_USER_INDEX: 2,
            XAT_ACCESS_BITS: 511,
            XAT_FILE_TYPE: 1,
            XAT_PAGES_IN_FILE: 5,
            XAT_BYTES_IN_FILE: 8192,
            XAT_DATE_CREATED: 100,
            XAT_LAST_READ_DATE: 200,
            XAT_LAST_WRITE_DATE: 300,
        }
        entry = ObjectEntry()
        xat_to_object_entry(xat, entry)

        assert entry.object_name == "HELLO"
        assert entry.type == "PROG"
        assert entry.user_name == "ADMIN"
        assert entry.user_index == 2
        assert entry.access_bits == 511
        assert entry.file_type == 1
        assert entry.pages_in_file == 5
        assert entry.bytes_in_file == 8192
        assert entry.date_created == 100
        assert entry.last_date_read == 200
        assert entry.last_date_written == 300

    def test_only_modifies_present_fields(self):
        entry = ObjectEntry()
        entry.object_name = "ORIGINAL"
        entry.access_bits = 999

        xat = {XAT_ACCESS_BITS: 42}
        xat_to_object_entry(xat, entry)

        assert entry.object_name == "ORIGINAL"
        assert entry.access_bits == 42


class TestJsonRoundTrip:
    def test_serialize_deserialize_preserves_data(self):
        entry = ObjectEntry()
        entry.object_name = "TESTFILE"
        entry.type = "DATA"
        entry.user_name = "SYSTEM"
        entry.user_index = 0
        entry.access_bits = 1279
        entry.file_type = 0
        entry.pages_in_file = 3
        entry.bytes_in_file = 5000
        entry.date_created = 99
        entry.last_date_read = 100
        entry.last_date_written = 101

        original = object_entry_to_xat(entry)
        json_str = serialize_xat(original)
        restored = deserialize_xat(json_str)

        for key in ALL_XAT_KEYS:
            assert restored[key] == original[key]

    def test_round_trip_through_entry(self):
        entry = ObjectEntry()
        entry.object_name = "ROUNDTRIP"
        entry.type = "SYMB"
        entry.user_name = "USER1"
        entry.user_index = 1
        entry.access_bits = 255
        entry.file_type = 2

        xat = object_entry_to_xat(entry)
        json_str = serialize_xat(xat)
        restored = deserialize_xat(json_str)

        new_entry = ObjectEntry()
        xat_to_object_entry(restored, new_entry)

        assert new_entry.object_name == "ROUNDTRIP"
        assert new_entry.type == "SYMB"
        assert new_entry.user_name == "USER1"
        assert new_entry.user_index == 1
        assert new_entry.access_bits == 255
        assert new_entry.file_type == 2

    def test_deserialize_rejects_non_object(self):
        with pytest.raises(ValueError):
            deserialize_xat('"just a string"')
        with pytest.raises(ValueError):
            deserialize_xat("[1, 2, 3]")


class TestGetXatFilename:
    def test_appends_xat(self):
        assert get_xat_filename("README.TEXT") == "README.TEXT.xat"
        assert get_xat_filename("HELLO.PROG") == "HELLO.PROG.xat"
        assert get_xat_filename("noext") == "noext.xat"


class TestIsXatFile:
    def test_identifies_xat_files(self):
        assert is_xat_file("README.TEXT.xat") is True
        assert is_xat_file("file.xat") is True
        assert is_xat_file("FILE.XAT") is True

    def test_rejects_non_xat_files(self):
        assert is_xat_file("README.TEXT") is False
        assert is_xat_file("xat") is False
        assert is_xat_file(".xat") is False
        assert is_xat_file("") is False


class TestFilesystemXatIntegration:
    def test_get_file_properties(self):
        image = create_test_image()
        fs = NdfsFileSystem(image)

        fs.write_file("SYSTEM/HELLO:TEXT", bytes([72, 69, 76, 76, 79]))

        props = fs.get_file_properties("SYSTEM/HELLO:TEXT")
        assert props is not None
        assert props[XAT_OBJECT_NAME] == "HELLO"
        assert props[XAT_TYPE] == "TEXT"
        assert props[XAT_USER_NAME] == "SYSTEM"

    def test_get_file_properties_not_found(self):
        image = create_test_image()
        fs = NdfsFileSystem(image)
        assert fs.get_file_properties("SYSTEM/NOFILE:DATA") is None

    def test_read_file_with_properties(self):
        image = create_test_image()
        fs = NdfsFileSystem(image)

        test_data = bytes([1, 2, 3, 4, 5])
        fs.write_file("SYSTEM/MYFILE:DATA", test_data)

        data, props = fs.read_file_with_properties("SYSTEM/MYFILE:DATA")
        assert len(data) == 5
        assert data[0] == 1
        assert props[XAT_OBJECT_NAME] == "MYFILE"
        assert props[XAT_BYTES_IN_FILE] == 5

    def test_write_file_with_properties(self):
        image = create_test_image()
        fs = NdfsFileSystem(image)

        test_data = bytes([10, 20, 30])
        xat_props = {
            XAT_ACCESS_BITS: 1279,
            XAT_FILE_TYPE: 3,
            XAT_DATE_CREATED: 42,
        }

        fs.write_file_with_properties("SYSTEM/RESTORED:TEXT", test_data, xat_props)

        props = fs.get_file_properties("SYSTEM/RESTORED:TEXT")
        assert props is not None
        assert props[XAT_ACCESS_BITS] == 1279
        assert props[XAT_FILE_TYPE] == 3
        assert props[XAT_DATE_CREATED] == 42

        data = fs.read_file("SYSTEM/RESTORED:TEXT")
        assert len(data) == 3
        assert data[0] == 10

    def test_full_round_trip(self):
        image = create_test_image()
        fs = NdfsFileSystem(image)

        original_data = bytes([65, 66, 67, 68])
        fs.write_file("SYSTEM/RTFILE:TEXT", original_data)

        data, properties = fs.read_file_with_properties("SYSTEM/RTFILE:TEXT")
        properties[XAT_ACCESS_BITS] = 777

        fs.delete_file("SYSTEM/RTFILE:TEXT")
        fs.write_file_with_properties("SYSTEM/RTFILE:TEXT", data, properties)

        data2, props2 = fs.read_file_with_properties("SYSTEM/RTFILE:TEXT")
        assert len(data2) == 4
        assert data2[0] == 65
        assert props2[XAT_ACCESS_BITS] == 777


class TestXatDeviceAndVersion:
    """Tests for the 3 new XAT fields: device_number, next_version, prev_version."""

    def test_from_object_captures_all_three(self):
        entry = ObjectEntry()
        entry.object_name = "TEST"
        entry.type = "DATA"
        entry.device_number = 42
        entry.next_version = 100
        entry.prev_version = 200

        xat = object_entry_to_xat(entry)
        assert xat[XAT_DEVICE_NUMBER] == 42
        assert xat[XAT_NEXT_VERSION] == 100
        assert xat[XAT_PREV_VERSION] == 200

    def test_json_round_trip_preserves_all_three(self):
        entry = ObjectEntry()
        entry.device_number = 7
        entry.next_version = 55
        entry.prev_version = 33

        xat = object_entry_to_xat(entry)
        json_str = serialize_xat(xat)
        restored = deserialize_xat(json_str)

        assert restored[XAT_DEVICE_NUMBER] == 7
        assert restored[XAT_NEXT_VERSION] == 55
        assert restored[XAT_PREV_VERSION] == 33

    def test_apply_restores_device_but_not_versions(self):
        """device_number is restored; next/prev_version are NOT (they're dangling refs)."""
        xat = {
            XAT_DEVICE_NUMBER: 42,
            XAT_NEXT_VERSION: 100,
            XAT_PREV_VERSION: 200,
        }
        entry = ObjectEntry()
        entry.device_number = 0
        entry.next_version = 999  # sentinel
        entry.prev_version = 888  # sentinel

        xat_to_object_entry(xat, entry)

        assert entry.device_number == 42  # restored
        assert entry.next_version == 999  # NOT changed
        assert entry.prev_version == 888  # NOT changed
