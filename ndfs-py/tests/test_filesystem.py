"""
Tests for ndfs.filesystem -- NdfsFileSystem (the big one).

Comprehensive read/write/user/persistence tests mirroring ndfs-filesystem.test.ts.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
HackerCorp Labs - https://github.com/HackerCorpLabs
"""
import os
import pytest

from ndfs.filesystem import NdfsFileSystem
from ndfs.constants import NDFS_PAGE_SIZE
from conftest import create_test_image, FIXTURE_DIR


class TestNdfsFileSystemConstructor:
    def test_opens_valid_test_image(self):
        image = create_test_image()
        ndfs = NdfsFileSystem(image)
        assert ndfs.get_directory_name() == "TESTDISK"

    def test_throws_on_too_small_image(self):
        with pytest.raises(ValueError):
            NdfsFileSystem(bytearray(100))

    def test_opens_unaligned_over_boundary(self):
        # A valid image plus 1024 trailing bytes (half a page over a boundary)
        # must still open: the partial page is dropped (floored).
        image = create_test_image()
        unaligned = bytearray(image) + bytearray(1024)
        ndfs = NdfsFileSystem(unaligned)
        assert ndfs.unaligned is True
        assert ndfs.get_directory_name() == "TESTDISK"
        # Forced read-only: writes are refused even though opened read-write.
        with pytest.raises(IOError):
            ndfs.delete_file("/NONEXISTENT")

    def test_opens_unaligned_under_boundary(self):
        # One whole page short, then 1024 extra bytes -> floors down a page.
        image = create_test_image()
        unaligned = bytearray(image[: len(image) - NDFS_PAGE_SIZE]) + bytearray(1024)
        ndfs = NdfsFileSystem(unaligned)
        assert ndfs.unaligned is True

    def test_aligned_image_not_unaligned(self):
        ndfs = NdfsFileSystem(create_test_image())
        assert ndfs.unaligned is False


class TestNdfsFileSystemListDirectory:
    def test_lists_users_at_root(self):
        ndfs = NdfsFileSystem(create_test_image())
        entries = ndfs.list_directory("")
        assert len(entries) == 1
        assert entries[0].name == "SYSTEM"
        assert entries[0].is_directory is True

    def test_lists_empty_user_directory(self):
        ndfs = NdfsFileSystem(create_test_image())
        entries = ndfs.list_directory("SYSTEM")
        assert len(entries) == 0

    def test_throws_on_subdirectory_path(self):
        ndfs = NdfsFileSystem(create_test_image())
        with pytest.raises(ValueError):
            ndfs.list_directory("SYSTEM/SUBDIR")


class TestNdfsFileSystemWriteAndRead:
    def test_writes_and_reads_a_small_file(self):
        ndfs = NdfsFileSystem(create_test_image())
        data = b"Hello NDFS!"
        ndfs.write_file("SYSTEM/HELLO:DATA", data)

        read = ndfs.read_file("SYSTEM/HELLO:DATA")
        assert read == b"Hello NDFS!"

    def test_writes_and_reads_a_multi_page_file(self):
        ndfs = NdfsFileSystem(create_test_image(200))
        data = bytearray(5000)
        for i in range(len(data)):
            data[i] = i & 0xFF
        ndfs.write_file("SYSTEM/BIGFILE:DATA", data)

        read = ndfs.read_file("SYSTEM/BIGFILE:DATA")
        assert len(read) == 5000
        for i in range(len(data)):
            assert read[i] == data[i]

    def test_writes_exactly_1_page_file(self):
        ndfs = NdfsFileSystem(create_test_image())
        data = bytearray(NDFS_PAGE_SIZE)
        for i in range(len(data)):
            data[i] = 0xAA
        ndfs.write_file("SYSTEM/ONEPAGE:DATA", data)

        read = ndfs.read_file("SYSTEM/ONEPAGE:DATA")
        assert len(read) == NDFS_PAGE_SIZE
        assert read[0] == 0xAA
        assert read[NDFS_PAGE_SIZE - 1] == 0xAA

    def test_overwrites_existing_file(self):
        ndfs = NdfsFileSystem(create_test_image(200))
        ndfs.write_file("SYSTEM/FILE:DATA", b"version 1")
        ndfs.write_file("SYSTEM/FILE:DATA", b"version 2 is longer")

        read = ndfs.read_file("SYSTEM/FILE:DATA")
        assert read == b"version 2 is longer"

    def test_writes_multiple_files(self):
        ndfs = NdfsFileSystem(create_test_image(200))
        for i in range(5):
            ndfs.write_file(f"SYSTEM/FILE{i}:DATA", f"content {i}".encode())
        entries = ndfs.list_directory("SYSTEM")
        assert len(entries) == 5


