/**
 * Tests for the SINTRAN CPU number / CPU type reader and writer (sintran_identity.c).
 *
 * A synthetic system pack is built with the image creator: both symbol lists (with
 * even parity, as the real packs store them), an RTFIL:DATA with the RT-loader
 * "DEFINED SEGMENT NAME" records, a SEGFIL0:DATA holding a self-describing segment
 * table and the two DPIT copies, and for the K shape a SINTRAN:DATA that holds the
 * save copy (segment file 4). The segment numbers in the fixture are deliberately
 * NOT the real 44B/53B in one case, so a test proves the lookup goes by name.
 * Shapes and addresses follow the real packs measured on 2026-10-29 (NDInsight
 * chapter SINTRAN/OS/28, sections 4 to 6; chapter 22 section 6.5 for the records).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "test_framework.h"
#include <ndfs/sintran_identity.h>
#include <ndfs/sintran.h>
#include <ndfs/image_creator.h>
#include <ndfs/filesystem.h>
#include <ndfs/parity.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define PAGE 2048

/* Fixture shape: logical base 4000B (LOGAD low bits = page 2, as on every real pack),
 * 35 pages per copy as on the K pack so the K FCPUN at 105034B (page 32 of the copy)
 * is inside. SEGFIL0 is 80 pages: table at page 5, image copy at page 20.
 * L shape: save copy at SEGFIL0 page 20 is the image, save at page 44 (fits 44+35 = 79).
 * K shape: save copy at page 10 of SINTRAN:DATA (segment file 4). */
#define TABLE_PAGE   5
#define IMAGE_MADR   20
#define SAVE_MADR_L  44
#define SAVE_MADR_K  10
#define SEGLE        35
#define LOGAD        0000102
#define SEGFIL_PAGES 80

#define SYSNO   04051
#define HWINF   04052
#define REVLE   04057
#define FCPUN_L 04414
#define FCPUN_K 0105034

typedef struct {
    bool k_shape;              /* save copy on segment file 4 in SINTRAN:DATA */
    bool with_fcpun_symbol;    /* false: SYMBOL-2-LIST lacks FCPUN */
    int  image_segno;          /* segment numbers the name records give */
    int  save_segno;
    int  sintran_data_pages;   /* K only; too short = refuse */
    bool with_name_records;    /* false: no RTFIL:DATA and no records in SEGFIL0 */
} shape_t;

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

/* Pack a name the way the RT-loader does: SIXBIT, right-justified base 64. */
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

/* Write one identity word set into a DPIT copy that starts at byte `base` of `buf`. */
static void put_dpit_words(uint8_t *buf, long base, int fcpun_addr, int sysno, int type, int revle, int fcpun)
{
    put_word(buf, (size_t)(base + (SYSNO - 04000) * 2), sysno);
    put_word(buf, (size_t)(base + (HWINF + 2 - 04000) * 2), type);
    put_word(buf, (size_t)(base + (REVLE - 04000) * 2), revle);
    put_word(buf, (size_t)(base + (fcpun_addr - 04000) * 2), fcpun);
}

static ndfs_error_t write_text_file(ndfs_filesystem_t *fs, const char *path, const char *text, bool parity)
{
    size_t len = strlen(text);
    uint8_t *buf = (uint8_t *)malloc(len);
    ndfs_error_t err;
    memcpy(buf, text, len);
    if (parity) ndfs_set_parity(buf, len);
    err = ndfs_write_file(fs, path, buf, len);
    free(buf);
    return err;
}

static shape_t l_shape(void)
{
    shape_t s = { false, true, 044, 053, 0, true };
    return s;
}

static shape_t k_shape(void)
{
    shape_t s = { true, true, 044, 053, 48, true };
    return s;
}

