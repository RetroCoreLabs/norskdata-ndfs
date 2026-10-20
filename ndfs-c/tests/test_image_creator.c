/**
 * Image creator tests.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 */

#include "test_framework.h"
#include <ndfs/ndfs.h>
#include <string.h>
#include <stdlib.h>
#include "boot_blobs.h"

/* ---- Helper: verify common post-creation properties ---- */

static int verify_image_basics(ndfs_filesystem_t *fs, const char *expected_name,
                               uint32_t min_pages)
{
    const ndfs_master_block_t *mb = NULL;
    char name[NDFS_NAME_MAX + 1];
    ndfs_user_entry_t *users = NULL;
    size_t user_count = 0;
    uint32_t free_pages = 0;
    bool ok = false;

    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    TEST_ASSERT_NOT_NULL(mb);
    TEST_ASSERT_EQUAL_STRING(expected_name, mb->directory_name);

    /* Pointers must be valid */
    TEST_ASSERT(ndfs_bp_is_valid(&mb->object_file_ptr));
    TEST_ASSERT(ndfs_bp_is_valid(&mb->user_file_ptr));
    TEST_ASSERT(ndfs_bp_is_valid(&mb->bit_file_ptr));

    /* Directory name */
    TEST_ASSERT_OK(ndfs_get_directory_name(fs, name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING(expected_name, name);

    /* SYSTEM user must exist */
    TEST_ASSERT_OK(ndfs_get_users(fs, &users, &user_count));
    TEST_ASSERT(user_count >= 1);
    TEST_ASSERT_EQUAL_STRING("SYSTEM", users[0].user_name);
    ndfs_free_users(users);

    /* Free pages should be reasonable */
    TEST_ASSERT_OK(ndfs_get_free_pages(fs, &free_pages));
    TEST_ASSERT(free_pages > 0);
    TEST_ASSERT(free_pages < min_pages);

    /* Integrity check */
    TEST_ASSERT_OK(ndfs_verify_integrity(fs, &ok));
    TEST_ASSERT(ok);

    /* Bitmap: page 0 should be used */
    TEST_ASSERT(ndfs_is_block_used(fs, 0));

    return 0;
}

static int test_create_floppy_360kb(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    const ndfs_master_block_t *mb;
    int r;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    strcpy(opts.directory_name, "FLP360");

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_NOT_NULL(fs);

    r = verify_image_basics(fs, "FLP360", 154);
    if (r != 0) { ndfs_close(fs); return r; }

    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    TEST_ASSERT_EQUAL(76, mb->object_file_ptr.block_id);
    TEST_ASSERT_EQUAL(78, mb->user_file_ptr.block_id);
    TEST_ASSERT_EQUAL(72, mb->bit_file_ptr.block_id);

    ndfs_close(fs);
    return 0;
}

static int test_create_floppy_12mb(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    const ndfs_master_block_t *mb;
    int r;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_12MB;
    strcpy(opts.directory_name, "FLP12M");

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_NOT_NULL(fs);

    r = verify_image_basics(fs, "FLP12M", 616);
    if (r != 0) { ndfs_close(fs); return r; }

    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    TEST_ASSERT_EQUAL(316, mb->object_file_ptr.block_id);
    TEST_ASSERT_EQUAL(318, mb->user_file_ptr.block_id);
    TEST_ASSERT_EQUAL(306, mb->bit_file_ptr.block_id);

    ndfs_close(fs);
    return 0;
}

static int test_create_smd_75mb(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    const ndfs_master_block_t *mb;
    int r;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_SMD_75MB;
    strcpy(opts.directory_name, "SMD75");
    opts.include_extended_info = true;
    opts.system_number = 42;
    opts.flag_word = 0x1234;

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_NOT_NULL(fs);

    r = verify_image_basics(fs, "SMD75", 38400);
    if (r != 0) { ndfs_close(fs); return r; }

    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    TEST_ASSERT_EQUAL(18476, mb->object_file_ptr.block_id);
    TEST_ASSERT_EQUAL(18478, mb->user_file_ptr.block_id);
    TEST_ASSERT_EQUAL(18468, mb->bit_file_ptr.block_id);

    ndfs_close(fs);
    return 0;
}

static int test_create_winchester_74mb(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    const ndfs_master_block_t *mb;
    int r;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_WINCHESTER_74MB;
    strcpy(opts.directory_name, "WIN74");

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_NOT_NULL(fs);

    /* File is 36864 pages (the real Micropolis 1325 image): 468 pages larger than the
     * declared capacity 36396. It used to be 36360, 36 pages SHORTER than the capacity. */
    r = verify_image_basics(fs, "WIN74", 36864);
    if (r != 0) { ndfs_close(fs); return r; }

    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    TEST_ASSERT_EQUAL(18204, mb->object_file_ptr.block_id);
    TEST_ASSERT_EQUAL(18206, mb->user_file_ptr.block_id);
    TEST_ASSERT_EQUAL(18198, mb->bit_file_ptr.block_id);
    TEST_ASSERT_EQUAL(36396, mb->ext_pages_available);

    {
        uint8_t *buf = NULL;
        size_t buf_size = 0;
        TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));
        TEST_ASSERT_EQUAL(36864u * NDFS_PAGE_SIZE, buf_size);
        free(buf);
    }

    ndfs_close(fs);
    return 0;
}

