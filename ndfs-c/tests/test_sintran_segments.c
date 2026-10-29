/**
 * Tests for the SINTRAN segment lister / exporter / importer (sintran_segments.c).
 *
 * A synthetic pack is built with the image creator: a contiguous SEGFIL0:DATA with a
 * self-describing segment table, a DPIT image copy whose kernel tables DBLST / ABLPAGE
 * / MASSNO point at the real disk pages of SEGFIL0 and of a second contiguous segment
 * file SEGFIL1:DATA, an RTFIL:DATA with the name records, and a symbol list with
 * BLST / ABLPA / MASSN / INIBU. Shapes follow the K and L packs measured on 2026-10-29.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "test_framework.h"
#include <ndfs/sintran_segments.h>
#include <ndfs/sintran_identity.h>
#include <ndfs/sintran.h>
#include <ndfs/image_creator.h>
#include <ndfs/filesystem.h>
#include <ndfs/parity.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define PAGE 2048
#define TABLE_PAGE 5
#define SEGFIL0_PAGES 64
#define SEGFIL1_PAGES 16

/* The L07 symbol values. */
#define BLST_SYM  04023
#define ABLPA_SYM 04461
#define MASSN_SYM 04441

#define SEG_IDPIT  044
#define SEG_SDPIT  053
#define SEG_FS     006      /* a code segment on segment file 0 */
#define SEG_USER   0101     /* a user segment on segment file 1 */
#define SEG_MEMORY 023      /* S3DPIT, memory only */
#define SEG_SAVE4  054      /* a save segment on segment file 4 (absolute) */
#define SEG_IMAGE  002      /* S3IMAGE: file 0, MADR 0, real pages */

static void put_word(uint8_t *b, size_t off, int val)
{
    b[off]     = (uint8_t)(val >> 8);
    b[off + 1] = (uint8_t)(val & 0xFF);
}

static void put_seg_entry(uint8_t *b, int page, int n,
                          int logad, int segle, int madr, int flag, int sgsta)
{
    size_t o = (size_t)page * PAGE + (size_t)n * 16;
    put_word(b, o + 4, logad);
    put_word(b, o + 6, segle);
    put_word(b, o + 8, madr);
    put_word(b, o + 10, flag);
    put_word(b, o + 12, sgsta);
}

static void put_name_record(uint8_t *b, size_t off, const char *name, int segno)
{
    uint64_t n = 0;
    size_t i;
    for (i = 0; name[i]; i++) n = (n << 6) | ((uint64_t)name[i] & 0x3F);
    put_word(b, off + 0, (int)((n >> 32) & 0xFFFF));
    put_word(b, off + 2, (int)((n >> 16) & 0xFFFF));
    put_word(b, off + 4, (int)(n & 0xFFFF));
    put_word(b, off + 6, segno);
    put_word(b, off + 8, 0);
    put_word(b, off + 10, 025);
    put_word(b, off + 12, segno);
    put_word(b, off + 14, 0);
}

static ndfs_error_t write_text_file(ndfs_filesystem_t *fs, const char *path, const char *text)
{
    size_t len = strlen(text);
    uint8_t *buf = (uint8_t *)malloc(len);
    ndfs_error_t err;
    memcpy(buf, text, len);
    ndfs_set_parity(buf, len);
    err = ndfs_write_file(fs, path, buf, len);
    free(buf);
    return err;
}

static bool contiguous(const uint32_t *blocks, size_t n)
{
    size_t i;
    for (i = 1; i < n; i++) if (blocks[i] != blocks[i - 1] + 1) return false;
    return true;
}

