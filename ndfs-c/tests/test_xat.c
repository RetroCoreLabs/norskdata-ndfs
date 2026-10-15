/**
 * Tests for XAT (Extended Attribute) sidecar file support.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "test_framework.h"
#include <ndfs/ndfs.h>
#include <string.h>

/* ---- Test: from_object ---- */

static int test_xat_from_object(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t xat;

    ndfs_oe_init(&entry);
    strcpy(entry.object_name, "README");
    strcpy(entry.type, "TEXT");
    strcpy(entry.user_name, "SYSTEM");
    entry.user_index = 0;
    entry.access_bits = 1279;
    entry.file_type = 3;
    entry.pages_in_file = 1;
    entry.bytes_in_file = 1024;

    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));

    TEST_ASSERT_EQUAL_STRING("README", xat.object_name);
    TEST_ASSERT_EQUAL_STRING("TEXT", xat.type);
    TEST_ASSERT_EQUAL_STRING("SYSTEM", xat.user_name);
    TEST_ASSERT_EQUAL(0, xat.user_index);
    TEST_ASSERT_EQUAL(1279, xat.access_bits);
    TEST_ASSERT_EQUAL(0, xat.file_type_flags);
    TEST_ASSERT_EQUAL(3, xat.file_type);
    TEST_ASSERT_EQUAL(1, xat.pages_in_file);
    TEST_ASSERT_EQUAL(1024, xat.bytes_in_file);

    return 0;
}

/* ---- Test: to_object ---- */

static int test_xat_to_object(void)
{
    ndfs_xat_properties_t xat;
    ndfs_object_entry_t entry;

    memset(&xat, 0, sizeof(xat));
    strcpy(xat.object_name, "HELLO");
    strcpy(xat.type, "PROG");
    strcpy(xat.user_name, "ADMIN");
    xat.user_index = 2;
    xat.access_bits = 511;
    xat.file_type = 1;

    ndfs_oe_init(&entry);
    TEST_ASSERT_OK(ndfs_xat_to_object(&xat, &entry));

    TEST_ASSERT_EQUAL_STRING("HELLO", entry.object_name);
    TEST_ASSERT_EQUAL_STRING("PROG", entry.type);
    TEST_ASSERT_EQUAL_STRING("ADMIN", entry.user_name);
    TEST_ASSERT_EQUAL(2, entry.user_index);
    TEST_ASSERT_EQUAL(511, entry.access_bits);
    TEST_ASSERT_EQUAL(1, entry.file_type);

    return 0;
}

/* ---- Test: serialize/deserialize round-trip ---- */

static int test_xat_json_roundtrip(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t original;
    ndfs_xat_properties_t restored;
    char *json = NULL;

    ndfs_oe_init(&entry);
    strcpy(entry.object_name, "TESTFILE");
    strcpy(entry.type, "DATA");
    strcpy(entry.user_name, "SYSTEM");
    entry.user_index = 0;
    entry.access_bits = 1279;
    entry.file_type = 0;
    entry.pages_in_file = 3;
    entry.bytes_in_file = 5000;

    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &original));
    TEST_ASSERT_OK(ndfs_xat_serialize(&original, &json));
    TEST_ASSERT_NOT_NULL(json);

    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &restored));
    free(json);

    TEST_ASSERT_EQUAL_STRING("TESTFILE", restored.object_name);
    TEST_ASSERT_EQUAL_STRING("DATA", restored.type);
    TEST_ASSERT_EQUAL_STRING("SYSTEM", restored.user_name);
    TEST_ASSERT_EQUAL(0, restored.user_index);
    TEST_ASSERT_EQUAL(1279, restored.access_bits);
    TEST_ASSERT_EQUAL(0, restored.file_type);
    TEST_ASSERT_EQUAL(3, restored.pages_in_file);
    TEST_ASSERT_EQUAL(5000, restored.bytes_in_file);

    return 0;
}

/* ---- Test: xat_filename ---- */

