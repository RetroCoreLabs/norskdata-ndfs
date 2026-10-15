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
