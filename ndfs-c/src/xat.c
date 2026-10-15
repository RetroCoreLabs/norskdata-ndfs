/**
 * XAT (Extended Attribute) sidecar file support implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/xat.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* ---- Helpers for JSON parsing ---- */

/**
 * Find a JSON string value for a given key.
 * Returns pointer to the first char of the value (after the opening quote),
 * or NULL if not found.
 */
static const char *find_json_string(const char *json, const char *key,
                                    char *out, size_t out_len)
{
    char search[128];
    const char *pos;
    const char *start;
    const char *end;
    size_t len;

    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search);
    if (!pos) return NULL;

    /* Skip past key and colon */
    pos += strlen(search);
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;

    if (*pos != '"') return NULL;
    pos++; /* skip opening quote */
    start = pos;

    /* Find closing quote */
    end = strchr(start, '"');
    if (!end) return NULL;

    len = (size_t)(end - start);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, start, len);
    out[len] = '\0';

    return end + 1;
}

/**
 * Find a JSON integer value for a given key.
 * Returns 1 if found, 0 if not found.
 */
static int find_json_int(const char *json, const char *key, uint32_t *out_val)
{
    char search[128];
    const char *pos;
    char *endptr;

    snprintf(search, sizeof(search), "\"%s\"", key);
    pos = strstr(json, search);
    if (!pos) return 0;

    pos += strlen(search);
    while (*pos && (*pos == ' ' || *pos == '\t' || *pos == ':')) pos++;

    /* Skip optional quotes around numbers */
    if (*pos == '"') pos++;

    /* Fields are uint32_t and can reach 0xFFFFFFFF. strtoul holds the full
     * 32-bit range even where long is only 32-bit (e.g. Windows/MinGW),
     * whereas strtol would saturate at 0x7FFFFFFF. */
    *out_val = (uint32_t)strtoul(pos, &endptr, 10);
    return (endptr != pos) ? 1 : 0;
}

/* ---- Public API ---- */

ndfs_error_t ndfs_xat_from_object(const ndfs_object_entry_t *entry,
                                  ndfs_xat_properties_t *out)
{
    if (!entry || !out) return NDFS_ERR_NULL_PTR;

    memset(out, 0, sizeof(*out));

    /* snprintf guarantees null-termination and avoids the strncpy truncation
     * warning when the source is exactly the field width. */
    snprintf(out->object_name, sizeof(out->object_name), "%s", entry->object_name);
    snprintf(out->type, sizeof(out->type), "%s", entry->type);
    snprintf(out->user_name, sizeof(out->user_name), "%s", entry->user_name);

    out->user_index = entry->user_index;
    out->access_bits = entry->access_bits;
    out->file_type_flags = entry->file_type_flags;
    out->file_type = entry->file_type;
    out->pages_in_file = entry->pages_in_file;
    out->bytes_in_file = entry->bytes_in_file;
    out->date_created = entry->date_created;
    out->last_read_date = entry->last_read_date;
    out->last_write_date = entry->last_write_date;
    out->device_number = entry->device_number;
    out->next_version = entry->next_version;
    out->prev_version = entry->prev_version;

    return NDFS_OK;
}

ndfs_error_t ndfs_xat_to_object(const ndfs_xat_properties_t *xat,
                                ndfs_object_entry_t *entry)
{
    if (!xat || !entry) return NDFS_ERR_NULL_PTR;

    /* Apply status-related fields */
    entry->access_bits = xat->access_bits;
    entry->file_type = xat->file_type;

    /* Copy name fields if non-empty */
    if (xat->object_name[0] != '\0') {
        strncpy(entry->object_name, xat->object_name, NDFS_NAME_MAX);
        entry->object_name[NDFS_NAME_MAX] = '\0';
    }
    if (xat->type[0] != '\0') {
        strncpy(entry->type, xat->type, NDFS_TYPE_MAX);
        entry->type[NDFS_TYPE_MAX] = '\0';
    }
    if (xat->user_name[0] != '\0') {
        strncpy(entry->user_name, xat->user_name, NDFS_NAME_MAX);
        entry->user_name[NDFS_NAME_MAX] = '\0';
    }

    entry->user_index = xat->user_index;

    /* device_number is a logical id, safe to restore in a new location. The
     * version pointers (next/prev) are recorded in the sidecar but NOT applied:
     * they reference object slots that are reassigned on import, so restoring
     * them would create a dangling/incorrect version chain. */
    entry->device_number = xat->device_number;

    return NDFS_OK;
}

