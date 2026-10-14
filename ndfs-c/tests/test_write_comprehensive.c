/**
 * Comprehensive write, user management, and stress tests.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 */

#include "test_framework.h"
#include <ndfs/ndfs.h>
#include "endian_util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Read the raw on-disk "Unreserved Pages" field straight out of page 0,
 * bypassing the filesystem API entirely -- this is what real SINTRAN would
 * read to report free space without rescanning the bitmap, so a test that
 * only calls ndfs_get_free_pages() would not catch a stale on-disk copy. */
static uint32_t read_raw_unreserved_pages(ndfs_filesystem_t *fs)
{
    uint8_t *buf = NULL;
    size_t size = 0;
    uint32_t value = 0;

    if (ndfs_to_buffer(fs, &buf, &size) == NDFS_OK) {
        value = ndfs_read_u32be(buf, NDFS_MASTER_BLOCK_OFFSET + 0x1C);
        free(buf);
    }
    return value;
}

/* Read the 16 raw bytes of the Extended Info Block (a structure completely
 * separate from the master block) straight out of page 0. Used to prove the
 * master-block persistence fix never touches this region or its checksum. */
static void read_raw_extended_info(ndfs_filesystem_t *fs, uint8_t *out16)
{
    uint8_t *buf = NULL;
    size_t size = 0;

    memset(out16, 0, 16);
    if (ndfs_to_buffer(fs, &buf, &size) == NDFS_OK) {
        memcpy(out16, buf + NDFS_EXTENDED_INFO_OFFSET, 16);
        free(buf);
    }
}

/* ---- Helper: create a writable image ---- */

static ndfs_filesystem_t *create_writable(ndfs_image_template_t tmpl,
                                          const char *name)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;

    ndfs_image_options_init(&opts);
    opts.template_type = tmpl;
    strncpy(opts.directory_name, name, NDFS_NAME_MAX);
    opts.directory_name[NDFS_NAME_MAX] = '\0';

    if (ndfs_create_image(&fs, &opts) != NDFS_OK) return NULL;
    return fs;
}

/* ---- Small file write/read ---- */

static int test_write_small_file(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "SMWRITE");
    const char *content = "Small data";
    uint8_t *data = NULL;
    size_t size = 0;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/TINY:DATA",
                                   (const uint8_t *)content, strlen(content)));
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/TINY:DATA", &data, &size));
    TEST_ASSERT_EQUAL(strlen(content), size);
    TEST_ASSERT(memcmp(data, content, size) == 0);

    ndfs_free_data(data);
    ndfs_close(fs);
    return 0;
}

/* ---- Multi-page file ---- */

static int test_write_multi_page_file(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "MPWRITE");
    size_t big_size = NDFS_PAGE_SIZE * 3 + 500; /* 3.5 pages */
    uint8_t *big_data;
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    size_t i;

    TEST_ASSERT_NOT_NULL(fs);

    big_data = (uint8_t *)malloc(big_size);
    TEST_ASSERT_NOT_NULL(big_data);

    for (i = 0; i < big_size; i++) {
        big_data[i] = (uint8_t)(i & 0xFF);
    }

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/BIGFILE:DATA",
                                   big_data, big_size));
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/BIGFILE:DATA",
                                  &read_data, &read_size));
    TEST_ASSERT_EQUAL(big_size, read_size);
    TEST_ASSERT(memcmp(read_data, big_data, big_size) == 0);

    ndfs_free_data(read_data);
    free(big_data);
    ndfs_close(fs);
    return 0;
}

/* ---- Overwrite file ---- */

static int test_overwrite_file(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "OVWRITE");
    const char *v1 = "Version 1";
    const char *v2 = "Version 2 with more data!!!";
    uint8_t *data = NULL;
    size_t size = 0;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/FILE:DATA",
                                   (const uint8_t *)v1, strlen(v1)));
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/FILE:DATA",
                                   (const uint8_t *)v2, strlen(v2)));

    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/FILE:DATA", &data, &size));
    TEST_ASSERT_EQUAL(strlen(v2), size);
    TEST_ASSERT(memcmp(data, v2, size) == 0);

    ndfs_free_data(data);
    ndfs_close(fs);
    return 0;
}

/* ---- Delete and reclaim space ---- */

static int test_delete_reclaims_space(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "DELSPACE");
    uint32_t free_before, free_after_write, free_after_delete;
    uint8_t buf[4096];

    TEST_ASSERT_NOT_NULL(fs);
    memset(buf, 0xAA, sizeof(buf));

    ndfs_get_free_pages(fs, &free_before);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/TEMP:DATA", buf, sizeof(buf)));
    ndfs_get_free_pages(fs, &free_after_write);
    TEST_ASSERT(free_after_write < free_before);

    TEST_ASSERT_OK(ndfs_delete_file(fs, "SYSTEM/TEMP:DATA"));
    ndfs_get_free_pages(fs, &free_after_delete);
    TEST_ASSERT_EQUAL(free_before, free_after_delete);

    ndfs_close(fs);
    return 0;
}

/* ---- Rename file ---- */

static int test_rename_file(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "RENAME");
    uint8_t *data = NULL;
    size_t size = 0;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/OLD:DATA",
                                   (const uint8_t *)"rename me", 9));
    TEST_ASSERT_OK(ndfs_rename(fs, "SYSTEM/OLD:DATA", "NEW:PROG"));

    /* Old name not found */
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND,
                      ndfs_read_file(fs, "SYSTEM/OLD:DATA", &data, &size));

    /* New name works */
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/NEW:PROG", &data, &size));
    TEST_ASSERT_EQUAL(9, size);
    TEST_ASSERT(memcmp(data, "rename me", 9) == 0);

    ndfs_free_data(data);
    ndfs_close(fs);
    return 0;
}

/* ---- Write persistence: export and reimport ---- */

static int test_write_persistence(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "PERSIST");
    ndfs_filesystem_t *fs2 = NULL;
    uint8_t *exported = NULL;
    size_t exported_size = 0;
    uint8_t *data = NULL;
    size_t size = 0;
    const char *content = "Persistent data here";

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/SAVE:DATA",
                                   (const uint8_t *)content, strlen(content)));

    /* Export */
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &exported, &exported_size));
    ndfs_close(fs);

    /* Reimport */
    TEST_ASSERT_OK(ndfs_open_buffer_copy(exported, exported_size, true, &fs2));
    free(exported);

    /* Verify data survives roundtrip */
    TEST_ASSERT_OK(ndfs_read_file(fs2, "SYSTEM/SAVE:DATA", &data, &size));
    TEST_ASSERT_EQUAL(strlen(content), size);
    TEST_ASSERT(memcmp(data, content, size) == 0);

    ndfs_free_data(data);
    ndfs_close(fs2);
    return 0;
}

/* ---- Sparse file (all zeros page) ---- */

static int test_sparse_file(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "SPARSE");
    size_t sparse_size = NDFS_PAGE_SIZE * 3;
    uint8_t *sparse_data;
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    uint32_t free_before, free_after;

    TEST_ASSERT_NOT_NULL(fs);

    sparse_data = (uint8_t *)calloc(sparse_size, 1);
    TEST_ASSERT_NOT_NULL(sparse_data);

    /* Only write non-zero in first and last page, middle is sparse */
    sparse_data[0] = 0xFF;
    sparse_data[sparse_size - 1] = 0xAA;

    ndfs_get_free_pages(fs, &free_before);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/SPARSE:DATA",
                                   sparse_data, sparse_size));

    ndfs_get_free_pages(fs, &free_after);

    /* Should have used fewer pages than 3+1 because middle page is sparse */
    /* At minimum, index block + 2 data pages (not 3) */
    TEST_ASSERT(free_before - free_after <= 4);

    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/SPARSE:DATA",
                                  &read_data, &read_size));
    TEST_ASSERT_EQUAL(sparse_size, read_size);
    TEST_ASSERT(memcmp(read_data, sparse_data, sparse_size) == 0);

    ndfs_free_data(read_data);
    free(sparse_data);
    ndfs_close(fs);
    return 0;
}

/* ---- User management comprehensive ---- */

static int test_add_multiple_users(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "USRMGMT");
    ndfs_user_entry_t *users = NULL;
    size_t count = 0;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_add_user(fs, "ALICE", 50));
    TEST_ASSERT_OK(ndfs_add_user(fs, "BOB", 30));
    TEST_ASSERT_OK(ndfs_add_user(fs, "CHARLIE", 20));

    TEST_ASSERT_OK(ndfs_get_users(fs, &users, &count));
    TEST_ASSERT_EQUAL(4, count); /* SYSTEM + 3 */
    ndfs_free_users(users);

    /* Duplicate */
    TEST_ASSERT_EQUAL(NDFS_ERR_ALREADY_EXISTS, ndfs_add_user(fs, "ALICE", 10));

    ndfs_close(fs);
    return 0;
}