static int test_create_scsi_126mb(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    const ndfs_master_block_t *mb;
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    int r;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_SCSI_126MB;
    strcpy(opts.directory_name, "SCSI126");

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_NOT_NULL(fs);

    r = verify_image_basics(fs, "SCSI126", 64656);
    if (r != 0) { ndfs_close(fs); return r; }

    /* Values of the real scsi-1.img: 64656 pages, capacity 61036. */
    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    TEST_ASSERT_EQUAL(30506, mb->object_file_ptr.block_id);
    TEST_ASSERT_EQUAL(30508, mb->user_file_ptr.block_id);
    TEST_ASSERT_EQUAL(30510, mb->bit_file_ptr.block_id);
    TEST_ASSERT_EQUAL(61036, mb->ext_pages_available);

    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));
    TEST_ASSERT_EQUAL(64656u * NDFS_PAGE_SIZE, buf_size);
    free(buf);

    ndfs_close(fs);
    return 0;
}

/* ---- SINTRAN rules (measured with the real SINTRAN M, docs/CREATE-DIRECTORY-ORACLE.md) ---- */

/* The page of bit n in a raw bit file: byte (n>>3)^1, bit n&7. */
static int raw_bit(const uint8_t *img, uint32_t bit_file, uint32_t page)
{
    return (img[(size_t)bit_file * NDFS_PAGE_SIZE + ((page >> 3) ^ 1u)] >> (page & 7u)) & 1;
}

static int create_and_export(ndfs_image_template_t tmpl, int32_t answer, uint8_t **buf, size_t *size,
                             ndfs_boot_choice_t boot)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    ndfs_image_options_init(&opts);
    opts.template_type = tmpl;
    opts.bit_file_answer = answer;
    opts.boot_choice = boot;
    strcpy(opts.directory_name, "RULES");
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_OK(ndfs_to_buffer(fs, buf, size));
    ndfs_close(fs);
    return 0;
}

static uint32_t raw_ptr(const uint8_t *img, size_t off)
{
    return ((uint32_t)img[0x7E0 + off] << 24 | (uint32_t)img[0x7E0 + off + 1] << 16 |
            (uint32_t)img[0x7E0 + off + 2] << 8 | img[0x7E0 + off + 3]) & 0x3FFFFFFFu;
}

static int test_bit_file_answers_smd(void)
{
    uint8_t *img = NULL; size_t size = 0;
    int r;

    /* answer -1 (end of disc): bit file = capacity - 3 bitmap pages = 36942; user index
     * 36940 (= address - 2), object index 36938 (= address - 4). Measured in run 1. */
    r = create_and_export(NDFS_TMPL_SMD_75MB, -1, &img, &size, NDFS_BOOTCODE_NONE);
    if (r) return r;
    TEST_ASSERT_EQUAL(36942, raw_ptr(img, 0x18));
    TEST_ASSERT_EQUAL(36940, raw_ptr(img, 0x14));
    TEST_ASSERT_EQUAL(36938, raw_ptr(img, 0x10));
    free(img); img = NULL;

    /* explicit address 40000 octal = 16384 (run 3): user 16382, object 16380 */
    r = create_and_export(NDFS_TMPL_SMD_75MB, 16384, &img, &size, NDFS_BOOTCODE_NONE);
    if (r) return r;
    TEST_ASSERT_EQUAL(16384, raw_ptr(img, 0x18));
    TEST_ASSERT_EQUAL(16382, raw_ptr(img, 0x14));
    TEST_ASSERT_EQUAL(16380, raw_ptr(img, 0x10));
    free(img);
    return 0;
}

