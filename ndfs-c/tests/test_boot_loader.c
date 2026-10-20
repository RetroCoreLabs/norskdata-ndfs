/**
 * Boot loader detection tests.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 */

#include "test_framework.h"
#include <ndfs/ndfs.h>
#include "endian_util.h"
#include <string.h>
#include <stdlib.h>

/* ---- Helper: create an empty image via the creator ---- */

static ndfs_filesystem_t *create_empty_image(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    opts.boot_choice = NDFS_BOOTCODE_NONE;   /* the test supplies its own boot area */
    strcpy(opts.directory_name, "BOOTTEST");

    if (ndfs_create_image(&fs, &opts) != NDFS_OK) return NULL;
    return fs;
}

/* ---- Tests ---- */

static int test_no_boot_on_empty_image(void)
{
    ndfs_filesystem_t *fs = create_empty_image();
    ndfs_boot_format_t fmt;
    bool bootable = true;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_detect_boot_format(fs, &fmt));
    TEST_ASSERT_EQUAL(NDFS_BOOT_NONE, fmt);

    TEST_ASSERT_OK(ndfs_is_bootable(fs, &bootable));
    TEST_ASSERT(!bootable);

    ndfs_close(fs);
    return 0;
}

static int test_load_boot_code_empty(void)
{
    ndfs_filesystem_t *fs = create_empty_image();
    ndfs_boot_code_t code;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_load_boot_code(fs, &code));
    TEST_ASSERT_EQUAL(NDFS_BOOT_NONE, code.format);

    ndfs_boot_code_destroy(&code);
    ndfs_close(fs);
    return 0;
}

static int test_detect_bpun_format(void)
{
    /* Create an image with a BPUN boot sector injected into page 0 */
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    uint8_t *exported = NULL;
    size_t exported_size = 0;
    ndfs_boot_format_t fmt;
    ndfs_boot_code_t code;
    size_t pos;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    opts.boot_choice = NDFS_BOOTCODE_NONE;   /* the test supplies its own boot area */
    strcpy(opts.directory_name, "BPUNTEST");
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));

    /* Export to get raw buffer */
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &exported, &exported_size));
    ndfs_close(fs);
    fs = NULL;

    /* Inject a minimal BPUN boot sector into page 0 */
    /* Format: ASCII preamble, '!' delimiter, Address(2), Count(2), Data, Checksum(2), Action(2) */
    pos = 0;
    exported[pos++] = '!'; /* BPUN delimiter */

    /* Address: 0x0020 (load at address 32) */
    ndfs_write_u16be(exported, pos, 0x0020);
    pos += 2;

    /* Word count: 2 (4 bytes of code) */
    ndfs_write_u16be(exported, pos, 2);
    pos += 2;

    /* Code data: 2 words */
    ndfs_write_u16be(exported, pos, 0x1234); /* word 1 */
    pos += 2;
    ndfs_write_u16be(exported, pos, 0x5678); /* word 2 */
    pos += 2;

    /* Checksum: sum of code words = 0x1234 + 0x5678 = 0x68AC */
    ndfs_write_u16be(exported, pos, 0x68AC);
    pos += 2;

    /* Action field */
    ndfs_write_u16be(exported, pos, 0x0000);

    /* Re-open with BPUN data */
    TEST_ASSERT_OK(ndfs_open_buffer_copy(exported, exported_size, false, &fs));
    free(exported);

    TEST_ASSERT_OK(ndfs_detect_boot_format(fs, &fmt));
    TEST_ASSERT_EQUAL(NDFS_BOOT_BPUN, fmt);

    /* Load and verify */
    TEST_ASSERT_OK(ndfs_load_boot_code(fs, &code));
    TEST_ASSERT_EQUAL(NDFS_BOOT_BPUN, code.format);
    TEST_ASSERT_EQUAL(0x0020, code.load_address);
    TEST_ASSERT_EQUAL(2, code.word_count);
    TEST_ASSERT_EQUAL(4, code.data_size);
    TEST_ASSERT(code.checksum_valid);

    {
        bool bootable = false;
        TEST_ASSERT_OK(ndfs_is_bootable(fs, &bootable));
        TEST_ASSERT(bootable);
    }

    ndfs_boot_code_destroy(&code);
    ndfs_close(fs);
    return 0;
}

