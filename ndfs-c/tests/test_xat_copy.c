/**
 * Tests for XAT (Extended Attribute) copy-across scenarios.
 *
 * Validates that XAT metadata and sparse data are preserved when
 * copying files across NDFS filesystem images.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "test_framework.h"
#include <ndfs/ndfs.h>
#include <string.h>

/* Helper: create a writable NDFS floppy image via image creator */
static ndfs_filesystem_t *create_test_fs(const char *dir_name)
{
    ndfs_image_options_t opts;
    ndfs_filesystem_t *fs = NULL;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    if (dir_name) {
        strncpy(opts.directory_name, dir_name, NDFS_NAME_MAX);
        opts.directory_name[NDFS_NAME_MAX] = '\0';
    }

    if (ndfs_create_image(&fs, &opts) != NDFS_OK) return NULL;
    return fs;
}

/* 1. Basic XAT round-trip: write -> get props -> serialize -> deserialize -> verify */
static int test_xat_copy_basic_roundtrip(void)
{
    ndfs_filesystem_t *fs;
    ndfs_xat_properties_t xat1, xat2;
    char *json = NULL;
    uint8_t data[] = {10, 20, 30, 40, 50};

    fs = create_test_fs("DISK1");
    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/HELLO:TEXT", data, sizeof(data)));
    TEST_ASSERT_OK(ndfs_get_file_properties(fs, "SYSTEM/HELLO:TEXT", &xat1));

    TEST_ASSERT_EQUAL_STRING("HELLO", xat1.object_name);
    TEST_ASSERT_EQUAL_STRING("TEXT", xat1.type);
    TEST_ASSERT_EQUAL_STRING("SYSTEM", xat1.user_name);

    /* Serialize to JSON and back */
    TEST_ASSERT_OK(ndfs_xat_serialize(&xat1, &json));
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &xat2));
    free(json);

    TEST_ASSERT_EQUAL_STRING("HELLO", xat2.object_name);
    TEST_ASSERT_EQUAL_STRING("TEXT", xat2.type);
    TEST_ASSERT_EQUAL_STRING("SYSTEM", xat2.user_name);
    TEST_ASSERT_EQUAL(xat1.user_index, xat2.user_index);
    TEST_ASSERT_EQUAL(xat1.access_bits, xat2.access_bits);
    TEST_ASSERT_EQUAL(xat1.file_type, xat2.file_type);

    /* Verify data read back */
    {
        uint8_t *read_data = NULL;
        size_t read_size = 0;
        TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/HELLO:TEXT", &read_data, &read_size));
        TEST_ASSERT_EQUAL(5, (int)read_size);
        TEST_ASSERT_EQUAL(10, read_data[0]);
        TEST_ASSERT_EQUAL(50, read_data[4]);
        ndfs_free_data(read_data);
    }

    ndfs_close(fs);
    return 0;
}

/* 2. Access bits preservation */
static int test_xat_copy_access_bits(void)
{
    ndfs_xat_properties_t xat, restored;
    ndfs_object_entry_t entry;
    char *json = NULL;

    memset(&entry, 0, sizeof(entry));
    ndfs_oe_init(&entry);
    strcpy(entry.object_name, "SECURE");
    strcpy(entry.type, "DATA");
    strcpy(entry.user_name, "SYSTEM");
    entry.access_bits = 0x7FFF;
    entry.file_type = 0;

    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_EQUAL(0x7FFF, xat.access_bits);

    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &restored));
    free(json);

    TEST_ASSERT_EQUAL(0x7FFF, restored.access_bits);

    /* Apply back to a new entry */
    {
        ndfs_object_entry_t new_entry;
        ndfs_oe_init(&new_entry);
        TEST_ASSERT_OK(ndfs_xat_to_object(&restored, &new_entry));
        TEST_ASSERT_EQUAL(0x7FFF, new_entry.access_bits);
    }

    return 0;
}

/* 3. File type flags preservation */
static int test_xat_copy_file_type_flags(void)
{
    ndfs_xat_properties_t xat, restored;
    char *json = NULL;

    memset(&xat, 0, sizeof(xat));
    strcpy(xat.object_name, "FLAGS");
    strcpy(xat.type, "DATA");
    xat.file_type_flags = 0x28; /* IndexedFile | AllocatedFile */

    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &restored));
    free(json);

    TEST_ASSERT_EQUAL(0x28, restored.file_type_flags);

    return 0;
}

