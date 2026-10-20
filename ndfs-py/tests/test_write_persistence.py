"""
Tests for write persistence -- export/reimport round-trips.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs.filesystem import NdfsFileSystem
from ndfs.constants import NDFS_PAGE_SIZE, MASTER_BLOCK_OFFSET, EXTENDED_INFO_OFFSET
from ndfs.endian import read_uint32_be
from ndfs.types import ImageTemplate, ImageCreationOptions


def _make_fs(pages=200):
    opts = ImageCreationOptions(
        template=ImageTemplate.Custom,
        directory_name="PERSIST",
        custom_pages=pages,
    )
    return NdfsFileSystem.create_image(opts)


class TestFileRoundTrip:
    def test_single_file_survives_export(self):
        ndfs1 = _make_fs()
        ndfs1.write_file("SYSTEM/PERSIST:DATA", b"persistent data")

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        content = ndfs2.read_file("SYSTEM/PERSIST:DATA")
        assert content == b"persistent data"

    def test_multiple_files_survive_export(self):
        ndfs1 = _make_fs()
        for i in range(5):
            ndfs1.write_file(f"SYSTEM/FILE{i}:DATA", f"content {i}".encode())

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        for i in range(5):
            content = ndfs2.read_file(f"SYSTEM/FILE{i}:DATA")
            assert content == f"content {i}".encode()

    def test_large_file_survives_export(self):
        ndfs1 = _make_fs()
        data = bytearray(5000)
        for i in range(len(data)):
            data[i] = i & 0xFF
        ndfs1.write_file("SYSTEM/BIGFILE:DATA", data)

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        content = ndfs2.read_file("SYSTEM/BIGFILE:DATA")
        assert len(content) == 5000
        for i in range(len(data)):
            assert content[i] == data[i]


class TestUserRoundTrip:
    def test_added_user_survives_export(self):
        ndfs1 = _make_fs()
        ndfs1.add_user("NEWUSER", 500)

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        users = ndfs2.get_users()
        assert len(users) == 2
        names = []
        for i in range(len(users)):
            names.append(users[i].user_name)
        assert "SYSTEM" in names
        assert "NEWUSER" in names

    def test_user_quota_survives_export(self):
        ndfs1 = _make_fs()
        ndfs1.update_user_quota(0, 2000)

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        user = ndfs2.get_user(0)
        assert user.pages_reserved == 2000


class TestDeletionRoundTrip:
    def test_deletion_persists_after_export(self):
        ndfs1 = _make_fs()
        ndfs1.write_file("SYSTEM/A:DATA", b"aaa")
        ndfs1.write_file("SYSTEM/B:DATA", b"bbb")
        ndfs1.delete_file("SYSTEM/A:DATA")

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        assert ndfs2.file_exists("SYSTEM/A:DATA") is False
        assert ndfs2.file_exists("SYSTEM/B:DATA") is True
        assert ndfs2.read_file("SYSTEM/B:DATA") == b"bbb"

    def test_free_pages_persist_after_export(self):
        ndfs1 = _make_fs()
        free_initial = ndfs1.get_free_pages()

        ndfs1.write_file("SYSTEM/TEMP:DATA", b"temporary")
        ndfs1.delete_file("SYSTEM/TEMP:DATA")

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        assert ndfs2.get_free_pages() == free_initial


class TestDirectoryNamePersists:
    def test_directory_name_survives_export(self):
        ndfs1 = _make_fs()
        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)
        assert ndfs2.get_directory_name() == "PERSIST"


class TestOverwriteRoundTrip:
    def test_overwritten_file_persists(self):
        ndfs1 = _make_fs()
        ndfs1.write_file("SYSTEM/FILE:DATA", b"version 1")
        ndfs1.write_file("SYSTEM/FILE:DATA", b"version 2")

        exported = ndfs1.to_buffer()
        ndfs2 = NdfsFileSystem(exported)

        content = ndfs2.read_file("SYSTEM/FILE:DATA")
        assert content == b"version 2"


def test_new_file_version_self_ref():
    """New files get a self-referential version chain and [user|slot] index."""
    fs = _make_fs()
    fs.write_file("SYSTEM/A:TEXT", b"\x01\x02\x03")
    fs.write_file("SYSTEM/B:TEXT", b"\x04\x05\x06")
    for e in fs.get_object_entries():
        assert e.next_version == e.disk_object_index
        assert e.prev_version == e.disk_object_index
        assert ((e.disk_object_index >> 8) & 0xFF) == e.user_index


def test_deleted_file_stays_deleted_after_reload():
    """A deleted file must not reappear after export + re-open."""
    fs = _make_fs()
    fs.write_file("SYSTEM/A:TEXT", b"\x01")
    fs.write_file("SYSTEM/B:TEXT", b"\x02")
    fs.delete_file("SYSTEM/A:TEXT")
    fs2 = NdfsFileSystem(fs.to_buffer())
    assert fs2.file_exists("SYSTEM/A:TEXT") is False
    assert fs2.file_exists("SYSTEM/B:TEXT") is True


def test_new_file_lands_in_owning_user_region():
    """A file created for a non-SYSTEM user gets an object index inside that
    user's region (high byte == user index), so SINTRAN places it correctly."""
    fs = _make_fs()
    fs.add_user("GUEST", 200)
    fs.write_file("GUEST/HELLO:TEXT", b"\x01\x02")
    e = next(o for o in fs.get_object_entries()
             if o.object_name == "HELLO" and o.user_name == "GUEST")
    assert e.user_index != 0
    assert ((e.disk_object_index >> 8) & 0xFF) == e.user_index
    assert e.user_index * 256 <= e.object_index < (e.user_index + 1) * 256
    assert e.next_version == e.disk_object_index
    # survives a round-trip
    fs2 = NdfsFileSystem(fs.to_buffer())
    e2 = next(o for o in fs2.get_object_entries()
              if o.object_name == "HELLO" and o.user_name == "GUEST")
    assert ((e2.disk_object_index >> 8) & 0xFF) == e2.user_index


