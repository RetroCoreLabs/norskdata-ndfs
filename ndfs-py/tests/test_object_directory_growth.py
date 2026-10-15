"""
Real-write regression tests for the NDFS object file (the volume-wide
directory listing every file across every user) growing past its plain-
Indexed capacity of 512 directory pages (16,384 files total, or 64 users
at MAX_USER_FILE_POINTERS==8 reserved index-pointer slots each).

Previously `_ensure_object_dir_page`'s Indexed branch had no bounds check:
once page_idx reached MAX_OBJECT_FILE_POINTERS (512) it would compute an
out-of-range offset into a single 2048-byte index page and blow up. Fixed
by growing the object file into a SubIndexed structure on first need (one-
time conversion: allocate a fresh sub-index block, install the existing
Indexed block as its slot-0 entry with no data migration, repoint the
master block at the new sub-index block, persist), then falling through to
the already-correct SubIndexed handling. Mirrors the equivalent fix already
applied to RetroFS.NDFS.FileSystems.NdfsFileSystem.EnsureObjectDirPage in
the C# port (see NdfsObjectDirectorySubIndexedGrowthTests.cs).

The read side (ObjectFile.load_from_pages) and the per-write persist path
(_object_page_block / _write_object_page) were already correct for both
Indexed and SubIndexed object-file layouts before this change -- only the
allocate-on-demand conversion was missing.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs.filesystem import NdfsFileSystem
from ndfs.types import ImageTemplate, ImageCreationOptions


def _make_fs(pages):
    opts = ImageCreationOptions(
        template=ImageTemplate.Custom,
        directory_name="OBJDIRGROW",
        custom_pages=pages,
    )
    return NdfsFileSystem.create_image(opts)


def _find_user(ndfs, name):
    for u in ndfs.get_users():
        if u.user_name.upper() == name.upper():
            return u
    return None


class TestUsersBeyondOldSixtyFourUserCeiling:
    def test_create_files_for_users_past_64_does_not_crash(self):
        # The old plain-Indexed object file capped out at 64 users
        # (512 index-pointer slots / MAX_USER_FILE_POINTERS==8 reserved
        # per user) -- well before any file-count limit is even reached.
        # This alone used to throw/crash for user #64 onward.
        ndfs = _make_fs(pages=3000)

        for i in range(80):
            name = f"USER{i:03d}"
            added = ndfs.add_user(name, 10)
            assert added is True, f"add_user must succeed for {name}"
            ndfs.write_file(f"{name}/FILE:DAT", bytes([i % 251]))

        users = ndfs.get_users()
        # SYSTEM (pre-existing) + 80 newly added.
        assert len(users) == 81

        entries = ndfs.get_object_entries()
        assert len(entries) == 80


class TestThirtyThousandFilesAcrossOneHundredTwentyUsers:
    def test_write_30000_files_survive_reopen_with_correct_ownership(self):
        # 30,000 files spread across 120 users (250 each -- well under the
        # 256-files-per-user cap, and well under NDFS's 256-user max)
        # forces the object file well past its old 16,384-file/64-user
        # ceiling: page_idx reaches roughly 120*8/32*32 = 960 worth of
        # directory pages, requiring at least 2 SubIndexed groups (each
        # covering 512 pages). This is the single most important test here
        # -- it is exactly the scenario the old unconditional-Indexed
        # bounds bug would have crashed on.
        user_count = 120
        files_per_user = 250
        total_files = user_count * files_per_user

        # Each 2-byte file costs exactly 1 real data page (no separate
        # index block needed for a single-page file -- see
        # _allocate_and_write_data's small-file path), plus ~1000 object-
        # directory pages (30000/32), plus a handful of group-index/
        # sub-index/user-file pages. Oversize generously to avoid running
        # out of space mid-test.
        ndfs = _make_fs(pages=100000)

        for u in range(user_count):
            name = f"USER{u:03d}"
            added = ndfs.add_user(name, 10)
            assert added is True, f"add_user must succeed for {name}"

        for u in range(user_count):
            name = f"USER{u:03d}"
            for f in range(files_per_user):
                ndfs.write_file(f"{name}/F{f}:DAT", bytes([u % 251, f % 251]))

        exported = ndfs.to_buffer()

        # Reopen fresh (not reusing in-memory state) and verify everything
        # survived, including that ownership decodes correctly for files
        # in groups beyond the first.
        ndfs2 = NdfsFileSystem(exported)

        entries = ndfs2.get_object_entries()
        assert len(entries) == total_files

        # Spot-check users from the FIRST group (page_idx < 512, e.g. user
        # 0), the group BOUNDARY (around page_idx 512, i.e. user ~64), and
        # the SECOND group (e.g. user 119).
        for user_to_check in (0, 64, 119):
            name = f"USER{user_to_check:03d}"
            user = _find_user(ndfs2, name)
            assert user is not None, f"{name} must exist"

            files_for_this_user = 0
            for e in entries:
                if e.user_index == user.user_index:
                    files_for_this_user += 1
                    # Ownership must decode correctly from the object
                    # index's high byte.
                    owner_from_object_index = e.object_index >> 8
                    assert owner_from_object_index == user.user_index, (
                        f"{name}'s file object_index high byte must equal "
                        f"their user_index, not collide with another "
                        f"user's region"
                    )
            assert files_for_this_user == files_per_user, (
                f"{name} must have exactly {files_per_user} files"
            )

        # Byte-level content check on one file from each of the 3
        # spot-checked users.
        assert ndfs2.read_file("USER000/F0:DAT") == bytes([0, 0])
        assert ndfs2.read_file("USER064/F100:DAT") == bytes([64 % 251, 100])
        assert ndfs2.read_file("USER119/F249:DAT") == bytes([119, 249 % 251])


class TestDeleteAndRecreateInSecondGroup:
    def test_delete_and_recreate_file_in_second_group_works(self):
        # Exercises delete/recreate for a user whose directory page lives
        # in the second SubIndexed group (not just create-only). USER069's
        # page_idx (69*8=552..559) is past the first group's 512-page cap.
        user_count = 70
        ndfs = _make_fs(pages=3000)

        for i in range(user_count):
            name = f"USER{i:03d}"
            added = ndfs.add_user(name, 10)
            assert added is True

        ndfs.write_file("USER069/FILE:DAT", bytes([1, 2, 3]))
        ndfs.delete_file("USER069/FILE:DAT")
        ndfs.write_file("USER069/FILE:DAT", bytes([4, 5, 6]))
        assert ndfs.read_file("USER069/FILE:DAT") == bytes([4, 5, 6])