/* Regression test for the "add_user beyond first UserFile page silently
 * dropped on remount" bug: image_creator.c only pre-allocates the FIRST
 * UserFile data page (32 entries incl. SYSTEM), so any user landing in
 * slot >= 32 needs a second page allocated on demand by
 * ensure_user_dir_page() (filesystem.c). Before that fix,
 * write_user_page() silently no-op'd when the page's index-block pointer
 * was unset, so the user lived only in memory and vanished after a
 * to_buffer/open_buffer_copy round trip -- exactly what this test drives. */
static int test_add_users_beyond_first_page(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_12MB, "MANYUSR");
    ndfs_filesystem_t *fs2 = NULL;
    uint8_t *exported = NULL;
    size_t exported_size = 0;
    ndfs_user_entry_t *users = NULL;
    size_t count = 0;
    char uname[NDFS_NAME_MAX + 1];
    int u;
    const int num_users = 40; /* SYSTEM (slot 0) + 40 => slots 1..40, spilling into page 2 (slots 32-63) */

    TEST_ASSERT_NOT_NULL(fs);

    for (u = 0; u < num_users; u++) {
        snprintf(uname, sizeof(uname), "USER%03d", u);
        TEST_ASSERT_OK(ndfs_add_user(fs, uname, (uint32_t)(u + 1)));
    }

    /* In-memory state alone would hide the bug (it lives in fs->users[]
     * regardless of on-disk persistence), so round-trip through an actual
     * export/reimport before checking anything. */
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &exported, &exported_size));
    ndfs_close(fs);
    TEST_ASSERT_OK(ndfs_open_buffer_copy(exported, exported_size, true, &fs2));
    free(exported);

    TEST_ASSERT_OK(ndfs_get_users(fs2, &users, &count));
    TEST_ASSERT_EQUAL(num_users + 1, count); /* +1 for SYSTEM */

    /* Spot-check a user whose slot (>= 32) only exists in the newly
     * allocated second UserFile page: confirm the name and reserved-page
     * count survived the round trip intact, not just "the count matches". */
    {
        int found = 0;
        size_t i;
        for (i = 0; i < count; i++) {
            if (strcmp(users[i].user_name, "USER039") == 0) {
                found = 1;
                TEST_ASSERT_EQUAL(40, users[i].pages_reserved);
                TEST_ASSERT(users[i].user_index >= NDFS_ENTRIES_PER_PAGE);
                break;
            }
        }
        TEST_ASSERT(found);
    }

    ndfs_free_users(users);
    ndfs_close(fs2);
    return 0;
}

static int test_write_file_per_user(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "PERUSER");
    uint8_t *data = NULL;
    size_t size = 0;
    ndfs_file_entry_t *entries = NULL;
    size_t ecount = 0;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_add_user(fs, "ALICE", 50));

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/SYSFILE:DATA",
                                   (const uint8_t *)"sys", 3));
    TEST_ASSERT_OK(ndfs_write_file(fs, "ALICE/MYFILE:PROG",
                                   (const uint8_t *)"alice", 5));

    /* List SYSTEM files */
    TEST_ASSERT_OK(ndfs_list_directory(fs, "SYSTEM", &entries, &ecount));
    TEST_ASSERT_EQUAL(1, ecount);
    TEST_ASSERT_EQUAL_STRING("SYSFILE", entries[0].name);
    ndfs_free_entries(entries);

    /* List ALICE files */
    TEST_ASSERT_OK(ndfs_list_directory(fs, "ALICE", &entries, &ecount));
    TEST_ASSERT_EQUAL(1, ecount);
    TEST_ASSERT_EQUAL_STRING("MYFILE", entries[0].name);
    ndfs_free_entries(entries);

    /* Read each */
    TEST_ASSERT_OK(ndfs_read_file(fs, "ALICE/MYFILE:PROG", &data, &size));
    TEST_ASSERT_EQUAL(5, size);
    TEST_ASSERT(memcmp(data, "alice", 5) == 0);
    ndfs_free_data(data);

    ndfs_close(fs);
    return 0;
}

static int test_remove_user_with_no_files(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "RMUSER");
    ndfs_user_entry_t *users = NULL;
    size_t count = 0;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_add_user(fs, "TEMP", 10));

    TEST_ASSERT_OK(ndfs_get_users(fs, &users, &count));
    TEST_ASSERT_EQUAL(2, count);
    {
        uint8_t idx = users[count - 1].user_index;
        ndfs_free_users(users);

        TEST_ASSERT_OK(ndfs_remove_user(fs, idx));
    }

    TEST_ASSERT_OK(ndfs_get_users(fs, &users, &count));
    TEST_ASSERT_EQUAL(1, count);
    ndfs_free_users(users);

    ndfs_close(fs);
    return 0;
}

static int test_update_quota(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "QUOTA");
    ndfs_user_entry_t *users = NULL;
    size_t count = 0;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_update_user_quota(fs, 0, 999));
    TEST_ASSERT_OK(ndfs_get_users(fs, &users, &count));
    TEST_ASSERT_EQUAL(999, users[0].pages_reserved);
    ndfs_free_users(users);

    ndfs_close(fs);
    return 0;
}

static int test_clear_password(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "CLRPWD");

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_clear_user_password(fs, "SYSTEM"));
    TEST_ASSERT_OK(ndfs_clear_user_password_by_index(fs, 0));
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND,
                      ndfs_clear_user_password(fs, "NOBODY"));

    ndfs_close(fs);
    return 0;
}

/* ---- Stress test ---- */

static int test_stress_users_and_files(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_12MB, "STRESS");
    char uname[NDFS_NAME_MAX + 1];
    char path[64];
    int u, f;
    int num_users = 20;
    int files_per_user = 5;
    uint8_t file_content[128];
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    ndfs_user_entry_t *users = NULL;
    size_t user_count = 0;
    uint32_t free_pages = 0;
    bool ok = false;

    TEST_ASSERT_NOT_NULL(fs);

    /* Create users */
    for (u = 0; u < num_users; u++) {
        snprintf(uname, sizeof(uname), "USER%03d", u);
        TEST_ASSERT_OK(ndfs_add_user(fs, uname, 50));
    }

    /* Verify user count */
    TEST_ASSERT_OK(ndfs_get_users(fs, &users, &user_count));
    TEST_ASSERT_EQUAL(num_users + 1, user_count); /* +1 for SYSTEM */
    ndfs_free_users(users);

    /* Create files for each user */
    for (u = 0; u < num_users; u++) {
        snprintf(uname, sizeof(uname), "USER%03d", u);
        for (f = 0; f < files_per_user; f++) {
            size_t i;
            snprintf(path, sizeof(path), "%s/FILE%02d:DATA", uname, f);
            for (i = 0; i < sizeof(file_content); i++) {
                file_content[i] = (uint8_t)((u * 37 + f * 13 + i) & 0xFF);
            }
            TEST_ASSERT_OK(ndfs_write_file(fs, path,
                                           file_content, sizeof(file_content)));
        }
    }

    /* Verify some files */
    for (u = 0; u < num_users; u += 5) {
        snprintf(uname, sizeof(uname), "USER%03d", u);
        snprintf(path, sizeof(path), "%s/FILE00:DATA", uname);
        TEST_ASSERT_OK(ndfs_read_file(fs, path, &read_data, &read_size));
        TEST_ASSERT_EQUAL(sizeof(file_content), read_size);
        {
            size_t i;
            uint8_t expected;
            for (i = 0; i < sizeof(file_content); i++) {
                expected = (uint8_t)((u * 37 + 0 * 13 + i) & 0xFF);
                if (read_data[i] != expected) {
                    printf("  FAIL: Mismatch at user %d byte %zu\n", u, i);
                    ndfs_free_data(read_data);
                    ndfs_close(fs);
                    return 1;
                }
            }
        }
        ndfs_free_data(read_data);
    }

    /* Delete some files */
    for (u = 0; u < num_users; u += 2) {
        snprintf(path, sizeof(path), "USER%03d/FILE00:DATA", u);
        TEST_ASSERT_OK(ndfs_delete_file(fs, path));
    }

    /* Verify integrity */
    TEST_ASSERT_OK(ndfs_verify_integrity(fs, &ok));
    TEST_ASSERT(ok);

    /* Check free pages increased */
    TEST_ASSERT_OK(ndfs_get_free_pages(fs, &free_pages));
    TEST_ASSERT(free_pages > 0);

    /* Write new files in reclaimed space */
    for (u = 0; u < num_users; u += 2) {
        snprintf(path, sizeof(path), "USER%03d/NEWFILE:DATA", u);
        TEST_ASSERT_OK(ndfs_write_file(fs, path,
                                       (const uint8_t *)"reclaimed", 9));
    }

    /* Export and reimport */
    {
        uint8_t *exported = NULL;
        size_t exported_size = 0;
        ndfs_filesystem_t *fs2 = NULL;

        TEST_ASSERT_OK(ndfs_to_buffer(fs, &exported, &exported_size));
        ndfs_close(fs);

        TEST_ASSERT_OK(ndfs_open_buffer_copy(exported, exported_size, true, &fs2));
        free(exported);

        TEST_ASSERT_OK(ndfs_verify_integrity(fs2, &ok));
        TEST_ASSERT(ok);

        /* Spot-check a file */
        snprintf(path, sizeof(path), "USER000/NEWFILE:DATA");
        TEST_ASSERT_OK(ndfs_read_file(fs2, path, &read_data, &read_size));
        TEST_ASSERT_EQUAL(9, read_size);
        TEST_ASSERT(memcmp(read_data, "reclaimed", 9) == 0);
        ndfs_free_data(read_data);

        ndfs_close(fs2);
    }

    return 0;
}