ndfs_error_t ndfs_xat_serialize(const ndfs_xat_properties_t *xat,
                                char **out_json)
{
    char *buf;
    int len;
    size_t cap;
    uint32_t total_holes = 0;
    uint16_t r;

    if (!xat || !out_json) return NDFS_ERR_NULL_PTR;

    /* The hole list is emitted as a flat ascending array of page indices, so all
     * four implementations produce the same JSON. Size the buffer for it: up to
     * 12 characters per index ("4294967295, "), plus the fixed part. */
    for (r = 0; r < xat->hole_run_count; r++) {
        total_holes += xat->hole_runs[r].page_count;
    }

    cap = 1536 + (size_t)total_holes * 12u;
    buf = (char *)malloc(cap);
    if (!buf) return NDFS_ERR_ALLOC;

    len = snprintf(buf, cap,
        "{\n"
        "  \"ndfs.object_name\": \"%s\",\n"
        "  \"ndfs.type\": \"%s\",\n"
        "  \"ndfs.user_name\": \"%s\",\n"
        "  \"ndfs.user_index\": %u,\n"
        "  \"ndfs.access_bits\": %u,\n"
        "  \"ndfs.file_type_flags\": %u,\n"
        "  \"ndfs.file_type\": %u,\n"
        "  \"ndfs.device_number\": %u,\n"
        "  \"ndfs.next_version\": %u,\n"
        "  \"ndfs.prev_version\": %u,\n"
        "  \"ndfs.pages_in_file\": %u,\n"
        "  \"ndfs.bytes_in_file\": %u,\n"
        "  \"ndfs.date_created\": %u,\n"
        "  \"ndfs.last_read_date\": %u,\n"
        "  \"ndfs.last_write_date\": %u",
        xat->object_name,
        xat->type,
        xat->user_name,
        (unsigned)xat->user_index,
        (unsigned)xat->access_bits,
        (unsigned)xat->file_type_flags,
        (unsigned)xat->file_type,
        (unsigned)xat->device_number,
        (unsigned)xat->next_version,
        (unsigned)xat->prev_version,
        (unsigned)xat->pages_in_file,
        (unsigned)xat->bytes_in_file,
        (unsigned)xat->date_created,
        (unsigned)xat->last_read_date,
        (unsigned)xat->last_write_date);

    if (len < 0) {
        free(buf);
        return NDFS_ERR_INVALID_ARG;
    }

    /* Append the hole list, expanding the runs into the flat form the other
     * implementations use. Omitted entirely when the holes were not determined,
     * so a missing key means "not checked" and an empty array means "none". */
    if (xat->hole_runs_valid) {
        int n = snprintf(buf + len, cap - (size_t)len, ",\n  \"ndfs.holes\": [");
        if (n < 0) { free(buf); return NDFS_ERR_INVALID_ARG; }
        len += n;

        {
            uint32_t emitted = 0;
            for (r = 0; r < xat->hole_run_count; r++) {
                uint32_t i;
                for (i = 0; i < xat->hole_runs[r].page_count; i++) {
                    n = snprintf(buf + len, cap - (size_t)len, "%s%u",
                                 emitted ? ", " : "",
                                 (unsigned)(xat->hole_runs[r].first_page + i));
                    if (n < 0) { free(buf); return NDFS_ERR_INVALID_ARG; }
                    len += n;
                    emitted++;
                }
            }
        }

        n = snprintf(buf + len, cap - (size_t)len, "]\n}");
        if (n < 0) { free(buf); return NDFS_ERR_INVALID_ARG; }
    } else {
        if (snprintf(buf + len, cap - (size_t)len, "\n}") < 0) {
            free(buf);
            return NDFS_ERR_INVALID_ARG;
        }
    }

    *out_json = buf;
    return NDFS_OK;
}