static ndfs_error_t build_pack(ndfs_filesystem_t **out_fs, shape_t sh)
{
    ndfs_image_options_t opts;
    ndfs_filesystem_t *fs = NULL;
    ndfs_error_t err;
    uint8_t *seg = (uint8_t *)calloc(SEGFIL_PAGES * PAGE, 1);
    uint8_t rtfil[PAGE * 2];
    int n, fcpun_addr = sh.k_shape ? FCPUN_K : FCPUN_L;
    const char *sym1_k = "SYSNO=004051\r\nHWINF=004052\r\nREVLE=004057\r\nINIBU=067172\r\n";
    const char *sym2_k = "FCPUN=105034\r\n33CPU=000156\r\n33CPN=000144\r\nSYSNO=004051\r\n";
    const char *sym2_k_nofcpun = "33CPU=000156\r\n33CPN=000144\r\nSYSNO=004051\r\n";
    const char *sym1_l = "FCPUN=004414\r\nREVLE=004057\r\nSYSNO=004051\r\nHWINF=004052\r\nINIBU=074123\r\n";
    const char *sym2_l = "GCPUN=061434\r\n33CPU=000000\r\n33CPN=000000\r\n";

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_SMD_75MB;
    strcpy(opts.directory_name, "PACK-ONE");
    opts.include_extended_info = true;
    opts.system_number = 100;
    opts.flag_word = 0x8000;      /* "entered by system 100", as every real pack here */
    err = ndfs_create_image(&fs, &opts);
    if (err != NDFS_OK) { free(seg); return err; }

    /* Segment table: entry 2 self-describing, filler so the page has >= 40 good entries,
     * then the two DPIT copies at the numbers the name records will give. */
    put_seg_entry(seg, TABLE_PAGE, 2, 0, 1, TABLE_PAGE, 1, 0xE000);
    for (n = 3; n <= 60; n++)
        if (n != sh.image_segno && n != sh.save_segno)
            put_seg_entry(seg, TABLE_PAGE, n, 0, 1, 1, 1, 0xE000);
    put_seg_entry(seg, TABLE_PAGE, sh.image_segno, LOGAD, SEGLE, IMAGE_MADR, 0000111, 0162000);
    put_seg_entry(seg, TABLE_PAGE, sh.save_segno, LOGAD, SEGLE,
                  sh.k_shape ? SAVE_MADR_K : SAVE_MADR_L, sh.k_shape ? 0100111 : 0000111, 0162000);

    put_dpit_words(seg, (long)IMAGE_MADR * PAGE, fcpun_addr, 21056, 9883, 010200, 21056);
    if (!sh.k_shape)
        put_dpit_words(seg, (long)SAVE_MADR_L * PAGE, fcpun_addr, 21056, 9883, 010200, 21056);
    err = ndfs_write_file(fs, "SYSTEM/SEGFIL0:DATA", seg, SEGFIL_PAGES * PAGE);
    free(seg);
    if (err != NDFS_OK) { ndfs_close(fs); return err; }

    if (sh.k_shape) {
        size_t sz = (size_t)sh.sintran_data_pages * PAGE;
        uint8_t *sd = (uint8_t *)calloc(sz, 1);
        if ((long)sz >= ((long)SAVE_MADR_K + SEGLE) * PAGE)
            put_dpit_words(sd, (long)SAVE_MADR_K * PAGE, fcpun_addr, 21056, 9883, 010200, 21056);
        err = ndfs_write_file(fs, "SYSTEM/SINTRAN:DATA", sd, sz);
        free(sd);
        if (err != NDFS_OK) { ndfs_close(fs); return err; }
    }

    if (sh.with_name_records) {
        /* RTFIL:DATA as on the real packs: records at page 0 and 1, other names around. */
        memset(rtfil, 0, sizeof(rtfil));
        put_name_record(rtfil, 0 * 16, "S3IMAGE", 002);
        put_name_record(rtfil, 1 * 16, "S3RTFIL", 010);
        put_name_record(rtfil, 2 * 16, "S3DPIT", 023);
        put_name_record(rtfil, 3 * 16, "S3ISGT", 027);
        put_name_record(rtfil, PAGE + 0 * 16, "S3PATCH", 043);
        put_name_record(rtfil, PAGE + 1 * 16, "S3IDPIT", sh.image_segno);
        put_name_record(rtfil, PAGE + 2 * 16, "S3SDPIT", sh.save_segno);
        err = ndfs_write_file(fs, "SYSTEM/RTFIL:DATA", rtfil, sizeof(rtfil));
        if (err != NDFS_OK) { ndfs_close(fs); return err; }
    }

    err = write_text_file(fs, "SYSTEM/SYMBOL-1-LIST:SYMB", sh.k_shape ? sym1_k : sym1_l, true);
    if (err != NDFS_OK) { ndfs_close(fs); return err; }
    err = write_text_file(fs, "SYSTEM/SYMBOL-2-LIST:SYMB",
                          sh.k_shape ? (sh.with_fcpun_symbol ? sym2_k : sym2_k_nofcpun) : sym2_l, true);
    if (err != NDFS_OK) { ndfs_close(fs); return err; }

    *out_fs = fs;
    return NDFS_OK;
}