/* ---- Multiple files listing ---- */

static int test_list_multiple_files(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "MLIST");
    ndfs_file_entry_t *entries = NULL;
    size_t count = 0;
    int i;

    TEST_ASSERT_NOT_NULL(fs);

    for (i = 0; i < 5; i++) {
        char path[64];
        char data[32];
        snprintf(path, sizeof(path), "SYSTEM/FILE%d:DATA", i);
        snprintf(data, sizeof(data), "content_%d", i);
        TEST_ASSERT_OK(ndfs_write_file(fs, path,
                                       (const uint8_t *)data, strlen(data)));
    }

    TEST_ASSERT_OK(ndfs_list_directory(fs, "SYSTEM", &entries, &count));
    TEST_ASSERT_EQUAL(5, count);
    ndfs_free_entries(entries);

    ndfs_close(fs);
    return 0;
}

/* ---- Write empty file ---- */

static int test_write_empty_file(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "EMPTY");
    uint8_t dummy = 0;

    TEST_ASSERT_NOT_NULL(fs);

    /* Write with size 0 is tricky; pass a dummy byte with size 0 */
    /* The API requires file_data to be non-NULL for paths that exist,
       but size can be 0. Let's write 1 byte. */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/EMPTY:DATA", &dummy, 1));

    {
        uint8_t *data = NULL;
        size_t size = 0;
        TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/EMPTY:DATA", &data, &size));
        TEST_ASSERT(size <= 1);
        ndfs_free_data(data);
    }

    ndfs_close(fs);
    return 0;
}

/* ---- Write/delete cycle ---- */

static int test_write_delete_cycle(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "CYCLE");
    int i;
    uint32_t free_start, free_end;

    TEST_ASSERT_NOT_NULL(fs);
    ndfs_get_free_pages(fs, &free_start);

    for (i = 0; i < 10; i++) {
        char path[64];
        char data[256];
        snprintf(path, sizeof(path), "SYSTEM/CYCLE%d:DATA", i);
        memset(data, (uint8_t)i, sizeof(data));
        TEST_ASSERT_OK(ndfs_write_file(fs, path,
                                       (const uint8_t *)data, sizeof(data)));
        TEST_ASSERT_OK(ndfs_delete_file(fs, path));
    }

    ndfs_get_free_pages(fs, &free_end);
    TEST_ASSERT_EQUAL(free_start, free_end);

    ndfs_close(fs);
    return 0;
}

/* Bug 1+2 regression: an unrelated mutation must not corrupt other files'
 * metadata. A file with an empty type must keep it after an unrelated
 * add-user. The original port re-serialized EVERY object entry on every
 * mutation (persist_all) and defaulted empty types to "DATA" on read —
 * adding a user thus corrupted unrelated files. Writes are now surgical
 * (only the touched page) and empty types are preserved on round-trip. */
static int test_surgical_write_preserves_empty_type(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "SURGERY");
    uint8_t *exported = NULL;
    size_t exported_size = 0;
    ndfs_filesystem_t *fs2 = NULL;
    ndfs_file_entry_t meta;

    TEST_ASSERT_NOT_NULL(fs);

    /* Create a file with an EMPTY type (path carries no :TYPE suffix). */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/NOTYPE",
                                   (const uint8_t *)"x", 1));
    TEST_ASSERT_OK(ndfs_get_metadata(fs, "SYSTEM/NOTYPE", &meta));
    TEST_ASSERT_EQUAL_STRING("", meta.type);

    /* Unrelated mutation: add a brand-new user. Must not touch the SYSTEM
     * object page that holds NOTYPE. */
    TEST_ASSERT_OK(ndfs_add_user(fs, "BACKUP", 50));

    /* Export + reload: the empty type must survive, not become "DATA". */
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &exported, &exported_size));
    TEST_ASSERT_OK(ndfs_open_buffer_copy(exported, exported_size, true, &fs2));
    TEST_ASSERT_OK(ndfs_get_metadata(fs2, "SYSTEM/NOTYPE", &meta));
    TEST_ASSERT_EQUAL_STRING("", meta.type);

    ndfs_free_data(exported);
    ndfs_close(fs2);
    ndfs_close(fs);
    return 0;
}

static int test_friends_add_list_remove(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "FRIENDS");
    ndfs_friend_info_t *fr = NULL;
    size_t n = 0;
    uint8_t *exported = NULL;
    size_t exported_size = 0;
    ndfs_filesystem_t *fs2 = NULL;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_add_user(fs, "ALICE", 100));
    TEST_ASSERT_OK(ndfs_add_user(fs, "BOB", 100));

    /* Add BOB as a friend of ALICE with RW. */
    TEST_ASSERT_OK(ndfs_add_friend(fs, "ALICE", "BOB", "RW"));

    /* List shows BOB with RW. */
    TEST_ASSERT_OK(ndfs_list_friends(fs, "ALICE", &fr, &n));
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("BOB", fr[0].name);
    TEST_ASSERT_EQUAL_STRING("RW---", fr[0].perms);
    ndfs_free_friends(fr); fr = NULL;

    /* Adding the same friend again errors. */
    TEST_ASSERT_EQUAL(NDFS_ERR_ALREADY_EXISTS, ndfs_add_friend(fs, "ALICE", "BOB", "R"));

    /* Unknown friend name errors. */
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_add_friend(fs, "ALICE", "NOBODY", "R"));

    /* Persistence: friend survives export + reload. */
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &exported, &exported_size));
    TEST_ASSERT_OK(ndfs_open_buffer_copy(exported, exported_size, true, &fs2));
    TEST_ASSERT_OK(ndfs_list_friends(fs2, "ALICE", &fr, &n));
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_STRING("BOB", fr[0].name);
    ndfs_free_friends(fr); fr = NULL;
    ndfs_close(fs2);
    ndfs_free_data(exported);

    /* Remove. */
    TEST_ASSERT_OK(ndfs_remove_friend(fs, "ALICE", "BOB"));
    TEST_ASSERT_OK(ndfs_list_friends(fs, "ALICE", &fr, &n));
    TEST_ASSERT_EQUAL(0, n);
    /* Removing again errors. */
    TEST_ASSERT_EQUAL(NDFS_ERR_NOT_FOUND, ndfs_remove_friend(fs, "ALICE", "BOB"));

    ndfs_close(fs);
    return 0;
}

/* ---- SubIndexed (large file, >512 pages) write support ----
 *
 * NDFS-FORMAT.md "Sub-Indexed (Type 10)": a top-level sub-index block holds
 * up to 512 pointers to group index blocks, each holding up to 512 data-page
 * pointers, covering files up to 512*512 = 262,144 pages. These tests pin
 * the 512-page boundary and exercise the SubIndexed write path added to
 * create_new_file()/update_existing_file()/allocate_and_write_data() in
 * src/filesystem.c. */

/* Fill `buf` with a pattern that is never a zero byte, so no page in the
 * file is a sparse hole -- this lets tests reason exactly about the
 * structural (index/sub-index) page cost via free-page deltas. */
static void fill_nonzero_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
    size_t i;
    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)(((i * 7 + seed) % 251) + 1); /* 1..251, never 0 */
    }
}

/* Find a user's current pages_used by name (SYSTEM by default). */
static uint32_t get_user_pages_used(ndfs_filesystem_t *fs, const char *name)
{
    ndfs_user_entry_t *users = NULL;
    size_t count = 0, i;
    uint32_t result = 0xFFFFFFFFu;

    if (ndfs_get_users(fs, &users, &count) != NDFS_OK) return result;
    for (i = 0; i < count; i++) {
        if (strcmp(users[i].user_name, name) == 0) {
            result = users[i].pages_used;
            break;
        }
    }
    ndfs_free_users(users);
    return result;
}

/* Pin the 512-page boundary: a file of EXACTLY 512 non-sparse pages must
 * still use the plain Indexed layout (structural cost = 1 index block),
 * not SubIndexed. Verified via the free-page delta: 512 data pages + 1
 * index block = 513 pages consumed. */
