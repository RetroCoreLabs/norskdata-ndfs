/**
 * Tests for SINTRAN's page allocation rules (docs/NDFS-ALLOCATION-RULES.md).
 *
 * The expected numbers come from real SINTRAN M runs in the nd100x emulator
 * (bit file address, first single page of 12 disk formats, contiguous stacks, quota).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "test_framework.h"
#include <ndfs/ndfs.h>
#include <string.h>
#include <stdlib.h>

/* A bit file for a disk with `cap` pages, bit file at `ba`, page 0 and the bit file pages marked. */
static int make_bf(ndfs_bit_file_t *bf, uint32_t cap, uint32_t ba)
{
    uint32_t n = (cap + 16383u) / 16384u, i;

    memset(bf, 0, sizeof(*bf));
    if (ndfs_bf_init(bf, cap) != NDFS_OK) return 1;
    bf->alloc_ceiling = cap;
    bf->bit_address = ba;
    ndfs_bf_mark_used(bf, 0);
    for (i = 0; i < n; i++) ndfs_bf_mark_used(bf, ba + i);
    return 0;
}

static uint32_t take_hint(ndfs_bit_file_t *bf)
{
    uint32_t p = 0;
    if (ndfs_bf_find_free_hint(bf, &p) != NDFS_OK) return 0;
    ndfs_bf_mark_used(bf, p);
    return p;
}

static uint32_t take_block(ndfs_bit_file_t *bf)
{
    uint32_t p = 0;
    if (ndfs_bf_find_free_block(bf, &p) != NDFS_OK) return 0;
    ndfs_bf_mark_used(bf, p);
    return p;
}

/* RSBPG rule: SMD 75 MB. The four pages measured with real SINTRAN: user data, user index,
 * object data, object index. */
static int test_hint_rule_smd75(void)
{
    ndfs_bit_file_t bf;

    TEST_ASSERT_EQUAL(0, make_bf(&bf, 36945, 18468));
    TEST_ASSERT_EQUAL(18479, take_hint(&bf));
    TEST_ASSERT_EQUAL(18478, take_hint(&bf));
    TEST_ASSERT_EQUAL(18477, take_hint(&bf));
    TEST_ASSERT_EQUAL(18476, take_hint(&bf));
    ndfs_bf_destroy(&bf);
    return 0;
}

/* DISC-288MB: BA 70191 ends in binary 1111, so the bit file itself holds the top of the home word
 * and the four pages sit directly below it. */
static int test_hint_rule_bit_file_in_home_word(void)
{
    ndfs_bit_file_t bf;

    TEST_ASSERT_EQUAL(0, make_bf(&bf, 140391, 70191));
    TEST_ASSERT_EQUAL(70190, take_hint(&bf));
    TEST_ASSERT_EQUAL(70189, take_hint(&bf));
    TEST_ASSERT_EQUAL(70188, take_hint(&bf));
    TEST_ASSERT_EQUAL(70187, take_hint(&bf));
    ndfs_bf_destroy(&bf);
    return 0;
}

/* Small Winchester: DISC-14MB, BA 3456, hint 3455 is already the top of its word. */
static int test_hint_rule_winchester14(void)
{
    ndfs_bit_file_t bf;

    TEST_ASSERT_EQUAL(0, make_bf(&bf, 6912, 3456));
    TEST_ASSERT_EQUAL(3455, take_hint(&bf));
    TEST_ASSERT_EQUAL(3454, take_hint(&bf));
    TEST_ASSERT_EQUAL(3453, take_hint(&bf));
    TEST_ASSERT_EQUAL(3452, take_hint(&bf));
    ndfs_bf_destroy(&bf);
    return 0;
}

/* The code spirals to the neighbouring words when the home word is full (read in the SINTRAN
 * code, not measured): word+1 lowest bit first, then word-1 highest bit first. */
static int test_hint_rule_spiral(void)
{
    ndfs_bit_file_t bf;
    uint32_t p;

    TEST_ASSERT_EQUAL(0, make_bf(&bf, 36945, 18468));
    for (p = 18464; p <= 18479; p++) ndfs_bf_mark_used(&bf, p);   /* the whole home word */
    TEST_ASSERT_EQUAL(18480, take_hint(&bf));                     /* word+1, lowest bit */
    TEST_ASSERT_EQUAL(18481, take_hint(&bf));
    for (p = 18482; p <= 18495; p++) ndfs_bf_mark_used(&bf, p);   /* word+1 full */
    TEST_ASSERT_EQUAL(18463, take_hint(&bf));                     /* word-1, highest bit */
    ndfs_bf_destroy(&bf);
    return 0;
}