class TestNdfsFileSystemDeleteFile:
    def test_deletes_a_file_and_frees_blocks(self):
        ndfs = NdfsFileSystem(create_test_image(200))
        free_before = ndfs.get_free_pages()
        ndfs.write_file("SYSTEM/TODELETE:DATA", b"delete me")
        free_after_write = ndfs.get_free_pages()
        assert free_after_write < free_before

        ndfs.delete_file("SYSTEM/TODELETE:DATA")
        free_after_delete = ndfs.get_free_pages()
        assert free_after_delete == free_before

    def test_throws_on_non_existent_file(self):
        ndfs = NdfsFileSystem(create_test_image())
        with pytest.raises(FileNotFoundError):
            ndfs.delete_file("SYSTEM/NOPE:DATA")


class TestNdfsFileSystemRename:
    def test_renames_a_file(self):
        ndfs = NdfsFileSystem(create_test_image())
        ndfs.write_file("SYSTEM/OLD:DATA", b"data")
        ndfs.rename("SYSTEM/OLD:DATA", "NEW:TEXT")

        assert ndfs.file_exists("SYSTEM/OLD:DATA") is False
        assert ndfs.file_exists("SYSTEM/NEW:TEXT") is True


class TestNdfsFileSystemUserManagement:
    def test_adds_a_user(self):
        ndfs = NdfsFileSystem(create_test_image())
        assert ndfs.add_user("NEWUSER", 500) is True
        users = ndfs.get_users()
        assert len(users) == 2

    def test_rejects_duplicate_user(self):
        ndfs = NdfsFileSystem(create_test_image())
        assert ndfs.add_user("SYSTEM", 500) is False

    def test_removes_user_with_no_files(self):
        ndfs = NdfsFileSystem(create_test_image())
        ndfs.add_user("TEMP", 100)
        users = ndfs.get_users()
        temp_user = None
        for i in range(len(users)):
            if users[i].user_name == "TEMP":
                temp_user = users[i]
                break
        assert temp_user is not None
        assert ndfs.remove_user(temp_user.user_index) is True

    def test_refuses_to_remove_user_with_files(self):
        ndfs = NdfsFileSystem(create_test_image())
        ndfs.write_file("SYSTEM/FILE:DATA", b"data")
        assert ndfs.remove_user(0) is False

    def test_updates_user_quota(self):
        ndfs = NdfsFileSystem(create_test_image())
        assert ndfs.update_user_quota(0, 2000) is True
        assert ndfs.get_user(0).pages_reserved == 2000

    def test_clears_user_password_by_index(self):
        ndfs = NdfsFileSystem(create_test_image())
        assert ndfs.clear_user_password(0) is True
        assert ndfs.get_user(0).password == 0

    def test_clears_user_password_by_name(self):
        ndfs = NdfsFileSystem(create_test_image())
        assert ndfs.clear_user_password("SYSTEM") is True


class TestNdfsFileSystemWritePersistence:
    def test_survives_export_and_reimport(self):
        ndfs1 = NdfsFileSystem(create_test_image(200))
        ndfs1.write_file("SYSTEM/PERSIST:DATA", b"persistent data")
        ndfs1.add_user("USER2", 300)

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        assert ndfs2.get_directory_name() == "TESTDISK"
        content = ndfs2.read_file("SYSTEM/PERSIST:DATA")
        assert content == b"persistent data"
        assert len(ndfs2.get_users()) == 2


class TestNdfsFileSystemReadOnly:
    def test_allows_reads_in_read_only_mode(self):
        ndfs = NdfsFileSystem(create_test_image(), read_only=True)
        assert ndfs.get_directory_name() == "TESTDISK"
        assert len(ndfs.list_directory("")) == 1

    def test_throws_on_write_in_read_only_mode(self):
        ndfs = NdfsFileSystem(create_test_image(), read_only=True)
        with pytest.raises(IOError):
            ndfs.write_file("SYSTEM/X:DATA", b"nope")


class TestNdfsFileSystemFixtures:
    def test_reads_withfiles_ndfs(self):
        filepath = os.path.join(FIXTURE_DIR, "withfiles.ndfs")
        with open(filepath, "rb") as f:
            data = f.read()
        ndfs = NdfsFileSystem(data, read_only=True)

        assert len(ndfs.get_directory_name()) > 0
        users = ndfs.get_users()
        assert len(users) > 0

        root = ndfs.list_directory("")
        assert len(root) > 0

        first_user = root[0].name
        files = ndfs.list_directory(first_user)
        assert len(files) >= 0

    def test_reads_empty_ndfs(self):
        filepath = os.path.join(FIXTURE_DIR, "empty.ndfs")
        with open(filepath, "rb") as f:
            data = f.read()
        ndfs = NdfsFileSystem(data, read_only=True)
        assert len(ndfs.get_directory_name()) > 0