def test_delete_sole_file_on_page_clears_it():
    """Deleting the only file on an object page must zero that page, else the
    stale entry's in-use bit survives and the file reappears on reload."""
    fs = _make_fs()
    fs.write_file("SYSTEM/ONLY:DATA", b"x")
    fs.delete_file("SYSTEM/ONLY:DATA")
    fs2 = NdfsFileSystem(fs.to_buffer())
    assert fs2.file_exists("SYSTEM/ONLY:DATA") is False
    assert len(fs2.get_object_entries()) == 0


def test_friends_add_list_remove():
    """Friend add/list/remove + persistence + error cases."""
    import pytest
    fs = _make_fs()
    fs.add_user("ALICE", 100)
    fs.add_user("BOB", 100)

    fs.add_friend("ALICE", "BOB", "RW")
    fr = fs.list_friends("ALICE")
    assert len(fr) == 1
    assert fr[0].name == "BOB"
    assert fr[0].perms == "RW---"

    # Already a friend -> error.
    with pytest.raises(ValueError):
        fs.add_friend("ALICE", "BOB", "R")
    # Unknown friend name -> error.
    with pytest.raises(KeyError):
        fs.add_friend("ALICE", "NOBODY", "R")

    # Survives export + reload.
    fs2 = NdfsFileSystem(fs.to_buffer())
    fr2 = fs2.list_friends("ALICE")
    assert len(fr2) == 1 and fr2[0].name == "BOB"

    # Remove.
    fs.remove_friend("ALICE", "BOB")
    assert fs.list_friends("ALICE") == []
    with pytest.raises(KeyError):
        fs.remove_friend("ALICE", "BOB")


def _raw_unreserved_pages(ndfs: NdfsFileSystem) -> int:
    """Read the on-disk "Unreserved Pages" master-block field directly from
    raw page-0 bytes, bypassing the filesystem API's parsed MasterBlock."""
    buf = ndfs.to_buffer()
    page0 = buf[:NDFS_PAGE_SIZE]
    return read_uint32_be(page0, MASTER_BLOCK_OFFSET + 0x1C)