/* Pages at or above the directory size and below block 7 are never given. */
static int test_hint_rule_respects_limits(void)
{
    ndfs_bit_file_t bf;
    uint32_t p = 0, i;

    TEST_ASSERT_EQUAL(0, make_bf(&bf, 100, 50));
    for (i = 7; i < 100; i++) ndfs_bf_mark_used(&bf, i);
    TEST_ASSERT_EQUAL(NDFS_ERR_NO_SPACE, ndfs_bf_find_free_hint(&bf, &p));
    ndfs_bf_mark_free(&bf, 99);
    TEST_ASSERT_OK(ndfs_bf_find_free_hint(&bf, &p));
    TEST_ASSERT_EQUAL(99, p);
    ndfs_bf_destroy(&bf);
    return 0;
}

/* RSPAG cold start: first single page of 12 real disk formats (measured with SINTRAN M). */
static int test_block_rule_cold_start_12_formats(void)
{
    static const struct { uint32_t cap, ba, first; } rows[] = {
        { 18486,   9243,  16384 },  /* DISC-38MB */
        { 34765,  17379,  32768 },  /* DISC-70MB */
        { 36945,  18468,  32768 },  /* DISC-75MB */
        { 69530,  34758,  65536 },  /* DISC-140MB-F */
        { 140391, 70191, 131072 },  /* DISC-288MB */
        { 220584, 110286, 212992 }, /* DISC-450MB */
        { 22032,  11016,  16384 },  /* DISC-45MB */
        { 36396,  18198,  32768 },  /* DISC-74MB */
        { 6912,   3456,   3328 },   /* DISC-14MB */
        { 10728,  5364,   5120 },   /* DISC-21MB */
        { 11016,  5508,   5376 },   /* DISC-23MB */
        { 13648,  6822,   6656 }    /* DISC-28MB */
    };
    size_t k;

    for (k = 0; k < sizeof(rows) / sizeof(rows[0]); k++) {
        ndfs_bit_file_t bf;
        TEST_ASSERT_EQUAL(0, make_bf(&bf, rows[k].cap, rows[k].ba));
        TEST_ASSERT_EQUAL(rows[k].first, take_block(&bf));
        TEST_ASSERT_EQUAL(rows[k].first + 1, take_block(&bf)); /* the next call stays in the block */
        ndfs_bf_destroy(&bf);
    }
    return 0;
}

/* A full block sends the search to the next block up, then the one below, and so on. */
static int test_block_rule_spiral(void)
{
    ndfs_bit_file_t bf;
    uint32_t p;

    TEST_ASSERT_EQUAL(0, make_bf(&bf, 36945, 18468));
    for (p = 32768; p < 33024; p++) ndfs_bf_mark_used(&bf, p);   /* cold start block full */
    TEST_ASSERT_EQUAL(33024, take_block(&bf));                   /* block + 1 */
    for (p = 33025; p < 33280; p++) ndfs_bf_mark_used(&bf, p);   /* block + 1 full */
    TEST_ASSERT_EQUAL(33280, take_block(&bf));                   /* the cursor moved to block + 1; its next block up */
    bf.cursor_block = 128;                                       /* back to the first block: up is full, so down */
    TEST_ASSERT_EQUAL(32512, take_block(&bf));
    ndfs_bf_destroy(&bf);
    return 0;
}

/* The cursor follows the last allocation: pages come from the block of the last page. */
static int test_block_rule_cursor_follows_last_page(void)
{
    ndfs_bit_file_t bf;
    uint32_t p;

    TEST_ASSERT_EQUAL(0, make_bf(&bf, 36945, 18468));
    TEST_ASSERT_EQUAL(32768, take_block(&bf));
    bf.cursor_block = 72;   /* block of 18432..18687, the block of BA + 1 */
    TEST_ASSERT_EQUAL(18432, take_block(&bf));
    for (p = 18433; p < 18468; p++) ndfs_bf_mark_used(&bf, p);
    TEST_ASSERT_EQUAL(18471, take_block(&bf));                   /* lowest free page of the block */
    ndfs_bf_destroy(&bf);
    return 0;
}