static int test_unreserved_pages_formula(void)
{
    /* Unreserved Pages = capacity - 1 - bitmap pages - 2 - 1 - the SYSTEM quota.
     * SMD 75MB: quota = 36945 / 2 = 18472 -> 36945 - 1 - 3 - 2 - 1 - 18472 = 18466. */
    uint8_t *img = NULL; size_t size = 0;
    int r = create_and_export(NDFS_TMPL_SMD_75MB, 0, &img, &size, NDFS_BOOTCODE_NONE);
    if (r) return r;
    TEST_ASSERT_EQUAL(18466, ((uint32_t)img[0x7E0 + 0x1C] << 24 | (uint32_t)img[0x7E0 + 0x1D] << 16 |
                              (uint32_t)img[0x7E0 + 0x1E] << 8 | img[0x7E0 + 0x1F]));
    free(img);
    return 0;
}

static int test_pages_above_capacity_marked_used(void)
{
    /* SMD 75MB: pages 36945 .. 37119 (up to the next multiple of 256, 37120) are used, 37120 is free. */
    uint8_t *img = NULL; size_t size = 0;
    uint32_t ba;
    int r = create_and_export(NDFS_TMPL_SMD_75MB, 0, &img, &size, NDFS_BOOTCODE_NONE);
    if (r) return r;
    ba = raw_ptr(img, 0x18);
    TEST_ASSERT_EQUAL(1, raw_bit(img, ba, 36945));
    TEST_ASSERT_EQUAL(1, raw_bit(img, ba, 37119));
    TEST_ASSERT_EQUAL(0, raw_bit(img, ba, 37120));
    /* page 0 used, the blocks 1..6 are NOT marked by SINTRAN's CREATE-DIRECTORY (measured) */
    TEST_ASSERT_EQUAL(1, raw_bit(img, ba, 0));
    TEST_ASSERT_EQUAL(0, raw_bit(img, ba, 1));
    TEST_ASSERT_EQUAL(0, raw_bit(img, ba, 6));
    free(img);
    return 0;
}

static int test_floppy_has_no_extended_info(void)
{
    /* SINTRAN writes no Extended Info Block on a floppy (measured with 154 and 616 page floppies). */
    uint8_t *img = NULL; size_t size = 0;
    size_t i;
    int r = create_and_export(NDFS_TMPL_FLOPPY_12MB, 0, &img, &size, NDFS_BOOTCODE_NONE);
    if (r) return r;
    for (i = 0x7D0; i < 0x7E0; i++) TEST_ASSERT_EQUAL(0, img[i]);
    free(img);
    return 0;
}

static int test_boot_code_written_and_override(void)
{
    uint8_t *img = NULL; size_t size = 0;
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    uint8_t own[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    size_t n = 0;
    const uint8_t *blob;
    size_t i;
    int r;

    /* default: the SMD boot area of the real disk is written, bytes 0..1999 */
    r = create_and_export(NDFS_TMPL_SMD_75MB, 0, &img, &size, NDFS_BOOTCODE_AUTO);
    if (r) return r;
    blob = ndfs_boot_blob(NDFS_BOOT_BLOB_SMD, &n);
    TEST_ASSERT_EQUAL(2000, n);
    TEST_ASSERT(memcmp(img, blob, 2000) == 0);
    TEST_ASSERT_EQUAL(0xD1, img[0]); /* first word PIOF = 0150405 octal = 0xD105, as in every real hard disk boot area */
    TEST_ASSERT_EQUAL(0x05, img[1]);
    free(img); img = NULL;

    /* NDFS_BOOTCODE_NONE leaves the area zero */
    r = create_and_export(NDFS_TMPL_SMD_75MB, 0, &img, &size, NDFS_BOOTCODE_NONE);
    if (r) return r;
    for (i = 0; i < 2000; i++) TEST_ASSERT_EQUAL(0, img[i]);
    free(img); img = NULL;

    /* own bytes win */
    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    opts.boot_data = own; opts.boot_size = sizeof(own);
    strcpy(opts.directory_name, "OWNBOOT");
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &img, &size));
    ndfs_close(fs);
    TEST_ASSERT(memcmp(img, own, sizeof(own)) == 0);
    TEST_ASSERT_EQUAL(0, img[8]);
    free(img);
    return 0;
}

