"""
Tests for ndfs.user_friend -- UserFriend packed permission entry.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest
from ndfs.user_friend import UserFriend


class TestUserFriendConstructor:
    def test_defaults_to_unused(self):
        f = UserFriend()
        assert f.entry_used is False
        assert f.bits == 0

    def test_parses_packed_bits(self):
        # bit15=1 (used), bit9=1 (write), bit8=1 (read), index=42
        bits = (1 << 15) | (1 << 9) | (1 << 8) | 42
        f = UserFriend(bits)
        assert f.entry_used is True
        assert f.read_access is True
        assert f.write_access is True
        assert f.append_access is False
        assert f.friend_user_index == 42


class TestUserFriendCreate:
    def test_creates_with_all_permissions(self):
        f = UserFriend.create(10, read=True, write=True, append=True, common=True, directory=True)
        assert f.entry_used is True
        assert f.friend_user_index == 10
        assert f.read_access is True
        assert f.write_access is True
        assert f.append_access is True
        assert f.common_access is True
        assert f.directory_access is True

    def test_creates_with_read_only(self):
        f = UserFriend.create(5, read=True)
        assert f.read_access is True
        assert f.write_access is False


class TestUserFriendSetFriendClear:
    def test_sets_friend_with_permissions(self):
        f = UserFriend()
        f.set_friend(7, 0x03)  # bits 0-1 = read+write
        assert f.entry_used is True
        assert f.friend_user_index == 7
        assert f.read_access is True
        assert f.write_access is True
        assert f.append_access is False

    def test_clears_entry(self):
        f = UserFriend.create(5, read=True, write=True)
        f.clear()
        assert f.entry_used is False
        assert f.bits == 0


class TestUserFriendBytes:
    def test_round_trips_through_bytes(self):
        f = UserFriend.create(42, read=True, write=False, append=True, common=False, directory=True)
        buf = bytearray(4)
        f.to_bytes(buf, 1)
        f2 = UserFriend.from_bytes(buf, 1)
        assert f2.entry_used is True
        assert f2.friend_user_index == 42
        assert f2.read_access is True
        assert f2.write_access is False
        assert f2.append_access is True
        assert f2.directory_access is True


class TestUserFriendGetPermissionString:
    def test_returns_dashes_for_unused(self):
        assert UserFriend().get_permission_string() == "-----"

    def test_returns_RWACD_for_all_permissions(self):
        f = UserFriend.create(0, read=True, write=True, append=True, common=True, directory=True)
        assert f.get_permission_string() == "RWACD"

    def test_returns_R_for_read_only(self):
        f = UserFriend.create(0, read=True)
        assert f.get_permission_string() == "R----"


class TestUserFriendParsePermissions:
    def test_rwa(self):
        assert UserFriend.parse_permissions("RWA") == 0x07

    def test_all(self):
        assert UserFriend.parse_permissions("rwacd") == 0x1F

    def test_empty_and_none(self):
        assert UserFriend.parse_permissions("") == 0
        assert UserFriend.parse_permissions(None) == 0

    def test_directory_only(self):
        assert UserFriend.parse_permissions("D") == 0x10

    def test_invalid_letter_raises(self):
        import pytest
        with pytest.raises(ValueError):
            UserFriend.parse_permissions("RWX")