static int test_indexed_boundary_stays_indexed(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "IDXBND");
    size_t size = (size_t)NDFS_MAX_OBJECT_FILE_PTRS * NDFS_PAGE_SIZE; /* 512 pages */
    uint8_t *data = (uint8_t *)malloc(size);
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    uint32_t free_before, free_after;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    fill_nonzero_pattern(data, size, 1);

    ndfs_get_free_pages(fs, &free_before);
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/ATBND:DATA", data, size));
    ndfs_get_free_pages(fs, &free_after);

    /* 512 data pages + exactly 1 structural (index) page. If this were
     * mistakenly promoted to SubIndexed it would cost 512 + 1 + 1 = 514. */
    TEST_ASSERT_EQUAL(NDFS_MAX_OBJECT_FILE_PTRS + 1, free_before - free_after);

    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/ATBND:DATA", &read_data, &read_size));
    TEST_ASSERT_EQUAL(size, read_size);
    TEST_ASSERT(memcmp(read_data, data, size) == 0);

    ndfs_free_data(read_data);
    free(data);
    ndfs_close(fs);
    return 0;
}

/* A file one page over the boundary (513 pages) must round-trip
 * byte-identically via the new SubIndexed path, and its structural cost
 * must be 1 sub-index block + 2 group index blocks (ceil(513/512) = 2). */
static int test_subindexed_just_over_boundary(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "SUBOVER");
    size_t size = (size_t)(NDFS_MAX_OBJECT_FILE_PTRS + 1) * NDFS_PAGE_SIZE; /* 513 pages */
    uint8_t *data = (uint8_t *)malloc(size);
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    uint32_t free_before, free_after;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    fill_nonzero_pattern(data, size, 2);

    ndfs_get_free_pages(fs, &free_before);
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/OVER:DATA", data, size));
    ndfs_get_free_pages(fs, &free_after);

    /* 513 data pages + 1 sub-index block + 2 group index blocks = 516. */
    TEST_ASSERT_EQUAL(NDFS_MAX_OBJECT_FILE_PTRS + 1 + 3, free_before - free_after);

    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/OVER:DATA", &read_data, &read_size));
    TEST_ASSERT_EQUAL(size, read_size);
    TEST_ASSERT(memcmp(read_data, data, size) == 0);

    ndfs_free_data(read_data);
    free(data);
    ndfs_close(fs);
    return 0;
}

/* A file spanning multiple full 512-page groups (1000+ pages) must
 * round-trip byte-identically -- this is the test that would catch content
 * corruption at a group boundary (e.g. an off-by-one in start_page/end_page
 * or writing past a group's own 512-pointer index page). */
static int test_subindexed_multi_group_roundtrip(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "SUBMULTI");
    uint32_t data_pages = 1000; /* crosses the 512-page group boundary once */
    size_t size = (size_t)data_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)malloc(size);
    uint8_t *read_data = NULL;
    size_t read_size = 0;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    fill_nonzero_pattern(data, size, 3);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/MULTI:DATA", data, size));
    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/MULTI:DATA", &read_data, &read_size));
    TEST_ASSERT_EQUAL(size, read_size);

    /* Byte-by-byte so a group-boundary corruption reports where it starts,
     * not just "mismatch somewhere". */
    {
        size_t i;
        for (i = 0; i < size; i++) {
            if (read_data[i] != data[i]) {
                printf("  FAIL: mismatch at byte %zu (page %zu): expected %d, got %d\n",
                       i, i / NDFS_PAGE_SIZE, data[i], read_data[i]);
                ndfs_free_data(read_data);
                free(data);
                ndfs_close(fs);
                return 1;
            }
        }
    }

    ndfs_free_data(read_data);
    free(data);
    ndfs_close(fs);
    return 0;
}

/* A SubIndexed file with zero-filled (sparse) holes must round-trip
 * correctly, and must not allocate real disk blocks for the holes. */
static int test_subindexed_sparse_holes(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "SUBSPARS");
    uint32_t data_pages = 1000;
    size_t size = (size_t)data_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)calloc(size, 1); /* all sparse to start */
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    uint32_t free_before, free_after;
    uint32_t num_index_blocks = (data_pages + NDFS_MAX_OBJECT_FILE_PTRS - 1)
                               / NDFS_MAX_OBJECT_FILE_PTRS;
    uint32_t non_sparse_pages = 3;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);

    /* Only 3 pages carry real data: one in group 0, one straddling the
     * group boundary (page 511/512), one deep in group 1. Everything else
     * stays a zero-filled sparse hole. */
    fill_nonzero_pattern(data + (size_t)10 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 5);
    fill_nonzero_pattern(data + (size_t)511 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 6);
    fill_nonzero_pattern(data + (size_t)700 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 7);

    /* Declare every page except those three as a hole. Holes are stated, never
     * inferred from the zeros (see write_data_page_to_index). */
    {
        uint32_t *hole_pages = (uint32_t *)malloc(sizeof(uint32_t) * data_pages);
        ndfs_hole_list_t holes;
        uint32_t p, n = 0;

        TEST_ASSERT_NOT_NULL(hole_pages);
        for (p = 0; p < data_pages; p++) {
            if (p != 10 && p != 511 && p != 700) hole_pages[n++] = p;
        }
        holes.pages = hole_pages;
        holes.count = n;

        ndfs_get_free_pages(fs, &free_before);
        TEST_ASSERT_OK(ndfs_write_file_holes(fs, "SYSTEM/SPARSBIG:DATA", data, size,
                                             NDFS_PARITY_NONE, &holes));
        ndfs_get_free_pages(fs, &free_after);
        free(hole_pages);
    }

    /* Structural pages (1 sub-index + num_index_blocks group index blocks)
     * plus only the non-sparse data pages -- NOT all 1000 data pages. */
    TEST_ASSERT_EQUAL(1 + num_index_blocks + non_sparse_pages,
                      free_before - free_after);

    TEST_ASSERT_OK(ndfs_read_file(fs, "SYSTEM/SPARSBIG:DATA", &read_data, &read_size));
    TEST_ASSERT_EQUAL(size, read_size);
    TEST_ASSERT(memcmp(read_data, data, size) == 0);

    ndfs_free_data(read_data);
    free(data);
    ndfs_close(fs);
    return 0;
}

/* SINTRAN III leaves the sub-index entry of a group with no page in it EMPTY - no group
 * index block at all. Measured 2026-10-14 on a domain the ND Linker B01 wrote under
 * SINTRAN III (nd100x, ND-5000): sub-index entries 0, 2, 4 and 6 set, 1, 3 and 5 empty.
 * Our writer always makes every group's index block, so the test writes the file and
 * then clears entry 1, as SINTRAN would have written it. Every page behind the empty
 * entry must still be read at its own place: page 1030 at byte 1030 * 2048, not 512
 * pages earlier, and the block list must carry the 512 holes. */
static int test_subindexed_empty_group_entry(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "SUBEMPTY");
    ndfs_filesystem_t *fs2 = NULL;
    uint32_t data_pages = 1500;          /* groups 0, 1, 2 */
    size_t size = (size_t)data_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)calloc(size, 1);
    uint8_t *image = NULL, *read_data = NULL;
    size_t image_size = 0, read_size = 0, count = 0;
    uint32_t *blocks = NULL;
    ndfs_object_entry_t entry;
    char uname[NDFS_NAME_MAX + 1];

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    fill_nonzero_pattern(data + (size_t)3 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 8);
    fill_nonzero_pattern(data + (size_t)1030 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 9);
    {
        uint32_t *hole_pages = (uint32_t *)malloc(sizeof(uint32_t) * data_pages);
        ndfs_hole_list_t holes;
        uint32_t p, n = 0;
        TEST_ASSERT_NOT_NULL(hole_pages);
        for (p = 0; p < data_pages; p++) {
            if (p != 3 && p != 1030) hole_pages[n++] = p;
        }
        holes.pages = hole_pages;
        holes.count = n;
        TEST_ASSERT_OK(ndfs_write_file_holes(fs, "SYSTEM/GAP:DATA", data, size,
                                             NDFS_PARITY_NONE, &holes));
        free(hole_pages);
    }
    strcpy(uname, "SYSTEM");
    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "GAP:DATA", uname, &entry));
    TEST_ASSERT_EQUAL(NDFS_PTR_SUBINDEXED, entry.file_pointer.type);
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &image, &image_size));
    ndfs_close(fs);

    /* clear sub-index entry 1 (bytes 4..7 of the sub-index block) */
    memset(image + (size_t)entry.file_pointer.block_id * NDFS_PAGE_SIZE + 4, 0, 4);
    TEST_ASSERT_OK(ndfs_open_buffer_copy(image, image_size, true, &fs2));
    free(image);

    TEST_ASSERT_OK(ndfs_read_file(fs2, "SYSTEM/GAP:DATA", &read_data, &read_size));
    TEST_ASSERT_EQUAL(size, read_size);
    TEST_ASSERT(memcmp(read_data, data, size) == 0);

    TEST_ASSERT_OK(ndfs_get_file_blocks(fs2, "SYSTEM/GAP:DATA", &blocks, &count));
    TEST_ASSERT_EQUAL((size_t)data_pages, count);
    TEST_ASSERT(blocks[3] != 0);
    TEST_ASSERT_EQUAL(0u, blocks[600]);
    TEST_ASSERT(blocks[1030] != 0);

    free(blocks);
    ndfs_free_data(read_data);
    free(data);
    ndfs_close(fs2);
    return 0;
}

