/**
 * Reading a floppy whose pages will not read.
 *
 * A real ND floppy comes off the drive with individual pages unreadable. The
 * file listing is what an archive is kept for, so a lost page must cost only
 * what was written ON that page:
 *
 *   - an object-file data page  -> its own 32 entries, nothing else
 *   - an object-file index page -> the entries the pages under it name
 *   - a user-file data page     -> the names of the users on it
 *   - a bit-file page           -> the allocation state it covered
 *
 * A page that cannot be read is modelled here the way it happens on the media:
 * the pointer names a page outside the image, so read_page() fails. Every case
 * has the same name in tests/damaged-media.test.ts, tests/test_damaged_media.py
 * and tests/test_damaged_media.c, and asserts the same numbers.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 */

#include "test_framework.h"
#include <ndfs/ndfs.h>
#include <string.h>
#include <stdlib.h>

/* A page number no image in these tests is long enough to hold. */
#define UNREADABLE 9000u

#define OBJECT_PTR (NDFS_MASTER_BLOCK_OFFSET + 0x10)
#define USER_PTR   (NDFS_MASTER_BLOCK_OFFSET + 0x14)
#define BIT_PTR    (NDFS_MASTER_BLOCK_OFFSET + 0x18)

/* 40 files: 32 in the first object-file data page, 8 in the second. */
#define FILE_COUNT 40

/**
 * A 360KB floppy carrying FILE_COUNT one-byte files, as a raw image the caller
 * owns and may damage. Returns NULL on any failure.
 */
static uint8_t *make_floppy(size_t *out_size)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    uint8_t *image = NULL;
    uint8_t byte;
    char path[64];
    int i;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    strcpy(opts.directory_name, "DAMAGED");

    if (ndfs_create_image(&fs, &opts) != NDFS_OK) return NULL;

    for (i = 0; i < FILE_COUNT; i++) {
        sprintf(path, "SYSTEM/FILE-%d:DATA", i);
        byte = (uint8_t)i;
        if (ndfs_write_file(fs, path, &byte, 1) != NDFS_OK) {
            ndfs_close(fs);
            return NULL;
        }
    }

    if (ndfs_to_buffer(fs, &image, out_size) != NDFS_OK) image = NULL;
    ndfs_close(fs);
    return image;
}

static void point_away(uint8_t *image, size_t offset, ndfs_pointer_type_t type)
{
    ndfs_block_pointer_t bp;
    bp.block_id = UNREADABLE;
    bp.type = type;
    ndfs_bp_to_bytes(&bp, image, offset);
}

/* Break one pointer slot of the object file's index page. */
static void break_object_data_page(uint8_t *image, uint32_t slot)
{
    ndfs_block_pointer_t idx = ndfs_bp_from_bytes(image, OBJECT_PTR);
    point_away(image, (size_t)idx.block_id * NDFS_PAGE_SIZE + slot * 4, NDFS_PTR_CONTIGUOUS);
}

/* Break the single pointer slot of the user file's index page. */
static void break_user_data_page(uint8_t *image, uint32_t slot)
{
    ndfs_block_pointer_t idx = ndfs_bp_from_bytes(image, USER_PTR);
    point_away(image, (size_t)idx.block_id * NDFS_PAGE_SIZE + slot * 4, NDFS_PTR_CONTIGUOUS);
}

static size_t count_objects(ndfs_filesystem_t *fs)
{
    ndfs_object_entry_t *entries = NULL;
    size_t count = 0;
    if (ndfs_get_object_entries(fs, &entries, &count) != NDFS_OK) return (size_t)-1;
    ndfs_free_object_entries(entries);
    return count;
}

/* ── an intact floppy is the baseline ────────────────────────────── */