static bool contains(const uint8_t *buf, size_t len, const char *needle)
{
    size_t n = strlen(needle), i;
    if (n > len) return false;
    for (i = 0; i + n <= len; i++)
        if (memcmp(buf + i, needle, n) == 0) return true;
    return false;
}

/* ── Pure helpers ────────────────────────────────────────────────────── */

static int test_find_symbol(void)
{
    const char *txt = "SYSNO=004051\r\n   FCPUN=004414\r\n33CPN=000000\r\nX=7\n";
    int v; size_t off; int w;
    TEST_ASSERT(ndfs_sintran_find_symbol((const uint8_t *)txt, strlen(txt), "SYSNO", &v, &off, &w));
    TEST_ASSERT_EQUAL(04051, v);
    TEST_ASSERT_EQUAL(6, (long)off);
    TEST_ASSERT_EQUAL(6, w);
    TEST_ASSERT(ndfs_sintran_find_symbol((const uint8_t *)txt, strlen(txt), "FCPUN", &v, &off, &w));
    TEST_ASSERT_EQUAL(04414, v);
    TEST_ASSERT(ndfs_sintran_find_symbol((const uint8_t *)txt, strlen(txt), "33CPN", &v, NULL, NULL));
    TEST_ASSERT_EQUAL(0, v);
    /* A name that is only the tail of another ("CPN" inside "33CPN") must not match. */
    TEST_ASSERT(!ndfs_sintran_find_symbol((const uint8_t *)txt, strlen(txt), "CPN", &v, NULL, NULL));
    TEST_ASSERT(!ndfs_sintran_find_symbol((const uint8_t *)txt, strlen(txt), "HWINF", &v, NULL, NULL));
    return 0;
}