static int test_detect_flomon_format(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    uint8_t *exported = NULL;
    size_t exported_size = 0;
    ndfs_boot_format_t fmt;
    ndfs_boot_code_t code;
    size_t pos;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    opts.boot_choice = NDFS_BOOTCODE_NONE;   /* the test supplies its own boot area */
    strcpy(opts.directory_name, "FLOTEST");
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));

    TEST_ASSERT_OK(ndfs_to_buffer(fs, &exported, &exported_size));
    ndfs_close(fs);
    fs = NULL;

    /* Inject FLOMON boot sector:
       '!' + Address(0) + Count(0) + Checksum(0) + WordCount(byte) + data */
    pos = 0;
    exported[pos++] = '!';

    /* Address = 0, Count = 0 */
    ndfs_write_u16be(exported, pos, 0x0000); pos += 2;
    ndfs_write_u16be(exported, pos, 0x0000); pos += 2;

    /* Checksum = 0 (FLOMON marker) */
    ndfs_write_u16be(exported, pos, 0x0000); pos += 2;

    /* Word count: 2 words */
    exported[pos++] = 2;

    /* FLOMON data: 00 HI 00 LO per word */
    exported[pos++] = 0x00;
    exported[pos++] = 0xAB; /* HI */
    exported[pos++] = 0x00;
    exported[pos++] = 0xCD; /* LO */

    exported[pos++] = 0x00;
    exported[pos++] = 0x12; /* HI */
    exported[pos++] = 0x00;
    exported[pos++] = 0x34; /* LO */

    TEST_ASSERT_OK(ndfs_open_buffer_copy(exported, exported_size, false, &fs));
    free(exported);

    TEST_ASSERT_OK(ndfs_detect_boot_format(fs, &fmt));
    TEST_ASSERT_EQUAL(NDFS_BOOT_FLOMON, fmt);

    TEST_ASSERT_OK(ndfs_load_boot_code(fs, &code));
    TEST_ASSERT_EQUAL(NDFS_BOOT_FLOMON, code.format);
    TEST_ASSERT_EQUAL(2, code.word_count);
    TEST_ASSERT_EQUAL(4, code.data_size);
    TEST_ASSERT_NOT_NULL(code.data);
    /* Check code data: AB CD 12 34 */
    TEST_ASSERT_EQUAL(0xAB, code.data[0]);
    TEST_ASSERT_EQUAL(0xCD, code.data[1]);
    TEST_ASSERT_EQUAL(0x12, code.data[2]);
    TEST_ASSERT_EQUAL(0x34, code.data[3]);

    ndfs_boot_code_destroy(&code);
    ndfs_close(fs);
    return 0;
}

static int test_flomon_zero_words(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    uint8_t *exported = NULL;
    size_t exported_size = 0;
    ndfs_boot_code_t code;
    size_t pos;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    opts.boot_choice = NDFS_BOOTCODE_NONE;   /* the test supplies its own boot area */
    strcpy(opts.directory_name, "FLO0WD");
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &exported, &exported_size));
    ndfs_close(fs);
    fs = NULL;

    /* FLOMON with word count = 0 */
    pos = 0;
    exported[pos++] = '!';
    ndfs_write_u16be(exported, pos, 0); pos += 2;
    ndfs_write_u16be(exported, pos, 0); pos += 2;
    ndfs_write_u16be(exported, pos, 0); pos += 2;
    exported[pos++] = 0; /* word count = 0 */

    TEST_ASSERT_OK(ndfs_open_buffer_copy(exported, exported_size, false, &fs));
    free(exported);

    TEST_ASSERT_OK(ndfs_load_boot_code(fs, &code));
    TEST_ASSERT_EQUAL(NDFS_BOOT_FLOMON, code.format);
    TEST_ASSERT_EQUAL(0, code.word_count);
    TEST_ASSERT_NULL(code.data);
    TEST_ASSERT_EQUAL(0, code.data_size);

    ndfs_boot_code_destroy(&code);
    ndfs_close(fs);
    return 0;
}

static int test_null_args(void)
{
    ndfs_boot_format_t fmt;
    ndfs_boot_code_t code;
    bool bootable;

    TEST_ASSERT_EQUAL(NDFS_ERR_NULL_PTR, ndfs_detect_boot_format(NULL, &fmt));
    TEST_ASSERT_EQUAL(NDFS_ERR_NULL_PTR, ndfs_detect_boot_format(NULL, NULL));
    TEST_ASSERT_EQUAL(NDFS_ERR_NULL_PTR, ndfs_load_boot_code(NULL, &code));
    TEST_ASSERT_EQUAL(NDFS_ERR_NULL_PTR, ndfs_is_bootable(NULL, &bootable));

    /* Destroy NULL should not crash */
    ndfs_boot_code_destroy(NULL);

    return 0;
}

/* ---- Raw hard-disk bootstrap detection (opcode/IOX based) ---- */

/* Injects a boot-area prefix into page 0, leaving the master block (the last
   32 bytes of page 0, at NDFS_MASTER_BLOCK_OFFSET) untouched so the image
   created by the template stays valid. */