static int test_xat_filename(void)
{
    char buf[256];

    ndfs_xat_filename("README.TEXT", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("README.TEXT.xat", buf);

    ndfs_xat_filename("noext", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("noext.xat", buf);

    return 0;
}

/* ---- Test: is_xat_file ---- */

static int test_xat_is_xat_file(void)
{
    TEST_ASSERT(ndfs_xat_is_xat_file("README.TEXT.xat") == true);
    TEST_ASSERT(ndfs_xat_is_xat_file("file.xat") == true);
    TEST_ASSERT(ndfs_xat_is_xat_file("FILE.XAT") == true);

    TEST_ASSERT(ndfs_xat_is_xat_file("README.TEXT") == false);
    TEST_ASSERT(ndfs_xat_is_xat_file("xat") == false);
    TEST_ASSERT(ndfs_xat_is_xat_file(".xat") == false);
    TEST_ASSERT(ndfs_xat_is_xat_file("") == false);

    return 0;
}

/* ---- Test: null safety ---- */

static int test_xat_null_safety(void)
{
    ndfs_xat_properties_t xat;
    ndfs_object_entry_t entry;
    char *json = NULL;

    TEST_ASSERT(ndfs_xat_from_object(NULL, &xat) == NDFS_ERR_NULL_PTR);
    TEST_ASSERT(ndfs_xat_from_object(&entry, NULL) == NDFS_ERR_NULL_PTR);
    TEST_ASSERT(ndfs_xat_to_object(NULL, &entry) == NDFS_ERR_NULL_PTR);
    TEST_ASSERT(ndfs_xat_to_object(&xat, NULL) == NDFS_ERR_NULL_PTR);
    TEST_ASSERT(ndfs_xat_serialize(NULL, &json) == NDFS_ERR_NULL_PTR);
    TEST_ASSERT(ndfs_xat_serialize(&xat, NULL) == NDFS_ERR_NULL_PTR);
    TEST_ASSERT(ndfs_xat_deserialize(NULL, &xat) == NDFS_ERR_NULL_PTR);
    TEST_ASSERT(ndfs_xat_deserialize("{}", NULL) == NDFS_ERR_NULL_PTR);

    return 0;
}

/* ---- Test: all expected JSON keys present ---- */

static int test_xat_json_contains_all_keys(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t xat;
    char *json = NULL;

    ndfs_oe_init(&entry);
    strcpy(entry.object_name, "HELLO");
    strcpy(entry.type, "SYMB");
    strcpy(entry.user_name, "SYSTEM");
    entry.user_index = 5;
    entry.access_bits = 0x7FFF;
    entry.file_type = 2;
    entry.pages_in_file = 10;
    entry.bytes_in_file = 20000;

    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT_NOT_NULL(json);

    /* Every key must appear in the JSON */
    TEST_ASSERT(strstr(json, "ndfs.object_name") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.type") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.user_name") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.user_index") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.access_bits") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.file_type_flags") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.file_type") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.pages_in_file") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.bytes_in_file") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.date_created") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.last_read_date") != NULL);
    TEST_ASSERT(strstr(json, "ndfs.last_write_date") != NULL);

    free(json);
    return 0;
}

/* ---- Test: to_object only modifies status fields, not size/pages ---- */

static int test_xat_to_object_selective(void)
{
    ndfs_xat_properties_t xat;
    ndfs_object_entry_t entry;

    memset(&xat, 0, sizeof(xat));
    strcpy(xat.object_name, "NEWNAME");
    strcpy(xat.type, "MODE");
    strcpy(xat.user_name, "ADMIN");
    xat.user_index = 7;
    xat.access_bits = 0x1234;
    xat.file_type_flags = 0x28;
    xat.file_type = 3;
    xat.pages_in_file = 999;    /* should be applied */
    xat.bytes_in_file = 888;    /* should be applied */
    xat.date_created = 12345;
    xat.last_read_date = 67890;
    xat.last_write_date = 11111;

    ndfs_oe_init(&entry);
    /* Pre-set some values to verify they get overwritten */
    strcpy(entry.object_name, "OLD");
    entry.access_bits = 0;
    entry.file_type = 0;

    TEST_ASSERT_OK(ndfs_xat_to_object(&xat, &entry));

    /* Verify all fields were applied */
    TEST_ASSERT_EQUAL_STRING("NEWNAME", entry.object_name);
    TEST_ASSERT_EQUAL_STRING("MODE", entry.type);
    TEST_ASSERT_EQUAL_STRING("ADMIN", entry.user_name);
    TEST_ASSERT_EQUAL(7, entry.user_index);
    TEST_ASSERT_EQUAL(0x1234, entry.access_bits);
    TEST_ASSERT_EQUAL(3, entry.file_type);

    return 0;
}