static int test_decode_packed_name(void)
{
    char out[NDFS_SEGNAME_MAX + 1];
    /* The two worked examples of NDInsight chapter 22 section 6.5. */
    TEST_ASSERT(ndfs_sintran_decode_packed_name(0x0004, 0xF325, 0x31D4, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("S3ISGT", out);
    TEST_ASSERT(ndfs_sintran_decode_packed_name(0x013C, 0xD250, 0x10C3, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("S3RTACC", out);
    TEST_ASSERT(!ndfs_sintran_decode_packed_name(0, 0, 0, out, sizeof(out)));
    /* A code outside A-Z / 0-9 (0x3F) is not a name. */
    TEST_ASSERT(!ndfs_sintran_decode_packed_name(0, 0, 0x003F, out, sizeof(out)));
    return 0;
}

static int test_find_segment_by_name(void)
{
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));
    put_name_record(buf, 16, "S3IDPIT", 044);
    put_name_record(buf, 48, "S3SDPIT", 053);
    /* A record with the wrong type must be ignored even with the right name. */
    put_name_record(buf, 80, "S3DPIT", 023);
    put_word(buf, 80 + 10, 024);
    TEST_ASSERT_EQUAL(044, ndfs_sintran_find_segment_by_name(buf, sizeof(buf), "S3IDPIT"));
    TEST_ASSERT_EQUAL(053, ndfs_sintran_find_segment_by_name(buf, sizeof(buf), "S3SDPIT"));
    TEST_ASSERT_EQUAL(-1, ndfs_sintran_find_segment_by_name(buf, sizeof(buf), "S3DPIT"));
    TEST_ASSERT_EQUAL(-1, ndfs_sintran_find_segment_by_name(buf, sizeof(buf), "S3FS"));
    return 0;
}

static int test_segment_word_offset(void)
{
    /* L pack, S3SDPIT: MADR 64, SEGLE 45, LOGAD 000102 -> SYSNO at 64*2048 + 41*2 = 131154. */
    TEST_ASSERT_EQUAL(131154L, ndfs_sintran_segment_word_offset(0000102, 45, 64, 04051));
    TEST_ASSERT_EQUAL(131160L, ndfs_sintran_segment_word_offset(0000102, 45, 64, 04054));
    /* Below the base or past the end: -1. */
    TEST_ASSERT_EQUAL(-1L, ndfs_sintran_segment_word_offset(0000102, 45, 64, 03777));
    TEST_ASSERT_EQUAL(-1L, ndfs_sintran_segment_word_offset(0000102, 45, 64, 04000 + 45 * 1024));
    /* A segment based elsewhere does not contain 4051B. */
    TEST_ASSERT_EQUAL(-1L, ndfs_sintran_segment_word_offset(0000114, 45, 64, 04051));
    return 0;
}

/* ── Read ────────────────────────────────────────────────────────────── */

static int test_read_l_shape(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_cpu_identity_t id;
    TEST_ASSERT_OK(build_pack(&fs, l_shape()));
    TEST_ASSERT_OK(ndfs_read_cpu_identity(fs, &id));
    TEST_ASSERT_EQUAL('L', id.version);
    TEST_ASSERT_EQUAL(04051, id.sym.sysno);
    TEST_ASSERT_EQUAL(04414, id.sym.fcpun);
    TEST_ASSERT_EQUAL(0, id.sym.cpn33);
    TEST_ASSERT_EQUAL(TABLE_PAGE, id.segment_table_page);
    TEST_ASSERT_EQUAL_STRING("SYSTEM/RTFIL:DATA", id.names_path);
    TEST_ASSERT_EQUAL(044, id.copies[0].segment_number);
    TEST_ASSERT_EQUAL(053, id.copies[1].segment_number);
    TEST_ASSERT(id.copies[0].present && id.copies[0].file_found);
    TEST_ASSERT(id.copies[1].present && id.copies[1].file_found);
    TEST_ASSERT_EQUAL(0, id.copies[1].segment_file);
    TEST_ASSERT_EQUAL_STRING("SYSTEM/SEGFIL0:DATA", id.copies[1].file_path);
    TEST_ASSERT_EQUAL(21056, id.copies[0].sysno);
    TEST_ASSERT_EQUAL(21056, id.copies[0].fcpun);
    TEST_ASSERT_EQUAL(9883, id.copies[0].cpu_type);
    TEST_ASSERT_EQUAL(010200, id.copies[0].revle);
    TEST_ASSERT_EQUAL(21056, id.copies[1].sysno);
    TEST_ASSERT(id.dir_ext_valid);
    TEST_ASSERT(id.dir_entered);
    TEST_ASSERT_EQUAL(100, id.dir_system_number);
    ndfs_close(fs);
    return 0;
}

static int test_read_k_shape_save_copy_in_sintran_data(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_cpu_identity_t id;
    TEST_ASSERT_OK(build_pack(&fs, k_shape()));
    TEST_ASSERT_OK(ndfs_read_cpu_identity(fs, &id));
    TEST_ASSERT_EQUAL('K', id.version);
    TEST_ASSERT_EQUAL(0105034, id.sym.fcpun);
    TEST_ASSERT_EQUAL(0144, id.sym.cpn33);
    TEST_ASSERT_EQUAL(0156, id.sym.cpu33);
    TEST_ASSERT_EQUAL(4, id.copies[1].segment_file);
    TEST_ASSERT_EQUAL_STRING("SYSTEM/SINTRAN:DATA", id.copies[1].file_path);
    TEST_ASSERT(id.copies[1].file_found);
    TEST_ASSERT_EQUAL(21056, id.copies[1].sysno);
    TEST_ASSERT_EQUAL(21056, id.copies[1].fcpun);
    TEST_ASSERT_EQUAL(9883, id.copies[1].cpu_type);
    TEST_ASSERT_EQUAL(21056, id.copies[0].fcpun);
    ndfs_close(fs);
    return 0;
}

static int test_read_segments_found_by_name_not_number(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_cpu_identity_t id;
    shape_t sh = l_shape();
    sh.image_segno = 045;     /* not the real 44B/53B: only the name records say where */
    sh.save_segno = 054;
    TEST_ASSERT_OK(build_pack(&fs, sh));
    TEST_ASSERT_OK(ndfs_read_cpu_identity(fs, &id));
    TEST_ASSERT_EQUAL(045, id.copies[0].segment_number);
    TEST_ASSERT_EQUAL(054, id.copies[1].segment_number);
    TEST_ASSERT_EQUAL(21056, id.copies[0].sysno);
    TEST_ASSERT_EQUAL(21056, id.copies[1].sysno);
    ndfs_close(fs);
    return 0;
}

static int test_read_names_from_segfil_when_rtfil_missing(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_cpu_identity_t id;
    uint8_t *seg; size_t size;
    uint8_t rec[2 * 16];
    shape_t sh = l_shape();
    sh.with_name_records = false;
    TEST_ASSERT_OK(build_pack(&fs, sh));
    /* No RTFIL:DATA and no records anywhere: refuse. */
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_read_cpu_identity(fs, &id));
    TEST_ASSERT_EQUAL(-1, id.copies[0].segment_number);
    /* Put the loader table copy inside SEGFIL0 (page 60, as the S3RTFIL segment does). */
    memset(rec, 0, sizeof(rec));
    put_name_record(rec, 0, "S3IDPIT", 044);
    put_name_record(rec, 16, "S3SDPIT", 053);
    TEST_ASSERT_OK(ndfs_patch_file_region(fs, "SYSTEM/SEGFIL0:DATA", 60 * PAGE, rec, sizeof(rec)));
    TEST_ASSERT_OK(ndfs_read_cpu_identity(fs, &id));
    TEST_ASSERT_EQUAL(044, id.copies[0].segment_number);
    TEST_ASSERT_EQUAL(21056, id.copies[1].sysno);
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/SEGFIL0:DATA", &seg, &size));
    ndfs_free_data(seg);
    ndfs_close(fs);
    return 0;
}

static int test_read_refuses_without_fcpun_symbol(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_cpu_identity_t id;
    shape_t sh = k_shape();
    sh.with_fcpun_symbol = false;
    TEST_ASSERT_OK(build_pack(&fs, sh));
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_read_cpu_identity(fs, &id));
    TEST_ASSERT_EQUAL(04051, id.sym.sysno);   /* what was found is still reported */
    TEST_ASSERT_EQUAL(-1, id.sym.fcpun);
    ndfs_close(fs);
    return 0;
}

