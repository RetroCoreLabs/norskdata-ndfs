/**
 * XAT (Extended Attribute) sidecar file support for NDFS.
 *
 * Preserves NDFS metadata (access bits, dates, file type flags, user info, etc.)
 * when files are copied to/from host filesystems.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_XAT_H
#define NDFS_XAT_H

#include "types.h"
#include "object_entry.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Most sparse-hole RUNS an XAT record can hold. See ndfs_xat_hole_run_t. */
#define NDFS_XAT_MAX_HOLE_RUNS 64

/**
 * One run of consecutive sparse holes, as LOGICAL page indices.
 *
 * Holes are stored as runs rather than as individual indices because real files
 * hole out in blocks, not singly: every sparse file measured on a live pack had
 * exactly ONE run (for example pages 54..63 of S3-CONFIG-E01:PROG). Runs keep the
 * structure small enough to sit on the stack, which matters because every caller
 * declares ndfs_xat_properties_t as a local.
 *
 * The JSON form is a flat ascending list of page indices, so that all four
 * implementations emit identical sidecars; runs are an internal representation
 * only.
 */
typedef struct {
    uint32_t first_page;   /**< first hole in the run                    */
    uint32_t page_count;   /**< how many consecutive pages the run covers */
} ndfs_xat_hole_run_t;

/** XAT properties structure: holds all NDFS metadata for sidecar files. */
typedef struct {
    char     object_name[NDFS_NAME_MAX + 1];
    char     type[NDFS_TYPE_MAX + 1];
    char     user_name[NDFS_NAME_MAX + 1];
    uint8_t  user_index;
    uint16_t access_bits;
    uint16_t file_type_flags;
    uint8_t  file_type;
    uint32_t pages_in_file;
    uint32_t bytes_in_file;
    uint32_t date_created;
    uint32_t last_read_date;
    uint32_t last_write_date;
    uint16_t device_number;   /* logical device id (restored on import)        */
    uint16_t next_version;    /* version-chain links — recorded, NOT restored  */
    uint16_t prev_version;    /* (they reference object slots that change)      */

    /* Sparse holes, as runs of logical page indices. Recorded, NOT restored --
     * an object entry has nowhere to put them; the holes live in the file's
     * index, which only the write path can build.
     *
     * Nothing else in the sidecar says WHERE the holes are. pages_in_file and
     * bytes_in_file together reveal THAT a file is sparse (bytes / 2048 exceeds
     * the page count), but not their positions -- so without this a sparse file
     * copied out to a host and back returns with a different layout. It also
     * preserves a distinction the data cannot: a hole and a page of genuine
     * zeros read back identically but differ on disk.
     *
     * hole_runs_valid is 0 when the caller could not determine the holes, which
     * is not the same as knowing there are none (hole_run_count == 0). */
    ndfs_xat_hole_run_t hole_runs[NDFS_XAT_MAX_HOLE_RUNS];
    uint16_t hole_run_count;      /**< runs actually present in hole_runs      */
    uint8_t  hole_runs_valid;     /**< 1 when the hole list was determined      */
    uint8_t  hole_runs_truncated; /**< 1 when more runs existed than fit        */
} ndfs_xat_properties_t;

/** XAT file extension. */
#define NDFS_XAT_EXTENSION ".xat"

/**
 * Extract XAT properties from an object entry.
 *
 * @param entry  Source object entry.
 * @param out    Receives the XAT properties.
 */
ndfs_error_t ndfs_xat_from_object(const ndfs_object_entry_t *entry,
                                  ndfs_xat_properties_t *out);

/**
 * Apply XAT properties to an object entry.
 * Restores fields that are valid in a new location (access_bits, file_type,
 * names, user_index, device_number). Version pointers and allocation/runtime
 * state are intentionally NOT restored — see ndfs_xat_to_object.
 *
 * @param xat    Source XAT properties.
 * @param entry  Target object entry to modify.
 */
ndfs_error_t ndfs_xat_to_object(const ndfs_xat_properties_t *xat,
                                ndfs_object_entry_t *entry);

/**
 * Record a file's sparse holes, compressing an ascending page-index list into
 * runs. Marks the hole list as determined, so the serializer emits the key.
 *
 * Pass count 0 to record "checked, and there are none", which is different from
 * never calling this at all -- in that case the key is omitted, meaning
 * "not checked".
 *
 * @param xat    Properties to update.
 * @param pages  Ascending logical page indices of the holes.
 * @param count  How many indices @p pages holds.
 */
ndfs_error_t ndfs_xat_set_holes(ndfs_xat_properties_t *xat,
                                const uint32_t *pages, size_t count);

/**
 * Serialize XAT properties to a JSON string.
 * Caller must free the returned string with free().
 *
 * @param xat       Source properties.
 * @param out_json  Receives malloc'd null-terminated JSON string.
 */
ndfs_error_t ndfs_xat_serialize(const ndfs_xat_properties_t *xat,
                                char **out_json);

/**
 * Deserialize XAT properties from a JSON string.
 *
 * @param json  Null-terminated JSON string.
 * @param out   Receives the parsed properties.
 */
ndfs_error_t ndfs_xat_deserialize(const char *json,
                                  ndfs_xat_properties_t *out);

/**
 * Get the .xat filename for a given data filename.
 * Appends ".xat" to the filename.
 *
 * @param data_file  Source data filename.
 * @param out        Destination buffer for the .xat filename.
 * @param out_len    Size of destination buffer.
 */
void ndfs_xat_filename(const char *data_file, char *out, size_t out_len);

/**
 * Check if a filename is an XAT sidecar file.
 *
 * @param filename  The filename to check.
 * @return true if filename ends with ".xat".
 */
bool ndfs_xat_is_xat_file(const char *filename);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_XAT_H */
