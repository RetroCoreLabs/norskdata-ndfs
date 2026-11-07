/**
 * Tests for ND-100 packed timestamp encode/decode.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_framework.h"
#include <ndfs/ndfs.h>
#include <string.h>

static int test_decode_known_value(void)
{
    /* 0xB9469AE4 / 3108412132, cross-verified against the ndfs-ts and
     * ndfs-py implementations and a real disk-dump sample: 1996-05-03 09:43:36. */
    ndfs_calendar_t cal;
    TEST_ASSERT(ndfs_nd_time_to_calendar(3108412132u, &cal) == true);
    TEST_ASSERT_EQUAL(1996, cal.year);
    TEST_ASSERT_EQUAL(5, cal.month);
    TEST_ASSERT_EQUAL(3, cal.day);
    TEST_ASSERT_EQUAL(9, cal.hour);
    TEST_ASSERT_EQUAL(43, cal.minute);
    TEST_ASSERT_EQUAL(36, cal.second);
    return 0;
}

static int test_encode_matches_known_value(void)
{
    ndfs_calendar_t cal;
    cal.year = 1996;
    cal.month = 5;
    cal.day = 3;
    cal.hour = 9;
    cal.minute = 43;
    cal.second = 36;
    TEST_ASSERT_EQUAL_UINT(3108412132u, ndfs_calendar_to_nd_time(&cal));
    return 0;
}

static int test_zero_is_not_set(void)
{
    ndfs_calendar_t cal;
    TEST_ASSERT(ndfs_nd_time_to_calendar(0, &cal) == false);
    return 0;
}

static int test_round_trip_epoch_boundaries(void)
{
    ndfs_calendar_t original, decoded;
    uint32_t raw;

    original.year = 1950;
    original.month = 1;
    original.day = 1;
    original.hour = 0;
    original.minute = 0;
    original.second = 0;
    raw = ndfs_calendar_to_nd_time(&original);
    TEST_ASSERT(raw != 0); /* year offset 0 still leaves nonzero bits from month/day */
    TEST_ASSERT(ndfs_nd_time_to_calendar(raw, &decoded) == true);
    TEST_ASSERT_EQUAL(1950, decoded.year);
    TEST_ASSERT_EQUAL(1, decoded.month);
    TEST_ASSERT_EQUAL(1, decoded.day);

    original.year = 2013;
    original.month = 12;
    original.day = 31;
    original.hour = 23;
    original.minute = 59;
    original.second = 59;
    raw = ndfs_calendar_to_nd_time(&original);
    TEST_ASSERT(ndfs_nd_time_to_calendar(raw, &decoded) == true);
    TEST_ASSERT_EQUAL(2013, decoded.year);
    TEST_ASSERT_EQUAL(23, decoded.hour);
    TEST_ASSERT_EQUAL(59, decoded.minute);
    TEST_ASSERT_EQUAL(59, decoded.second);
    return 0;
}

static int test_encode_year_out_of_range_returns_zero(void)
{
    ndfs_calendar_t cal;
    cal.year = 1949;
    cal.month = 1;
    cal.day = 1;
    cal.hour = 0;
    cal.minute = 0;
    cal.second = 0;
    TEST_ASSERT_EQUAL_UINT(0u, ndfs_calendar_to_nd_time(&cal));

    cal.year = 2014;
    TEST_ASSERT_EQUAL_UINT(0u, ndfs_calendar_to_nd_time(&cal));
    return 0;
}

static int test_hour_24_to_31_clamps_to_zero(void)
{
    /* Raw hour field (bits 16-12) can hold 0-31; the source system is known
     * to encode values 24-31 in this field, which decode as hour 0 rather
     * than being rejected. */
    ndfs_calendar_t base, decoded;
    uint32_t raw;

    base.year = 1996;
    base.month = 5;
    base.day = 3;
    base.hour = 24; /* out of calendar range, but a legal 5-bit field value */
    base.minute = 43;
    base.second = 36;
    raw = ndfs_calendar_to_nd_time(&base);

    TEST_ASSERT(ndfs_nd_time_to_calendar(raw, &decoded) == true);
    TEST_ASSERT_EQUAL(0, decoded.hour);
    return 0;
}

