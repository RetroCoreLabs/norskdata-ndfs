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

import calendar
from datetime import datetime
from typing import Optional

# ND epoch year.
ND_EPOCH: int = 1950

# Last year the 6-bit year field holds.
ND_MAX_YEAR: int = 2013


def fit_nd_range(dt: datetime) -> datetime:
    """Move a date into the years an ND timestamp can hold (1950-2013).

    The year field has 6 bits, so today's date does not fit. Rule chosen by
    Ronny Hansen on 2026-11-07: a date before 1950 becomes the same day in
    1950; a date after 2013 keeps its day, month and time and moves back 30
    years, again until it fits (2026-11-07 becomes 1996-11-07, 2050 becomes
    1990). 29 February becomes 28 February when the new year has no
    29 February. A date already in range is returned unchanged.
    """
    year = dt.year
    if year < ND_EPOCH:
        year = ND_EPOCH
    while year > ND_MAX_YEAR:
        year -= 30
    day = dt.day
    if dt.month == 2 and day == 29 and not calendar.isleap(year):
        day = 28
    return dt.replace(year=year, day=day)


def nd_time_now() -> int:
    """The host's local time now, moved into range with fit_nd_range and
    packed. This is what the writers stamp on a file they create or write."""
    return date_to_nd_time(fit_nd_range(datetime.now()))


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
