"""
Tests for ndfs.nd_time -- ND-100 timestamp packing/unpacking.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest
from datetime import datetime
from ndfs.nd_time import nd_time_to_date, date_to_nd_time


class TestNdTimeToDate:
    def test_returns_none_for_value_0(self):
        assert nd_time_to_date(0) is None

    def test_decodes_a_known_timestamp(self):
        # Year 1985 (35 years from 1950), month 6, day 15, hour 10, min 30, sec 45
        value = (
            ((35 & 0x3F) << 26)
            | ((6 & 0x0F) << 22)
            | ((15 & 0x1F) << 17)
            | ((10 & 0x1F) << 12)
            | ((30 & 0x3F) << 6)
            | (45 & 0x3F)
        )
        dt = nd_time_to_date(value)
        assert dt is not None
        assert dt.year == 1985
        assert dt.month == 6
        assert dt.day == 15
        assert dt.hour == 10
        assert dt.minute == 30
        assert dt.second == 45


class TestDateToNdTime:
    def test_returns_0_for_none(self):
        assert date_to_nd_time(None) == 0

    def test_returns_0_for_year_before_epoch(self):
        assert date_to_nd_time(datetime(1940, 1, 1)) == 0

    def test_returns_0_for_year_beyond_range(self):
        assert date_to_nd_time(datetime(2020, 1, 1)) == 0  # 2020-1950=70 > 63


class TestNdTimeRoundTrip:
    def test_round_trips_1985_06_15_10_30_45(self):
        original = datetime(1985, 6, 15, 10, 30, 45)
        packed = date_to_nd_time(original)
        unpacked = nd_time_to_date(packed)
        assert unpacked is not None
        assert unpacked.year == 1985
        assert unpacked.month == 6
        assert unpacked.day == 15
        assert unpacked.hour == 10
        assert unpacked.minute == 30
        assert unpacked.second == 45

    def test_round_trips_1950_01_01_00_00_01(self):
        original = datetime(1950, 1, 1, 0, 0, 1)
        packed = date_to_nd_time(original)
        unpacked = nd_time_to_date(packed)
        assert unpacked.year == 1950
        assert unpacked.month == 1
        assert unpacked.day == 1
        assert unpacked.second == 1

    def test_round_trips_2013_12_31_23_59_59(self):
        original = datetime(2013, 12, 31, 23, 59, 59)
        packed = date_to_nd_time(original)
        unpacked = nd_time_to_date(packed)
        assert unpacked.year == 2013
        assert unpacked.month == 12
        assert unpacked.day == 31
        assert unpacked.hour == 23
        assert unpacked.minute == 59
        assert unpacked.second == 59