static ndfs_filesystem_t *create_image_with_page0(const uint8_t *page0_prefix, size_t prefix_len)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    uint8_t *exported = NULL;
    size_t exported_size = 0;

    if (prefix_len > NDFS_MASTER_BLOCK_OFFSET) prefix_len = NDFS_MASTER_BLOCK_OFFSET;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    opts.boot_choice = NDFS_BOOTCODE_NONE;   /* the test supplies its own boot area */
    strcpy(opts.directory_name, "RAWTEST");
    if (ndfs_create_image(&fs, &opts) != NDFS_OK) return NULL;

    if (ndfs_to_buffer(fs, &exported, &exported_size) != NDFS_OK) {
        ndfs_close(fs);
        return NULL;
    }
    ndfs_close(fs);
    fs = NULL;

    memcpy(exported, page0_prefix, prefix_len);

    if (ndfs_open_buffer_copy(exported, exported_size, false, &fs) != NDFS_OK) {
        free(exported);
        return NULL;
    }
    free(exported);
    return fs;
}

static int test_rejects_nonuniform_garbage_without_prologue_opcode(void)
{
    /* Regression: the old heuristic accepted ANY non-uniform block without a
       '!' byte as "valid binary code". A real bootstrap must start with the
       PIOF/IOF prologue opcode; this buffer does not. */
    uint8_t page0[2048];
    ndfs_filesystem_t *fs;
    ndfs_boot_format_t fmt;
    size_t i;

    for (i = 0; i < sizeof(page0); i++) {
        page0[i] = (uint8_t)((i * 7 + 3) & 0xFF);
        if (page0[i] == 0x21) page0[i] = 0x22; /* avoid accidental BPUN delimiter */
    }

    fs = create_image_with_page0(page0, sizeof(page0));
    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_detect_boot_format(fs, &fmt));
    TEST_ASSERT_EQUAL(NDFS_BOOT_NONE, fmt);

    ndfs_close(fs);
    return 0;
}

static int test_detects_smd_controller_type(void)
{
    /* PIOF (0xD105) followed by IOX 1540 octal (SMD/ECC base) =
       opcode 0164000 | 1540 octal = 0165540 octal = 0xEB60 big-endian. */
    uint8_t page0[2048];
    ndfs_filesystem_t *fs;
    ndfs_boot_format_t fmt;
    ndfs_boot_controller_type_t ctype;

    memset(page0, 0, sizeof(page0));
    page0[0] = 0xD1; page0[1] = 0x05;
    page0[2] = 0xEB; page0[3] = 0x60;

    fs = create_image_with_page0(page0, sizeof(page0));
    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_detect_boot_format(fs, &fmt));
    TEST_ASSERT_EQUAL(NDFS_BOOT_BINARY, fmt);

    TEST_ASSERT_OK(ndfs_detect_boot_controller_type(fs, &ctype));
    TEST_ASSERT_EQUAL(NDFS_CONTROLLER_SMD_ECC, ctype);

    ndfs_close(fs);
    return 0;
}

static int test_detects_scsi_controller_type(void)
{
    /* IOF (0xD101) followed by the fixed indirect IOXT opcode
       (octal 150415 = 0xD10D big-endian). */
    uint8_t page0[2048];
    ndfs_filesystem_t *fs;
    ndfs_boot_controller_type_t ctype;

    memset(page0, 0, sizeof(page0));
    page0[0] = 0xD1; page0[1] = 0x01;
    page0[2] = 0xD1; page0[3] = 0x0D;

    fs = create_image_with_page0(page0, sizeof(page0));
    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_detect_boot_controller_type(fs, &ctype));
    TEST_ASSERT_EQUAL(NDFS_CONTROLLER_SCSI, ctype);

    ndfs_close(fs);
    return 0;
}

static int test_boot_code_destroy_zeroed(void)
{
    ndfs_boot_code_t code;
    memset(&code, 0, sizeof(code));
    ndfs_boot_code_destroy(&code); /* Should not crash */
    return 0;
}

void run_boot_loader_tests(void)
{
    TEST_SUITE_BEGIN("Boot Loader Tests");
    RUN_TEST(test_no_boot_on_empty_image);
    RUN_TEST(test_load_boot_code_empty);
    RUN_TEST(test_detect_bpun_format);
    RUN_TEST(test_detect_flomon_format);
    RUN_TEST(test_flomon_zero_words);
    RUN_TEST(test_null_args);
    RUN_TEST(test_boot_code_destroy_zeroed);
    RUN_TEST(test_rejects_nonuniform_garbage_without_prologue_opcode);
    RUN_TEST(test_detects_smd_controller_type);
    RUN_TEST(test_detects_scsi_controller_type);
}