static int test_intact_lists_every_file_and_reports_no_damage(void)
{
    uint8_t *image;
    size_t size = 0;
    ndfs_filesystem_t *fs = NULL;
    ndfs_damage_report_t dmg;
    ndfs_user_entry_t *users = NULL;
    size_t user_count = 0;

    image = make_floppy(&size);
    TEST_ASSERT_NOT_NULL(image);
    TEST_ASSERT_OK(ndfs_open_buffer_copy(image, size, true, &fs));

    TEST_ASSERT_EQUAL_UINT(FILE_COUNT, count_objects(fs));
    TEST_ASSERT_OK(ndfs_get_damage_report(fs, &dmg));
    TEST_ASSERT_EQUAL_UINT(0, dmg.object_pages);
    TEST_ASSERT_EQUAL_UINT(0, dmg.user_pages);
    TEST_ASSERT_EQUAL_UINT(0, dmg.bit_file_pages);

    TEST_ASSERT_OK(ndfs_get_users(fs, &users, &user_count));
    TEST_ASSERT_EQUAL_UINT(1, user_count);
    TEST_ASSERT_EQUAL_STRING("SYSTEM", users[0].user_name);
    ndfs_free_users(users);

    ndfs_close(fs);
    free(image);
    return 0;
}

/* ── an object-file data page that will not read ─────────────────── */

static int test_object_data_page_costs_its_own_32_entries(void)
{
    uint8_t *image;
    size_t size = 0;
    ndfs_filesystem_t *fs = NULL;
    ndfs_damage_report_t dmg;

    image = make_floppy(&size);
    TEST_ASSERT_NOT_NULL(image);
    break_object_data_page(image, 0);
    TEST_ASSERT_OK(ndfs_open_buffer_copy(image, size, true, &fs));

    TEST_ASSERT_EQUAL_UINT(FILE_COUNT - 32, count_objects(fs));
    TEST_ASSERT_OK(ndfs_get_damage_report(fs, &dmg));
    TEST_ASSERT_EQUAL_UINT(1, dmg.object_pages);

    ndfs_close(fs);
    free(image);
    return 0;
}

static int test_object_data_page_leaves_surviving_object_index(void)
{
    uint8_t *image;
    size_t size = 0;
    ndfs_filesystem_t *fs = NULL;
    ndfs_object_entry_t *before = NULL, *after = NULL;
    size_t before_count = 0, after_count = 0, i, j;

    image = make_floppy(&size);
    TEST_ASSERT_NOT_NULL(image);
    TEST_ASSERT_OK(ndfs_open_buffer_copy(image, size, true, &fs));
    TEST_ASSERT_OK(ndfs_get_object_entries(fs, &before, &before_count));
    ndfs_close(fs);

    break_object_data_page(image, 0);
    TEST_ASSERT_OK(ndfs_open_buffer_copy(image, size, true, &fs));
    TEST_ASSERT_OK(ndfs_get_object_entries(fs, &after, &after_count));

    for (i = 0; i < after_count; i++) {
        int found = 0;
        for (j = 0; j < before_count; j++) {
            if (strcmp(after[i].object_name, before[j].object_name) == 0) {
                TEST_ASSERT_EQUAL_UINT(before[j].object_index, after[i].object_index);
                found = 1;
                break;
            }
        }
        TEST_ASSERT(found);
    }

    ndfs_free_object_entries(before);
    ndfs_free_object_entries(after);
    ndfs_close(fs);
    free(image);
    return 0;
}

static int test_two_lost_object_pages_are_counted_separately(void)
{
    uint8_t *image;
    size_t size = 0;
    ndfs_filesystem_t *fs = NULL;
    ndfs_damage_report_t dmg;

    image = make_floppy(&size);
    TEST_ASSERT_NOT_NULL(image);
    break_object_data_page(image, 0);
    break_object_data_page(image, 1);
    TEST_ASSERT_OK(ndfs_open_buffer_copy(image, size, true, &fs));

    TEST_ASSERT_EQUAL_UINT(0, count_objects(fs));
    TEST_ASSERT_OK(ndfs_get_damage_report(fs, &dmg));
    TEST_ASSERT_EQUAL_UINT(2, dmg.object_pages);

    ndfs_close(fs);
    free(image);
    return 0;
}

/* ── the object-file index page itself ───────────────────────────── */

static int test_object_index_page_cannot_be_worked_around(void)
{
    uint8_t *image;
    size_t size = 0;
    ndfs_filesystem_t *fs = NULL;

    image = make_floppy(&size);
    TEST_ASSERT_NOT_NULL(image);
    point_away(image, OBJECT_PTR, NDFS_PTR_INDEXED);

    TEST_ASSERT(ndfs_open_buffer_copy(image, size, true, &fs) != NDFS_OK);

    free(image);
    return 0;
}