/* Build the pack. Fills *segfil0_page and *segfil1_page with where the two files landed. */
static ndfs_error_t build_pack(ndfs_filesystem_t **out_fs, bool with_symbols,
                               uint32_t *segfil0_page, uint32_t *segfil1_page)
{
    ndfs_image_options_t opts;
    ndfs_filesystem_t *fs = NULL;
    ndfs_error_t err;
    uint8_t *seg = (uint8_t *)calloc(SEGFIL0_PAGES * PAGE, 1);
    uint8_t *seg1 = (uint8_t *)calloc(SEGFIL1_PAGES * PAGE, 1);
    uint8_t rtfil[PAGE];
    uint32_t *blocks; size_t nb;
    int n, p;
    const int IDPIT_MADR = 20, SDPIT_MADR = 40, FS_MADR = 10, USER_MADR = 2, SAVE4_MADR = 30;
    const int DPIT_LOGAD = 0000102;     /* base page 2 = 4000B */

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_SMD_75MB;
    strcpy(opts.directory_name, "PACK-ONE");
    opts.include_extended_info = true;
    opts.system_number = 100;
    opts.flag_word = 0x8000;
    err = ndfs_create_image(&fs, &opts);
    if (err != NDFS_OK) { free(seg); free(seg1); return err; }

    /* Both segment files contiguous, as SINTRAN requires (@ALLOCATE-FILE). */
    err = ndfs_create_contiguous_file(fs, "SYSTEM/SEGFIL0:DATA", SEGFIL0_PAGES);
    if (err == NDFS_OK) err = ndfs_create_contiguous_file(fs, "SYSTEM/SEGFIL1:DATA", SEGFIL1_PAGES);
    if (err != NDFS_OK) { free(seg); free(seg1); ndfs_close(fs); return err; }
    err = ndfs_get_file_blocks(fs, "SYSTEM/SEGFIL0:DATA", &blocks, &nb);
    if (err != NDFS_OK) { free(seg); free(seg1); ndfs_close(fs); return err; }
    if (!contiguous(blocks, nb)) { free(blocks); free(seg); free(seg1); ndfs_close(fs); return NDFS_ERR_CORRUPT; }
    *segfil0_page = blocks[0];
    free(blocks);
    err = ndfs_get_file_blocks(fs, "SYSTEM/SEGFIL1:DATA", &blocks, &nb);
    if (err != NDFS_OK) { free(seg); free(seg1); ndfs_close(fs); return err; }
    *segfil1_page = blocks[0];
    free(blocks);

    /* Segment table: self-describing entry 2, filler, then the real entries. */
    /* Entry 027 (S3ISGT on the real packs) describes the table page itself, as the
     * self-validating scan needs; entry 2 is S3IMAGE. */
    put_seg_entry(seg, TABLE_PAGE, 027, 0, 1, TABLE_PAGE, 1, 0xE000);
    for (n = 3; n <= 60; n++)
        if (n != SEG_IDPIT && n != SEG_SDPIT && n != SEG_FS && n != SEG_MEMORY && n != SEG_SAVE4 && n != SEG_IMAGE && n != 027)
            put_seg_entry(seg, TABLE_PAGE, n, 0, 1, 1, 1, 0xE000);
    put_seg_entry(seg, TABLE_PAGE, SEG_FS,     0000026, 4, FS_MADR,    0000111, 0162000);
    put_seg_entry(seg, TABLE_PAGE, SEG_MEMORY, DPIT_LOGAD, 0, 0,       0000111, 0162000);
    put_seg_entry(seg, TABLE_PAGE, SEG_IMAGE,  0000000, 2, 0,          0000111, 0162000);
    put_seg_entry(seg, TABLE_PAGE, SEG_IDPIT,  DPIT_LOGAD, 4, IDPIT_MADR, 0000111, 0162000);
    put_seg_entry(seg, TABLE_PAGE, SEG_SDPIT,  DPIT_LOGAD, 4, SDPIT_MADR, 0000111, 0162000);
    put_seg_entry(seg, TABLE_PAGE, SEG_SAVE4,  0000030, 2, SAVE4_MADR, 0100111, 0162000);
    /* A segment the RT-loader wrote onto segment file 1: FLAG 020000, the 5OK bit clear
     * (it means "loaded in memory", not "on disk"), as measured on 2026-10-29. */
    put_seg_entry(seg, TABLE_PAGE, SEG_USER,   0000060, 3, USER_MADR,  0020000, 0162000);

    /* Recognisable content: every page of a segment carries its number and page index. */
    for (p = 0; p < 4; p++) { put_word(seg, (size_t)(FS_MADR + p) * PAGE, 0x5300 | p); }
    for (p = 0; p < 2; p++) { put_word(seg, (size_t)p * PAGE, 0x0200 | p); }        /* S3IMAGE at MADR 0 */
    for (p = 0; p < 4; p++) { put_word(seg, (size_t)(IDPIT_MADR + p) * PAGE, 0x4400 | p); }
    for (p = 0; p < 4; p++) { put_word(seg, (size_t)(SDPIT_MADR + p) * PAGE, 0x5300 | p); }
    for (p = 0; p < 3; p++) { put_word(seg1, (size_t)(USER_MADR + p) * PAGE, 0xA100 | p); }

    /* The kernel tables in the DPIT image copy (base 4000B): DBLST in blocks, 2 per page. */
    {
        long base = (long)IDPIT_MADR * PAGE;
        uint32_t dblst0 = *segfil0_page * 2, dblst1 = *segfil1_page * 2;
        put_word(seg, (size_t)(base + (BLST_SYM + 0 - 04000) * 2), (int)(dblst0 >> 16));
        put_word(seg, (size_t)(base + (BLST_SYM + 1 - 04000) * 2), (int)(dblst0 & 0xFFFF));
        put_word(seg, (size_t)(base + (BLST_SYM + 2 - 04000) * 2), (int)(dblst1 >> 16));
        put_word(seg, (size_t)(base + (BLST_SYM + 3 - 04000) * 2), (int)(dblst1 & 0xFFFF));
        /* segment file 2: defined but on another disk unit */
        put_word(seg, (size_t)(base + (BLST_SYM + 5 - 04000) * 2), 2000);
        put_word(seg, (size_t)(base + (ABLPA_SYM + 0 - 04000) * 2), 2);
        put_word(seg, (size_t)(base + (ABLPA_SYM + 1 - 04000) * 2), 2);
        put_word(seg, (size_t)(base + (MASSN_SYM + 0 - 04000) * 2), 01100);
        put_word(seg, (size_t)(base + (MASSN_SYM + 1 - 04000) * 2), 01100);
        put_word(seg, (size_t)(base + (MASSN_SYM + 2 - 04000) * 2), 01500);
        put_word(seg, (size_t)(base + (04051 - 04000) * 2), 100);         /* SYSNO, for show */
    }

    err = ndfs_write_file(fs, "SYSTEM/SEGFIL0:DATA", seg, SEGFIL0_PAGES * PAGE);
    if (err == NDFS_OK) err = ndfs_write_file(fs, "SYSTEM/SEGFIL1:DATA", seg1, SEGFIL1_PAGES * PAGE);
    free(seg); free(seg1);
    if (err != NDFS_OK) { ndfs_close(fs); return err; }

    /* Segment file 4 is absolute: disk page 1 + MADR. Page 31 and 32 of a fresh 75 MB pack
     * are free, so writing them raw harms nothing. */
    {
        uint8_t page[PAGE];
        memset(page, 0, sizeof(page));
        put_word(page, 0, 0x5400);
        err = ndfs_write_disk_page(fs, 1 + SAVE4_MADR, page);
        if (err == NDFS_OK) { put_word(page, 0, 0x5401); err = ndfs_write_disk_page(fs, 2 + SAVE4_MADR, page); }
        if (err != NDFS_OK) { ndfs_close(fs); return err; }
    }

    memset(rtfil, 0, sizeof(rtfil));
    put_name_record(rtfil, 0 * 16, "S3IMAGE", SEG_IMAGE);
    put_name_record(rtfil, 1 * 16, "S3FS", SEG_FS);
    put_name_record(rtfil, 2 * 16, "S3DPIT", SEG_MEMORY);
    put_name_record(rtfil, 3 * 16, "S3IDPIT", SEG_IDPIT);
    put_name_record(rtfil, 4 * 16, "S3SDPIT", SEG_SDPIT);
    put_name_record(rtfil, 5 * 16, "S3SPRMA", SEG_SAVE4);
    put_name_record(rtfil, 6 * 16, "USERSEG", SEG_USER);
    err = ndfs_write_file(fs, "SYSTEM/RTFIL:DATA", rtfil, sizeof(rtfil));
    if (err != NDFS_OK) { ndfs_close(fs); return err; }

    if (with_symbols) {
        err = write_text_file(fs, "SYSTEM/SYMBOL-1-LIST:SYMB",
            "BLST=004023\r\nDBLST=004023\r\nMASSN=004441\r\nMASSU=004451\r\nABLPA=004461\r\n"
            "SYSNO=004051\r\nINIBU=074123\r\n");
        if (err != NDFS_OK) { ndfs_close(fs); return err; }
    }
    *out_fs = fs;
    return NDFS_OK;
}

