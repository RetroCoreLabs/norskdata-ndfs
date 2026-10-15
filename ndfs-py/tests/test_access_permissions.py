"""
Tests for ndfs.access_permissions -- AccessPermissions class.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest
from ndfs.access_permissions import (
    AccessPermissions,
    PERM_READ,
    PERM_WRITE,
    PERM_APPEND,
    PERM_EXECUTE,
    PERM_DELETE,
)
from ndfs.types import FileAccessType


class TestAccessPermissionsDefault:
    def test_creates_0x4FF_default_permissions(self):
        p = AccessPermissions.default()
        assert p.raw_bits == 0x4FF


class TestAccessPermissionsFromTiers:
    def test_encodes_separate_tier_values(self):
        # Own=all(0x1f), Friend=RW(0x03), Public=R(0x01)
        p = AccessPermissions.from_tiers(0x1F, 0x03, 0x01)
        assert p.can_read(FileAccessType.Own) is True
        assert p.can_delete(FileAccessType.Own) is True
        assert p.can_read(FileAccessType.Friend) is True
        assert p.can_write(FileAccessType.Friend) is True
        assert p.can_append(FileAccessType.Friend) is False
        assert p.can_read(FileAccessType.Public) is True
        assert p.can_write(FileAccessType.Public) is False


class TestAccessPermissionsChecks:
    def test_checks_individual_permissions(self):
        p = AccessPermissions(0)
        p.set_permission(FileAccessType.Own, PERM_READ, True)
        p.set_permission(FileAccessType.Own, PERM_WRITE, True)
        assert p.can_read(FileAccessType.Own) is True
        assert p.can_write(FileAccessType.Own) is True
        assert p.can_delete(FileAccessType.Own) is False


class TestAccessPermissionsGetPermissionString:
    def test_formats_all_permissions(self):
        p = AccessPermissions.from_tiers(0x1F, 0, 0)
        assert p.get_permission_string(FileAccessType.Own) == "DXAWR"

    def test_formats_no_permissions(self):
        p = AccessPermissions(0)
        assert p.get_permission_string(FileAccessType.Own) == "-----"


class TestAccessPermissionsOwnerOnly:
    def test_gives_full_access_to_owner_none_to_others(self):
        p = AccessPermissions.owner_only()
        assert p.can_read(FileAccessType.Own) is True
        assert p.can_delete(FileAccessType.Own) is True
        assert p.can_read(FileAccessType.Friend) is False
        assert p.can_read(FileAccessType.Public) is False