/* ── a user-file data page that will not read ────────────────────── */

static int test_user_data_page_costs_the_names_and_not_one_file(void)
{
    uint8_t *image;
    size_t size = 0;
    ndfs_filesystem_t *fs = NULL;
    ndfs_damage_report_t dmg;
    ndfs_user_entry_t *users = NULL;
    size_t user_count = 0;

    image = make_floppy(&size);
    TEST_ASSERT_NOT_NULL(image);
    break_user_data_page(image, 0);
    TEST_ASSERT_OK(ndfs_open_buffer_copy(image, size, true, &fs));

    TEST_ASSERT_EQUAL_UINT(FILE_COUNT, count_objects(fs));
    TEST_ASSERT_OK(ndfs_get_users(fs, &users, &user_count));
    TEST_ASSERT_EQUAL_UINT(0, user_count);
    ndfs_free_users(users);

    TEST_ASSERT_OK(ndfs_get_damage_report(fs, &dmg));
    TEST_ASSERT_EQUAL_UINT(1, dmg.user_pages);
    TEST_ASSERT_EQUAL_UINT(0, dmg.object_pages);

    ndfs_close(fs);
    free(image);
    return 0;
}

/* ── a bit-file page that will not read ──────────────────────────── */

static int test_bit_file_page_costs_the_allocation_state_only(void)
{
    uint8_t *image;
    size_t size = 0;
    ndfs_filesystem_t *fs = NULL;
    ndfs_damage_report_t dmg;

    image = make_floppy(&size);
    TEST_ASSERT_NOT_NULL(image);
    point_away(image, BIT_PTR, NDFS_PTR_CONTIGUOUS);
    TEST_ASSERT_OK(ndfs_open_buffer_copy(image, size, true, &fs));

    TEST_ASSERT_EQUAL_UINT(FILE_COUNT, count_objects(fs));
    TEST_ASSERT_OK(ndfs_get_damage_report(fs, &dmg));
    TEST_ASSERT_EQUAL_UINT(1, dmg.bit_file_pages);
    TEST_ASSERT_EQUAL_UINT(0, dmg.object_pages);

    ndfs_close(fs);
    free(image);
    return 0;
}

/* ── all three at once ───────────────────────────────────────────── */

static int test_all_three_report_each_count_and_list_what_survived(void)
{
    uint8_t *image;
    size_t size = 0;
    ndfs_filesystem_t *fs = NULL;
    ndfs_damage_report_t dmg;
    char name[NDFS_NAME_MAX + 1];

    image = make_floppy(&size);
    TEST_ASSERT_NOT_NULL(image);
    break_object_data_page(image, 0);
    break_user_data_page(image, 0);
    point_away(image, BIT_PTR, NDFS_PTR_CONTIGUOUS);
    TEST_ASSERT_OK(ndfs_open_buffer_copy(image, size, true, &fs));

    TEST_ASSERT_EQUAL_UINT(FILE_COUNT - 32, count_objects(fs));
    TEST_ASSERT_OK(ndfs_get_damage_report(fs, &dmg));
    TEST_ASSERT_EQUAL_UINT(1, dmg.object_pages);
    TEST_ASSERT_EQUAL_UINT(1, dmg.user_pages);
    TEST_ASSERT_EQUAL_UINT(1, dmg.bit_file_pages);

    TEST_ASSERT_OK(ndfs_get_directory_name(fs, name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("DAMAGED", name);

    ndfs_close(fs);
    free(image);
    return 0;
}

void run_damaged_media_tests(void)
{
    TEST_SUITE_BEGIN("Damaged media");
    RUN_TEST(test_intact_lists_every_file_and_reports_no_damage);
    RUN_TEST(test_object_data_page_costs_its_own_32_entries);
    RUN_TEST(test_object_data_page_leaves_surviving_object_index);
    RUN_TEST(test_two_lost_object_pages_are_counted_separately);
    RUN_TEST(test_object_index_page_cannot_be_worked_around);
    RUN_TEST(test_user_data_page_costs_the_names_and_not_one_file);
    RUN_TEST(test_bit_file_page_costs_the_allocation_state_only);
    RUN_TEST(test_all_three_report_each_count_and_list_what_survived);
}