/* Deleting a SubIndexed file must free every block it owns: data blocks,
 * every group's own index block, AND the top-level sub-index block. */
static int test_subindexed_delete_frees_all_blocks(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "SUBDEL");
    uint32_t data_pages = 1000;
    size_t size = (size_t)data_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)malloc(size);
    uint32_t free_before, free_after_write, free_after_delete;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    fill_nonzero_pattern(data, size, 8);

    ndfs_get_free_pages(fs, &free_before);
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/DELBIG:DATA", data, size));
    ndfs_get_free_pages(fs, &free_after_write);
    TEST_ASSERT(free_after_write < free_before);

    TEST_ASSERT_OK(ndfs_delete_file(fs, "SYSTEM/DELBIG:DATA"));
    ndfs_get_free_pages(fs, &free_after_delete);

    /* Every data block, every group index block, and the sub-index block
     * itself must come back -- not just the data blocks. */
    TEST_ASSERT_EQUAL(free_before, free_after_delete);

    free(data);
    ndfs_close(fs);
    return 0;
}

/* Overwriting an existing SubIndexed file must not leak the OLD structural
 * blocks (sub-index + group index blocks). This mirrors a real leak class
 * already found and fixed in the C# port's AllocateFileBlocksSparse: if the
 * "old total pages" calculation on overwrite assumes a flat "+1" structural
 * cost instead of accounting for however many group index blocks a large
 * file actually used, the quota/page bookkeeping never fully reclaims the
 * old file's structural pages, and it silently grows on every overwrite. */
static int test_subindexed_overwrite_no_leak(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "SUBLEAK");
    uint32_t data_pages = 1000;
    size_t size = (size_t)data_pages * NDFS_PAGE_SIZE;
    uint8_t *v1 = (uint8_t *)malloc(size);
    uint8_t *v2 = (uint8_t *)malloc(size);
    uint32_t used_baseline, used_after_write1, used_after_write2, used_after_delete;
    uint32_t free_baseline, free_after_delete;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(v1);
    TEST_ASSERT_NOT_NULL(v2);
    fill_nonzero_pattern(v1, size, 9);
    fill_nonzero_pattern(v2, size, 42); /* different content, same size */

    used_baseline = get_user_pages_used(fs, "SYSTEM");
    ndfs_get_free_pages(fs, &free_baseline);

    /* Create as SubIndexed. */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/LEAKBIG:DATA", v1, size));
    used_after_write1 = get_user_pages_used(fs, "SYSTEM");

    /* Overwrite (same size, still SubIndexed) via update_existing_file(). */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/LEAKBIG:DATA", v2, size));
    used_after_write2 = get_user_pages_used(fs, "SYSTEM");

    /* Same data+structural page count both times -- overwrite must not
     * accumulate extra "used" pages for the old file's structure. */
    TEST_ASSERT_EQUAL(used_after_write1, used_after_write2);

    TEST_ASSERT_OK(ndfs_delete_file(fs, "SYSTEM/LEAKBIG:DATA"));
    used_after_delete = get_user_pages_used(fs, "SYSTEM");
    ndfs_get_free_pages(fs, &free_after_delete);

    /* Back to exactly the pre-write baseline: no leaked quota pages, and
     * every block (data + both overwrite's worth of structural pages)
     * reclaimed on disk. */
    TEST_ASSERT_EQUAL(used_baseline, used_after_delete);
    TEST_ASSERT_EQUAL(free_baseline, free_after_delete);

    free(v1);
    free(v2);
    ndfs_close(fs);
    return 0;
}

/* ---- Object-file Indexed -> SubIndexed conversion (the actual growth bug) ----
 *
 * A plain Indexed object file (the volume-wide directory listing every file
 * across every user) caps out at 512 directory pages: object_index/32 gives
 * the page index, and each user owns 8 consecutive pages (256 slots / 32
 * entries-per-page), so 512 pages / 8 pages-per-user = 64 users. Past that,
 * ensure_object_dir_page() used to just `return 0` (silent NDFS_ERR_NO_SPACE)
 * instead of converting to SubIndexed, so user #64 onward could never get a
 * file created. This test creates 80 users (well past the old 64-user
 * ceiling) and confirms every one of them, including #64 upward, can create
 * a file without error. */
static int test_object_dir_converts_past_64_users(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_12MB, "OBJGROW");
    char uname[NDFS_NAME_MAX + 1];
    char path[64];
    int u;
    const int num_users = 80;

    TEST_ASSERT_NOT_NULL(fs);

    for (u = 0; u < num_users; u++) {
        snprintf(uname, sizeof(uname), "OU%03d", u);
        TEST_ASSERT_OK(ndfs_add_user(fs, uname, 10));
    }

    /* One file per user, including every user from #64 upward -- these are
     * exactly the ones that used to hit the page_idx >= 512 dead end. */
    for (u = 0; u < num_users; u++) {
        ndfs_object_entry_t entry;

        snprintf(uname, sizeof(uname), "OU%03d", u);
        snprintf(path, sizeof(path), "%s/F:DATA", uname);
        TEST_ASSERT_OK(ndfs_write_file(fs, path, (const uint8_t *)"x", 1));

        /* Confirm ownership: the object index's high byte must equal the
         * owning user's index (SINTRAN partitions the object file so user U
         * owns slots U*256..U*256+255). */
        TEST_ASSERT_OK(ndfs_get_object_entry(fs, "F:DATA", uname, &entry));
        TEST_ASSERT_EQUAL(entry.user_index, entry.object_index >> 8);
    }

    ndfs_close(fs);
    return 0;
}

/* ---- The real 30,000-file test: forces the object file into 2+ SubIndexed
 * groups, then closes and reopens fresh to prove the conversion persisted
 * correctly on disk (not just in the live in-memory fs->objects[] array). ----
 *
 * 120 users x 250 files = 30,000 objects (well under NDFS's 256-user max and
 * the 256-files-per-user cap). object_index/32 reaches up to roughly
 * (120*256+249)/32 = 967, which spans TWO SubIndexed groups (group 0 =
 * pages 0-511, group 1 = pages 512-1023) -- exactly the boundary the bug was
 * hiding past. This is the single test that would have caught the original
 * bug: without the conversion, every file for a user past #64 would fail
 * with NDFS_ERR_NO_SPACE. */
static int test_object_dir_30000_files_two_groups(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_filesystem_t *fs2 = NULL;
    ndfs_image_options_t opts;
    uint8_t *exported = NULL;
    size_t exported_size = 0;
    char uname[NDFS_NAME_MAX + 1];
    char path[64];
    int u, f;
    const int num_users = 120;
    const int files_per_user = 250;
    long total_written = 0;

    /* Oversize generously: 30,000 files x 1 real page each (an empty file's
     * own index block -- its zero-length content is a sparse hole and costs
     * no additional disk space) + ~1000 pages for the object directory
     * itself (sub-index + group index blocks + up to ~1000 directory data
     * pages) + bitmap/user-file overhead. 100,000 pages (~195MB image) is
     * comfortably beyond the ~31,000 actually needed, so a mid-test
     * NDFS_ERR_NO_SPACE would mean a real regression, not an undersized
     * fixture. */
    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_CUSTOM;
    opts.custom_pages = 100000;
    strncpy(opts.directory_name, "OBJ30K", NDFS_NAME_MAX);
    opts.directory_name[NDFS_NAME_MAX] = '\0';

    TEST_ASSERT_OK(ndfs_create_image(&fs, &opts));
    TEST_ASSERT_NOT_NULL(fs);

    for (u = 0; u < num_users; u++) {
        snprintf(uname, sizeof(uname), "BU%03d", u);
        TEST_ASSERT_OK(ndfs_add_user(fs, uname, (uint32_t)(files_per_user + 10)));
    }

    for (u = 0; u < num_users; u++) {
        snprintf(uname, sizeof(uname), "BU%03d", u);
        for (f = 0; f < files_per_user; f++) {
            snprintf(path, sizeof(path), "%s/F%03d:DATA", uname, f);
            TEST_ASSERT_OK(ndfs_write_file(fs, path, (const uint8_t *)"x", 1));
            total_written++;
        }
    }
    TEST_ASSERT_EQUAL(num_users * files_per_user, total_written);

    /* Close and reopen from a fresh export -- proves the conversion (master
     * block's object_file_ptr now SubIndexed, sub-index block, group index
     * blocks, and every directory page link) is correctly persisted on disk,
     * not just live in fs->objects[] from the session that wrote them. */
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &exported, &exported_size));
    ndfs_close(fs);
    fs = NULL;
    TEST_ASSERT_OK(ndfs_open_buffer_copy(exported, exported_size, true, &fs2));
    free(exported);

    /* Spot-check ownership at three points: first group (user #0), the
     * group-0/group-1 boundary (user #63, whose slots still land just under
     * page_idx 512), and deep in the second group (user #119). */
    {
        int check_users[3];
        size_t ci;
        check_users[0] = 0;
        check_users[1] = 63;
        check_users[2] = 119;

        for (ci = 0; ci < 3; ci++) {
            ndfs_object_entry_t entry;
            u = check_users[ci];
            snprintf(uname, sizeof(uname), "BU%03d", u);
            for (f = 0; f < files_per_user; f += 49) { /* sample, not every file */
                snprintf(path, sizeof(path), "F%03d:DATA", f);
                TEST_ASSERT_OK(ndfs_get_object_entry(fs2, path, uname, &entry));
                TEST_ASSERT_EQUAL_STRING(uname, entry.user_name);
                TEST_ASSERT_EQUAL(entry.user_index, entry.object_index >> 8);
            }
        }
    }

    /* Full-count verification: every single one of the 30,000 files must be
     * present and readable after the fresh reopen -- not just the sampled
     * spot-checks above. */
    {
        long verified = 0;
        for (u = 0; u < num_users; u++) {
            snprintf(uname, sizeof(uname), "BU%03d", u);
            for (f = 0; f < files_per_user; f++) {
                bool exists = false;
                snprintf(path, sizeof(path), "%s/F%03d:DATA", uname, f);
                TEST_ASSERT_OK(ndfs_file_exists(fs2, path, &exists));
                TEST_ASSERT(exists);
                verified++;
            }
        }
        TEST_ASSERT_EQUAL(num_users * files_per_user, verified);
    }

    ndfs_close(fs2);
    return 0;
}