/* ── Names ───────────────────────────────────────────────────────────── */

static int test_collect_names(void)
{
    uint8_t buf[128];
    char names[256][NDFS_SEGNAME_MAX + 1];
    memset(buf, 0, sizeof(buf));
    memset(names, 0, sizeof(names));
    put_name_record(buf, 0, "S3FS", 006);
    put_name_record(buf, 16, "S3SDPIT", 053);
    put_name_record(buf, 32, "S3SDPIT", 054);          /* a second record for a name: first wins */
    TEST_ASSERT_EQUAL(3, ndfs_sintran_collect_segment_names(buf, sizeof(buf), names));
    TEST_ASSERT_EQUAL_STRING("S3FS", names[006]);
    TEST_ASSERT_EQUAL_STRING("S3SDPIT", names[053]);
    TEST_ASSERT_EQUAL_STRING("S3SDPIT", names[054]);
    TEST_ASSERT_EQUAL_STRING("", names[1]);
    return 0;
}

/* ── List ────────────────────────────────────────────────────────────── */

static int test_list_segments(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_segment_list_t *l = (ndfs_segment_list_t *)malloc(sizeof(*l));
    uint32_t sf0, sf1;
    int i;
    TEST_ASSERT_OK(build_pack(&fs, true, &sf0, &sf1));
    TEST_ASSERT_OK(ndfs_list_segments(fs, l));
    TEST_ASSERT_EQUAL('L', l->version);
    TEST_ASSERT_EQUAL(TABLE_PAGE, l->segment_table_page);
    TEST_ASSERT(l->have_symbols);
    TEST_ASSERT_EQUAL_STRING("SYSTEM/RTFIL:DATA", l->names_path);

    /* Segment files from the kernel tables. */
    TEST_ASSERT(l->segfiles[0].defined);
    TEST_ASSERT_EQUAL((long)sf0, l->segfiles[0].base_page);
    TEST_ASSERT_EQUAL_STRING("SYSTEM/SEGFIL0:DATA", l->segfiles[0].file_path);
    TEST_ASSERT(l->segfiles[1].defined);
    TEST_ASSERT_EQUAL((long)sf1, l->segfiles[1].base_page);
    TEST_ASSERT_EQUAL_STRING("SYSTEM/SEGFIL1:DATA", l->segfiles[1].file_path);
    TEST_ASSERT(l->segfiles[2].defined);
    TEST_ASSERT(!l->segfiles[2].on_this_disk);
    TEST_ASSERT(!l->segfiles[3].defined);
    TEST_ASSERT(l->segfiles[4].defined);
    TEST_ASSERT_EQUAL(1L, l->segfiles[4].base_page);

    /* The segments. */
    i = ndfs_find_segment(l, "S3FS");
    TEST_ASSERT(i >= 0);
    TEST_ASSERT_EQUAL(SEG_FS, l->segments[i].number);
    TEST_ASSERT_EQUAL((long)sf0 + 10, l->segments[i].disk_page);
    TEST_ASSERT_EQUAL(4, l->segments[i].segle);
    TEST_ASSERT_EQUAL(0, l->segments[i].segfile);
    i = ndfs_find_segment(l, "userseg");                 /* case-insensitive */
    TEST_ASSERT(i >= 0);
    TEST_ASSERT_EQUAL(1, l->segments[i].segfile);
    TEST_ASSERT_EQUAL((long)sf1 + 2, l->segments[i].disk_page);
    i = ndfs_find_segment(l, "S3DPIT");
    TEST_ASSERT(i >= 0);
    TEST_ASSERT(l->segments[i].memory_only);
    TEST_ASSERT_EQUAL(-1L, l->segments[i].disk_page);
    i = ndfs_find_segment(l, "S3IMAGE");                 /* MADR 0 is the first page of file 0 */
    TEST_ASSERT(i >= 0);
    TEST_ASSERT(!l->segments[i].memory_only);
    TEST_ASSERT_EQUAL((long)sf0, l->segments[i].disk_page);
    i = ndfs_find_segment(l, "S3SPRMA");
    TEST_ASSERT(i >= 0);
    TEST_ASSERT_EQUAL(4, l->segments[i].segfile);
    TEST_ASSERT_EQUAL(31L, l->segments[i].disk_page);
    /* By number: octal with B, decimal without. */
    TEST_ASSERT_EQUAL(ndfs_find_segment(l, "S3SDPIT"), ndfs_find_segment(l, "53B"));
    TEST_ASSERT_EQUAL(ndfs_find_segment(l, "S3SDPIT"), ndfs_find_segment(l, "43"));
    TEST_ASSERT_EQUAL(-1, ndfs_find_segment(l, "NOSUCH"));
    TEST_ASSERT(ndfs_find_segment(l, "60B") >= 0);         /* filler entry 060B: listed, unnamed */
    free(l);
    ndfs_close(fs);
    return 0;
}