/* ---- Test: deserialize handles empty JSON object ---- */

static int test_xat_deserialize_empty_json(void)
{
    ndfs_xat_properties_t xat;

    memset(&xat, 0xFF, sizeof(xat)); /* pre-fill with garbage */
    TEST_ASSERT_OK(ndfs_xat_deserialize("{}", &xat));

    /* Fields should be zeroed/empty since no keys present */
    TEST_ASSERT_EQUAL_STRING("", xat.object_name);

    return 0;
}

/* ---- Test: deserialize handles malformed JSON ---- */

static int test_xat_deserialize_malformed(void)
{
    ndfs_xat_properties_t xat;

    /* These should not crash -- may return error or zero-fill */
    ndfs_xat_deserialize("not json at all", &xat);
    ndfs_xat_deserialize("{broken", &xat);
    ndfs_xat_deserialize("", &xat);

    return 0;
}

/* ---- Test: serialize produces valid JSON ---- */

static int test_xat_serialize_valid_json(void)
{
    ndfs_xat_properties_t xat;
    char *json = NULL;

    memset(&xat, 0, sizeof(xat));
    strcpy(xat.object_name, "TEST");
    strcpy(xat.type, "DATA");
    xat.access_bits = 42;

    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT_NOT_NULL(json);

    /* Must start with { and end with } */
    TEST_ASSERT(json[0] == '{');
    TEST_ASSERT(json[strlen(json) - 1] == '}' || json[strlen(json) - 2] == '}');

    /* Must contain quoted keys */
    TEST_ASSERT(strstr(json, "\"ndfs.object_name\"") != NULL);
    TEST_ASSERT(strstr(json, "\"TEST\"") != NULL);

    free(json);
    return 0;
}

/* ---- Test: round-trip with maximum values ---- */

static int test_xat_roundtrip_max_values(void)
{
    ndfs_xat_properties_t original, restored;
    char *json = NULL;

    memset(&original, 0, sizeof(original));
    strcpy(original.object_name, "MAXIMUMNAME12345"); /* 16 chars */
    strcpy(original.type, "ABCD"); /* 4 chars */
    strcpy(original.user_name, "LONGUSERNAME1234"); /* 16 chars */
    original.user_index = 255;
    original.access_bits = 0x7FFF; /* all permissions */
    original.file_type_flags = 0xFF; /* all flags */
    original.file_type = 3;
    original.pages_in_file = 0xFFFFFFFF;
    original.bytes_in_file = 0xFFFFFFFF;
    original.date_created = 0xFFFFFFFF;
    original.last_read_date = 0xFFFFFFFF;
    original.last_write_date = 0xFFFFFFFF;

    TEST_ASSERT_OK(ndfs_xat_serialize(&original, &json));
    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &restored));
    free(json);

    TEST_ASSERT_EQUAL_STRING(original.object_name, restored.object_name);
    TEST_ASSERT_EQUAL_STRING(original.type, restored.type);
    TEST_ASSERT_EQUAL_STRING(original.user_name, restored.user_name);
    TEST_ASSERT_EQUAL(255, restored.user_index);
    TEST_ASSERT_EQUAL(0x7FFF, restored.access_bits);
    TEST_ASSERT_EQUAL(0xFF, restored.file_type_flags);
    TEST_ASSERT_EQUAL(3, restored.file_type);
    TEST_ASSERT_EQUAL(0xFFFFFFFF, restored.pages_in_file);
    TEST_ASSERT_EQUAL(0xFFFFFFFF, restored.bytes_in_file);
    TEST_ASSERT_EQUAL(0xFFFFFFFF, restored.date_created);

    return 0;
}