class TestNdfsFileSystemMetadata:
    def test_returns_metadata_for_existing_file(self):
        ndfs = NdfsFileSystem(create_test_image())
        ndfs.write_file("SYSTEM/INFO:TEXT", b"metadata test")
        meta = ndfs.get_metadata("SYSTEM/INFO:TEXT")
        assert meta is not None
        assert meta.name == "INFO"
        assert meta.type == "TEXT"
        assert meta.size == 13
        assert meta.is_directory is False

    def test_returns_none_for_missing_file(self):
        ndfs = NdfsFileSystem(create_test_image())
        assert ndfs.get_metadata("SYSTEM/NOPE:DATA") is None


class TestNdfsFileSystemBitmap:
    def test_reports_block_usage(self):
        ndfs = NdfsFileSystem(create_test_image(50))
        # Blocks 0-11 should be used (system + structures)
        assert ndfs.is_block_used(0) is True
        assert ndfs.is_block_used(11) is True
        # Higher blocks should be free
        assert ndfs.is_block_used(20) is False

    def test_reports_free_page_count(self):
        ndfs = NdfsFileSystem(create_test_image(50))
        free = ndfs.get_free_pages()
        assert free > 0
        assert free < 50

    def test_used_and_free_pages_sum_to_total(self):
        # get_used_pages() has no dedicated test elsewhere in this suite --
        # tested via the invariant used+free == total (the physical page
        # count of the image), rather than a hardcoded expected count, so
        # this does not depend on exactly how many structural pages a write
        # costs. Checked both on a fresh image and after a write, so a
        # get_used_pages() that silently returns 0 or a stale value would
        # break the invariant either way.
        ndfs = NdfsFileSystem(create_test_image(100))
        total = len(ndfs.to_buffer()) // NDFS_PAGE_SIZE

        assert ndfs.get_used_pages() + ndfs.get_free_pages() == total

        ndfs.write_file("SYSTEM/USEDPGS:DATA", b"x" * (3 * NDFS_PAGE_SIZE))
        assert ndfs.get_used_pages() + ndfs.get_free_pages() == total
        assert ndfs.get_used_pages() > 0


class TestNdfsFileSystemGetObjectEntry:
    """get_object_entry() had no dedicated test anywhere in this suite --
    only exercised transitively through write_file()'s internal lookups."""

    def test_finds_entry_by_name_and_user(self):
        ndfs = NdfsFileSystem(create_test_image())
        ndfs.write_file("SYSTEM/FOUNDIT:DATA", b"hello")

        entry = ndfs.get_object_entry("FOUNDIT", "SYSTEM", "DATA")

        assert entry is not None
        assert entry.object_name.strip() == "FOUNDIT"
        assert entry.bytes_in_file == 5

    def test_returns_none_for_missing_entry(self):
        ndfs = NdfsFileSystem(create_test_image())
        assert ndfs.get_object_entry("NOPE", "SYSTEM", "DATA") is None

    def test_file_type_disambiguates_same_name_different_type(self):
        # The exact scenario get_object_entry's file_type parameter exists
        # for: two files sharing a name, differing only in type.
        ndfs = NdfsFileSystem(create_test_image())
        ndfs.write_file("SYSTEM/AAA:LIST", b"L" * 10)
        ndfs.write_file("SYSTEM/AAA:MODE", b"M" * 20)

        list_entry = ndfs.get_object_entry("AAA", "SYSTEM", "LIST")
        mode_entry = ndfs.get_object_entry("AAA", "SYSTEM", "MODE")

        assert list_entry is not None and list_entry.bytes_in_file == 10
        assert mode_entry is not None and mode_entry.bytes_in_file == 20


class TestNdfsFileSystemIntegrity:
    def test_reports_valid_for_clean_image(self):
        ndfs = NdfsFileSystem(create_test_image())
        assert ndfs.verify_integrity() is True


class TestNdfsFileSystemGenerateReport:
    """generate_report() had no dedicated test anywhere in this suite,
    unlike ndfs-c's equivalent (test_filesystem.c)."""

    def test_report_names_the_volume_and_counts(self):
        ndfs = NdfsFileSystem(create_test_image())
        ndfs.add_user("GUEST", 50)
        ndfs.write_file("SYSTEM/REPORTME:DATA", b"x" * 10)

        report = ndfs.generate_report()

        assert "NDFS Filesystem Report" in report
        assert "Users: 2" in report
        assert "Files: 1" in report
        assert "GUEST" in report
        assert "REPORTME" in report
        assert report.endswith("\n")


class TestNdfsFileSystemEdgeCases:
    def test_handles_file_with_dot_separator(self):
        ndfs = NdfsFileSystem(create_test_image())
        ndfs.write_file("SYSTEM/README.TXT", b"dot separated")
        meta = ndfs.get_metadata("SYSTEM/README.TXT")
        assert meta is not None
        assert meta.name == "README"
        assert meta.type == "TXT"