static int test_decode_null_out_is_safe(void)
{
    TEST_ASSERT(ndfs_nd_time_to_calendar(3108412132u, NULL) == false);
    return 0;
}

static int test_format_known_value(void)
{
    char buf[32];
    TEST_ASSERT(ndfs_nd_time_format(3108412132u, buf, sizeof(buf)) == true);
    TEST_ASSERT_EQUAL_STRING("1996-05-03 09:43:36", buf);
    return 0;
}

static int test_format_not_set(void)
{
    char buf[32];
    TEST_ASSERT(ndfs_nd_time_format(0, buf, sizeof(buf)) == false);
    TEST_ASSERT_EQUAL_STRING("(not set)", buf);
    return 0;
}

/* Fit one date and check the year and day it lands on; month and time never change. */
static int fit_case(unsigned year, unsigned month, unsigned day,
                    unsigned want_year, unsigned want_day)
{
    ndfs_calendar_t cal = { 0 };
    cal.year = year; cal.month = month; cal.day = day;
    cal.hour = 13; cal.minute = 45; cal.second = 30;
    ndfs_calendar_fit_nd_range(&cal);
    TEST_ASSERT_EQUAL(want_year, cal.year);
    TEST_ASSERT_EQUAL(month, cal.month);
    TEST_ASSERT_EQUAL(want_day, cal.day);
    TEST_ASSERT_EQUAL(13, cal.hour);
    TEST_ASSERT_EQUAL(45, cal.minute);
    TEST_ASSERT_EQUAL(30, cal.second);
    return 0;
}

/* Ronny's rule (2026-11-07): before 1950 -> 1950; after 2013 -> back 30 years until it fits. */
static int test_fit_nd_range(void)
{
    TEST_ASSERT_EQUAL(0, fit_case(2026, 11, 7, 1996, 7));   /* today -> 1996-11-07 */
    TEST_ASSERT_EQUAL(0, fit_case(2014, 1, 1, 1984, 1));    /* first year that does not fit */
    TEST_ASSERT_EQUAL(0, fit_case(2043, 12, 31, 2013, 31)); /* still one step */
    TEST_ASSERT_EQUAL(0, fit_case(2044, 1, 1, 1984, 1));    /* two steps */
    TEST_ASSERT_EQUAL(0, fit_case(2013, 12, 31, 2013, 31)); /* in range: unchanged */
    TEST_ASSERT_EQUAL(0, fit_case(1950, 1, 1, 1950, 1));
    TEST_ASSERT_EQUAL(0, fit_case(1949, 6, 15, 1950, 15));  /* too old */
    TEST_ASSERT_EQUAL(0, fit_case(1800, 3, 2, 1950, 2));
    TEST_ASSERT_EQUAL(0, fit_case(2028, 2, 29, 1998, 28));  /* 1998 has no 29 February */
    TEST_ASSERT_EQUAL(0, fit_case(2044, 2, 29, 1984, 29));  /* 1984 has one */
    TEST_ASSERT_EQUAL(0, fit_case(1948, 2, 29, 1950, 28));
    ndfs_calendar_fit_nd_range(NULL);                       /* must not crash */
    return 0;
}

/* "Now" always packs into a valid, nonzero ND date. */
static int test_now_is_in_range(void)
{
    uint32_t raw = ndfs_nd_time_now();
    ndfs_calendar_t cal;

    TEST_ASSERT(raw != 0);
    TEST_ASSERT(ndfs_nd_time_to_calendar(raw, &cal));
    TEST_ASSERT(cal.year >= 1950 && cal.year <= 2013);
    return 0;
}

void run_nd_time_tests(void)
{
    TEST_SUITE_BEGIN("ND Time Tests");

    RUN_TEST(test_decode_known_value);
    RUN_TEST(test_encode_matches_known_value);
    RUN_TEST(test_zero_is_not_set);
    RUN_TEST(test_round_trip_epoch_boundaries);
    RUN_TEST(test_encode_year_out_of_range_returns_zero);
    RUN_TEST(test_hour_24_to_31_clamps_to_zero);
    RUN_TEST(test_decode_null_out_is_safe);
    RUN_TEST(test_format_known_value);
    RUN_TEST(test_format_not_set);
    RUN_TEST(test_fit_nd_range);
    RUN_TEST(test_now_is_in_range);
}