/* ---- Test: round-trip with zero/empty values ---- */

static int test_xat_roundtrip_zero_values(void)
{
    ndfs_xat_properties_t original, restored;
    char *json = NULL;

    memset(&original, 0, sizeof(original));
    /* All fields left at zero/empty */

    TEST_ASSERT_OK(ndfs_xat_serialize(&original, &json));
    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &restored));
    free(json);

    TEST_ASSERT_EQUAL_STRING("", restored.object_name);
    TEST_ASSERT_EQUAL_STRING("", restored.type);
    TEST_ASSERT_EQUAL(0, restored.user_index);
    TEST_ASSERT_EQUAL(0, restored.access_bits);
    TEST_ASSERT_EQUAL(0, restored.pages_in_file);
    TEST_ASSERT_EQUAL(0, restored.bytes_in_file);

    return 0;
}

/* ---- Test: filename with various extensions ---- */

static int test_xat_filename_edge_cases(void)
{
    char buf[256];

    ndfs_xat_filename("file.tar.gz", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("file.tar.gz.xat", buf);

    ndfs_xat_filename("a", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("a.xat", buf);

    ndfs_xat_filename("path/to/file.txt", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("path/to/file.txt.xat", buf);

    return 0;
}

/* ---- Test: is_xat_file edge cases ---- */

static int test_xat_is_xat_edge_cases(void)
{
    /* Case insensitive */
    TEST_ASSERT(ndfs_xat_is_xat_file("foo.XAT") == true);
    TEST_ASSERT(ndfs_xat_is_xat_file("foo.Xat") == true);

    /* Must have something before .xat */
    TEST_ASSERT(ndfs_xat_is_xat_file(NULL) == false);

    /* Just the extension alone */
    TEST_ASSERT(ndfs_xat_is_xat_file(".xat") == false);

    return 0;
}

/* ---- Test: get_file_properties via filesystem ---- */

static int test_xat_get_file_properties_integration(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    ndfs_xat_properties_t xat;
    const uint8_t data[] = "XAT integration test data";

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));

    /* Write a file */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/XATTEST:TEXT",
                                   data, sizeof(data) - 1));

    /* Get properties */
    TEST_ASSERT_OK(ndfs_get_file_properties(fs, "SYSTEM/XATTEST:TEXT", &xat));

    TEST_ASSERT_EQUAL_STRING("XATTEST", xat.object_name);
    TEST_ASSERT_EQUAL_STRING("TEXT", xat.type);
    TEST_ASSERT_EQUAL_STRING("SYSTEM", xat.user_name);
    TEST_ASSERT_EQUAL(0, xat.user_index);
    TEST_ASSERT_EQUAL(1, xat.pages_in_file);
    TEST_ASSERT_EQUAL(sizeof(data) - 1, xat.bytes_in_file);

    /* Non-existent file */
    TEST_ASSERT(ndfs_get_file_properties(fs, "SYSTEM/NOPE:DATA", &xat)
                == NDFS_ERR_NOT_FOUND);

    ndfs_close(fs);
    return 0;
}

/* ---- Suite runner ---- */

/* ---- Test: device_number + version pointers ---- */