static int test_read_not_a_system_pack(void)
{
    ndfs_image_options_t opts;
    ndfs_filesystem_t *fs = NULL;
    ndfs_cpu_identity_t id;
    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_12MB;
    strcpy(opts.directory_name, "FLOPPY");
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_read_cpu_identity(fs, &id));
    ndfs_close(fs);
    return 0;
}

/* ── Write ───────────────────────────────────────────────────────────── */

static int test_write_l_shape_both_copies_symbols_and_stamp(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_cpu_identity_t after;
    uint8_t *sym2; size_t len;
    TEST_ASSERT_OK(build_pack(&fs, l_shape()));
    TEST_ASSERT_OK(ndfs_write_cpu_identity(fs, 21238, 9883, false, &after));
    TEST_ASSERT_EQUAL(21238, after.copies[0].sysno);
    TEST_ASSERT_EQUAL(21238, after.copies[0].fcpun);
    TEST_ASSERT_EQUAL(21238, after.copies[1].sysno);
    TEST_ASSERT_EQUAL(21238, after.copies[1].fcpun);
    TEST_ASSERT_EQUAL(9883, after.copies[1].cpu_type);
    TEST_ASSERT_EQUAL(010200, after.copies[1].revle);        /* untouched */
    TEST_ASSERT_EQUAL(21238, after.sym.cpn33);
    TEST_ASSERT_EQUAL(9883, after.sym.cpu33);
    TEST_ASSERT(!after.dir_entered);
    TEST_ASSERT_EQUAL(0, after.dir_system_number);
    TEST_ASSERT(after.dir_ext_valid);                         /* checksum rewritten */

    /* The symbol list kept its line shape, "33CPN=051366", and the new digits carry
     * even parity like the rest of the file ('1' = 0x31 has three 1-bits, so 0xB1). */
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/SYMBOL-2-LIST:SYMB", &sym2, &len));
    {
        uint8_t expect[6] = { '0', '5', '1', '3', '6', '6' };
        uint8_t *plain = (uint8_t *)malloc(len);
        size_t i, at = (size_t)-1;
        memcpy(plain, sym2, len);
        ndfs_strip_parity(plain, len);
        for (i = 0; i + 12 <= len; i++)
            if (memcmp(plain + i, "33CPN=051366", 12) == 0) { at = i + 6; break; }
        TEST_ASSERT(at != (size_t)-1);
        ndfs_set_parity(expect, 6);
        TEST_ASSERT(memcmp(sym2 + at, expect, 6) == 0);
        TEST_ASSERT(contains(plain, len, "33CPU=023233\r\n"));
        free(plain);
    }
    ndfs_free_data(sym2);
    ndfs_close(fs);
    return 0;
}

static int test_write_type_optional_and_keep_stamp(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_cpu_identity_t after;
    TEST_ASSERT_OK(build_pack(&fs, l_shape()));
    TEST_ASSERT_OK(ndfs_write_cpu_identity(fs, 7, -1, true, &after));
    TEST_ASSERT_EQUAL(7, after.copies[0].sysno);
    TEST_ASSERT_EQUAL(9883, after.copies[0].cpu_type);        /* kept */
    TEST_ASSERT_EQUAL(0, after.sym.cpu33);                    /* kept */
    TEST_ASSERT(after.dir_entered);                           /* kept */
    TEST_ASSERT_EQUAL(100, after.dir_system_number);
    ndfs_close(fs);
    return 0;
}

