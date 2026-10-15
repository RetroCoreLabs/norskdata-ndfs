"""
Tests for access control -- own, friend, public access, friend override.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs.user_entry import UserEntry
from ndfs.object_entry import ObjectEntry
from ndfs.user_friend import UserFriend
from ndfs.access_permissions import AccessPermissions, PERM_READ, PERM_WRITE, PERM_DELETE
from ndfs.access_control import get_access_level, get_friend_permissions, check_access
from ndfs.types import FileAccessType, FileOperationType


def _make_user(index, name):
    user = UserEntry()
    user.user_index = index
    user.set_name(name)
    return user


def _make_file(owner_index, access_bits=0x04FF):
    obj = ObjectEntry()
    obj.user_index = owner_index
    obj.access_bits = access_bits
    return obj


class TestOwnAccess:
    def test_owner_gets_own_level(self):
        owner = _make_user(0, "OWNER")
        obj = _make_file(0)
        level = get_access_level(obj, owner, owner)
        assert level == FileAccessType.Own

    def test_owner_can_read_own_file(self):
        owner = _make_user(0, "OWNER")
        # 0x04FF = public R, friend DXAWR, own DXAWR
        obj = _make_file(0, 0x04FF)
        assert check_access(obj, owner, owner, FileOperationType.Read) is True

    def test_owner_can_write_own_file(self):
        owner = _make_user(0, "OWNER")
        obj = _make_file(0, 0x04FF)
        assert check_access(obj, owner, owner, FileOperationType.Write) is True

    def test_owner_can_delete_own_file(self):
        owner = _make_user(0, "OWNER")
        obj = _make_file(0, 0x04FF)
        assert check_access(obj, owner, owner, FileOperationType.Delete) is True

    def test_owner_denied_when_permission_cleared(self):
        owner = _make_user(0, "OWNER")
        # No own permissions
        obj = _make_file(0, 0x7C00)  # Only public permissions set
        assert check_access(obj, owner, owner, FileOperationType.Read) is False


class TestFriendAccess:
    def test_friend_gets_friend_level(self):
        owner = _make_user(0, "OWNER")
        friend = _make_user(1, "FRIEND")
        owner.add_friend(1, 0x1F)  # Full friend permissions

        obj = _make_file(0)
        level = get_access_level(obj, friend, owner)
        assert level == FileAccessType.Friend

    def test_friend_can_read_with_file_permissions(self):
        owner = _make_user(0, "OWNER")
        friend = _make_user(1, "FRIEND")
        owner.add_friend(1, 0x01)  # Read only in friend entry

        # File with friend read permission
        obj = _make_file(0, 0x04FF)
        assert check_access(obj, friend, owner, FileOperationType.Read) is True

    def test_friend_denied_write_without_permission(self):
        owner = _make_user(0, "OWNER")
        friend = _make_user(1, "FRIEND")
        owner.add_friend(1, 0x01)  # Read only

        obj = _make_file(0, 0x04FF)
        # Friend-specific permissions override; only read is allowed
        assert check_access(obj, friend, owner, FileOperationType.Write) is False

    def test_non_friend_gets_public_level(self):
        owner = _make_user(0, "OWNER")
        stranger = _make_user(2, "STRANGER")

        obj = _make_file(0)
        level = get_access_level(obj, stranger, owner)
        assert level == FileAccessType.Public


class TestPublicAccess:
    def test_public_can_read_with_permission(self):
        owner = _make_user(0, "OWNER")
        public = _make_user(2, "PUBLIC")

        # 0x04FF has public read (bit 10 = R)
        obj = _make_file(0, 0x04FF)
        assert check_access(obj, public, owner, FileOperationType.Read) is True

    def test_public_denied_write(self):
        owner = _make_user(0, "OWNER")
        public = _make_user(2, "PUBLIC")

        # 0x04FF: public bits = 0x01 (only read)
        obj = _make_file(0, 0x04FF)
        assert check_access(obj, public, owner, FileOperationType.Write) is False

    def test_public_denied_delete(self):
        owner = _make_user(0, "OWNER")
        public = _make_user(2, "PUBLIC")

        obj = _make_file(0, 0x04FF)
        assert check_access(obj, public, owner, FileOperationType.Delete) is False


class TestFriendOverride:
    def test_friend_specific_perms_override_file_level(self):
        owner = _make_user(0, "OWNER")
        friend = _make_user(1, "FRIEND")

        # Add friend with write access
        owner.add_friend(1, 0x03)  # Read + Write

        # File has NO friend write permission (only own)
        obj = _make_file(0, 0x001F)  # Own full, friend none, public none
        # But friend-specific override should grant write
        assert check_access(obj, friend, owner, FileOperationType.Write) is True

    def test_friend_entry_read_permission(self):
        owner = _make_user(0, "OWNER")
        friend = _make_user(1, "FRIEND")

        # Friend with read-only access
        owner.add_friend(1, 0x01)  # Read only

        obj = _make_file(0, 0x001F)  # Own full, no friend/public
        assert check_access(obj, friend, owner, FileOperationType.Read) is True
        assert check_access(obj, friend, owner, FileOperationType.Write) is False

    def test_get_friend_permissions(self):
        owner = _make_user(0, "OWNER")
        friend = _make_user(1, "FRIEND")
        owner.add_friend(1, 0x07)  # R+W+A

        friend_entry = get_friend_permissions(friend, owner)
        assert friend_entry is not None
        assert friend_entry.read_access is True
        assert friend_entry.write_access is True
        assert friend_entry.append_access is True

    def test_no_friend_entry_returns_none(self):
        owner = _make_user(0, "OWNER")
        stranger = _make_user(2, "STRANGER")

        friend_entry = get_friend_permissions(stranger, owner)
        assert friend_entry is None


class TestAccessPermissionsClass:
    def test_default_permissions(self):
        perms = AccessPermissions.default()
        assert perms.can_read(FileAccessType.Own) is True
        assert perms.can_write(FileAccessType.Own) is True
        assert perms.can_read(FileAccessType.Public) is True

    def test_owner_only_permissions(self):
        perms = AccessPermissions.owner_only()
        assert perms.can_read(FileAccessType.Own) is True
        assert perms.can_read(FileAccessType.Friend) is False
        assert perms.can_read(FileAccessType.Public) is False

    def test_set_permission(self):
        perms = AccessPermissions(0)
        perms.set_permission(FileAccessType.Own, PERM_READ, True)
        assert perms.can_read(FileAccessType.Own) is True
        assert perms.can_write(FileAccessType.Own) is False

    def test_permission_string(self):
        perms = AccessPermissions(0x1F)  # Own full
        s = perms.get_permission_string(FileAccessType.Own)
        assert s == "DXAWR"