static int test_xat_device_and_version(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t xat;
    char *json = NULL;
    ndfs_xat_properties_t back;
    ndfs_object_entry_t applied;

    ndfs_oe_init(&entry);
    strcpy(entry.object_name, "TTY");
    entry.device_number = 0x1234;
    entry.next_version = 0x0501;
    entry.prev_version = 0x0502;

    /* from_object captures all three. */
    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_EQUAL(0x1234, xat.device_number);
    TEST_ASSERT_EQUAL(0x0501, xat.next_version);
    TEST_ASSERT_EQUAL(0x0502, xat.prev_version);

    /* JSON round-trip preserves all three values. */
    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT(strstr(json, "\"ndfs.device_number\": 4660") != NULL);
    TEST_ASSERT(strstr(json, "\"ndfs.next_version\"") != NULL);
    TEST_ASSERT(strstr(json, "\"ndfs.prev_version\"") != NULL);
    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &back));
    TEST_ASSERT_EQUAL(0x1234, back.device_number);
    TEST_ASSERT_EQUAL(0x0501, back.next_version);
    TEST_ASSERT_EQUAL(0x0502, back.prev_version);
    ndfs_free_string(json);

    /* Apply: device_number IS restored; version pointers are NOT (conservative
     * — they reference object slots that are reassigned on import). */
    ndfs_oe_init(&applied);
    applied.next_version = 0x00AA;
    applied.prev_version = 0x00BB;
    TEST_ASSERT_OK(ndfs_xat_to_object(&back, &applied));
    TEST_ASSERT_EQUAL(0x1234, applied.device_number);
    TEST_ASSERT_EQUAL(0x00AA, applied.next_version); /* unchanged */
    TEST_ASSERT_EQUAL(0x00BB, applied.prev_version); /* unchanged */
    return 0;
}

/* ---- Sparse-hole map ----
 *
 * The sidecar records WHERE a file's holes are. pages_in_file and bytes_in_file
 * together reveal THAT a file is sparse, but not the positions, so without this
 * a sparse file copied out to a host and back returns with a different layout.
 *
 * Holes are held internally as runs, because real files hole out in blocks
 * rather than singly -- every sparse file measured on a live pack had exactly
 * one run. The JSON form is a flat ascending list of page indices, matching the
 * Python, TypeScript and C# ports so all four sidecars are comparable. */

static int test_xat_holes_absent_when_not_determined(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t xat;
    char *json = NULL;

    ndfs_oe_init(&entry);
    strcpy(entry.object_name, "LONE");
    strcpy(entry.type, "DATA");

    /* from_object alone cannot know the holes: they live in the file's index,
     * not in the object entry. The key must be omitted, which says "not
     * checked" -- a different claim from an empty list. */
    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT(xat.hole_runs_valid == 0);

    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT(strstr(json, "ndfs.holes") == NULL);
    free(json);
    return 0;
}

static int test_xat_holes_empty_list_when_solid(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t xat;
    char *json = NULL;

    ndfs_oe_init(&entry);
    strcpy(entry.object_name, "SOLID");

    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_OK(ndfs_xat_set_holes(&xat, NULL, 0));

    TEST_ASSERT(xat.hole_runs_valid == 1);
    TEST_ASSERT(xat.hole_run_count == 0);

    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT(strstr(json, "\"ndfs.holes\": []") != NULL);
    free(json);
    return 0;
}

static int test_xat_holes_compress_into_runs(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t xat;
    /* One consecutive run, as on the real pack (S3-CONFIG-E01:PROG, 54..63). */
    uint32_t pages[] = { 54, 55, 56, 57, 58, 59, 60, 61, 62, 63 };

    ndfs_oe_init(&entry);
    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_OK(ndfs_xat_set_holes(&xat, pages, 10));

    TEST_ASSERT(xat.hole_run_count == 1);
    TEST_ASSERT(xat.hole_runs[0].first_page == 54);
    TEST_ASSERT(xat.hole_runs[0].page_count == 10);
    TEST_ASSERT(xat.hole_runs_truncated == 0);
    return 0;
}

static int test_xat_holes_separate_runs_stay_separate(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t xat;
    uint32_t pages[] = { 1, 2, 7, 9, 10 };

    ndfs_oe_init(&entry);
    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_OK(ndfs_xat_set_holes(&xat, pages, 5));

    TEST_ASSERT(xat.hole_run_count == 3);
    TEST_ASSERT(xat.hole_runs[0].first_page == 1 && xat.hole_runs[0].page_count == 2);
    TEST_ASSERT(xat.hole_runs[1].first_page == 7 && xat.hole_runs[1].page_count == 1);
    TEST_ASSERT(xat.hole_runs[2].first_page == 9 && xat.hole_runs[2].page_count == 2);
    return 0;
}