static int test_scsi_last_page_written(void)
{
    uint8_t *img = NULL; size_t size = 0;
    int r = create_and_export(NDFS_TMPL_SCSI_126MB, 0, &img, &size, NDFS_BOOTCODE_NONE);
    if (r) return r;
    TEST_ASSERT_EQUAL(64656u * NDFS_PAGE_SIZE, size);
    TEST_ASSERT(memcmp(img + (size_t)64655 * NDFS_PAGE_SIZE, ndfs_scsi_last_page(), NDFS_PAGE_SIZE) == 0);
    free(img);
    return 0;
}

static int test_disk_type_table(void)
{
    uint32_t pages = 0;
    size_t i;

    /* Sizes read from SINTRAN's device-name table. */
    TEST_ASSERT_EQUAL(40, ndfs_disk_type_count());
    TEST_ASSERT_OK(ndfs_disk_type_find("DISC-75MB", &pages));
    TEST_ASSERT_EQUAL(36945, pages);
    TEST_ASSERT_OK(ndfs_disk_type_find("disc-74mb", &pages));   /* case-insensitive */
    TEST_ASSERT_EQUAL(36396, pages);
    TEST_ASSERT_OK(ndfs_disk_type_find("DISC-450MB-F", &pages));
    TEST_ASSERT_EQUAL(220584, pages);
    TEST_ASSERT_OK(ndfs_disk_type_find("DISC-14MB", &pages));
    TEST_ASSERT_EQUAL(6912, pages);
    TEST_ASSERT_OK(ndfs_disk_type_find("DISC-90MB", &pages));
    TEST_ASSERT_EQUAL(7389, pages);

    /* Not in the table: SCSI sizes are set from the drive, unit-numbered names are not
     * base names, empty and unknown names. */
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_disk_type_find("DISC-SCSI-1", &pages));
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_disk_type_find("DISC-75MB-1", &pages));
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_disk_type_find("DISC-75", &pages));
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_disk_type_find("", &pages));
    TEST_ASSERT_EQUAL(NDFS_ERR_NULL_PTR, ndfs_disk_type_find(NULL, &pages));
    TEST_ASSERT_EQUAL(NDFS_ERR_NULL_PTR, ndfs_disk_type_find("DISC-75MB", NULL));

    /* Family, tested flag and "size not known" entries. */
    {
        const ndfs_disk_type_t *t = NULL;
        size_t k;
        for (k = 0; k < ndfs_disk_type_count(); k++) {
            if (strcmp(ndfs_disk_type_get(k)->name, "DISC-74MB") == 0) t = ndfs_disk_type_get(k);
        }
        TEST_ASSERT(t != NULL);
        TEST_ASSERT_EQUAL(NDFS_DISK_FAMILY_WINCHESTER, t->family);
        TEST_ASSERT(t->tested);
        for (k = 0; k < ndfs_disk_type_count(); k++) {
            if (strcmp(ndfs_disk_type_get(k)->name, "DISC-75MB") == 0) t = ndfs_disk_type_get(k);
        }
        TEST_ASSERT_EQUAL(NDFS_DISK_FAMILY_SMD, t->family);
        TEST_ASSERT(t->tested);
        for (k = 0; k < ndfs_disk_type_count(); k++) {
            if (strcmp(ndfs_disk_type_get(k)->name, "DISC-30MB") == 0) t = ndfs_disk_type_get(k);
        }
        TEST_ASSERT(!t->tested); /* SINTRAN halted when asked to create it in the emulator */
    }
    /* Valid name, size not known: found, pages 0. */
    TEST_ASSERT_OK(ndfs_disk_type_find("DISC-10MB", &pages));
    TEST_ASSERT_EQUAL(0, pages);

    TEST_ASSERT(ndfs_disk_type_get(0) != NULL);
    TEST_ASSERT(ndfs_disk_type_get(ndfs_disk_type_count()) == NULL);

    /* Every entry can be found by its own name and is large enough for the custom path. */
    for (i = 0; i < ndfs_disk_type_count(); i++) {
        const ndfs_disk_type_t *t = ndfs_disk_type_get(i);
        TEST_ASSERT_OK(ndfs_disk_type_find(t->name, &pages));
        TEST_ASSERT_EQUAL(t->pages, pages);
        TEST_ASSERT(pages == 0 || pages >= 32);
    }
    return 0;
}