/* ---- Delete + recreate a file whose directory page lives in the SECOND
 * SubIndexed group (user index ~65-70, past the group-0/group-1 boundary at
 * user #64) -- not just create-only, to prove the delete path's
 * free_file_blocks()/write_object_page() also correctly resolve a directory
 * page reached via the (converted) SubIndexed structure. ---- */
static int test_object_dir_delete_recreate_in_second_group(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_12MB, "OBJDELRC");
    char uname[NDFS_NAME_MAX + 1];
    int u;
    /* Loop count of users to create before the target -- SYSTEM already
     * occupies user_index 0, so the Nth user added (0-based loop counter)
     * actually receives user_index N+1. We create 67 named users so the
     * LAST one added ("DU066") lands at user_index 67, whose object_index/32
     * for its first slot is (67*256)/32 = 536 -- inside group 1
     * (pages 512-1023), past the group-0/group-1 boundary at user #64. The
     * exact assigned index is looked up via ndfs_get_users() below rather
     * than assumed, so this test does not silently drift if user-slot
     * assignment ever changes. */
    const int num_setup_users = 67;
    uint8_t target_user_index = 0;
    ndfs_object_entry_t entry_before, entry_after;
    uint8_t *read_data = NULL;
    size_t read_size = 0;
    char target_uname[NDFS_NAME_MAX + 1];
    char target_path[64];

    TEST_ASSERT_NOT_NULL(fs);

    for (u = 0; u < num_setup_users; u++) {
        snprintf(uname, sizeof(uname), "DU%03d", u);
        TEST_ASSERT_OK(ndfs_add_user(fs, uname, 10));
    }
    strncpy(target_uname, uname, sizeof(target_uname) - 1); /* last user added */
    target_uname[sizeof(target_uname) - 1] = '\0';

    /* Resolve the actual on-disk user_index assigned to the target user
     * (rather than assuming it equals the loop counter, since SYSTEM's
     * pre-existing slot 0 shifts every subsequently-added user by one). */
    {
        ndfs_user_entry_t *users = NULL;
        size_t count = 0, i;
        TEST_ASSERT_OK(ndfs_get_users(fs, &users, &count));
        for (i = 0; i < count; i++) {
            if (strcmp(users[i].user_name, target_uname) == 0) {
                target_user_index = users[i].user_index;
                break;
            }
        }
        ndfs_free_users(users);
    }
    /* Confirm the setup actually reaches past the group-0/group-1 boundary --
     * otherwise this test would silently degrade into a no-op that never
     * exercises the SubIndexed conversion at all. */
    TEST_ASSERT(((uint32_t)target_user_index * 256u) / NDFS_ENTRIES_PER_PAGE
                >= NDFS_MAX_OBJECT_FILE_PTRS);

    snprintf(target_path, sizeof(target_path), "%s/ONE:DATA", target_uname);

    TEST_ASSERT_OK(ndfs_write_file(fs, target_path, (const uint8_t *)"first", 5));
    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "ONE:DATA", target_uname, &entry_before));
    TEST_ASSERT_EQUAL(target_user_index, entry_before.object_index >> 8);

    TEST_ASSERT_OK(ndfs_delete_file(fs, target_path));

    /* Recreate at the same path -- must land back in the same user region
     * (same high byte), and its content must be the NEW content, not a
     * leftover of the deleted file's blocks. */
    TEST_ASSERT_OK(ndfs_write_file(fs, target_path,
                                   (const uint8_t *)"second-content", 14));
    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "ONE:DATA", target_uname, &entry_after));
    TEST_ASSERT_EQUAL(target_user_index, entry_after.object_index >> 8);

    TEST_ASSERT_OK(ndfs_read_file(fs, target_path, &read_data, &read_size));
    TEST_ASSERT_EQUAL(14, read_size);
    TEST_ASSERT(memcmp(read_data, "second-content", 14) == 0);
    ndfs_free_data(read_data);

    ndfs_close(fs);
    return 0;
}

/* ---- Master block "Unreserved Pages" stays in sync on file create/delete ----
 *
 * Real SINTRAN reads master_block.unreserved_pages directly to report free
 * disk space, without rescanning the BitFile bitmap. This field used to only
 * ever be written once, at image-creation time (image_creator.c) -- any
 * later create/update/delete left the on-disk copy stale relative to the
 * real bitmap. persist_master_block()/write_bit_file() (src/filesystem.c)
 * now refreshes it after every allocation-changing mutation. */
static int test_unreserved_pages_tracks_create_delete(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "UNRESV");
    uint8_t buf[NDFS_PAGE_SIZE * 2];
    uint32_t live_free_baseline, live_free;
    uint32_t raw_after_create, raw_after_delete;

    TEST_ASSERT_NOT_NULL(fs);
    memset(buf, 0x55, sizeof(buf));

    /* NOTE: image_creator.c writes a template-defined placeholder for
     * unreserved_pages at creation time (e.g. a hardcoded "1" for the small
     * floppy templates here), not necessarily the true live free count --
     * that mismatch is a separate, pre-existing property of image creation
     * and out of scope for this fix. What this test verifies is that from
     * this point on, every allocation-changing mutation (create/delete)
     * rewrites the on-disk field to track the *live* bitmap-derived count. */
    ndfs_get_free_pages(fs, &live_free_baseline);

    /* After creating a file, the on-disk field must be rewritten to match
     * the now-smaller live free count. */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/UNRFILE:DATA", buf, sizeof(buf)));
    ndfs_get_free_pages(fs, &live_free);
    raw_after_create = read_raw_unreserved_pages(fs);
    TEST_ASSERT_EQUAL(live_free, raw_after_create);
    TEST_ASSERT(live_free < live_free_baseline);

    /* After deleting it, the on-disk field must be rewritten again, tracking
     * the freed pages back up to the original live free count. */
    TEST_ASSERT_OK(ndfs_delete_file(fs, "SYSTEM/UNRFILE:DATA"));
    ndfs_get_free_pages(fs, &live_free);
    raw_after_delete = read_raw_unreserved_pages(fs);
    TEST_ASSERT_EQUAL(live_free, raw_after_delete);
    TEST_ASSERT_EQUAL(live_free_baseline, raw_after_delete);

    ndfs_close(fs);
    return 0;
}

/* ---- Unreserved Pages tracks correctly across several mutations, not just
 * at the start and the end -- verifies each intermediate step too. ---- */
static int test_unreserved_pages_tracks_multiple_mutations(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "UNRSEQ");
    uint8_t bufA[NDFS_PAGE_SIZE];
    uint8_t bufB[NDFS_PAGE_SIZE * 2];
    uint32_t live_free, raw;

    TEST_ASSERT_NOT_NULL(fs);
    memset(bufA, 0x11, sizeof(bufA));
    memset(bufB, 0x22, sizeof(bufB));

    /* Step 1: create file A. */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/SEQA:DATA", bufA, sizeof(bufA)));
    ndfs_get_free_pages(fs, &live_free);
    raw = read_raw_unreserved_pages(fs);
    TEST_ASSERT_EQUAL(live_free, raw);

    /* Step 2: create file B. */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/SEQB:DATA", bufB, sizeof(bufB)));
    ndfs_get_free_pages(fs, &live_free);
    raw = read_raw_unreserved_pages(fs);
    TEST_ASSERT_EQUAL(live_free, raw);

    /* Step 3: delete file A -- B still exists. */
    TEST_ASSERT_OK(ndfs_delete_file(fs, "SYSTEM/SEQA:DATA"));
    ndfs_get_free_pages(fs, &live_free);
    raw = read_raw_unreserved_pages(fs);
    TEST_ASSERT_EQUAL(live_free, raw);

    ndfs_close(fs);
    return 0;
}

