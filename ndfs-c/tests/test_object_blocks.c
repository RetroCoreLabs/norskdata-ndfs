/**
 * Object blocks: more than 256 files in one user area, and the API that grants them.
 *
 * A SINTRAN user area holds 256 files per OBJECT BLOCK, one by default and up to
 * 16 (4096 files) on version K. Every implementation in this repository assumed
 * 256 was a hard limit until 2026-08-01; these tests cover the structure that
 * assumption hid, and the @GIVE-OBJECT-BLOCKS equivalent that raises the ceiling.
 *
 * Ground truth was measured on a live SINTRAN III K pack - see
 * docs/NDFS-OBJECT-BLOCKS-SPEC.md.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "test_framework.h"
#include <ndfs/filesystem.h>
#include <ndfs/image_creator.h>
#include <ndfs/object_entry.h>
#include <ndfs/user_entry.h>
#include <stdio.h>
#include <string.h>

/* ---- Helpers ---- */

/* A pack big enough to hold well past 256 one-page files. */
static ndfs_filesystem_t *create_pack(const char *name, uint32_t pages)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_CUSTOM;
    opts.custom_pages = pages;
    strncpy(opts.directory_name, name, NDFS_NAME_MAX);
    opts.directory_name[NDFS_NAME_MAX] = '\0';

    if (ndfs_create_image(&fs, &opts) != NDFS_OK) return NULL;
    return fs;
}

/* Write one tiny file called Fnnnn:DATA into SYSTEM. */
static ndfs_error_t put_file(ndfs_filesystem_t *fs, int n)
{
    char path[64];
    uint8_t byte = 0x41;
    snprintf(path, sizeof(path), "SYSTEM/F%04d:DATA", n);
    return ndfs_write_file(fs, path, &byte, 1);
}

/* ---- Byte 47: MXOBL / ACOBL ---- */

static int test_default_user_has_one_block(void)
{
    /* A fresh user gets one object block, which is what SINTRAN does. */
    ndfs_user_entry_t ue;
    ndfs_ue_init(&ue);
    TEST_ASSERT_EQUAL(1, ue.max_object_blocks);
    TEST_ASSERT_EQUAL(1, ue.allocated_object_blocks);
    return 0;
}

static int test_byte47_roundtrip(void)
{
    /* 0x31 is the value read off user BIGMAN on the live pack: MXOBL 4 (matching
     * its reported MAXIMUM NUMBER OF FILES of 1024) and ACOBL 2 (matching the
     * 482 files it held). A default user stores 0x00, which is why the byte
     * looked unused on every pack examined before this work. */
    static const struct { uint8_t raw; int max; int alloc; } cases[] = {
        { 0x00,  1,  1 },   /* default user: 256 files            */
        { 0x31,  4,  2 },   /* BIGMAN on the verified pack        */
        { 0xFF, 16, 16 },   /* the SINTRAN K ceiling: 4096 files  */
        { 0x10,  2,  1 },   /* granted a second block, unused yet */
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ndfs_user_entry_t ue;
        uint8_t buf[NDFS_ENTRY_SIZE];
        uint8_t out[NDFS_ENTRY_SIZE];

        memset(buf, 0, sizeof(buf));
        buf[0] = NDFS_USER_ENTRY_FLAG;
        buf[47] = cases[i].raw;

        TEST_ASSERT_OK(ndfs_ue_from_bytes(buf, NDFS_ENTRY_SIZE, 0, &ue));
        TEST_ASSERT_EQUAL(cases[i].max, ue.max_object_blocks);
        TEST_ASSERT_EQUAL(cases[i].alloc, ue.allocated_object_blocks);

        ndfs_ue_to_bytes(&ue, out);
        TEST_ASSERT_EQUAL(cases[i].raw, out[47]);
    }
    return 0;
}

/* ---- The file number is not the physical position ---- */

static int test_file_number_vector(void)
{
    /* F0500 on the live pack is owned by user 8, sits at physical position
     * 18483, and SINTRAN reports it as FILE 307. The old flat model gave 51,
     * owned by user 72 - a user that does not exist. */
    TEST_ASSERT_EQUAL(307, ndfs_oe_compute_file_number(18483, 8));

    /* Inside a user's FIRST block the number IS the offset, which is why the
     * old model looked correct for years. */
    TEST_ASSERT_EQUAL(0, ndfs_oe_compute_file_number(8 * 32, 1));

    /* A position outside any of that user's blocks must report -1 rather than a
     * plausible wrong answer. */
    TEST_ASSERT(ndfs_oe_compute_file_number(8 * 32, 2) < 0);
    return 0;
}

/* ---- Granting: the @GIVE-OBJECT-BLOCKS equivalent ---- */

