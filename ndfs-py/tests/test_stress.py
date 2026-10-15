"""
Stress tests -- 50 users, 200 files, write/read/delete cycles, data integrity.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs.filesystem import NdfsFileSystem
from ndfs.constants import NDFS_PAGE_SIZE
from ndfs.types import ImageTemplate, ImageCreationOptions


def _make_large_fs(pages=5000):
    opts = ImageCreationOptions(
        template=ImageTemplate.Custom,
        directory_name="STRESS",
        custom_pages=pages,
    )
    return NdfsFileSystem.create_image(opts)


class TestManyUsers:
    def test_50_users(self):
        ndfs = _make_large_fs()
        for i in range(50):
            result = ndfs.add_user(f"USER{i:03d}", 50)
            assert result is True
        users = ndfs.get_users()
        assert len(users) == 51  # SYSTEM + 50


class TestManyFiles:
    def test_200_files(self):
        ndfs = _make_large_fs()
        for i in range(200):
            ndfs.write_file(f"SYSTEM/F{i:04d}:DATA", f"file content {i}".encode())

        entries = ndfs.list_directory("SYSTEM")
        assert len(entries) == 200

        # Verify random samples
        content = ndfs.read_file("SYSTEM/F0000:DATA")
        assert content == b"file content 0"
        content = ndfs.read_file("SYSTEM/F0199:DATA")
        assert content == b"file content 199"


class TestWriteReadDeleteCycles:
    def test_write_delete_cycle(self):
        ndfs = _make_large_fs()

        for cycle in range(10):
            # Write 10 files
            for i in range(10):
                ndfs.write_file(
                    f"SYSTEM/CYC{i}:DATA",
                    f"cycle {cycle} file {i}".encode(),
                )

            # Verify all exist
            entries = ndfs.list_directory("SYSTEM")
            assert len(entries) == 10

            # Delete all
            for i in range(10):
                ndfs.delete_file(f"SYSTEM/CYC{i}:DATA")

            entries = ndfs.list_directory("SYSTEM")
            assert len(entries) == 0

    def test_alternating_write_delete(self):
        ndfs = _make_large_fs()

        for i in range(50):
            ndfs.write_file(f"SYSTEM/ALT{i}:DATA", f"data {i}".encode())
            if i > 0 and i % 5 == 0:
                # Delete every 5th previous file
                ndfs.delete_file(f"SYSTEM/ALT{i - 5}:DATA")

        # Should have files 1-4, 6-9, 11-14, ... (deleted every 5th starting from 0, 5, 10, ...)
        entries = ndfs.list_directory("SYSTEM")
        assert len(entries) > 30


class TestDataIntegrity:
    def test_large_file_integrity(self):
        ndfs = _make_large_fs()
        # Write files with known patterns
        for i in range(20):
            data = bytearray(NDFS_PAGE_SIZE * 2)
            for j in range(len(data)):
                data[j] = ((i * 37 + j) & 0xFF)
            ndfs.write_file(f"SYSTEM/INT{i:03d}:DATA", data)

        # Read back and verify
        for i in range(20):
            content = ndfs.read_file(f"SYSTEM/INT{i:03d}:DATA")
            assert len(content) == NDFS_PAGE_SIZE * 2
            for j in range(len(content)):
                expected = ((i * 37 + j) & 0xFF)
                assert content[j] == expected, f"Integrity check failed for file {i} at byte {j}"

    def test_integrity_after_round_trip(self):
        ndfs1 = _make_large_fs()
        for i in range(30):
            ndfs1.write_file(f"SYSTEM/RT{i:03d}:DATA", f"roundtrip {i}".encode())

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        for i in range(30):
            content = ndfs2.read_file(f"SYSTEM/RT{i:03d}:DATA")
            assert content == f"roundtrip {i}".encode()


class TestMultiUserStress:
    def test_files_across_multiple_users(self):
        ndfs = _make_large_fs()

        # Create users
        for i in range(5):
            ndfs.add_user(f"USR{i}", 500)

        # Write files for each user
        for u in range(5):
            for f in range(10):
                ndfs.write_file(
                    f"USR{u}/FILE{f}:DATA",
                    f"user {u} file {f}".encode(),
                )

        # Verify each user's files
        for u in range(5):
            entries = ndfs.list_directory(f"USR{u}")
            assert len(entries) == 10
            for f in range(10):
                content = ndfs.read_file(f"USR{u}/FILE{f}:DATA")
                assert content == f"user {u} file {f}".encode()
