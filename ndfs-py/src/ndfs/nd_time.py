"""
ND-100 timestamp format: 32-bit packed date/time.

Bits 31-26: year (0-63, add 1950 for calendar year)
Bits 25-22: month (1-12)
Bits 21-17: day (1-31)
Bits 16-12: hour (0-23)
Bits 11-6:  minute (0-59)
Bits 5-0:   second (0-59)

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from datetime import datetime
from typing import Optional

# ND epoch year.
ND_EPOCH: int = 1950


def nd_time_to_date(value: int) -> Optional[datetime]:
    """Unpack a 32-bit ND timestamp to a datetime, or None if the value is 0."""
    if value == 0:
        return None

    year = ((value >> 26) & 0x3F) + ND_EPOCH
    month = (value >> 22) & 0x0F
    day = (value >> 17) & 0x1F
    hour = (value >> 12) & 0x1F
    minute = (value >> 6) & 0x3F
    second = value & 0x3F

    return datetime(year, month, day, hour, minute, second)


def date_to_nd_time(dt: Optional[datetime]) -> int:
    """Pack a datetime into a 32-bit ND timestamp. Returns 0 for None input."""
    if dt is None:
        return 0

    year = dt.year - ND_EPOCH
    if year < 0 or year > 63:
        return 0

    return (
        ((year & 0x3F) << 26)
        | ((dt.month & 0x0F) << 22)
        | ((dt.day & 0x1F) << 17)
        | ((dt.hour & 0x1F) << 12)
        | ((dt.minute & 0x3F) << 6)
        | (dt.second & 0x3F)
    ) & 0xFFFFFFFF