static int test_give_adds_to_the_maximum(void)
{
    /* Matches the live measurement: BIGMAN reported MAXIMUM NUMBER OF FILES 256,
     * and @GIVE-OBJECT-BLOCKS BIGMAN,3 took it to 1024. The command ADDS. */
    ndfs_filesystem_t *fs = create_pack("GIVE", 4000);
    ndfs_object_block_info_t info;
    uint8_t new_max = 0;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_give_object_blocks(fs, "SYSTEM", 3, &new_max));
    TEST_ASSERT_EQUAL(4, new_max);

    TEST_ASSERT_OK(ndfs_get_object_block_info(fs, "SYSTEM", &info));
    TEST_ASSERT_EQUAL(4, info.max_object_blocks);
    TEST_ASSERT_EQUAL(1024, info.max_files);
    /* Granting must not pre-allocate: blocks come on demand, as SINTRAN does. */
    TEST_ASSERT_EQUAL(1, info.allocated_object_blocks);
    TEST_ASSERT_EQUAL(12, info.grantable_blocks);

    ndfs_close(fs);
    return 0;
}

static int test_give_refuses_past_sixteen(void)
{
    ndfs_filesystem_t *fs = create_pack("CEIL", 4000);
    ndfs_object_block_info_t info;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_give_object_blocks(fs, "SYSTEM", 15, NULL));

    /* 16 blocks = 4096 files is the version-K ceiling. */
    TEST_ASSERT_EQUAL(NDFS_ERR_OUT_OF_RANGE,
                      ndfs_give_object_blocks(fs, "SYSTEM", 1, NULL));

    TEST_ASSERT_OK(ndfs_get_object_block_info(fs, "SYSTEM", &info));
    TEST_ASSERT_EQUAL(NDFS_MAX_OBJECT_BLOCKS, info.max_object_blocks);
    TEST_ASSERT_EQUAL(0, info.grantable_blocks);

    ndfs_close(fs);
    return 0;
}

static int test_give_rejects_an_overshoot_rather_than_clamping(void)
{
    /* Clamping would leave the caller believing it received what it asked for,
     * and the shortfall would surface later as a premature "user is full". */
    ndfs_filesystem_t *fs = create_pack("OVER", 4000);
    ndfs_object_block_info_t info;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_EQUAL(NDFS_ERR_OUT_OF_RANGE,
                      ndfs_give_object_blocks(fs, "SYSTEM", 20, NULL));

    TEST_ASSERT_OK(ndfs_get_object_block_info(fs, "SYSTEM", &info));
    TEST_ASSERT_EQUAL(1, info.max_object_blocks);   /* unchanged */

    ndfs_close(fs);
    return 0;
}

static int test_give_validates_its_arguments(void)
{
    ndfs_filesystem_t *fs = create_pack("ARGS", 4000);

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_EQUAL(NDFS_ERR_INVALID_ARG,
                      ndfs_give_object_blocks(fs, "SYSTEM", 0, NULL));
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND,
                      ndfs_give_object_blocks(fs, "NOSUCHUSER", 1, NULL));

    ndfs_close(fs);
    return 0;
}

static int test_take_refuses_to_orphan_files(void)
{
    /* The maximum may never fall below the blocks actually allocated: a file's
     * number is derived from the block it sits in, so removing one would orphan
     * every file in it. */
    ndfs_filesystem_t *fs = create_pack("TAKE", 6000);
    ndfs_object_block_info_t info;
    uint8_t new_max = 0;
    int i;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_give_object_blocks(fs, "SYSTEM", 3, NULL));

    /* Force a second block into use. */
    for (i = 0; i < NDFS_FILES_PER_OBJECT_BLOCK + 1; i++) {
        TEST_ASSERT_OK(put_file(fs, i));
    }
    TEST_ASSERT_OK(ndfs_get_object_block_info(fs, "SYSTEM", &info));
    TEST_ASSERT_EQUAL(2, info.allocated_object_blocks);

    /* Dropping the maximum to 1 would strand the files in block 1. */
    TEST_ASSERT_EQUAL(NDFS_ERR_OUT_OF_RANGE,
                      ndfs_take_object_blocks(fs, "SYSTEM", 3, NULL));

    /* Down to the allocated count is fine. */
    TEST_ASSERT_OK(ndfs_take_object_blocks(fs, "SYSTEM", 2, &new_max));
    TEST_ASSERT_EQUAL(2, new_max);

    ndfs_close(fs);
    return 0;
}

/* ---- Creating past 256 files ---- */

