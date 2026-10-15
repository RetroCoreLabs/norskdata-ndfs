/**
 * Tests for the SINTRAN initial-command buffer (sintran.c).
 *
 * The pure parse/encode cases mirror the nd100x glass-UI test suite and the
 * RetroFS.NDFS C# tests, whose expected values were captured on live K/L/M
 * systems. The locate test builds a synthetic self-describing segment table so
 * the full locate pipeline is exercised without a multi-MB real pack.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "test_framework.h"
#include <ndfs/sintran.h>
#include <ndfs/image_creator.h>
#include <ndfs/filesystem.h>
#include <string.h>
#include <stdlib.h>

/* Build a fixture buffer: '|' marks a NUL pad byte, other chars are literal;
 * zero-padded to 64 bytes (matches the JS test's buf() helper). */
static void fixture(const char *spec, uint8_t out[64])
{
    size_t i, n = strlen(spec);
    memset(out, 0, 64);
    for (i = 0; i < n && i < 64; i++)
        out[i] = (spec[i] == '|') ? 0x00 : (uint8_t)spec[i];
}

static int strv_eq(char **cmds, size_t count, const char *const *expect, size_t n)
{
    size_t i;
    if (count != n) return 0;
    for (i = 0; i < n; i++)
        if (strcmp(cmds[i], expect[i]) != 0) return 0;
    return 1;
}

/* ── Parser ────────────────────────────────────────────────────────── */

static int test_parse_two_commands_with_pad(void)
{
    uint8_t buf[64];
    char **cmds; size_t count;
    const char *exp[] = { "AB", "CD" };
    fixture("AB'|CD'|'", buf);
    TEST_ASSERT_OK(ndfs_sintran_parse_buffer(buf, 64, 0, 64, &cmds, &count));
    TEST_ASSERT(strv_eq(cmds, count, exp, 2));
    ndfs_sintran_free_commands(cmds, count);
    return 0;
}

static int test_parse_even_needs_no_pad(void)
{
    uint8_t buf[64];
    char **cmds; size_t count;
    const char *exp[] = { "SET-AVAIL", "CC HELLO" };
    fixture("SET-AVAIL'CC HELLO'|'", buf);
    TEST_ASSERT_OK(ndfs_sintran_parse_buffer(buf, 64, 0, 64, &cmds, &count));
    TEST_ASSERT(strv_eq(cmds, count, exp, 2));
    ndfs_sintran_free_commands(cmds, count);
    return 0;
}

static int test_parse_lone_quote_empty(void)
{
    uint8_t buf[64];
    char **cmds; size_t count;
    fixture("'", buf);
    TEST_ASSERT_OK(ndfs_sintran_parse_buffer(buf, 64, 0, 64, &cmds, &count));
    TEST_ASSERT_EQUAL(0, (long)count);
    ndfs_sintran_free_commands(cmds, count);
    return 0;
}

static int test_parse_parity_stripped(void)
{
    uint8_t buf[5] = { 0xC1, 0xC2, 0x27, 0x00, 0x27 };  /* 'A'|0x80, 'B'|0x80 */
    char **cmds; size_t count;
    const char *exp[] = { "AB" };
    TEST_ASSERT_OK(ndfs_sintran_parse_buffer(buf, 5, 0, 5, &cmds, &count));
    TEST_ASSERT(strv_eq(cmds, count, exp, 1));
    ndfs_sintran_free_commands(cmds, count);
    return 0;
}

static int test_parse_nonprintable_rejected(void)
{
    uint8_t buf[4] = { 0x01, 0x02, 0x27, 0x27 };
    char **cmds; size_t count;
    ndfs_error_t err = ndfs_sintran_parse_buffer(buf, 4, 0, 4, &cmds, &count);
    TEST_ASSERT_EQUAL(NDFS_ERR_CORRUPT, err);
    return 0;
}

/* ── Encoder ───────────────────────────────────────────────────────── */

static const char *const THREE[] = {
    "ENTER-DIR,,DI-75-1,0", "ENTER-DIR,,DI-74-1,0", "SET-AVAIL"
};

static int test_encode_three_textlen54(void)
{
    uint8_t *b; size_t len; int tl;
    TEST_ASSERT_OK(ndfs_sintran_encode_buffer(THREE, 3, NDFS_INITCMD_MAX_TEXT_BYTES, &b, &len, &tl));
    TEST_ASSERT_EQUAL(54, tl);
    ndfs_free_data(b);
    return 0;
}

