"""
SINTRAN disk types: the fixed SMD and Winchester sizes and the layout lengths.

Names and sizes come from the device-name table (segment 101-S3DNAM) of the SINTRAN L and M carves:
each disk unit name carries one fixed 32-bit number, the directory size in pages. SCSI disks are not in
the table: SINTRAN sets their size from the drive. ``layout_pages`` is the length of the full SINTRAN disk
layout (cylinders x heads x sectors / 2) of the DT table entry whose comment names the type; 0 = unknown.

See docs/DISK-GEOMETRY-AND-BOOT-FINDINGS.md and docs/CREATE-DIRECTORY-PLACEMENT.md.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

import enum
from dataclasses import dataclass
from typing import List, Optional


class DiskFamily(enum.Enum):
    """Controller family of a SINTRAN disk type, from the generation source."""
    UNKNOWN = "unknown"          #: named in the manual only; not in the carved name table
    WINCHESTER = "winchester"    #: ST506 style controller (IOX 500 family)
    SMD = "smd"                  #: big disc controller (IOX 1540 family)


@dataclass(frozen=True)
class DiskType:
    """One SINTRAN disk type name and the directory size SINTRAN gives it."""
    name: str            #: name without unit number, for example "DISC-75MB"
    pages: int           #: directory size in pages (declared capacity); 0 = size not known
    family: DiskFamily
    tested: bool         #: real SINTRAN M created a directory on it, or entered an ndfs image of it
    drive: str           #: drive model from the SINTRAN manual
    layout_pages: int    #: pages in the full SINTRAN disk layout; 0 = not known


DISK_TYPES: List[DiskType] = [
    DiskType('DISC-14MB', 6912, DiskFamily.WINCHESTER, True, 'CDC 9410 Finch', 6912),
    DiskType('DISC-16MB', 8000, DiskFamily.WINCHESTER, False, 'Micropolis 1304 (ST506)', 0),
    DiskType('DISC-21MB', 10728, DiskFamily.WINCHESTER, True, 'CDC 9410 Finch', 10890),
    DiskType('DISC-23MB', 11016, DiskFamily.WINCHESTER, True, 'Micropolis 1304 (ST506)', 11205),
    DiskType('DISC-28MB', 13648, DiskFamily.WINCHESTER, True, 'Micropolis 1325 (ST506)', 13797),
    DiskType('DISC-45MB', 22032, DiskFamily.WINCHESTER, True, 'Micropolis 1304/1325 (ST506)', 22410),
    DiskType('DISC-74MB', 36396, DiskFamily.WINCHESTER, True, 'Micropolis 1325 (ST506)', 36792),
    DiskType('DISC-30MB', 7389, DiskFamily.SMD, False, 'CDC 9448 CMD big cartridge', 0),
    DiskType('DISC-60MB', 7389, DiskFamily.SMD, False, 'CDC 9762 SMD (ND-IO)', 0),
    DiskType('DISC-90MB', 7389, DiskFamily.SMD, False, 'CDC 9448 CMD', 0),
    DiskType('DISC-38MB', 18486, DiskFamily.SMD, True, 'CDC 9762 SMD (ND-IO)', 18495),
    DiskType('DISC-70MB', 34765, DiskFamily.SMD, True, 'CDC 9710 RSD', 37035),
    DiskType('DISC-75MB', 36945, DiskFamily.SMD, True, 'CDC 9762 SMD', 37035),
    DiskType('DISC-2-75MB', 36945, DiskFamily.SMD, False, 'CDC 9730 MMD (150 MB, sub-divided)', 0),
    DiskType('DISC-3-75MB', 36945, DiskFamily.SMD, False, 'CDC 9766 SMD (225 MB, sub-divided)', 0),
    DiskType('DISC-140MB-F', 69530, DiskFamily.SMD, False, 'Fujitsu M2322K', 0),
    DiskType('DISC-2-70MB-F', 34765, DiskFamily.SMD, False, 'Fujitsu M2322K (140 MB, sub-divided)', 0),
    DiskType('DISC-225MB-R', 110292, DiskFamily.SMD, False, 'CDC 9766 SMD', 0),
    DiskType('DISC-225MB-F', 110292, DiskFamily.SMD, False, 'not named in the manual', 0),
    DiskType('DISC-225MB-E', 110292, DiskFamily.SMD, False, 'not named in the manual', 0),
    DiskType('DISC-225MB-N', 110292, DiskFamily.SMD, False, 'not named in the manual', 0),
    DiskType('DISC-288MB-R', 140391, DiskFamily.SMD, True, 'CDC 9766 SMD', 140733),
    DiskType('DISC-288MB-F', 140391, DiskFamily.SMD, True, 'CDC 9715 FSD 340', 153576),
    DiskType('DISC-288MB-E', 140391, DiskFamily.SMD, False, 'CDC 9720 EMD 368', 158210),
    DiskType('DISC-288MB-N', 140391, DiskFamily.SMD, False, 'not named in the manual', 144025),
    DiskType('DISC-4-70MB-R', 34765, DiskFamily.SMD, False, 'CDC 9766 SMD (280 MB, sub-divided)', 0),
    DiskType('DISC-4-70MB-F', 34765, DiskFamily.SMD, False, 'CDC 9715 FSD 340 (280 MB, sub-divided)', 0),
    DiskType('DISC-4-70MB-E', 34765, DiskFamily.SMD, False, 'CDC 9720 EMD 368 (280 MB, sub-divided)', 0),
    DiskType('DISC-4-70MB-N', 34765, DiskFamily.SMD, False, 'not named in the manual', 0),
    DiskType('DISC-6-70MB-F', 34765, DiskFamily.SMD, False, 'CDC 9715 FSD 515 (450 MB, sub-divided)', 0),
    DiskType('DISC-6-70MB-N', 34765, DiskFamily.SMD, False, 'not named in the manual', 0),
    DiskType('DISC-450MB-F', 220584, DiskFamily.SMD, True, 'CDC 9715 FSD 515', 230364),
    DiskType('DISC-450MB-N', 220584, DiskFamily.SMD, True, 'not named in the manual', 223820),
    DiskType('DISC-10MB', 0, DiskFamily.UNKNOWN, False, 'CDC 9427 Hawk cartridge (manual only)', 0),
    DiskType('DISC-33MB', 0, DiskFamily.UNKNOWN, False, 'CDC 9762 SMD, ND-IO (manual only)', 0),
    DiskType('DISC-50MB', 0, DiskFamily.UNKNOWN, False, 'CDC 9448 CMD (manual only)', 0),
    DiskType('DISC-2-225MB-F', 0, DiskFamily.UNKNOWN, False, 'CDC 9715 FSD 515 (450 MB, sub-divided; manual only)', 0),
    DiskType('DISC-2-225MB-E', 0, DiskFamily.UNKNOWN, False, 'NEC D3525-H (450 MB, sub-divided; manual only)', 0),
    DiskType('DISC-6-70MB-E', 0, DiskFamily.UNKNOWN, False, 'NEC D3525-H (450 MB, sub-divided; manual only)', 0),
    DiskType('DISC-450MB-E', 0, DiskFamily.UNKNOWN, False, 'NEC D3525-H (manual only)', 0),
]


def disk_type_count() -> int:
    return len(DISK_TYPES)


def get_disk_type(index: int) -> Optional[DiskType]:
    """Disk type number *index*, or None if out of range."""
    if 0 <= index < len(DISK_TYPES):
        return DISK_TYPES[index]
    return None


def find_disk_type(name: str) -> Optional[DiskType]:
    """Look a disk type up by name (case-insensitive, for example "disc-75mb")."""
    upper = name.upper()
    for t in DISK_TYPES:
        if t.name == upper:
            return t
    return None