static int test_write_k_shape_writes_sintran_data(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_cpu_identity_t after;
    uint8_t *sd; size_t len;
    TEST_ASSERT_OK(build_pack(&fs, k_shape()));
    TEST_ASSERT_OK(ndfs_write_cpu_identity(fs, 21238, -1, false, &after));
    TEST_ASSERT_EQUAL(21238, after.copies[0].sysno);
    TEST_ASSERT_EQUAL(21238, after.copies[0].fcpun);
    TEST_ASSERT_EQUAL(21238, after.copies[1].sysno);
    TEST_ASSERT_EQUAL(21238, after.copies[1].fcpun);
    TEST_ASSERT_EQUAL(9883, after.copies[1].cpu_type);
    TEST_ASSERT_EQUAL(21238, after.sym.cpn33);
    TEST_ASSERT_EQUAL(0156, after.sym.cpu33);                 /* type not given: kept */
    /* The save copy really went into SINTRAN:DATA page 10. */
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/SINTRAN:DATA", &sd, &len));
    TEST_ASSERT_EQUAL(21238, (sd[SAVE_MADR_K * PAGE + (SYSNO - 04000) * 2] << 8) |
                             sd[SAVE_MADR_K * PAGE + (SYSNO - 04000) * 2 + 1]);
    ndfs_free_data(sd);
    ndfs_close(fs);
    return 0;
}

static int test_write_refuses_when_save_file_too_short(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_cpu_identity_t id;
    shape_t sh = k_shape();
    sh.sintran_data_pages = 16;   /* SYSNO (page 10) is inside, FCPUN (page 10 + 32) is not */
    TEST_ASSERT_OK(build_pack(&fs, sh));
    TEST_ASSERT_OK(ndfs_read_cpu_identity(fs, &id));
    TEST_ASSERT(id.copies[1].present);
    TEST_ASSERT(id.copies[1].file_found);
    TEST_ASSERT(id.copies[1].sysno_offset >= 0);
    TEST_ASSERT_EQUAL(-1L, id.copies[1].fcpun_offset);
    TEST_ASSERT_EQUAL(-1, id.copies[1].fcpun);
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_write_cpu_identity(fs, 21238, -1, false, NULL));
    TEST_ASSERT_OK(ndfs_read_cpu_identity(fs, &id));
    TEST_ASSERT_EQUAL(21056, id.copies[0].sysno);             /* nothing changed */
    TEST_ASSERT(id.dir_entered);
    ndfs_close(fs);
    return 0;
}

static int test_write_rejects_bad_values(void)
{
    ndfs_filesystem_t *fs = NULL;
    TEST_ASSERT_OK(build_pack(&fs, l_shape()));
    TEST_ASSERT_EQUAL(NDFS_ERR_INVALID_ARG, ndfs_write_cpu_identity(fs, -1, -1, false, NULL));
    TEST_ASSERT_EQUAL(NDFS_ERR_INVALID_ARG, ndfs_write_cpu_identity(fs, 65536, -1, false, NULL));
    TEST_ASSERT_EQUAL(NDFS_ERR_INVALID_ARG, ndfs_write_cpu_identity(fs, 1, 65536, false, NULL));
    ndfs_close(fs);
    return 0;
}

void run_sintran_identity_tests(void)
{
    TEST_SUITE_BEGIN("SINTRAN CPU number and type");
    RUN_TEST(test_find_symbol);
    RUN_TEST(test_decode_packed_name);
    RUN_TEST(test_find_segment_by_name);
    RUN_TEST(test_segment_word_offset);
    RUN_TEST(test_read_l_shape);
    RUN_TEST(test_read_k_shape_save_copy_in_sintran_data);
    RUN_TEST(test_read_segments_found_by_name_not_number);
    RUN_TEST(test_read_names_from_segfil_when_rtfil_missing);
    RUN_TEST(test_read_refuses_without_fcpun_symbol);
    RUN_TEST(test_read_not_a_system_pack);
    RUN_TEST(test_write_l_shape_both_copies_symbols_and_stamp);
    RUN_TEST(test_write_type_optional_and_keep_stamp);
    RUN_TEST(test_write_k_shape_writes_sintran_data);
    RUN_TEST(test_write_refuses_when_save_file_too_short);
    RUN_TEST(test_write_rejects_bad_values);
}