ndfs_error_t ndfs_xat_set_holes(ndfs_xat_properties_t *xat,
                                const uint32_t *pages, size_t count)
{
    size_t i;

    if (!xat) return NDFS_ERR_NULL_PTR;
    if (count && !pages) return NDFS_ERR_NULL_PTR;

    xat->hole_run_count = 0;
    xat->hole_runs_truncated = 0;
    xat->hole_runs_valid = 1;

    for (i = 0; i < count; i++) {
        /* Extend the current run when this page continues it, else start a new
         * one. Input must be ascending; anything else starts a fresh run, which
         * still round-trips through the flat JSON form. */
        if (xat->hole_run_count > 0) {
            ndfs_xat_hole_run_t *last = &xat->hole_runs[xat->hole_run_count - 1];
            if (pages[i] == last->first_page + last->page_count) {
                last->page_count++;
                continue;
            }
        }

        if (xat->hole_run_count >= NDFS_XAT_MAX_HOLE_RUNS) {
            xat->hole_runs_truncated = 1;
            break;
        }

        xat->hole_runs[xat->hole_run_count].first_page = pages[i];
        xat->hole_runs[xat->hole_run_count].page_count = 1;
        xat->hole_run_count++;
    }

    return NDFS_OK;
}

ndfs_error_t ndfs_xat_deserialize(const char *json,
                                  ndfs_xat_properties_t *out)
{
    uint32_t val;

    if (!json || !out) return NDFS_ERR_NULL_PTR;

    memset(out, 0, sizeof(*out));

    find_json_string(json, "ndfs.object_name", out->object_name,
                     sizeof(out->object_name));
    find_json_string(json, "ndfs.type", out->type, sizeof(out->type));
    find_json_string(json, "ndfs.user_name", out->user_name,
                     sizeof(out->user_name));

    if (find_json_int(json, "ndfs.user_index", &val))
        out->user_index = (uint8_t)val;
    if (find_json_int(json, "ndfs.access_bits", &val))
        out->access_bits = (uint16_t)val;
    if (find_json_int(json, "ndfs.file_type_flags", &val))
        out->file_type_flags = (uint16_t)val;
    if (find_json_int(json, "ndfs.file_type", &val))
        out->file_type = (uint8_t)val;
    if (find_json_int(json, "ndfs.device_number", &val))
        out->device_number = (uint16_t)val;
    if (find_json_int(json, "ndfs.next_version", &val))
        out->next_version = (uint16_t)val;
    if (find_json_int(json, "ndfs.prev_version", &val))
        out->prev_version = (uint16_t)val;
    if (find_json_int(json, "ndfs.pages_in_file", &val))
        out->pages_in_file = (uint32_t)val;
    if (find_json_int(json, "ndfs.bytes_in_file", &val))
        out->bytes_in_file = (uint32_t)val;
    if (find_json_int(json, "ndfs.date_created", &val))
        out->date_created = (uint32_t)val;
    if (find_json_int(json, "ndfs.last_read_date", &val))
        out->last_read_date = (uint32_t)val;
    if (find_json_int(json, "ndfs.last_write_date", &val))
        out->last_write_date = (uint32_t)val;

    return NDFS_OK;
}

void ndfs_xat_filename(const char *data_file, char *out, size_t out_len)
{
    size_t len;

    if (!data_file || !out || out_len == 0) return;

    len = strlen(data_file);
    if (len + 5 >= out_len) {
        /* Truncate if necessary */
        if (out_len > 5) {
            memcpy(out, data_file, out_len - 5);
            memcpy(out + out_len - 5, ".xat", 4);
            out[out_len - 1] = '\0';
        } else {
            out[0] = '\0';
        }
        return;
    }

    memcpy(out, data_file, len);
    memcpy(out + len, ".xat", 5); /* includes null terminator */
}

bool ndfs_xat_is_xat_file(const char *filename)
{
    size_t len;
    size_t ext_len = 4; /* strlen(".xat") */

    if (!filename) return false;

    len = strlen(filename);
    if (len <= ext_len) return false;

    /* Case-insensitive compare of last 4 chars */
    {
        const char *suffix = filename + len - ext_len;
        if (tolower((unsigned char)suffix[0]) == '.' &&
            tolower((unsigned char)suffix[1]) == 'x' &&
            tolower((unsigned char)suffix[2]) == 'a' &&
            tolower((unsigned char)suffix[3]) == 't') {
            return true;
        }
    }

    return false;
}