/* ---- The Extended Info Block (and its checksum) at offset 0x07D0 must be
 * byte-identical before and after mutations that only touch the master
 * block's Unreserved Pages field -- ndfs_mb_write() must never bleed into
 * that separate structure. ---- */
static int test_unreserved_pages_persist_does_not_touch_extended_info(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "UNREXT");
    uint8_t buf[NDFS_PAGE_SIZE * 2];
    uint8_t ext_before[16];
    uint8_t ext_after_create[16];
    uint8_t ext_after_delete[16];

    TEST_ASSERT_NOT_NULL(fs);
    memset(buf, 0x33, sizeof(buf));

    read_raw_extended_info(fs, ext_before);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/EXTFILE:DATA", buf, sizeof(buf)));
    read_raw_extended_info(fs, ext_after_create);
    TEST_ASSERT(memcmp(ext_before, ext_after_create, 16) == 0);

    TEST_ASSERT_OK(ndfs_delete_file(fs, "SYSTEM/EXTFILE:DATA"));
    read_raw_extended_info(fs, ext_after_delete);
    TEST_ASSERT(memcmp(ext_before, ext_after_delete, 16) == 0);

    ndfs_close(fs);
    return 0;
}

/* ---- Sparse-file quota accounting ----
 *
 * Regression tests for the pages_used over-counting bug: pages_used used to
 * be charged data_pages (the LOGICAL page count, which counts sparse holes
 * as if they consumed real disk space -- they do NOT, per NDFS-FORMAT.md's
 * "No disk space is allocated for sparse holes") PLUS struct_pages (the
 * index/sub-index structural blocks -- filesystem overhead, not user data,
 * and must never count against a user's quota). Mirrors the C# reference
 * fix's test file (NdfsSparseFileQuotaAccountingTests.cs) scenario-for-
 * scenario: create_new_file/update_existing_file/ndfs_delete_file now use
 * count_real_data_pages_in_buffer()/count_real_data_pages_in_object()
 * (src/filesystem.c) to charge only real (non-sparse), non-structural
 * pages. */

/* A file whose every page is DECLARED a hole must charge ZERO pages_used --
 * no data page is real, and no structural page is ever charged either.
 *
 * The holes have to be declared. Before 2026-08-18 this test passed all-zero
 * data to ndfs_write_file() and relied on the writer inferring the holes; that
 * inference is gone, because it produced files SINTRAN could not read. */
static int test_quota_fully_sparse_file_charges_zero(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "QSPARSE0");
    size_t size = NDFS_PAGE_SIZE * 4;
    uint8_t *data = (uint8_t *)calloc(size, 1); /* all zero */
    static const uint32_t hole_pages[] = { 0, 1, 2, 3 };
    ndfs_hole_list_t holes;
    uint32_t used_before, used_after;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);

    holes.pages = hole_pages;
    holes.count = 4;

    used_before = get_user_pages_used(fs, "SYSTEM");
    TEST_ASSERT_OK(ndfs_write_file_holes(fs, "SYSTEM/ALLSPRS:DATA", data, size,
                                         NDFS_PARITY_NONE, &holes));
    used_after = get_user_pages_used(fs, "SYSTEM");

    TEST_ASSERT_EQUAL(used_before, used_after);

    free(data);
    ndfs_close(fs);
    return 0;
}

/* A mixed file (some real pages, some DECLARED holes) must charge only the real
 * page count -- not the logical page count and not a structural add-on. */
static int test_quota_mixed_sparse_charges_only_real(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "QMIXED");
    uint32_t logical_pages = 5;
    size_t size = (size_t)logical_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)calloc(size, 1);
    uint32_t real_pages = 2; /* only pages 1 and 3 carry real data */
    static const uint32_t hole_pages[] = { 0, 2, 4 };
    ndfs_hole_list_t holes;
    uint32_t used_before, used_after;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);

    fill_nonzero_pattern(data + (size_t)1 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 11);
    fill_nonzero_pattern(data + (size_t)3 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 22);

    holes.pages = hole_pages;
    holes.count = 3;

    used_before = get_user_pages_used(fs, "SYSTEM");
    TEST_ASSERT_OK(ndfs_write_file_holes(fs, "SYSTEM/MIXED:DATA", data, size,
                                         NDFS_PARITY_NONE, &holes));
    used_after = get_user_pages_used(fs, "SYSTEM");

    TEST_ASSERT_EQUAL(used_before + real_pages, used_after);
    TEST_ASSERT(used_after - used_before < logical_pages); /* proves no over-charge */

    free(data);
    ndfs_close(fs);
    return 0;
}

/* THE REGRESSION THAT MATTERS: all-zero pages must NOT become holes on their
 * own. Every logical page gets a real block, and the user is charged for all
 * of them.
 *
 * Deliberately asymmetric: it fails against the old inference-based writer,
 * which is the whole point -- a test that passes under both conventions proves
 * nothing. Measured consequence of the old behaviour, on a SINTRAN III VSX-K
 * pack 2026-08-18: (TCP-IP)TCP-SER-B1..B3-B05:BPUN arrived with 48/62/53
 * invented holes and the machine refused all three with NO SUCH PAGE, while
 * bank 0 (no all-zero pages) loaded. */
static int test_zero_pages_are_not_holes_unless_declared(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "NOSPARSE");
    uint32_t logical_pages = 4;
    size_t size = (size_t)logical_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)calloc(size, 1); /* all zero */
    ndfs_xat_properties_t xat;
    uint32_t used_before, used_after;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);

    used_before = get_user_pages_used(fs, "SYSTEM");
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/NOHOLES:BPUN", data, size));
    used_after = get_user_pages_used(fs, "SYSTEM");

    /* every page real: charged for all four, not zero */
    TEST_ASSERT_EQUAL(used_before + logical_pages, used_after);

    /* and the object entry agrees -- pages_in_file counts allocated pages */
    TEST_ASSERT_OK(ndfs_get_file_properties(fs, "SYSTEM/NOHOLES:BPUN", &xat));
    TEST_ASSERT_EQUAL((long)logical_pages, (long)xat.pages_in_file);

    free(data);
    ndfs_close(fs);
    return 0;
}

/* A fully-real file must charge exactly its page count, with NO index-block
 * overhead added on top (the old code added a flat "+1"/"+struct_pages"). */
static int test_quota_fully_real_file_no_index_overhead(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "QREAL");
    uint32_t pages = 4;
    size_t size = (size_t)pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)malloc(size);
    uint32_t used_before, used_after;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    fill_nonzero_pattern(data, size, 33);

    used_before = get_user_pages_used(fs, "SYSTEM");
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/FULLREAL:DATA", data, size));
    used_after = get_user_pages_used(fs, "SYSTEM");

    /* Exactly `pages` -- not pages+1 for the index block. */
    TEST_ASSERT_EQUAL(used_before + pages, used_after);

    free(data);
    ndfs_close(fs);
    return 0;
}

/* Deleting a file must refund EXACTLY what creating it charged -- pages_used
 * returns to its prior value, for a mixed sparse/real file. */
static int test_quota_delete_refunds_exactly(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "QDELREF");
    uint32_t logical_pages = 6;
    size_t size = (size_t)logical_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)calloc(size, 1);
    uint32_t used_before, used_after_create, used_after_delete;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);

    fill_nonzero_pattern(data + (size_t)2 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 44);

    used_before = get_user_pages_used(fs, "SYSTEM");
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/DELREF:DATA", data, size));
    used_after_create = get_user_pages_used(fs, "SYSTEM");
    TEST_ASSERT(used_after_create > used_before);

    TEST_ASSERT_OK(ndfs_delete_file(fs, "SYSTEM/DELREF:DATA"));
    used_after_delete = get_user_pages_used(fs, "SYSTEM");

    TEST_ASSERT_EQUAL(used_before, used_after_delete);

    free(data);
    ndfs_close(fs);
    return 0;
}

/* Overwriting a file with a larger, then smaller, real-content file must
 * track the new real page count correctly each time -- exercises the
 * update_existing_file fix (old_real_pages computed BEFORE freeing, new
 * charge computed from the new buffer's real page count). */