static int test_create_from_disk_type(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    const ndfs_master_block_t *mb;
    uint32_t pages = 0;
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    int r;

    /* What ndtool --create DISC-14MB does: look up the size, create a custom image. */
    TEST_ASSERT_OK(ndfs_disk_type_find("DISC-14MB", &pages));
    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_CUSTOM;
    opts.custom_pages = pages;
    opts.include_extended_info = true;
    strcpy(opts.directory_name, "DISC14");

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    r = verify_image_basics(fs, "DISC14", 6912);
    if (r != 0) { ndfs_close(fs); return r; }

    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    TEST_ASSERT_EQUAL(6912, mb->ext_pages_available);

    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));
    TEST_ASSERT_EQUAL(6912u * NDFS_PAGE_SIZE, buf_size);
    free(buf);

    ndfs_close(fs);
    return 0;
}

static int test_disk_type_layout_pages(void)
{
    /* Full SINTRAN layout lengths (DT table); 0 where the name maps to no single DT entry. */
    TEST_ASSERT_EQUAL(140733, ndfs_disk_type_by_name("DISC-288MB-R")->layout_pages);
    TEST_ASSERT_EQUAL(153576, ndfs_disk_type_by_name("DISC-288MB-F")->layout_pages);
    TEST_ASSERT_EQUAL(230364, ndfs_disk_type_by_name("DISC-450MB-F")->layout_pages);
    TEST_ASSERT_EQUAL(223820, ndfs_disk_type_by_name("DISC-450MB-N")->layout_pages);
    TEST_ASSERT_EQUAL(37035,  ndfs_disk_type_by_name("DISC-75MB")->layout_pages);
    TEST_ASSERT_EQUAL(0,      ndfs_disk_type_by_name("DISC-30MB")->layout_pages);
    TEST_ASSERT_EQUAL(0,      ndfs_disk_type_by_name("DISC-140MB-F")->layout_pages);
    return 0;
}

/* The device is longer than the directory size (DISC-288MB-F: 153576 pages, directory 140391).
 * SINTRAN sizes the bit file from the directory size: 9 pages, not the 10 that the device
 * length would give. The page after the bit file must stay untouched by bit file writes,
 * and fsck must not call page bit_file+9 a referenced-but-free block. */
static int test_bit_file_sized_from_capacity(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    const ndfs_master_block_t *mb;
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    uint32_t after;
    char *report = NULL;
    int fsck_errors = -1;
    static const uint8_t marker[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t content[16] = { 0 };
    size_t i;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_CUSTOM;
    opts.custom_pages = 140391;
    opts.device_pages = 153576;
    opts.include_extended_info = true;
    strcpy(opts.directory_name, "BIGDEV");
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));

    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));
    TEST_ASSERT_EQUAL(153576u * NDFS_PAGE_SIZE, buf_size);
    free(buf);

    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    after = mb->bit_file_ptr.block_id + 9;   /* first page past a 9 page bit file */

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/BIG:DATA", marker, sizeof(marker)));

    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));
    for (i = 0; i < NDFS_PAGE_SIZE; i++) {
        if (buf[(size_t)after * NDFS_PAGE_SIZE + i] != content[0]) {
            free(buf);
            ndfs_close(fs);
            TEST_ASSERT(0 && "page after the bit file was written");
        }
    }
    free(buf);

    TEST_ASSERT_OK(ndfs_fsck(fs, &report, &fsck_errors));
    TEST_ASSERT_EQUAL(0, fsck_errors);
    free(report);
    ndfs_close(fs);
    return 0;
}

static int test_device_pages_smaller_than_capacity_ignored(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    uint8_t *buf = NULL;
    size_t buf_size = 0;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_CUSTOM;
    opts.custom_pages = 6912;
    opts.device_pages = 100;
    opts.include_extended_info = true;
    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));
    TEST_ASSERT_EQUAL(6912u * NDFS_PAGE_SIZE, buf_size);
    free(buf);
    ndfs_close(fs);
    return 0;
}

static int test_create_custom(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    uint32_t free_pages = 0;
    int r;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_CUSTOM;
    opts.custom_pages = 100;
    strcpy(opts.directory_name, "CUSTOM");

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_NOT_NULL(fs);

    r = verify_image_basics(fs, "CUSTOM", 100);
    if (r != 0) { ndfs_close(fs); return r; }

    TEST_ASSERT_OK(ndfs_get_free_pages(fs, &free_pages));
    TEST_ASSERT(free_pages > 80); /* Most pages should be free */

    ndfs_close(fs);
    return 0;
}