static int test_list_without_symbols_falls_back_to_segfil0_file(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_segment_list_t *l = (ndfs_segment_list_t *)malloc(sizeof(*l));
    uint32_t sf0, sf1;
    int i;
    TEST_ASSERT_OK(build_pack(&fs, false, &sf0, &sf1));
    TEST_ASSERT_OK(ndfs_list_segments(fs, l));
    TEST_ASSERT(!l->have_symbols);
    TEST_ASSERT_EQUAL('?', l->version);
    TEST_ASSERT(l->segfiles[0].defined);
    TEST_ASSERT_EQUAL((long)sf0, l->segfiles[0].base_page);
    TEST_ASSERT(!l->segfiles[1].defined);
    TEST_ASSERT(!l->segfiles[4].defined);
    i = ndfs_find_segment(l, "S3FS");
    TEST_ASSERT_EQUAL((long)sf0 + 10, l->segments[i].disk_page);
    i = ndfs_find_segment(l, "USERSEG");
    TEST_ASSERT_EQUAL(-1L, l->segments[i].disk_page);    /* segment file 1 unknown */
    free(l);
    ndfs_close(fs);
    return 0;
}

/* ── Export / import ─────────────────────────────────────────────────── */

static int test_export_matches_file_bytes(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_segment_list_t *l = (ndfs_segment_list_t *)malloc(sizeof(*l));
    uint32_t sf0, sf1;
    uint8_t *data, *file; size_t size, fsize;
    int i;
    TEST_ASSERT_OK(build_pack(&fs, true, &sf0, &sf1));
    TEST_ASSERT_OK(ndfs_list_segments(fs, l));

    /* S3FS: 4 pages at SEGFIL0 page 10 - must equal the file's bytes. */
    i = ndfs_find_segment(l, "S3FS");
    TEST_ASSERT_OK(ndfs_export_segment(fs, l, i, &data, &size));
    TEST_ASSERT_EQUAL(4 * PAGE, (long)size);
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/SEGFIL0:DATA", &file, &fsize));
    TEST_ASSERT(memcmp(data, file + 10 * PAGE, size) == 0);
    TEST_ASSERT_EQUAL(0x5302, (data[2 * PAGE] << 8) | data[2 * PAGE + 1]);
    ndfs_free_data(data); ndfs_free_data(file);

    /* USERSEG on segment file 1. */
    i = ndfs_find_segment(l, "USERSEG");
    TEST_ASSERT_OK(ndfs_export_segment(fs, l, i, &data, &size));
    TEST_ASSERT_EQUAL(3 * PAGE, (long)size);
    TEST_ASSERT_EQUAL(0xA101, (data[PAGE] << 8) | data[PAGE + 1]);
    ndfs_free_data(data);

    /* The absolute save segment on file 4. */
    i = ndfs_find_segment(l, "S3SPRMA");
    TEST_ASSERT_OK(ndfs_export_segment(fs, l, i, &data, &size));
    TEST_ASSERT_EQUAL(0x5400, (data[0] << 8) | data[1]);
    TEST_ASSERT_EQUAL(0x5401, (data[PAGE] << 8) | data[PAGE + 1]);
    ndfs_free_data(data);

    /* S3IMAGE at MADR 0: the first pages of SEGFIL0. */
    i = ndfs_find_segment(l, "S3IMAGE");
    TEST_ASSERT_OK(ndfs_export_segment(fs, l, i, &data, &size));
    TEST_ASSERT_EQUAL(0x0201, (data[PAGE] << 8) | data[PAGE + 1]);
    ndfs_free_data(data);

    /* Memory-only: refused. */
    i = ndfs_find_segment(l, "S3DPIT");
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_export_segment(fs, l, i, &data, &size));
    free(l);
    ndfs_close(fs);
    return 0;
}