static int test_creating_file_257_allocates_a_second_block(void)
{
    ndfs_filesystem_t *fs = create_pack("GROW", 6000);
    ndfs_object_block_info_t info;
    int i;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_give_object_blocks(fs, "SYSTEM", 3, NULL));

    for (i = 0; i < NDFS_FILES_PER_OBJECT_BLOCK; i++) {
        TEST_ASSERT_OK(put_file(fs, i));
    }
    TEST_ASSERT_OK(ndfs_get_object_block_info(fs, "SYSTEM", &info));
    TEST_ASSERT_EQUAL(1, info.allocated_object_blocks);

    /* This is the write that used to fail with "object table is full". */
    TEST_ASSERT_OK(put_file(fs, NDFS_FILES_PER_OBJECT_BLOCK));

    TEST_ASSERT_OK(ndfs_get_object_block_info(fs, "SYSTEM", &info));
    TEST_ASSERT_EQUAL(2, info.allocated_object_blocks);
    TEST_ASSERT_EQUAL(NDFS_FILES_PER_OBJECT_BLOCK + 1, info.files_in_use);

    ndfs_close(fs);
    return 0;
}

static int test_soft_limit_is_distinguishable_from_the_hard_one(void)
{
    /* With one block the limit is 256 - and now the error says which kind of
     * limit that is, so a copy loop can tell the operator what to do. */
    ndfs_filesystem_t *fs = create_pack("LIMIT", 6000);
    int i;

    TEST_ASSERT_NOT_NULL(fs);

    for (i = 0; i < NDFS_FILES_PER_OBJECT_BLOCK; i++) {
        TEST_ASSERT_OK(put_file(fs, i));
    }

    /* MXOBL is 1, well below 16, so this is grantable - not the hard ceiling. */
    TEST_ASSERT_EQUAL(NDFS_ERR_OBJ_BLOCKS_FULL,
                      put_file(fs, NDFS_FILES_PER_OBJECT_BLOCK));

    /* Granting a block makes the very same write succeed. */
    TEST_ASSERT_OK(ndfs_give_object_blocks(fs, "SYSTEM", 1, NULL));
    TEST_ASSERT_OK(put_file(fs, NDFS_FILES_PER_OBJECT_BLOCK));

    ndfs_close(fs);
    return 0;
}

static int test_file_numbers_are_contiguous_across_the_boundary(void)
{
    /* 260 files must be numbered 0..259 with no duplicates and no gaps, even
     * though the physical positions jump a whole index block at 256. */
    ndfs_filesystem_t *fs = create_pack("NUMS", 6000);
    ndfs_object_entry_t *objs = NULL;
    size_t count = 0, i;
    const int total = NDFS_FILES_PER_OBJECT_BLOCK + 4;
    unsigned char seen[NDFS_FILES_PER_OBJECT_BLOCK + 4];
    int owned = 0;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_give_object_blocks(fs, "SYSTEM", 3, NULL));

    for (i = 0; i < (size_t)total; i++) {
        TEST_ASSERT_OK(put_file(fs, (int)i));
    }

    memset(seen, 0, sizeof(seen));
    TEST_ASSERT_OK(ndfs_get_object_entries(fs, &objs, &count));
    TEST_ASSERT_NOT_NULL(objs);

    for (i = 0; i < count; i++) {
        if (objs[i].user_index != 0) continue;       /* SYSTEM is index 0 */
        owned++;
        TEST_ASSERT((int32_t)objs[i].file_number >= 0);
        TEST_ASSERT((int)objs[i].file_number < total);
        TEST_ASSERT_EQUAL(0, seen[objs[i].file_number]);   /* no duplicates */
        seen[objs[i].file_number] = 1;
    }
    ndfs_free_object_entries(objs);

    TEST_ASSERT_EQUAL(total, owned);
    for (i = 0; i < (size_t)total; i++) {
        TEST_ASSERT_EQUAL(1, seen[i]);                     /* no gaps */
    }

    ndfs_close(fs);
    return 0;
}

void run_object_block_tests(void)
{
    TEST_SUITE_BEGIN("Object Block Tests");
    RUN_TEST(test_default_user_has_one_block);
    RUN_TEST(test_byte47_roundtrip);
    RUN_TEST(test_file_number_vector);
    RUN_TEST(test_give_adds_to_the_maximum);
    RUN_TEST(test_give_refuses_past_sixteen);
    RUN_TEST(test_give_rejects_an_overshoot_rather_than_clamping);
    RUN_TEST(test_give_validates_its_arguments);
    RUN_TEST(test_take_refuses_to_orphan_files);
    RUN_TEST(test_creating_file_257_allocates_a_second_block);
    RUN_TEST(test_soft_limit_is_distinguishable_from_the_hard_one);
    RUN_TEST(test_file_numbers_are_contiguous_across_the_boundary);
}