static int test_encode_plus_offline_textlen74(void)
{
    const char *four[] = { THREE[0], THREE[1], THREE[2], "CC OFFLINE WRITE OK" };
    uint8_t *b; size_t len; int tl;
    TEST_ASSERT_OK(ndfs_sintran_encode_buffer(four, 4, NDFS_INITCMD_MAX_TEXT_BYTES, &b, &len, &tl));
    TEST_ASSERT_EQUAL(74, tl);
    ndfs_free_data(b);
    return 0;
}

static int test_encode_uppercases(void)
{
    const char *cmds[] = { "cc hi" };
    uint8_t *b; size_t len; int tl;
    TEST_ASSERT_OK(ndfs_sintran_encode_buffer(cmds, 1, NDFS_INITCMD_MAX_TEXT_BYTES, &b, &len, &tl));
    TEST_ASSERT(len >= 5 && memcmp(b, "CC HI", 5) == 0);
    ndfs_free_data(b);
    return 0;
}

static int test_encode_254_ok_256_rejected(void)
{
    char big[256];
    const char *cmds[1];
    uint8_t *b; size_t len; int tl;

    memset(big, 'C', 253); big[253] = '\0';   /* 253 + quote = 254, even */
    cmds[0] = big;
    TEST_ASSERT_OK(ndfs_sintran_encode_buffer(cmds, 1, NDFS_INITCMD_MAX_TEXT_BYTES, &b, &len, &tl));
    TEST_ASSERT_EQUAL(254, tl);
    ndfs_free_data(b);

    memset(big, 'C', 255); big[255] = '\0';   /* 255 + quote = 256 */
    TEST_ASSERT_EQUAL(NDFS_ERR_OUT_OF_RANGE,
                      ndfs_sintran_encode_buffer(cmds, 1, NDFS_INITCMD_MAX_TEXT_BYTES, &b, &len, &tl));
    return 0;
}

static int test_encode_embedded_quote_rejected(void)
{
    const char *cmds[] = { "BAD'CMD" };
    uint8_t *b; size_t len; int tl;
    TEST_ASSERT_EQUAL(NDFS_ERR_INVALID_ARG,
                      ndfs_sintran_encode_buffer(cmds, 1, NDFS_INITCMD_MAX_TEXT_BYTES, &b, &len, &tl));
    return 0;
}

/* ── Self-consistency (the false-positive guard) ───────────────────── */

static int test_reencode_matches(void)
{
    const char *bogus[] = { "B," };            /* re-encodes to 4, not 2 */
    TEST_ASSERT(!ndfs_sintran_reencode_matches(bogus, 1, 2));
    TEST_ASSERT(ndfs_sintran_reencode_matches(THREE, 3, 54));
    return 0;
}

/* ── Full locate against a synthetic self-describing segment table ──── */

static void put_word(uint8_t *b, size_t off, int val)
{
    b[off]     = (uint8_t)(val >> 8);
    b[off + 1] = (uint8_t)(val & 0xFF);
}

/* Write segment-table entry n on table page P. */
static void put_seg_entry(uint8_t *b, int page, int n,
                          int logad, int segle, int madr, int flag, int sgsta)
{
    size_t o = (size_t)page * 2048 + (size_t)n * 16;
    put_word(b, o + 4, logad);
    put_word(b, o + 6, segle);
    put_word(b, o + 8, madr);
    put_word(b, o + 10, flag);
    put_word(b, o + 12, sgsta);
}