/* Without a known bit file address the old top-down search is used. */
static int test_rules_fall_back_without_bit_address(void)
{
    ndfs_bit_file_t bf;
    uint32_t a = 0, b = 0, c = 0;

    TEST_ASSERT_EQUAL(0, make_bf(&bf, 1000, 500));
    bf.bit_address = 0;
    TEST_ASSERT_OK(ndfs_bf_find_free(&bf, &a));
    TEST_ASSERT_OK(ndfs_bf_find_free_hint(&bf, &b));
    TEST_ASSERT_OK(ndfs_bf_find_free_block(&bf, &c));
    TEST_ASSERT_EQUAL(a, b);
    TEST_ASSERT_EQUAL(a, c);
    ndfs_bf_destroy(&bf);
    return 0;
}

/* ---- File system level -------------------------------------------------------------------- */

static ndfs_filesystem_t *new_smd75(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_SMD_75MB;
    opts.include_extended_info = true;
    strcpy(opts.directory_name, "ALLOC");
    if (ndfs_create_image(&fs, &opts) != NDFS_OK) return NULL;
    return fs;
}

static uint32_t used_of(ndfs_filesystem_t *fs, const char *name)
{
    ndfs_user_entry_t *users = NULL;
    size_t n = 0, i;
    uint32_t r = 0xFFFFFFFFu;

    if (ndfs_get_users(fs, &users, &n) != NDFS_OK) return r;
    for (i = 0; i < n; i++) if (strcmp(users[i].user_name, name) == 0) r = users[i].pages_used;
    ndfs_free_users(users);
    return r;
}

/* Contiguous files stack downward from the directory size (36945): measured C1 36935, C2 36915. */
static int test_contiguous_stack_from_top(void)
{
    ndfs_filesystem_t *fs = new_smd75();
    ndfs_object_entry_t e;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_create_contiguous_file(fs, "SYSTEM/C1:DATA", 10));
    TEST_ASSERT_OK(ndfs_create_contiguous_file(fs, "SYSTEM/C2:DATA", 20));
    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "C1:DATA", "SYSTEM", &e));
    TEST_ASSERT_EQUAL(36935, e.file_pointer.block_id);
    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "C2:DATA", "SYSTEM", &e));
    TEST_ASSERT_EQUAL(36915, e.file_pointer.block_id);
    ndfs_close(fs);
    return 0;
}

/* A contiguous file of one page uses the data page rule: 32768 on a cold SMD 75 MB. */
static int test_single_page_file_uses_block_rule(void)
{
    ndfs_filesystem_t *fs = new_smd75();
    ndfs_object_entry_t e;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_create_contiguous_file(fs, "SYSTEM/N1:DATA", 1));
    TEST_ASSERT_OK(ndfs_create_contiguous_file(fs, "SYSTEM/N2:DATA", 1));
    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "N1:DATA", "SYSTEM", &e));
    TEST_ASSERT_EQUAL(32768, e.file_pointer.block_id);
    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "N2:DATA", "SYSTEM", &e));
    TEST_ASSERT_EQUAL(32769, e.file_pointer.block_id);
    ndfs_close(fs);
    return 0;
}

/* Indexed file: the index block comes from the hint rule, the data pages from the data page rule
 * (measured in the cold run: index 18475, data right after the single page files). */
static int test_indexed_file_placement(void)
{
    ndfs_filesystem_t *fs = new_smd75();
    ndfs_object_entry_t e;
    uint32_t *blocks = NULL;
    size_t count = 0;
    uint8_t data[3 * NDFS_PAGE_SIZE];

    TEST_ASSERT_NOT_NULL(fs);
    memset(data, 0x5A, sizeof(data));
    TEST_ASSERT_OK(ndfs_create_contiguous_file(fs, "SYSTEM/N1:DATA", 1));
    TEST_ASSERT_OK(ndfs_create_contiguous_file(fs, "SYSTEM/N2:DATA", 1));
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/N3:DATA", data, sizeof(data)));
    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "N3:DATA", "SYSTEM", &e));
    TEST_ASSERT_EQUAL(18475, e.file_pointer.block_id);              /* index block, hint rule */
    TEST_ASSERT_OK(ndfs_get_file_blocks(fs, "SYSTEM/N3:DATA", &blocks, &count));
    TEST_ASSERT_EQUAL(3, count);
    TEST_ASSERT_EQUAL(32770, blocks[0]);
    TEST_ASSERT_EQUAL(32771, blocks[1]);
    TEST_ASSERT_EQUAL(32772, blocks[2]);
    free(blocks);
    ndfs_close(fs);
    return 0;
}