static int test_import_round_trip_and_refusals(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_segment_list_t *l = (ndfs_segment_list_t *)malloc(sizeof(*l));
    uint32_t sf0, sf1;
    uint8_t *data, *back; size_t size, bsize;
    uint8_t big[5 * PAGE];
    int i;
    TEST_ASSERT_OK(build_pack(&fs, true, &sf0, &sf1));
    TEST_ASSERT_OK(ndfs_list_segments(fs, l));
    i = ndfs_find_segment(l, "S3FS");

    /* Modify the export and write it back; the file view must show it. */
    TEST_ASSERT_OK(ndfs_export_segment(fs, l, i, &data, &size));
    data[100] = 0xDE; data[101] = 0xAD; data[3 * PAGE + 7] = 0x77;
    TEST_ASSERT_OK(ndfs_import_segment(fs, l, i, data, size));
    TEST_ASSERT_OK(ndfs_export_segment(fs, l, i, &back, &bsize));
    TEST_ASSERT(memcmp(data, back, size) == 0);
    ndfs_free_data(back);
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/SEGFIL0:DATA", &back, &bsize));
    TEST_ASSERT_EQUAL(0xDE, back[10 * PAGE + 100]);
    TEST_ASSERT_EQUAL(0x77, back[13 * PAGE + 7]);
    ndfs_free_data(back);

    /* A shorter import keeps the tail of the last page. */
    data[0] = 0x11; data[1] = 0x22;
    TEST_ASSERT_OK(ndfs_import_segment(fs, l, i, data, 10));
    TEST_ASSERT_OK(ndfs_export_segment(fs, l, i, &back, &bsize));
    TEST_ASSERT_EQUAL(0x11, back[0]);
    TEST_ASSERT_EQUAL(0xDE, back[100]);                 /* untouched tail of page 0 */
    ndfs_free_data(back);
    ndfs_free_data(data);

    /* Too long: refused, nothing written. */
    memset(big, 0xEE, sizeof(big));
    TEST_ASSERT_EQUAL(NDFS_ERR_OUT_OF_RANGE, ndfs_import_segment(fs, l, i, big, sizeof(big)));
    TEST_ASSERT_OK(ndfs_export_segment(fs, l, i, &back, &bsize));
    TEST_ASSERT_EQUAL(0x11, back[0]);
    ndfs_free_data(back);

    /* Memory-only and other-disk segments: refused. */
    i = ndfs_find_segment(l, "S3DPIT");
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_import_segment(fs, l, i, big, 10));
    free(l);
    ndfs_close(fs);
    return 0;
}