/* 4. File type code preservation (SYMB = 2) */
static int test_xat_copy_file_type_code(void)
{
    ndfs_xat_properties_t xat, restored;
    ndfs_object_entry_t entry;
    char *json = NULL;

    ndfs_oe_init(&entry);
    strcpy(entry.object_name, "SYMBOLS");
    strcpy(entry.type, "SYMB");
    entry.file_type = 2;

    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_EQUAL(2, xat.file_type);

    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &restored));
    free(json);

    TEST_ASSERT_EQUAL(2, restored.file_type);

    /* Apply to new entry */
    {
        ndfs_object_entry_t new_entry;
        ndfs_oe_init(&new_entry);
        TEST_ASSERT_OK(ndfs_xat_to_object(&restored, &new_entry));
        TEST_ASSERT_EQUAL(2, new_entry.file_type);
    }

    return 0;
}

/* 5. User association preservation */
static int test_xat_copy_user_association(void)
{
    ndfs_xat_properties_t xat, restored;
    ndfs_object_entry_t entry;
    char *json = NULL;

    ndfs_oe_init(&entry);
    strcpy(entry.object_name, "MYFILE");
    strcpy(entry.type, "TEXT");
    strcpy(entry.user_name, "GUEST");
    entry.user_index = 1;

    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat));
    TEST_ASSERT_EQUAL_STRING("GUEST", xat.user_name);
    TEST_ASSERT_EQUAL(1, xat.user_index);

    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &restored));
    free(json);

    TEST_ASSERT_EQUAL_STRING("GUEST", restored.user_name);
    TEST_ASSERT_EQUAL(1, restored.user_index);

    return 0;
}

/* 6. Date fields preservation */
static int test_xat_copy_date_fields(void)
{
    ndfs_xat_properties_t xat, restored;
    char *json = NULL;

    memset(&xat, 0, sizeof(xat));
    strcpy(xat.object_name, "DATED");
    strcpy(xat.type, "DATA");
    xat.date_created = 12345;
    xat.last_read_date = 55555;
    xat.last_write_date = 67890;

    TEST_ASSERT_OK(ndfs_xat_serialize(&xat, &json));
    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &restored));
    free(json);

    TEST_ASSERT_EQUAL(12345, (long)restored.date_created);
    TEST_ASSERT_EQUAL(55555, (long)restored.last_read_date);
    TEST_ASSERT_EQUAL(67890, (long)restored.last_write_date);

    return 0;
}

/* 7. Sparse file with XAT: 4 pages, pages 1 and 3 all zeros */
static int test_xat_copy_sparse_file(void)
{
    ndfs_filesystem_t *fs;
    ndfs_xat_properties_t xat;
    size_t file_size = 4 * NDFS_PAGE_SIZE;
    uint8_t *content;
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    size_t i;

    fs = create_test_fs("SPARSE");
    TEST_ASSERT_NOT_NULL(fs);

    content = (uint8_t *)calloc(file_size, 1);
    TEST_ASSERT_NOT_NULL(content);

    /* Page 0: 0xAA */
    for (i = 0; i < NDFS_PAGE_SIZE; i++) content[i] = 0xAA;
    /* Page 1: zeros */
    /* Page 2: 0xBB */
    for (i = 2 * NDFS_PAGE_SIZE; i < 3 * NDFS_PAGE_SIZE; i++) content[i] = 0xBB;
    /* Page 3: zeros */

    {
        /* Holes are declared, never inferred from the zero pages. */
        static const uint32_t hole_pages[] = { 1, 3 };
        ndfs_hole_list_t holes;
        holes.pages = hole_pages;
        holes.count = 2;
        TEST_ASSERT_OK(ndfs_write_file_holes(fs, "SYSTEM/SPARSE:DATA", content,
                                             file_size, NDFS_PARITY_NONE, &holes));
    }
    TEST_ASSERT_OK(ndfs_get_file_properties(fs, "SYSTEM/SPARSE:DATA", &xat));

    /* pages_in_file counts the pages ACTUALLY ALLOCATED, not the logical extent:
     * pages 0 and 2 hold data, pages 1 and 3 are sparse holes that occupy no
     * disk. Was 4 before the 2026-07-30 correction, which matched SINTRAN --
     * a real pack records 89 for a file whose index spans 99 slots with 10
     * holes. The logical extent is not lost, it is
     * ceil(bytes_in_file / NDFS_PAGE_SIZE), asserted on the next line. */
    TEST_ASSERT_EQUAL(2, (long)xat.pages_in_file);
    TEST_ASSERT_EQUAL((long)file_size, (long)xat.bytes_in_file);

    /* Read back and verify */
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/SPARSE:DATA", &read_data, &read_size));
    TEST_ASSERT_EQUAL((long)file_size, (long)read_size);

    for (i = 0; i < NDFS_PAGE_SIZE; i++) {
        TEST_ASSERT_EQUAL(0xAA, read_data[i]);
    }
    for (i = NDFS_PAGE_SIZE; i < 2 * NDFS_PAGE_SIZE; i++) {
        TEST_ASSERT_EQUAL(0, read_data[i]);
    }
    for (i = 2 * NDFS_PAGE_SIZE; i < 3 * NDFS_PAGE_SIZE; i++) {
        TEST_ASSERT_EQUAL(0xBB, read_data[i]);
    }
    for (i = 3 * NDFS_PAGE_SIZE; i < 4 * NDFS_PAGE_SIZE; i++) {
        TEST_ASSERT_EQUAL(0, read_data[i]);
    }

    ndfs_free_data(read_data);
    free(content);
    ndfs_close(fs);
    return 0;
}

