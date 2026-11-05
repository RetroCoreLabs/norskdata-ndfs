/**
 * Tests for ObjectEntry.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 */

#include "test_framework.h"
#include <ndfs/object_entry.h>
#include <string.h>

static int test_init(void)
{
    ndfs_object_entry_t oe;
    ndfs_oe_init(&oe);
    TEST_ASSERT_EQUAL(NDFS_OBJECT_IN_USE, oe.header);
    TEST_ASSERT_EQUAL_STRING("DATA", oe.type);
    TEST_ASSERT_EQUAL(0, oe.pages_in_file);
    TEST_ASSERT_EQUAL(0, oe.bytes_in_file);
    return 0;
}

static int test_roundtrip(void)
{
    ndfs_object_entry_t oe, oe2;
    uint8_t buf[NDFS_ENTRY_SIZE];
    ndfs_error_t err;

    ndfs_oe_init(&oe);
    strcpy(oe.object_name, "MYFILE");
    strcpy(oe.type, "PROG");
    oe.user_index = 2;
    oe.file_type = 1; /* PROG */
    oe.pages_in_file = 5;
    oe.bytes_in_file = 9000;
    oe.file_pointer.block_id = 42;
    oe.file_pointer.type = NDFS_PTR_INDEXED;

    err = ndfs_oe_to_bytes(&oe, buf, NDFS_ENTRY_SIZE, 0);
    TEST_ASSERT_OK(err);

    err = ndfs_oe_from_bytes(buf, NDFS_ENTRY_SIZE, 0, &oe2);
    TEST_ASSERT_OK(err);
    TEST_ASSERT_EQUAL_STRING("MYFILE", oe2.object_name);
    TEST_ASSERT_EQUAL_STRING("PROG", oe2.type);
    TEST_ASSERT_EQUAL(2, oe2.user_index);
    TEST_ASSERT_EQUAL(1, oe2.file_type);
    TEST_ASSERT_EQUAL(5, oe2.pages_in_file);
    TEST_ASSERT_EQUAL(9000, oe2.bytes_in_file);
    TEST_ASSERT_EQUAL(42, oe2.file_pointer.block_id);
    TEST_ASSERT_EQUAL(NDFS_PTR_INDEXED, oe2.file_pointer.type);
    return 0;
}

static int test_bytes_in_file_encoding(void)
{
    /* bytes_in_file = stored_value + 1 */
    ndfs_object_entry_t oe, oe2;
    uint8_t buf[NDFS_ENTRY_SIZE];

    ndfs_oe_init(&oe);
    strcpy(oe.object_name, "TEST");
    oe.bytes_in_file = 1; /* Should store 0 in bytes 56-59 */
    oe.file_pointer.block_id = 10;
    oe.file_pointer.type = NDFS_PTR_CONTIGUOUS;

    ndfs_oe_to_bytes(&oe, buf, NDFS_ENTRY_SIZE, 0);

    /* Bytes 56-59 should be 0 (bytes_in_file - 1 = 0) */
    TEST_ASSERT_EQUAL(0, buf[56]);
    TEST_ASSERT_EQUAL(0, buf[57]);
    TEST_ASSERT_EQUAL(0, buf[58]);
    TEST_ASSERT_EQUAL(0, buf[59]);

    ndfs_oe_from_bytes(buf, NDFS_ENTRY_SIZE, 0, &oe2);
    TEST_ASSERT_EQUAL(1, oe2.bytes_in_file);
    return 0;
}

/* An empty file is stored as 0xFFFFFFFF (0 - 1), as SINTRAN stores it: nd100x SMD0.IMG holds
 * it in ten SINTRAN-made entries such as (SYSTEM)SINTRAN:DATA and (SYSTEM)TERMINAL. Before
 * 2026-11-04 the writer stored 0, which reads back as 1 byte. */
static int test_empty_file_stored_as_all_ones(void)
{
    ndfs_object_entry_t oe, oe2;
    uint8_t buf[NDFS_ENTRY_SIZE];

    ndfs_oe_init(&oe);
    strcpy(oe.object_name, "EMPTY");
    oe.bytes_in_file = 0;
    oe.file_pointer.block_id = 10;
    oe.file_pointer.type = NDFS_PTR_INDEXED;

    ndfs_oe_to_bytes(&oe, buf, NDFS_ENTRY_SIZE, 0);
    TEST_ASSERT_EQUAL(0xFF, buf[56]);
    TEST_ASSERT_EQUAL(0xFF, buf[57]);
    TEST_ASSERT_EQUAL(0xFF, buf[58]);
    TEST_ASSERT_EQUAL(0xFF, buf[59]);

    ndfs_oe_from_bytes(buf, NDFS_ENTRY_SIZE, 0, &oe2);
    TEST_ASSERT_EQUAL(0, oe2.bytes_in_file);

    /* And reading SINTRAN's own bytes then writing them back changes nothing. */
    ndfs_oe_to_bytes(&oe2, buf, NDFS_ENTRY_SIZE, 0);
    TEST_ASSERT_EQUAL(0xFF, buf[56]);
    TEST_ASSERT_EQUAL(0xFF, buf[59]);
    return 0;
}

static int test_not_in_use(void)
{
    ndfs_object_entry_t oe;
    uint8_t buf[NDFS_ENTRY_SIZE];
    memset(buf, 0, NDFS_ENTRY_SIZE);
    buf[0] = 0x00; /* Not in use */

    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND,
                      ndfs_oe_from_bytes(buf, NDFS_ENTRY_SIZE, 0, &oe));
    return 0;
}