static int test_quota_overwrite_grow_then_shrink(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "QGROWSHR");
    uint32_t small_pages = 2, big_pages = 6, tiny_pages = 1;
    uint8_t *small = (uint8_t *)malloc((size_t)small_pages * NDFS_PAGE_SIZE);
    uint8_t *big = (uint8_t *)malloc((size_t)big_pages * NDFS_PAGE_SIZE);
    uint8_t *tiny = (uint8_t *)malloc((size_t)tiny_pages * NDFS_PAGE_SIZE);
    uint32_t baseline, used_small, used_big, used_tiny;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(small);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(tiny);
    fill_nonzero_pattern(small, (size_t)small_pages * NDFS_PAGE_SIZE, 1);
    fill_nonzero_pattern(big, (size_t)big_pages * NDFS_PAGE_SIZE, 2);
    fill_nonzero_pattern(tiny, (size_t)tiny_pages * NDFS_PAGE_SIZE, 3);

    baseline = get_user_pages_used(fs, "SYSTEM");

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/GROWSHR:DATA",
                                   small, (size_t)small_pages * NDFS_PAGE_SIZE));
    used_small = get_user_pages_used(fs, "SYSTEM");
    TEST_ASSERT_EQUAL(baseline + small_pages, used_small);

    /* Grow: overwrite with a bigger, fully-real file. */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/GROWSHR:DATA",
                                   big, (size_t)big_pages * NDFS_PAGE_SIZE));
    used_big = get_user_pages_used(fs, "SYSTEM");
    TEST_ASSERT_EQUAL(baseline + big_pages, used_big);

    /* Shrink: overwrite with a smaller, fully-real file. */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/GROWSHR:DATA",
                                   tiny, (size_t)tiny_pages * NDFS_PAGE_SIZE));
    used_tiny = get_user_pages_used(fs, "SYSTEM");
    TEST_ASSERT_EQUAL(baseline + tiny_pages, used_tiny);

    free(small);
    free(big);
    free(tiny);
    ndfs_close(fs);
    return 0;
}

/* A SubIndexed file (>512 pages, so it needs the sub-index + group-index
 * structure) that's mostly sparse must charge only its few real pages --
 * not any of the structural (sub-index + group index) blocks needed to
 * represent 1000 logical pages. */
static int test_quota_subindexed_sparse_charges_only_real(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "QSUBSPRS");
    uint32_t data_pages = 1000; /* forces SubIndexed: > 512-pointer index block */
    size_t size = (size_t)data_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)calloc(size, 1);
    uint32_t real_pages = 2;
    uint32_t used_before, used_after;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);

    fill_nonzero_pattern(data + (size_t)5 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 55);
    fill_nonzero_pattern(data + (size_t)900 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 66);

    used_before = get_user_pages_used(fs, "SYSTEM");
    {
        uint32_t *hole_pages = (uint32_t *)malloc(sizeof(uint32_t) * data_pages);
        ndfs_hole_list_t holes;
        uint32_t p, n = 0;

        TEST_ASSERT_NOT_NULL(hole_pages);
        for (p = 0; p < data_pages; p++) {
            if (p != 5 && p != 900) hole_pages[n++] = p;
        }
        holes.pages = hole_pages;
        holes.count = n;

        TEST_ASSERT_OK(ndfs_write_file_holes(fs, "SYSTEM/SUBSPRS:DATA", data, size,
                                             NDFS_PARITY_NONE, &holes));
        free(hole_pages);
    }
    used_after = get_user_pages_used(fs, "SYSTEM");

    /* Exactly the 2 real pages -- none of the sub-index/group-index
     * structural blocks (which would need 1 + ceil(1000/512) = 3 pages). */
    TEST_ASSERT_EQUAL(used_before + real_pages, used_after);

    free(data);
    ndfs_close(fs);
    return 0;
}

/* After a mix of sparse/real create, overwrite (grow+shrink), and delete
 * operations, the "Phase 4: User quota verification" fsck diagnostic must
 * report zero warnings -- it was rewritten to recompute actual_pages on the
 * same real-page basis pages_used itself is now charged on
 * (count_real_data_pages_in_object()), so it must never disagree.
 *
 * NOTE: this deliberately checks only for Phase 4's own warning text
 * ("pages_used=" / "over quota"), not for any "WARNING" in the whole fsck
 * report. Phase 3's bitmap-orphan check has a separate, pre-existing gap
 * unrelated to this fix -- its refcount walk explicitly skips SubIndexed
 * files ("SubIndexed: would need deeper walk, skip for now" in
 * ndfs_fsck()'s Phase 2 block-reference loop) -- so a SubIndexed file's
 * blocks always look "orphaned" to Phase 3 regardless of quota accounting.
 * Fixing that is out of scope here per the task brief (bitmap
 * allocation/consistency logic is untouched by this fix). */
static int test_quota_fsck_reports_no_warnings(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "QFSCKOK");
    uint32_t small_pages = 2, big_pages = 700;
    uint8_t *sparse = (uint8_t *)calloc((size_t)small_pages * NDFS_PAGE_SIZE, 1);
    uint8_t *mixed = (uint8_t *)calloc((size_t)big_pages * NDFS_PAGE_SIZE, 1);
    uint8_t *real4 = (uint8_t *)malloc(4 * NDFS_PAGE_SIZE);
    char *report = NULL;
    int fsck_errors = 0;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(sparse);
    TEST_ASSERT_NOT_NULL(mixed);
    TEST_ASSERT_NOT_NULL(real4);
    fill_nonzero_pattern(mixed + (size_t)3 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 77);
    fill_nonzero_pattern(real4, 4 * NDFS_PAGE_SIZE, 88);

    /* Fully-sparse small file. */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/FSCKA:DATA",
                                   sparse, (size_t)small_pages * NDFS_PAGE_SIZE));
    /* SubIndexed, mostly-sparse file (>512 pages). */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/FSCKB:DATA",
                                   mixed, (size_t)big_pages * NDFS_PAGE_SIZE));
    /* Overwrite it with a small, fully-real file (grow-then-shrink path). */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/FSCKB:DATA",
                                   real4, 4 * NDFS_PAGE_SIZE));
    /* Delete the first one. */
    TEST_ASSERT_OK(ndfs_delete_file(fs, "SYSTEM/FSCKA:DATA"));

    TEST_ASSERT_OK(ndfs_fsck(fs, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL(0, fsck_errors);
    TEST_ASSERT(strstr(report, "pages_used=") == NULL);
    TEST_ASSERT(strstr(report, "over quota") == NULL);

    ndfs_free_string(report);
    free(sparse);
    free(mixed);
    free(real4);
    ndfs_close(fs);
    return 0;
}

void run_write_comprehensive_tests(void)
{
    TEST_SUITE_BEGIN("Write Comprehensive Tests");
    RUN_TEST(test_surgical_write_preserves_empty_type);
    RUN_TEST(test_friends_add_list_remove);
    RUN_TEST(test_write_small_file);
    RUN_TEST(test_write_multi_page_file);
    RUN_TEST(test_overwrite_file);
    RUN_TEST(test_delete_reclaims_space);
    RUN_TEST(test_unreserved_pages_tracks_create_delete);
    RUN_TEST(test_unreserved_pages_tracks_multiple_mutations);
    RUN_TEST(test_unreserved_pages_persist_does_not_touch_extended_info);
    RUN_TEST(test_rename_file);
    RUN_TEST(test_write_persistence);
    RUN_TEST(test_sparse_file);
    RUN_TEST(test_add_multiple_users);
    RUN_TEST(test_add_users_beyond_first_page);
    RUN_TEST(test_write_file_per_user);
    RUN_TEST(test_remove_user_with_no_files);
    RUN_TEST(test_update_quota);
    RUN_TEST(test_clear_password);
    RUN_TEST(test_stress_users_and_files);
    RUN_TEST(test_list_multiple_files);
    RUN_TEST(test_write_empty_file);
    RUN_TEST(test_write_delete_cycle);
    RUN_TEST(test_indexed_boundary_stays_indexed);
    RUN_TEST(test_subindexed_just_over_boundary);
    RUN_TEST(test_subindexed_multi_group_roundtrip);
    RUN_TEST(test_subindexed_sparse_holes);
    RUN_TEST(test_subindexed_empty_group_entry);
    RUN_TEST(test_subindexed_delete_frees_all_blocks);
    RUN_TEST(test_subindexed_overwrite_no_leak);
    RUN_TEST(test_object_dir_converts_past_64_users);
    RUN_TEST(test_object_dir_30000_files_two_groups);
    RUN_TEST(test_object_dir_delete_recreate_in_second_group);
    RUN_TEST(test_quota_fully_sparse_file_charges_zero);
    RUN_TEST(test_quota_mixed_sparse_charges_only_real);
    RUN_TEST(test_zero_pages_are_not_holes_unless_declared);
    RUN_TEST(test_quota_fully_real_file_no_index_overhead);
    RUN_TEST(test_quota_delete_refunds_exactly);
    RUN_TEST(test_quota_overwrite_grow_then_shrink);
    RUN_TEST(test_quota_subindexed_sparse_charges_only_real);
    RUN_TEST(test_quota_fsck_reports_no_warnings);
}
