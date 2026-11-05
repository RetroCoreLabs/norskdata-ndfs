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


class TestSintranAccessForm:
    """SINTRAN's access fields: one field, then the PUBLIC,FRIEND,OWN triple
    (@SET-FILE-ACCESS, SINTRAN III Reference Manual ND-60.128.5)."""

    def test_one_field(self):
        from ndfs.access_permissions import parse_access_rights
        assert parse_access_rights("N") == 0
        assert parse_access_rights("DCAWR") == 0x1F
        assert parse_access_rights("ra") == 0x05
        for bad in ("", "NR", "X"):
            with pytest.raises(ValueError):
                parse_access_rights(bad)

    def test_triple(self):
        from ndfs.access_permissions import apply_access_triple
        # @SET-FILE-ACCESS F-1:DATA N,N,RWACD -> owner only.
        assert apply_access_triple(0x07FF, "N,N,RWACD") == 0x001F
        # The generated default (rule 2 of @SET-DEFAULT-FILE-ACCESS):
        # public READ, friend RWA, owner all = 0x04FF.
        assert apply_access_triple(0, "R,RWA,RWACD") == 0x04FF
        # Empty fields keep their tier.
        assert apply_access_triple(0x04FF, ",,N") == 0x04E0
        assert apply_access_triple(0x04FF, "N") == 0x00FF
        assert apply_access_triple(0x04FF, "") == 0x04FF
        assert apply_access_triple(0, "R, RWA, RWACD") == 0x04FF
        for bad in ("R,R,R,R", "R,NW,R"):
            with pytest.raises(ValueError):
                apply_access_triple(0, bad)