static int test_create_custom_too_small(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_CUSTOM;
    opts.custom_pages = 10; /* Too small */

    TEST_ASSERT_EQUAL(NDFS_ERR_TOO_SMALL, ndfs_create_image(&fs, &opts));
    return 0;
}

static int test_create_null_args(void)
{
    ndfs_image_options_t opts;
    ndfs_filesystem_t *fs = NULL;

    ndfs_image_options_init(&opts);
    TEST_ASSERT_EQUAL(NDFS_ERR_NULL_PTR, ndfs_create_image(NULL, &opts));
    TEST_ASSERT_EQUAL(NDFS_ERR_NULL_PTR, ndfs_create_image(&fs, NULL));
    return 0;
}

static int test_write_to_created_image(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    uint8_t *data = NULL;
    size_t size = 0;
    const char *content = "Hello from created image!";

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    strcpy(opts.directory_name, "WRTEST");

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));

    /* Write a file */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/TEST:DATA",
                                   (const uint8_t *)content, strlen(content)));

    /* Read it back */
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/TEST:DATA", &data, &size));
    TEST_ASSERT_EQUAL(strlen(content), size);
    TEST_ASSERT(memcmp(data, content, size) == 0);

    ndfs_free_data(data);
    ndfs_close(fs);
    return 0;
}

static int test_create_and_export(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_filesystem_t *fs2 = NULL;
    ndfs_image_options_t opts;
    uint8_t *exported = NULL;
    size_t exported_size = 0;
    char name[NDFS_NAME_MAX + 1];

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    strcpy(opts.directory_name, "EXPORT");

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));

    /* Write a file */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/HELLO:DATA",
                                   (const uint8_t *)"world", 5));

    /* Export */
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &exported, &exported_size));
    ndfs_close(fs);

    /* Re-open */
    TEST_ASSERT_OK(ndfs_open_buffer_copy(exported, exported_size, true, &fs2));
    free(exported);

    TEST_ASSERT_OK(ndfs_get_directory_name(fs2, name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("EXPORT", name);

    /* File should still be there */
    {
        uint8_t *data = NULL;
        size_t sz = 0;
        TEST_ASSERT_OK(ndfs_read_file(fs2, "SYSTEM/HELLO:DATA", &data, &sz));
        TEST_ASSERT_EQUAL(5, sz);
        TEST_ASSERT(memcmp(data, "world", 5) == 0);
        ndfs_free_data(data);
    }

    ndfs_close(fs2);
    return 0;
}

static int test_directory_name_uppercased(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;
    char name[NDFS_NAME_MAX + 1];

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    strcpy(opts.directory_name, "lowercase");

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_OK(ndfs_get_directory_name(fs, name, sizeof(name)));
    TEST_ASSERT_EQUAL_STRING("LOWERCASE", name);

    ndfs_close(fs);
    return 0;
}

void run_image_creator_tests(void)
{
    TEST_SUITE_BEGIN("Image Creator Tests");
    RUN_TEST(test_create_floppy_360kb);
    RUN_TEST(test_create_floppy_12mb);
    RUN_TEST(test_create_smd_75mb);
    RUN_TEST(test_create_winchester_74mb);
    RUN_TEST(test_create_scsi_126mb);
    RUN_TEST(test_bit_file_answers_smd);
    RUN_TEST(test_unreserved_pages_formula);
    RUN_TEST(test_pages_above_capacity_marked_used);
    RUN_TEST(test_floppy_has_no_extended_info);
    RUN_TEST(test_boot_code_written_and_override);
    RUN_TEST(test_scsi_last_page_written);
    RUN_TEST(test_disk_type_table);
    RUN_TEST(test_create_from_disk_type);
    RUN_TEST(test_disk_type_layout_pages);
    RUN_TEST(test_bit_file_sized_from_capacity);
    RUN_TEST(test_device_pages_smaller_than_capacity_ignored);
    RUN_TEST(test_create_custom);
    RUN_TEST(test_create_custom_too_small);
    RUN_TEST(test_create_null_args);
    RUN_TEST(test_write_to_created_image);
    RUN_TEST(test_create_and_export);
    RUN_TEST(test_directory_name_uppercased);
}
