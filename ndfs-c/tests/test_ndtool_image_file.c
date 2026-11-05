/**
 * Tests for tools/ndtool/image_file.c: when ndtool writes the image file.
 *
 * Outside the shell a changing command writes the file at once. Inside the
 * shell it must change only the in-memory copy, so `quit` without `save`
 * leaves the file as it was (the ndtool README's promise). Measured broken
 * before 2026-11-04: shell `chmod`, then `quit` answered "no", changed the
 * file.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_framework.h"
#include "ndtool.h"
#include <ndfs/ndfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_FILE_TMP "test_ndtool_image_file_tmp.ndfs"

static ndfs_filesystem_t *make_image(void)
{
    ndfs_filesystem_t *fs = NULL;
    ndfs_image_options_t opts;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_FLOPPY_360KB;
    strncpy(opts.directory_name, "SAVE", NDFS_NAME_MAX);
    opts.directory_name[NDFS_NAME_MAX] = '\0';
    if (ndfs_create_image(&fs, &opts) != NDFS_OK) return NULL;
    return fs;
}

/* True if the file on disk holds a user called NAME. */
static bool file_has_user(const char *name)
{
    size_t size = 0;
    uint8_t *data = ndtool_read_local_file(IMAGE_FILE_TMP, &size);
    ndfs_filesystem_t *fs = NULL;
    ndfs_user_entry_t *users = NULL;
    size_t count = 0, i;
    bool found = false;

    if (!data) return false;
    if (ndfs_open_buffer_copy(data, size, true, &fs) == NDFS_OK) {
        if (ndfs_get_users(fs, &users, &count) == NDFS_OK) {
            for (i = 0; i < count; i++) {
                if (strcmp(users[i].user_name, name) == 0) found = true;
            }
            ndfs_free_users(users);
        }
        ndfs_close(fs);
    }
    free(data);
    return found;
}

/* Outside the shell: ndtool_save_image writes the file. */
static int test_save_writes_outside_shell(void)
{
    ndtool_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.fs = make_image();
    TEST_ASSERT_NOT_NULL(ctx.fs);
    ctx.image_path = IMAGE_FILE_TMP;
    remove(IMAGE_FILE_TMP);

    TEST_ASSERT_OK(ndfs_add_user(ctx.fs, "ALICE", 10));
    TEST_ASSERT_EQUAL(0, ndtool_save_image(&ctx));
    TEST_ASSERT(file_has_user("ALICE"));
    TEST_ASSERT(!ctx.shell_unsaved);

    ndfs_close(ctx.fs);
    remove(IMAGE_FILE_TMP);
    return 0;
}

/* Inside the shell: the same call leaves the file alone until save. */
static int test_shell_writes_only_on_save(void)
{
    ndtool_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.fs = make_image();
    TEST_ASSERT_NOT_NULL(ctx.fs);
    ctx.image_path = IMAGE_FILE_TMP;
    remove(IMAGE_FILE_TMP);
    TEST_ASSERT_EQUAL(0, ndtool_write_image(&ctx));   /* the file before */

    ctx.in_shell = true;
    TEST_ASSERT_OK(ndfs_add_user(ctx.fs, "ALICE", 10));
    TEST_ASSERT_EQUAL(0, ndtool_save_image(&ctx));    /* what a cmd_* calls */
    TEST_ASSERT(ctx.shell_unsaved);
    TEST_ASSERT(!file_has_user("ALICE"));             /* not written */

    TEST_ASSERT_EQUAL(0, ndtool_write_image(&ctx));   /* shell `save` */
    TEST_ASSERT(!ctx.shell_unsaved);
    TEST_ASSERT(file_has_user("ALICE"));

    ndfs_close(ctx.fs);
    remove(IMAGE_FILE_TMP);
    return 0;
}

/* -n: nothing is written, in or out of the shell. */
static int test_dry_run_writes_nothing(void)
{
    ndtool_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.fs = make_image();
    TEST_ASSERT_NOT_NULL(ctx.fs);
    ctx.image_path = IMAGE_FILE_TMP;
    remove(IMAGE_FILE_TMP);
    TEST_ASSERT_EQUAL(0, ndtool_write_image(&ctx));

    ctx.dry_run = true;
    TEST_ASSERT_OK(ndfs_add_user(ctx.fs, "ALICE", 10));
    TEST_ASSERT_EQUAL(0, ndtool_save_image(&ctx));
    TEST_ASSERT(!file_has_user("ALICE"));

    ndfs_close(ctx.fs);
    remove(IMAGE_FILE_TMP);
    return 0;
}

void run_ndtool_image_file_tests(void)
{
    TEST_SUITE_BEGIN("ndtool Image File Tests");

    RUN_TEST(test_save_writes_outside_shell);
    RUN_TEST(test_shell_writes_only_on_save);
    RUN_TEST(test_dry_run_writes_nothing);
}
