"""
Tests for ndfs.user_entry -- UserEntry class.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest
from ndfs.user_entry import UserEntry
from ndfs.constants import ENTRY_SIZE, USER_ENTRY_FLAG, NDFS_NAME_TERMINATOR
from ndfs.endian import write_uint16_be, write_uint32_be


def build_user_bytes(
    name: str,
    user_index: int,
    pages_reserved: int,
    pages_used: int,
    password: int = 0,
) -> bytearray:
    """Build a raw 64-byte user entry."""
    buf = bytearray(ENTRY_SIZE)
    buf[0] = USER_ENTRY_FLAG  # 0x81
    # Name at offset 2
    for i in range(min(len(name), 16)):
        buf[2 + i] = ord(name[i])
    if len(name) < 16:
        buf[2 + len(name)] = NDFS_NAME_TERMINATOR
    write_uint16_be(buf, 18, password)
    write_uint32_be(buf, 28, pages_reserved)
    write_uint32_be(buf, 32, pages_used)
    buf[37] = user_index
    return buf


class TestUserEntryFromBytes:
    def test_parses_valid_user_entry(self):
        data = build_user_bytes("SYSTEM", 0, 1000, 50, 0x1234)
        user = UserEntry.from_bytes(data, 0)
        assert user is not None
        assert user.user_name == "SYSTEM"
        assert user.user_index == 0
        assert user.pages_reserved == 1000
        assert user.pages_used == 50
        assert user.password == 0x1234

    def test_returns_none_for_invalid_flag(self):
        data = bytearray(ENTRY_SIZE)
        data[0] = 0x00  # not 0x81
        assert UserEntry.from_bytes(data, 0) is None

    def test_parses_at_offset(self):
        data = bytearray(ENTRY_SIZE * 2)
        user_bytes = build_user_bytes("USER1", 5, 200, 10)
        data[ENTRY_SIZE:ENTRY_SIZE * 2] = user_bytes
        user = UserEntry.from_bytes(data, ENTRY_SIZE)
        assert user is not None
        assert user.user_name == "USER1"
        assert user.user_index == 5


class TestUserEntryToBytesRoundTrip:
    def test_round_trips_all_fields(self):
        user = UserEntry()
        user.user_name = "TESTUSER"
        user.user_index = 3
        user.pages_reserved = 500
        user.pages_used = 100
        user.password = 0xABCD

        raw = user.to_bytes()
        parsed = UserEntry.from_bytes(raw, 0)

        assert parsed is not None
        assert parsed.user_name == "TESTUSER"
        assert parsed.user_index == 3
        assert parsed.pages_reserved == 500
        assert parsed.pages_used == 100
        assert parsed.password == 0xABCD

    def test_round_trips_unmodelled_flag_bits_in_byte_0(self):
        # A real pack may set bits in byte 0 beyond the 0x81 pair from_bytes
        # tests on. to_bytes used to hardcode 0x81, so any read/modify/write
        # destroyed them -- and the user-file writer re-serializes every slot
        # in the page, not just the edited one.
        raw = bytearray(ENTRY_SIZE)
        raw[0] = 0xC1  # valid user (0x81) + an unmodelled bit 6
        raw[2] = ord("S")
        raw[3] = ord("Y")
        raw[4] = NDFS_NAME_TERMINATOR
        raw[37] = 7

        entry = UserEntry.from_bytes(raw, 0)
        assert entry is not None
        assert entry.uf == 0xC1

        assert entry.to_bytes()[0] == 0xC1

    def test_fresh_entry_is_marked_valid(self):
        # An entry built from scratch must still be marked valid, or from_bytes
        # rejects the slot on the next mount and the user vanishes.
        user = UserEntry()
        user.user_name = "FRESH"

        raw = user.to_bytes()

        assert raw[0] & USER_ENTRY_FLAG == USER_ENTRY_FLAG
        assert UserEntry.from_bytes(raw, 0) is not None


class TestUserEntryQuota:
    def test_detects_over_quota(self):
        user = UserEntry()
        user.pages_reserved = 100
        user.pages_used = 150
        assert user.is_over_quota() is True

    def test_detects_under_quota(self):
        user = UserEntry()
        user.pages_reserved = 100
        user.pages_used = 50
        assert user.is_over_quota() is False
        assert user.get_free_pages() == 50


class TestUserEntrySetName:
    def test_uppercases_name(self):
        user = UserEntry()
        user.set_name("lowercase")
        assert user.user_name == "LOWERCASE"

    def test_truncates_to_16_chars(self):
        user = UserEntry()
        user.set_name("VERYLONGNAMETHATEXCEEDS")
        assert len(user.user_name) == 16

    def test_throws_on_empty_name(self):
        user = UserEntry()
        with pytest.raises(ValueError):
            user.set_name("")


class TestUserEntryFriends:
    def test_adds_and_finds_friend(self):
        user = UserEntry()
        assert user.add_friend(5, 0x03) is True  # read + write
        assert user.is_friend(5) is True
        assert user.is_friend(6) is False

    def test_removes_friend(self):
        user = UserEntry()
        user.add_friend(5, 0x01)
        assert user.remove_friend(5) is True
        assert user.is_friend(5) is False

    def test_returns_false_when_friend_slots_full(self):
        user = UserEntry()
        for i in range(8):
            user.add_friend(i, 0x01)
        assert user.add_friend(99, 0x01) is False

    def test_get_friend_returns_entry_with_permissions(self):
        user = UserEntry()
        user.add_friend(10, 0x03)  # read + write
        f = user.get_friend(10)
        assert f is not None
        assert f.read_access is True
        assert f.write_access is True