def _raw_ext_checksum_bytes(ndfs: NdfsFileSystem) -> bytes:
    """Read the 2-byte Extended Info Block checksum directly from raw page-0
    bytes at offset EXTENDED_INFO_OFFSET (0x07D0) -- a structure unrelated to
    the master block that must never be touched by _persist_master_block."""
    buf = ndfs.to_buffer()
    page0 = buf[:NDFS_PAGE_SIZE]
    return bytes(page0[EXTENDED_INFO_OFFSET:EXTENDED_INFO_OFFSET + 2])


class TestMasterBlockUnreservedPagesSync:
    """The master block's "Unreserved Pages" field (page 0, offset MASTER_BLOCK_OFFSET+0x1C) is the pool
    for user quotas, not a free page count. SINTRAN keeps it as capacity - bit file pages - 4 minus the sum
    of the users' reserved pages (measured, docs/NDFS-ALLOCATION-RULES.md section 7a). Files do not change
    it; giving, changing and taking away quota does."""

    def test_fresh_image_value(self):
        fs = _make_fs(2000)
        # 2000 - 1 bit file page - 4 = 1995, SYSTEM holds the default quota of 500
        assert _raw_unreserved_pages(fs) == 1995 - 1000

    def test_files_do_not_change_the_pool(self):
        fs = _make_fs(2000)
        base = _raw_unreserved_pages(fs)
        fs.write_file("SYSTEM/A:DATA", b"hello world")
        assert _raw_unreserved_pages(fs) == base
        fs.delete_file("SYSTEM/A:DATA")
        assert _raw_unreserved_pages(fs) == base

    def test_quota_changes_move_the_pool(self):
        fs = _make_fs(2000)
        base = _raw_unreserved_pages(fs)
        assert fs.add_user("ALPHA", 100)
        assert _raw_unreserved_pages(fs) == base - 100
        index = next(u.user_index for u in fs.get_users() if u.user_name == "ALPHA")
        assert fs.update_user_quota(index, 300)
        assert _raw_unreserved_pages(fs) == base - 300
        assert fs.update_user_quota(index, 50)
        assert _raw_unreserved_pages(fs) == base - 50
        assert fs.remove_user(index)
        assert _raw_unreserved_pages(fs) == base

    def test_extended_info_checksum_untouched_by_mutations(self):
        """The Extended Info Block checksum at raw offset 0x07D0 is a
        separate structure from the master block and must stay
        byte-identical across create/delete mutations."""
        fs = _make_fs()
        checksum_before = _raw_ext_checksum_bytes(fs)

        fs.write_file("SYSTEM/A:DATA", b"one")
        fs.write_file("SYSTEM/B:DATA", b"two")
        fs.delete_file("SYSTEM/A:DATA")

        checksum_after = _raw_ext_checksum_bytes(fs)
        assert checksum_after == checksum_before


def test_surgical_write_preserves_empty_type():
    """Bug 1+2 regression: an unrelated mutation must not corrupt other files'
    metadata. A file with an empty type must keep it after an unrelated
    add-user. The original port re-serialized EVERY object entry on every
    mutation and defaulted empty types to "DATA" on read, so adding a user
    corrupted unrelated files. Writes are now surgical and empty types are
    preserved."""
    fs = _make_fs()
    # File with an EMPTY type (path carries no :TYPE suffix).
    fs.write_file("SYSTEM/NOTYPE", b"x")
    assert fs.get_metadata("SYSTEM/NOTYPE").type == ""

    # Unrelated mutation: add a brand-new user.
    fs.add_user("BACKUP", 50)

    # Export + reload: the empty type must survive, not become "DATA".
    fs2 = NdfsFileSystem(fs.to_buffer())
    assert fs2.get_metadata("SYSTEM/NOTYPE").type == ""