static int test_raw_page_api(void)
{
    ndfs_image_options_t opts;
    ndfs_filesystem_t *fs = NULL;
    uint8_t page[PAGE], back[PAGE];
    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_12MB;
    strcpy(opts.directory_name, "FLOPPY");
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT(ndfs_get_total_pages(fs) > 100);        /* the floppy template, whatever its exact size */
    { uint32_t total = ndfs_get_total_pages(fs);
    memset(page, 0x5A, sizeof(page));
    TEST_ASSERT_OK(ndfs_write_disk_page(fs, 100, page));
    TEST_ASSERT_OK(ndfs_read_disk_page(fs, 100, back));
    TEST_ASSERT(memcmp(page, back, PAGE) == 0);
    TEST_ASSERT_EQUAL(NDFS_ERR_OUT_OF_RANGE, ndfs_read_disk_page(fs, total, back));
    TEST_ASSERT_EQUAL(NDFS_ERR_OUT_OF_RANGE, ndfs_write_disk_page(fs, total, page)); }
    ndfs_close(fs);
    return 0;
}

void run_sintran_segments_tests(void)
{
    TEST_SUITE_BEGIN("SINTRAN segments");
    RUN_TEST(test_collect_names);
    RUN_TEST(test_raw_page_api);
    RUN_TEST(test_list_segments);
    RUN_TEST(test_list_without_symbols_falls_back_to_segfil0_file);
    RUN_TEST(test_export_matches_file_bytes);
    RUN_TEST(test_import_round_trip_and_refusals);
}