static int test_full_name(void)
{
    ndfs_object_entry_t oe;
    char buf[32];

    ndfs_oe_init(&oe);
    strcpy(oe.object_name, "MYFILE");
    strcpy(oe.type, "PROG");

    ndfs_oe_full_name(&oe, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("MYFILE:PROG", buf);
    return 0;
}

static int test_name_uppercase(void)
{
    ndfs_object_entry_t oe, oe2;
    uint8_t buf[NDFS_ENTRY_SIZE];

    ndfs_oe_init(&oe);
    strcpy(oe.object_name, "lowercase");
    strcpy(oe.type, "data");
    oe.file_pointer.block_id = 10;
    oe.file_pointer.type = NDFS_PTR_CONTIGUOUS;

    ndfs_oe_to_bytes(&oe, buf, NDFS_ENTRY_SIZE, 0);
    ndfs_oe_from_bytes(buf, NDFS_ENTRY_SIZE, 0, &oe2);

    TEST_ASSERT_EQUAL_STRING("LOWERCASE", oe2.object_name);
    TEST_ASSERT_EQUAL_STRING("DATA", oe2.type);
    return 0;
}

/* Build an entry with all extended fields set, serialize, parse it back, and
 * confirm every modelled field survives. Then re-serialize and confirm the
 * encoding is stable (idempotent), which is the real round-trip guarantee. */
static int test_extended_fields_roundtrip(void)
{
    uint8_t buf[NDFS_ENTRY_SIZE];
    uint8_t out[NDFS_ENTRY_SIZE];
    ndfs_object_entry_t oe, oe2;

    ndfs_oe_init(&oe);
    strcpy(oe.object_name, "LOAD");
    strcpy(oe.type, "MODE");
    oe.next_version = 20;
    oe.prev_version = 20;
    oe.access_bits = 0x00FF;
    oe.file_type_flags = NDFS_FT_INDEXED;
    oe.device_number = 0;
    oe.user_index = 0;
    oe.current_open_count = 0;
    oe.total_open_count = 145;
    oe.date_created = 0x01020304;
    oe.last_read_date = 0x05060708;
    oe.last_write_date = 0x090A0B0C;
    oe.pages_in_file = 5;
    oe.bytes_in_file = 4621;
    oe.file_pointer.block_id = 18419;
    oe.file_pointer.type = NDFS_PTR_INDEXED;

    TEST_ASSERT_OK(ndfs_oe_to_bytes(&oe, buf, NDFS_ENTRY_SIZE, 0));
    TEST_ASSERT_OK(ndfs_oe_from_bytes(buf, NDFS_ENTRY_SIZE, 0, &oe2));

    TEST_ASSERT_EQUAL_STRING("LOAD", oe2.object_name);
    TEST_ASSERT_EQUAL_STRING("MODE", oe2.type);
    TEST_ASSERT_EQUAL(20, oe2.next_version);
    TEST_ASSERT_EQUAL(20, oe2.prev_version);
    TEST_ASSERT_EQUAL(0x00FF, oe2.access_bits);
    TEST_ASSERT_EQUAL(NDFS_FT_INDEXED, oe2.file_type_flags);
    TEST_ASSERT_EQUAL(145, oe2.total_open_count);
    TEST_ASSERT_EQUAL(0x01020304, oe2.date_created);
    TEST_ASSERT_EQUAL(0x05060708, oe2.last_read_date);
    TEST_ASSERT_EQUAL(0x090A0B0C, oe2.last_write_date);
    TEST_ASSERT_EQUAL(5, oe2.pages_in_file);
    TEST_ASSERT_EQUAL(4621, oe2.bytes_in_file);
    TEST_ASSERT_EQUAL(18419, oe2.file_pointer.block_id);

    /* Encoding must be stable. */
    TEST_ASSERT_OK(ndfs_oe_to_bytes(&oe2, out, NDFS_ENTRY_SIZE, 0));
    TEST_ASSERT(memcmp(buf, out, NDFS_ENTRY_SIZE) == 0);
    return 0;
}

/* Editing access bits on a loaded entry must change only offset 26-27. */
static int test_access_edit_preserves_rest(void)
{
    uint8_t buf[NDFS_ENTRY_SIZE];
    uint8_t out[NDFS_ENTRY_SIZE];
    ndfs_object_entry_t oe;
    size_t i;

    for (i = 0; i < NDFS_ENTRY_SIZE; i++) buf[i] = (uint8_t)(i + 1);
    buf[0] = 0x80;
    buf[26] = 0x00; buf[27] = 0x07;

    TEST_ASSERT_OK(ndfs_oe_from_bytes(buf, NDFS_ENTRY_SIZE, 0, &oe));
    oe.access_bits = 0x03FF;
    TEST_ASSERT_OK(ndfs_oe_to_bytes(&oe, out, NDFS_ENTRY_SIZE, 0));

    TEST_ASSERT_EQUAL(0x03, out[26]);
    TEST_ASSERT_EQUAL(0xFF, out[27]);
    /* Bytes 33 and 35 are unmodelled; confirm they survived from raw. */
    TEST_ASSERT_EQUAL(buf[33], out[33]);
    TEST_ASSERT_EQUAL(buf[35], out[35]);
    return 0;
}

void run_object_entry_tests(void)
{
    TEST_SUITE_BEGIN("ObjectEntry Tests");
    RUN_TEST(test_init);
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_bytes_in_file_encoding);
    RUN_TEST(test_empty_file_stored_as_all_ones);
    RUN_TEST(test_not_in_use);
    RUN_TEST(test_full_name);
    RUN_TEST(test_name_uppercase);
    RUN_TEST(test_extended_fields_roundtrip);
    RUN_TEST(test_access_edit_preserves_rest);
}