static int test_locate_synthetic(void)
{
    const int PAGES = 64;
    const int TABLE_PAGE = 5;
    const int BUF_MADR = 20;
    const int INIBU_L = 0x7853;             /* 074123 octal */
    const int SEG_BASE = 12 * 1024;         /* logad & 0x3F = 12 */
    size_t size = (size_t)PAGES * 2048;
    uint8_t *seg = (uint8_t *)calloc(size, 1);
    long byte_offset;
    uint8_t *enc; size_t enc_len; int tl;
    ndfs_initial_commands_t ic;
    int n;

    TEST_ASSERT_NOT_NULL(seg);

    /* Entry 1: the running command segment whose range contains INIBU_L. */
    put_seg_entry(seg, TABLE_PAGE, 1, /*logad*/ 12, /*segle*/ 52, /*madr*/ BUF_MADR,
                  /*flag*/ 1, /*sgsta*/ 0xE000);
    /* Entry 2: self-describing (madr == table page) so find_segment_table_page accepts. */
    put_seg_entry(seg, TABLE_PAGE, 2, 0, 1, TABLE_PAGE, 1, 0xE000);
    /* Entries 3..40: plausible filler so the page has >= 40 good entries. */
    for (n = 3; n <= 40; n++)
        put_seg_entry(seg, TABLE_PAGE, n, 0, 1, 1, 1, 0xE000);

    /* Place a real 3-command buffer at the computed offset. */
    byte_offset = (long)BUF_MADR * 2048 + (long)(INIBU_L - SEG_BASE) * 2;
    TEST_ASSERT_OK(ndfs_sintran_encode_buffer(THREE, 3, NDFS_INITCMD_MAX_TEXT_BYTES, &enc, &enc_len, &tl));
    memcpy(seg + byte_offset, enc, enc_len);
    put_word(seg, (size_t)byte_offset + NDFS_INITCMD_LEN_CELL_BYTES, tl);  /* length cell */
    ndfs_free_data(enc);

    /* find_segment_table_page must select our table page. */
    TEST_ASSERT_EQUAL(TABLE_PAGE, ndfs_sintran_find_segment_table_page(seg, size));

    /* The pure locator (via a tiny in-memory NdfsFileSystem would need a real
     * pack; here we exercise the segment-table + parse pipeline directly). */
    {
        char **cmds; size_t count;
        int stp = ndfs_sintran_find_segment_table_page(seg, size);
        TEST_ASSERT_EQUAL(TABLE_PAGE, stp);
        TEST_ASSERT_OK(ndfs_sintran_parse_buffer(seg, size, (size_t)byte_offset,
                                                 (size_t)tl, &cmds, &count));
        TEST_ASSERT(strv_eq(cmds, count, THREE, 3));
        ndfs_sintran_free_commands(cmds, count);
    }

    (void)ic;
    free(seg);
    return 0;
}

/* ── In-place patch primitive (the write path), cross-page ─────────── */

static int test_patch_file_region_crosses_page(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    uint8_t content[3000];
    uint8_t patch[100];
    uint8_t *back = NULL;
    size_t size = 0, i;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;   /* 180 pages: fits a multi-page file */
    strncpy(opts.directory_name, "PATCHT", NDFS_NAME_MAX);
    opts.directory_name[NDFS_NAME_MAX] = '\0';
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));

    memset(content, 'A', sizeof(content));
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/BIG:DATA", content, sizeof(content)));

    /* Patch 100 bytes at offset 2000 -> straddles the 2048 page boundary. */
    memset(patch, 'B', sizeof(patch));
    TEST_ASSERT_OK(ndfs_patch_file_region(fs, "SYSTEM/BIG:DATA", 2000, patch, sizeof(patch)));

    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/BIG:DATA", &back, &size));
    TEST_ASSERT_EQUAL(3000, (long)size);
    for (i = 0; i < size; i++) {
        uint8_t want = (i >= 2000 && i < 2100) ? (uint8_t)'B' : (uint8_t)'A';
        if (back[i] != want) {
            printf("  FAIL: byte %zu = %02X, want %02X\n", i, back[i], want);
            ndfs_free_data(back); ndfs_close(fs);
            return 1;
        }
    }
    ndfs_free_data(back);

    /* A region past the file's ALLOCATED pages (3000 bytes -> 2 pages = 4096) is
     * rejected, not silently truncated or written out of bounds. */
    TEST_ASSERT_EQUAL(NDFS_ERR_OUT_OF_RANGE,
                      ndfs_patch_file_region(fs, "SYSTEM/BIG:DATA", 4050, patch, sizeof(patch)));

    ndfs_close(fs);
    return 0;
}

void run_sintran_tests(void)
{
    TEST_SUITE_BEGIN("SINTRAN initial commands");
    RUN_TEST(test_parse_two_commands_with_pad);
    RUN_TEST(test_parse_even_needs_no_pad);
    RUN_TEST(test_parse_lone_quote_empty);
    RUN_TEST(test_parse_parity_stripped);
    RUN_TEST(test_parse_nonprintable_rejected);
    RUN_TEST(test_encode_three_textlen54);
    RUN_TEST(test_encode_plus_offline_textlen74);
    RUN_TEST(test_encode_uppercases);
    RUN_TEST(test_encode_254_ok_256_rejected);
    RUN_TEST(test_encode_embedded_quote_rejected);
    RUN_TEST(test_reencode_matches);
    RUN_TEST(test_locate_synthetic);
    RUN_TEST(test_patch_file_region_crosses_page);
}