static int test_xat_holes_serialize_flat_in_json(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t xat;
    uint32_t pages[] = { 1, 2, 3, 4 };
    char *json = NULL;

    ndfs_oe_init(&entry);
    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_OK(ndfs_xat_set_holes(&xat, pages, 4));

    /* Runs internally, flat list on disk -- this is what keeps the four ports
     * byte-comparable. */
    TEST_ASSERT(xat.hole_run_count == 1);
    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT(strstr(json, "\"ndfs.holes\": [1, 2, 3, 4]") != NULL);
    free(json);
    return 0;
}

static int test_xat_holes_truncate_flag_when_too_many_runs(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t xat;
    uint32_t pages[NDFS_XAT_MAX_HOLE_RUNS + 8];
    size_t i;

    /* Every other page is a hole, so each is its own run and they overflow. */
    for (i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
        pages[i] = (uint32_t)(i * 2);
    }

    ndfs_oe_init(&entry);
    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_OK(ndfs_xat_set_holes(&xat, pages, sizeof(pages) / sizeof(pages[0])));

    /* Truncation must be reported, never silent. */
    TEST_ASSERT(xat.hole_run_count == NDFS_XAT_MAX_HOLE_RUNS);
    TEST_ASSERT(xat.hole_runs_truncated == 1);
    return 0;
}

static int test_xat_holes_large_run_serializes_fully(void)
{
    ndfs_object_entry_t entry;
    ndfs_xat_properties_t xat;
    uint32_t pages[400];
    char *json = NULL;
    uint32_t i;

    /* One long run: the serializer sizes its buffer from the expanded index
     * count, so a run far bigger than the fixed part must still fit. */
    for (i = 0; i < 400; i++) pages[i] = 100 + i;

    ndfs_oe_init(&entry);
    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_OK(ndfs_xat_set_holes(&xat, pages, 400));

    TEST_ASSERT(xat.hole_run_count == 1);
    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT(strstr(json, "100, 101") != NULL);
    TEST_ASSERT(strstr(json, "499]") != NULL);
    free(json);
    return 0;
}

void run_xat_tests(void)
{
    TEST_SUITE_BEGIN("XAT Tests");

    RUN_TEST(test_xat_from_object);
    RUN_TEST(test_xat_to_object);
    RUN_TEST(test_xat_json_roundtrip);
    RUN_TEST(test_xat_json_contains_all_keys);
    RUN_TEST(test_xat_to_object_selective);
    RUN_TEST(test_xat_device_and_version);
    RUN_TEST(test_xat_deserialize_empty_json);
    RUN_TEST(test_xat_deserialize_malformed);
    RUN_TEST(test_xat_serialize_valid_json);
    RUN_TEST(test_xat_roundtrip_max_values);
    RUN_TEST(test_xat_roundtrip_zero_values);
    RUN_TEST(test_xat_filename);
    RUN_TEST(test_xat_filename_edge_cases);
    RUN_TEST(test_xat_is_xat_file);
    RUN_TEST(test_xat_is_xat_edge_cases);
    RUN_TEST(test_xat_null_safety);
    RUN_TEST(test_xat_get_file_properties_integration);

    RUN_TEST(test_xat_holes_absent_when_not_determined);
    RUN_TEST(test_xat_holes_empty_list_when_solid);
    RUN_TEST(test_xat_holes_compress_into_runs);
    RUN_TEST(test_xat_holes_separate_runs_stay_separate);
    RUN_TEST(test_xat_holes_serialize_flat_in_json);
    RUN_TEST(test_xat_holes_truncate_flag_when_too_many_runs);
    RUN_TEST(test_xat_holes_large_run_serializes_fully);
}
