/**
 * ND-100 packed timestamp encode/decode.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "ndfs/nd_time.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ND_EPOCH   1950u
#define ND_MAX_YEAR 2013u

bool ndfs_nd_time_to_calendar(uint32_t raw, ndfs_calendar_t *out)
{
    unsigned year, month, day, hour, minute, second;

    if (raw == 0 || out == NULL)
        return false;

    year   = ((raw >> 26) & 0x3Fu) + ND_EPOCH;
    month  = (raw >> 22) & 0x0Fu;
    day    = (raw >> 17) & 0x1Fu;
    hour   = (raw >> 12) & 0x1Fu;
    minute = (raw >> 6) & 0x3Fu;
    second = raw & 0x3Fu;

    /* The source system encodes hour values 24-31; clamp rather than reject. */
    if (hour > 23u)
        hour = 0u;

    if (year < ND_EPOCH || year > ND_MAX_YEAR ||
        month < 1u || month > 12u ||
        day < 1u || day > 31u ||
        minute > 59u || second > 59u)
    {
        return false;
    }

    out->year = year;
    out->month = month;
    out->day = day;
    out->hour = hour;
    out->minute = minute;
    out->second = second;
    return true;
}

uint32_t ndfs_calendar_to_nd_time(const ndfs_calendar_t *cal)
{
    uint32_t raw;

    if (cal == NULL || cal->year < ND_EPOCH || cal->year > ND_MAX_YEAR)
        return 0;

    raw = 0;
    raw |= (uint32_t)(cal->year - ND_EPOCH) << 26;
    raw |= (uint32_t)(cal->month & 0x0Fu) << 22;
    raw |= (uint32_t)(cal->day & 0x1Fu) << 17;
    raw |= (uint32_t)(cal->hour & 0x1Fu) << 12;
    raw |= (uint32_t)(cal->minute & 0x3Fu) << 6;
    raw |= (uint32_t)(cal->second & 0x3Fu);
    return raw;
}

bool ndfs_nd_time_format(uint32_t raw, char *out, size_t len)
{
    ndfs_calendar_t cal;

    if (!ndfs_nd_time_to_calendar(raw, &cal))
    {
        if (out != NULL && len > 0)
            snprintf(out, len, "(not set)");
        return false;
    }

    if (out != NULL)
    {
        snprintf(out, len, "%04u-%02u-%02u %02u:%02u:%02u",
                 cal.year, cal.month, cal.day, cal.hour, cal.minute, cal.second);
    }
    return true;
}

/* True for a Gregorian leap year. */
static bool is_leap_year(unsigned year)
{
    return (year % 4u == 0u && year % 100u != 0u) || year % 400u == 0u;
}

void ndfs_calendar_fit_nd_range(ndfs_calendar_t *cal)
{
    if (cal == NULL)
        return;

    /* Rule chosen by Ronny Hansen, 2026-11-07: too old -> the same day in 1950; too new -> back
     * 30 years at a time, keeping day, month and time, until the 6-bit year field holds it. */
    if (cal->year < ND_EPOCH)
        cal->year = ND_EPOCH;
    while (cal->year > ND_MAX_YEAR)
        cal->year -= 30u;

    if (cal->month == 2u && cal->day == 29u && !is_leap_year(cal->year))
        cal->day = 28u;
}

uint32_t ndfs_nd_time_now(void)
{
    time_t now = time(NULL);
    struct tm *local;
    ndfs_calendar_t cal;

    if (now == (time_t)-1)
        return 0;
    local = localtime(&now);
    if (local == NULL)
        return 0;

    cal.year = (unsigned)(local->tm_year + 1900);
    cal.month = (unsigned)(local->tm_mon + 1);
    cal.day = (unsigned)local->tm_mday;
    cal.hour = (unsigned)local->tm_hour;
    cal.minute = (unsigned)local->tm_min;
    /* tm_sec can be 60 on a leap second; the ND field stops at 59. */
    cal.second = local->tm_sec > 59 ? 59u : (unsigned)local->tm_sec;
    ndfs_calendar_fit_nd_range(&cal);
    return ndfs_calendar_to_nd_time(&cal);
}