/* 8. Large sparse file XAT: 10 pages, only first and last have data */
static int test_xat_copy_large_sparse(void)
{
    ndfs_filesystem_t *fs;
    ndfs_xat_properties_t xat;
    size_t file_size = 10 * NDFS_PAGE_SIZE;
    uint8_t *content;
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    size_t i;

    fs = create_test_fs("LSPARSE");
    TEST_ASSERT_NOT_NULL(fs);

    content = (uint8_t *)calloc(file_size, 1);
    TEST_ASSERT_NOT_NULL(content);

    /* First page: 0x11 */
    for (i = 0; i < NDFS_PAGE_SIZE; i++) content[i] = 0x11;
    /* Last page: 0xFF */
    for (i = 9 * NDFS_PAGE_SIZE; i < 10 * NDFS_PAGE_SIZE; i++) content[i] = 0xFF;

    {
        /* Pages 1..8 are declared holes; pages 0 and 9 carry data. */
        static const uint32_t hole_pages[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
        ndfs_hole_list_t holes;
        holes.pages = hole_pages;
        holes.count = 8;
        TEST_ASSERT_OK(ndfs_write_file_holes(fs, "SYSTEM/BIGSPARSE:DATA", content,
                                             file_size, NDFS_PARITY_NONE, &holes));
    }
    TEST_ASSERT_OK(ndfs_get_file_properties(fs, "SYSTEM/BIGSPARSE:DATA", &xat));

    /* Only the first and last pages hold data; the 8 in between are sparse
     * holes occupying no disk. pages_in_file counts allocated pages, so 2 --
     * see the note in test_xat_copy_sparse_file. */
    TEST_ASSERT_EQUAL(2, (long)xat.pages_in_file);

    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/BIGSPARSE:DATA", &read_data, &read_size));
    TEST_ASSERT_EQUAL((long)file_size, (long)read_size);

    for (i = 0; i < NDFS_PAGE_SIZE; i++) {
        TEST_ASSERT_EQUAL(0x11, read_data[i]);
    }
    for (i = NDFS_PAGE_SIZE; i < 9 * NDFS_PAGE_SIZE; i++) {
        TEST_ASSERT_EQUAL(0, read_data[i]);
    }
    for (i = 9 * NDFS_PAGE_SIZE; i < 10 * NDFS_PAGE_SIZE; i++) {
        TEST_ASSERT_EQUAL(0xFF, read_data[i]);
    }

    ndfs_free_data(read_data);
    free(content);
    ndfs_close(fs);
    return 0;
}

/* 9. Mixed content sparse: alternating data/zero pages */
static int test_xat_copy_mixed_sparse(void)
{
    ndfs_filesystem_t *fs;
    size_t num_pages = 6;
    size_t file_size = num_pages * NDFS_PAGE_SIZE;
    uint8_t *content;
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    size_t p, i;

    fs = create_test_fs("MIXD");
    TEST_ASSERT_NOT_NULL(fs);

    content = (uint8_t *)calloc(file_size, 1);
    TEST_ASSERT_NOT_NULL(content);

    /* Even pages: data, Odd pages: zeros */
    for (p = 0; p < num_pages; p++) {
        if (p % 2 == 0) {
            uint8_t fill = (uint8_t)(((p + 1) * 0x10) & 0xFF);
            for (i = 0; i < NDFS_PAGE_SIZE; i++) {
                content[p * NDFS_PAGE_SIZE + i] = fill;
            }
        }
    }

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/MIXED:DATA", content, file_size));

    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/MIXED:DATA", &read_data, &read_size));
    TEST_ASSERT_EQUAL((long)file_size, (long)read_size);

    for (i = 0; i < file_size; i++) {
        TEST_ASSERT_EQUAL(content[i], read_data[i]);
    }

    ndfs_free_data(read_data);
    free(content);
    ndfs_close(fs);
    return 0;
}

/* 10. XAT with overwrite: apply file A's XAT to file B (via xat_to_object) */
static int test_xat_copy_overwrite(void)
{
    ndfs_xat_properties_t xat_a;
    ndfs_object_entry_t entry_a, entry_b;

    ndfs_oe_init(&entry_a);
    strcpy(entry_a.object_name, "FILEA");
    strcpy(entry_a.type, "TEXT");
    entry_a.access_bits = 0x1234;
    entry_a.file_type = 3;
    entry_a.pages_in_file = 1;
    entry_a.bytes_in_file = 100;

    TEST_ASSERT_OK(ndfs_xat_from_object(&entry_a, &xat_a));

    /* Create entry B with different content metadata */
    ndfs_oe_init(&entry_b);
    strcpy(entry_b.object_name, "FILEB");
    strcpy(entry_b.type, "TEXT");
    entry_b.access_bits = 0;
    entry_b.file_type = 0;
    entry_b.pages_in_file = 5;
    entry_b.bytes_in_file = 500;

    /* Apply A's XAT to B */
    TEST_ASSERT_OK(ndfs_xat_to_object(&xat_a, &entry_b));

    /* Status bits from A */
    TEST_ASSERT_EQUAL(0x1234, entry_b.access_bits);
    TEST_ASSERT_EQUAL(3, entry_b.file_type);

    /* Name from A (xat_to_object copies name if non-empty) */
    TEST_ASSERT_EQUAL_STRING("FILEA", entry_b.object_name);

    return 0;
}

/* 11. Copy between two images: write on fs1, read props, create on fs2, verify props */
static int test_xat_copy_between_images(void)
{
    ndfs_filesystem_t *fs1, *fs2;
    ndfs_xat_properties_t xat1, xat2;
    uint8_t data1[] = {65, 66};
    uint8_t data2[] = {67, 68, 69};
    uint8_t *rd = NULL;
    size_t rd_sz = 0;

    fs1 = create_test_fs("SRC");
    TEST_ASSERT_NOT_NULL(fs1);

    fs2 = create_test_fs("DST");
    TEST_ASSERT_NOT_NULL(fs2);

    /* Write files on fs1 */
    TEST_ASSERT_OK(ndfs_write_file(fs1, "SYSTEM/FILE1:TEXT", data1, sizeof(data1)));
    TEST_ASSERT_OK(ndfs_write_file(fs1, "SYSTEM/FILE2:DATA", data2, sizeof(data2)));

    /* Extract properties from fs1 */
    TEST_ASSERT_OK(ndfs_get_file_properties(fs1, "SYSTEM/FILE1:TEXT", &xat1));
    TEST_ASSERT_OK(ndfs_get_file_properties(fs1, "SYSTEM/FILE2:DATA", &xat2));

    TEST_ASSERT_EQUAL_STRING("FILE1", xat1.object_name);
    TEST_ASSERT_EQUAL_STRING("TEXT", xat1.type);
    TEST_ASSERT_EQUAL_STRING("FILE2", xat2.object_name);
    TEST_ASSERT_EQUAL_STRING("DATA", xat2.type);

    /* Write same files on fs2 */
    TEST_ASSERT_OK(ndfs_write_file(fs2, "SYSTEM/FILE1:TEXT", data1, sizeof(data1)));
    TEST_ASSERT_OK(ndfs_write_file(fs2, "SYSTEM/FILE2:DATA", data2, sizeof(data2)));

    /* Verify properties match on fs2 */
    {
        ndfs_xat_properties_t xat1b, xat2b;
        TEST_ASSERT_OK(ndfs_get_file_properties(fs2, "SYSTEM/FILE1:TEXT", &xat1b));
        TEST_ASSERT_OK(ndfs_get_file_properties(fs2, "SYSTEM/FILE2:DATA", &xat2b));

        TEST_ASSERT_EQUAL_STRING("FILE1", xat1b.object_name);
        TEST_ASSERT_EQUAL_STRING("TEXT", xat1b.type);
        TEST_ASSERT_EQUAL_STRING("FILE2", xat2b.object_name);
        TEST_ASSERT_EQUAL_STRING("DATA", xat2b.type);
    }

    /* Verify data on fs2 */
    TEST_ASSERT_OK(ndfs_read_file(fs2, "SYSTEM/FILE1:TEXT", &rd, &rd_sz));
    TEST_ASSERT_EQUAL(2, (int)rd_sz);
    TEST_ASSERT_EQUAL(65, rd[0]);
    TEST_ASSERT_EQUAL(66, rd[1]);
    ndfs_free_data(rd);
    rd = NULL;

    TEST_ASSERT_OK(ndfs_read_file(fs2, "SYSTEM/FILE2:DATA", &rd, &rd_sz));
    TEST_ASSERT_EQUAL(3, (int)rd_sz);
    TEST_ASSERT_EQUAL(67, rd[0]);
    TEST_ASSERT_EQUAL(69, rd[2]);
    ndfs_free_data(rd);

    ndfs_close(fs1);
    ndfs_close(fs2);
    return 0;
}

/* 12. Status bits survive re-write (via XAT round-trip) */
static int test_xat_copy_status_survive_rewrite(void)
{
    ndfs_xat_properties_t xat1, xat2;
    ndfs_object_entry_t entry;
    char *json = NULL;

    ndfs_oe_init(&entry);
    strcpy(entry.object_name, "PERSIST");
    strcpy(entry.type, "DATA");
    entry.access_bits = 0x0FFF;
    entry.file_type = 5;

    TEST_ASSERT_OK(ndfs_xat_from_object(&entry, &xat1));
    TEST_ASSERT_OK(ndfs_xat_serialize(&xat1, &json));
    TEST_ASSERT_OK(ndfs_xat_deserialize(json, &xat2));
    free(json);

    /* Simulate re-write: new entry with different data-level metadata */
    {
        ndfs_object_entry_t new_entry;
        ndfs_oe_init(&new_entry);
        strcpy(new_entry.object_name, "PERSIST");
        strcpy(new_entry.type, "DATA");
        new_entry.pages_in_file = 10;
        new_entry.bytes_in_file = 20000;
        new_entry.access_bits = 0; /* will be overwritten */

        TEST_ASSERT_OK(ndfs_xat_to_object(&xat2, &new_entry));

        /* Status bits from original */
        TEST_ASSERT_EQUAL(0x0FFF, new_entry.access_bits);
        TEST_ASSERT_EQUAL(5, new_entry.file_type);
    }

    return 0;
}

/* 13. XAT filename convention */
static int test_xat_copy_filename_convention(void)
{
    char buf[256];

    ndfs_xat_filename("README.TEXT", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("README.TEXT.xat", buf);

    ndfs_xat_filename("foo", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("foo.xat", buf);

    TEST_ASSERT(ndfs_xat_is_xat_file("README.TEXT.xat") == true);
    TEST_ASSERT(ndfs_xat_is_xat_file("README.TEXT") == false);

    return 0;
}

/* ---- Suite runner ---- */

void run_xat_copy_tests(void)
{
    TEST_SUITE_BEGIN("XAT Copy-Across Tests");

    RUN_TEST(test_xat_copy_basic_roundtrip);
    RUN_TEST(test_xat_copy_access_bits);
    RUN_TEST(test_xat_copy_file_type_flags);
    RUN_TEST(test_xat_copy_file_type_code);
    RUN_TEST(test_xat_copy_user_association);
    RUN_TEST(test_xat_copy_date_fields);
    RUN_TEST(test_xat_copy_sparse_file);
    RUN_TEST(test_xat_copy_large_sparse);
    RUN_TEST(test_xat_copy_mixed_sparse);
    RUN_TEST(test_xat_copy_overwrite);
    RUN_TEST(test_xat_copy_between_images);
    RUN_TEST(test_xat_copy_status_survive_rewrite);
    RUN_TEST(test_xat_copy_filename_convention);
}