/* Quota: data pages + index blocks + the owner's object data page, as SINTRAN charges it. */
static int test_quota_charges_index_and_object_pages(void)
{
    ndfs_filesystem_t *fs = new_smd75();
    uint8_t data[3 * NDFS_PAGE_SIZE];
    uint8_t one[NDFS_PAGE_SIZE];

    TEST_ASSERT_NOT_NULL(fs);
    memset(data, 0x11, sizeof(data));
    memset(one, 0x22, sizeof(one));

    TEST_ASSERT_EQUAL(1, used_of(fs, "SYSTEM"));                    /* its object data page */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/Q1:DATA", data, sizeof(data)));
    TEST_ASSERT_EQUAL(1 + 3 + 1, used_of(fs, "SYSTEM"));            /* + 3 data + 1 index */

    TEST_ASSERT_OK(ndfs_add_user(fs, "ALPHA", 50));
    TEST_ASSERT_EQUAL(0, used_of(fs, "ALPHA"));
    TEST_ASSERT_OK(ndfs_write_file(fs, "ALPHA/A1:DATA", one, sizeof(one)));
    TEST_ASSERT_EQUAL(1 + 1 + 1, used_of(fs, "ALPHA"));             /* data + index + object page */

    TEST_ASSERT_OK(ndfs_delete_file(fs, "ALPHA/A1:DATA"));
    TEST_ASSERT_EQUAL(1, used_of(fs, "ALPHA"));                     /* the object page stays charged */
    TEST_ASSERT_OK(ndfs_delete_file(fs, "SYSTEM/Q1:DATA"));
    TEST_ASSERT_EQUAL(1, used_of(fs, "SYSTEM"));
    ndfs_close(fs);
    return 0;
}

/* A large file: 600 pages need a top block and two sub-index blocks, 3 pages, all charged. */
static int test_quota_large_file_sub_index(void)
{
    ndfs_filesystem_t *fs = new_smd75();
    size_t size = 600u * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)malloc(size);
    char *report = NULL;
    int fsck_errors = -1;
    size_t i;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    for (i = 0; i < size; i++) data[i] = (uint8_t)(i * 7 + 1);
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/BIGF:DATA", data, size));
    TEST_ASSERT_EQUAL(1 + 600 + 3, used_of(fs, "SYSTEM"));
    TEST_ASSERT_OK(ndfs_fsck(fs, &report, &fsck_errors));
    TEST_ASSERT_EQUAL(0, fsck_errors);
    TEST_ASSERT(strstr(report, "WARNING: User") == NULL);
    free(report);
    free(data);
    ndfs_close(fs);
    return 0;
}

void run_alloc_rules_tests(void)
{
    TEST_SUITE_BEGIN("SINTRAN Allocation Rules Tests");
    RUN_TEST(test_hint_rule_smd75);
    RUN_TEST(test_hint_rule_bit_file_in_home_word);
    RUN_TEST(test_hint_rule_winchester14);
    RUN_TEST(test_hint_rule_spiral);
    RUN_TEST(test_hint_rule_respects_limits);
    RUN_TEST(test_block_rule_cold_start_12_formats);
    RUN_TEST(test_block_rule_spiral);
    RUN_TEST(test_block_rule_cursor_follows_last_page);
    RUN_TEST(test_rules_fall_back_without_bit_address);
    RUN_TEST(test_contiguous_stack_from_top);
    RUN_TEST(test_single_page_file_uses_block_rule);
    RUN_TEST(test_indexed_file_placement);
    RUN_TEST(test_quota_charges_index_and_object_pages);
    RUN_TEST(test_quota_large_file_sub_index);
}
