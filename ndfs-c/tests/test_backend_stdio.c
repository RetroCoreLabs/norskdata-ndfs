/**
 * Block-IO seam tests: prove the three backends are byte-for-byte equivalent.
 *
 * The library's ONE storage path is the ndfs_block_io seam. This suite opens
 * the SAME image through:
 *   - the in-RAM buffer backend   (ndfs_open_buffer_copy),
 *   - the host-file backend        (ndfs_open_file, gated by NDFS_WITH_STDIO_BACKEND),
 *   - a mock consumer backend      (ndfs_open_block over an in-RAM array),
 * and asserts every one re-exports an identical image and reads identical file
 * content. That three-way agreement is the proof the seam is transparent and
 * the embedded path (which the host build can't run) cannot silently rot.
 *
 * It also drives a write THROUGH the host-file backend and reopens the file to
 * prove the write-back cache flushes surgically to disk on close.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include "test_framework.h"
#include <ndfs/ndfs.h>
#include <stdint.h>

#define STDIO_TMP_IMAGE "test_backend_stdio_tmp.ndfs"

#define FILE_A "SYSTEM/AAA:DAT"   /* small: single data page   */
#define FILE_B "SYSTEM/BBB:DAT"   /* ~5000 bytes: spans 3 pages (Indexed) */

/* ── A mock consumer backend: read-only view over an in-RAM image ──────
 * This is the shape an embedded consumer fills in (ndm_media_read/write);
 * here it just memcpys out of a flat array, driven through ndfs_open_block. */
typedef struct {
    const uint8_t *data;
    uint32_t       total_pages;
} mock_ctx;

static bool mock_read_block(void *ctx, uint32_t page_id, uint8_t *out)
{
    mock_ctx *m = (mock_ctx *)ctx;
    if (page_id >= m->total_pages) return false;
    memcpy(out, m->data + (size_t)page_id * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE);
    return true;
}

/* Build a small image with two files and return it as a freshly-malloc'd
 * buffer the caller owns. Returns NULL on any failure. */
static uint8_t *build_reference_image(size_t *out_size)
{
    ndfs_filesystem_t   *fs = NULL;
    ndfs_image_options_t opts;
    uint8_t             *a = NULL, *b = NULL, *img = NULL;
    size_t               i;

    ndfs_image_options_init(&opts);
    opts.template_type = NDFS_TMPL_CUSTOM;
    opts.custom_pages  = 200;
    strcpy(opts.directory_name, "SEAMTST");

    if (ndfs_create_image(&fs, &opts) != NDFS_OK) return NULL;

    /* Small single-page file. */
    a = (uint8_t *)malloc(100);
    if (!a) { ndfs_close(fs); return NULL; }
    for (i = 0; i < 100; i++) a[i] = (uint8_t)(i * 7 + 1);   /* nonzero pattern */
    if (ndfs_write_file(fs, FILE_A, a, 100) != NDFS_OK) { free(a); ndfs_close(fs); return NULL; }

    /* Multi-page file (forces an Indexed data structure). */
    b = (uint8_t *)malloc(5000);
    if (!b) { free(a); ndfs_close(fs); return NULL; }
    for (i = 0; i < 5000; i++) b[i] = (uint8_t)(i * 13 + 5);
    if (ndfs_write_file(fs, FILE_B, b, 5000) != NDFS_OK) { free(a); free(b); ndfs_close(fs); return NULL; }

    if (ndfs_to_buffer(fs, &img, out_size) != NDFS_OK) img = NULL;

    free(a);
    free(b);
    ndfs_close(fs);
    return img;
}

/* Read a file and assert its bytes match `expect`/`expect_len`. Returns 0 on
 * success (test-style), 1 on mismatch. */
static int check_file(ndfs_filesystem_t *fs, const char *path,
                      const uint8_t *expect, size_t expect_len)
{
    uint8_t *data = NULL;
    size_t   len  = 0;
    if (ndfs_read_file(fs, path, &data, &len) != NDFS_OK) return 1;
    if (len != expect_len) { ndfs_free_data(data); return 1; }
    if (memcmp(data, expect, expect_len) != 0) { ndfs_free_data(data); return 1; }
    ndfs_free_data(data);
    return 0;
}

/* Re-export `fs` and assert the bytes equal the reference image. */
static int check_roundtrip(ndfs_filesystem_t *fs, const uint8_t *ref, size_t ref_size)
{
    uint8_t *out = NULL;
    size_t   out_size = 0;
    if (ndfs_to_buffer(fs, &out, &out_size) != NDFS_OK) return 1;
    if (out_size != ref_size) { ndfs_free_data(out); return 1; }
    if (memcmp(out, ref, ref_size) != 0) { ndfs_free_data(out); return 1; }
    ndfs_free_data(out);
    return 0;
}

/* The two known file contents, rebuilt for comparison. */
static void fill_expected(uint8_t *a100, uint8_t *b5000)
{
    size_t i;
    for (i = 0; i < 100; i++)  a100[i]  = (uint8_t)(i * 7 + 1);
    for (i = 0; i < 5000; i++) b5000[i] = (uint8_t)(i * 13 + 5);
}

/* ── Test 1: buffer vs mock-block backend produce identical results ──── */
static int test_buffer_vs_mock_equivalence(void)
{
    uint8_t *ref = NULL;
    size_t   ref_size = 0;
    uint8_t  a[100], b[5000];
    ndfs_filesystem_t *fs_buf = NULL, *fs_mock = NULL;
    mock_ctx  mctx;
    ndfs_block_io io;

    ref = build_reference_image(&ref_size);
    TEST_ASSERT_NOT_NULL(ref);
    fill_expected(a, b);

    /* Buffer backend. */
    TEST_ASSERT_OK(ndfs_open_buffer_copy(ref, ref_size, true, &fs_buf));
    TEST_ASSERT_EQUAL(0, check_file(fs_buf, FILE_A, a, 100));
    TEST_ASSERT_EQUAL(0, check_file(fs_buf, FILE_B, b, 5000));
    TEST_ASSERT_EQUAL(0, check_roundtrip(fs_buf, ref, ref_size));
    ndfs_close(fs_buf);

    /* Mock consumer backend via ndfs_open_block. */
    mctx.data = ref;
    mctx.total_pages = (uint32_t)(ref_size / NDFS_PAGE_SIZE);
    io.read_block  = mock_read_block;
    io.write_block = NULL;           /* read-only backend */
    io.destroy     = NULL;           /* we own ref, library must not free it */
    io.ctx         = &mctx;
    TEST_ASSERT_OK(ndfs_open_block(&io, mctx.total_pages, true, &fs_mock));
    TEST_ASSERT_EQUAL(0, check_file(fs_mock, FILE_A, a, 100));
    TEST_ASSERT_EQUAL(0, check_file(fs_mock, FILE_B, b, 5000));
    TEST_ASSERT_EQUAL(0, check_roundtrip(fs_mock, ref, ref_size));
    ndfs_close(fs_mock);

    free(ref);
    return 0;
}

#ifdef NDFS_WITH_STDIO_BACKEND
/* ── Test 2: host-file backend matches the buffer backend byte-for-byte ── */
static int test_hostfile_equivalence(void)
{
    uint8_t *ref = NULL;
    size_t   ref_size = 0;
    uint8_t  a[100], b[5000];
    ndfs_filesystem_t *fs = NULL;
    FILE    *f;

    ref = build_reference_image(&ref_size);
    TEST_ASSERT_NOT_NULL(ref);
    fill_expected(a, b);

    /* Dump the reference image to a real file on disk. */
    f = fopen(STDIO_TMP_IMAGE, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(ref_size, fwrite(ref, 1, ref_size, f));
    fclose(f);

    /* Open it through the streaming host-file backend and compare. */
    TEST_ASSERT_OK(ndfs_open_file(STDIO_TMP_IMAGE, true, &fs));
    TEST_ASSERT_EQUAL(0, check_file(fs, FILE_A, a, 100));
    TEST_ASSERT_EQUAL(0, check_file(fs, FILE_B, b, 5000));
    TEST_ASSERT_EQUAL(0, check_roundtrip(fs, ref, ref_size));
    ndfs_close(fs);

    free(ref);
    remove(STDIO_TMP_IMAGE);
    return 0;
}

/* ── Test 3: writing through the host-file backend persists to disk ────
 * Proves the write-back page cache flushes on close and the on-disk file is
 * mutated surgically -- reopening a fresh handle sees the new file. */
static int test_hostfile_write_persists(void)
{
    uint8_t *ref = NULL;
    size_t   ref_size = 0;
    uint8_t  c[300];
    size_t   i;
    ndfs_filesystem_t *fs = NULL;
    FILE    *f;

    ref = build_reference_image(&ref_size);
    TEST_ASSERT_NOT_NULL(ref);

    f = fopen(STDIO_TMP_IMAGE, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(ref_size, fwrite(ref, 1, ref_size, f));
    fclose(f);
    free(ref);

    for (i = 0; i < 300; i++) c[i] = (uint8_t)(300 - i);   /* nonzero pattern */

    /* Open writable, add a new file, close (flush to disk). */
    TEST_ASSERT_OK(ndfs_open_file(STDIO_TMP_IMAGE, false, &fs));
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/CCC:DAT", c, 300));
    ndfs_close(fs);

    /* Reopen a brand-new handle; the write must have hit the file. */
    fs = NULL;
    TEST_ASSERT_OK(ndfs_open_file(STDIO_TMP_IMAGE, true, &fs));
    TEST_ASSERT_EQUAL(0, check_file(fs, "SYSTEM/CCC:DAT", c, 300));
    /* The pre-existing files must still be intact too. */
    {
        uint8_t a[100], b[5000];
        fill_expected(a, b);
        TEST_ASSERT_EQUAL(0, check_file(fs, FILE_A, a, 100));
        TEST_ASSERT_EQUAL(0, check_file(fs, FILE_B, b, 5000));
    }
    ndfs_close(fs);

    remove(STDIO_TMP_IMAGE);
    return 0;
}
#endif /* NDFS_WITH_STDIO_BACKEND */

/* ── Test 4: bad-open paths must fail cleanly and release the backend ──
 * Regression guard for the ndfs_open_block destroy contract: a backend that
 * already opened a resource (here ndfs_open_file's FILE*) must be torn down via
 * its destroy hook on EVERY failure, including total_pages == 0 (sub-page image)
 * and a NULL read_block.  We can't observe the freed FILE* directly without a
 * leak sanitiser, but we assert the failure is reported (not a crash / not a
 * bogus success) and that out_fs is left untouched. */
static int test_open_block_failure_paths(void)
{
    ndfs_filesystem_t *fs = (ndfs_filesystem_t *)0x1;   /* sentinel: must stay untouched */
    ndfs_block_io io;
    mock_ctx mctx;
    static const uint8_t dummy[NDFS_PAGE_SIZE] = {0};

    mctx.data = dummy;
    mctx.total_pages = 1;

    /* NULL io pointer -> NULL_PTR, no deref crash. */
    TEST_ASSERT_EQUAL(NDFS_ERR_NULL_PTR, ndfs_open_block(NULL, 1, true, &fs));

    /* NULL read_block -> NULL_PTR (open_internal invokes destroy). */
    io.read_block = NULL; io.write_block = NULL; io.destroy = NULL; io.ctx = &mctx;
    TEST_ASSERT_EQUAL(NDFS_ERR_NULL_PTR, ndfs_open_block(&io, 1, true, &fs));

    /* Zero pages -> TOO_SMALL. */
    io.read_block = mock_read_block;
    TEST_ASSERT_EQUAL(NDFS_ERR_TOO_SMALL, ndfs_open_block(&io, 0, true, &fs));

    /* out_fs sentinel must not have been written on any failure path. */
    TEST_ASSERT(fs == (ndfs_filesystem_t *)0x1);
    return 0;
}

#ifdef NDFS_WITH_STDIO_BACKEND
/* ── Test: writes are committed to the backend PER MUTATION, not just at close.
 * Open a writable host-file handle, write a file, and -- WITHOUT closing that
 * handle -- open a SECOND independent read-only handle on the same path. The
 * new file must already be visible, proving ndfs_write_file flushed the dirty
 * pages to disk when it returned (write-back committed per mutation), rather
 * than deferring everything to ndfs_close. */
static int test_hostfile_commit_per_mutation(void)
{
    uint8_t *ref = NULL;
    size_t   ref_size = 0;
    uint8_t  d[400];
    size_t   i;
    ndfs_filesystem_t *w = NULL, *r = NULL;
    FILE    *f;

    ref = build_reference_image(&ref_size);
    TEST_ASSERT_NOT_NULL(ref);
    f = fopen(STDIO_TMP_IMAGE, "wb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL(ref_size, fwrite(ref, 1, ref_size, f));
    fclose(f);
    free(ref);

    for (i = 0; i < 400; i++) d[i] = (uint8_t)(i * 3 + 2);

    /* Writable handle -- write a new file but DO NOT close it. */
    TEST_ASSERT_OK(ndfs_open_file(STDIO_TMP_IMAGE, false, &w));
    TEST_ASSERT_OK(ndfs_write_file(w, "SYSTEM/DDD:DAT", d, 400));

    /* Second, independent read-only view of the same on-disk file must already
     * see the committed write -- w is still open (no close-flush has run). */
    TEST_ASSERT_OK(ndfs_open_file(STDIO_TMP_IMAGE, true, &r));
    TEST_ASSERT_EQUAL(0, check_file(r, "SYSTEM/DDD:DAT", d, 400));
    ndfs_close(r);

    ndfs_close(w);
    remove(STDIO_TMP_IMAGE);
    return 0;
}

/* A sub-page host file (total_pages == 0) must fail without stranding the FILE*
 * the backend opened.  Before the fix ndfs_open_block returned early and leaked
 * both the FILE* and the ctx; now open_internal drives the destroy hook. */
static int test_hostfile_subpage_clean_fail(void)
{
    ndfs_filesystem_t *fs = NULL;
    FILE *f = fopen(STDIO_TMP_IMAGE, "wb");
    TEST_ASSERT_NOT_NULL(f);
    { uint8_t tiny[100] = {1}; fwrite(tiny, 1, sizeof(tiny), f); }
    fclose(f);

    /* Must report TOO_SMALL (image < one page) and not leak / crash. */
    TEST_ASSERT_EQUAL(NDFS_ERR_TOO_SMALL, ndfs_open_file(STDIO_TMP_IMAGE, true, &fs));
    TEST_ASSERT_NULL(fs);

    /* A non-existent file must fail cleanly too (fopen returns NULL). */
    remove(STDIO_TMP_IMAGE);
    TEST_ASSERT_EQUAL(NDFS_ERR_IO, ndfs_open_file(STDIO_TMP_IMAGE, true, &fs));
    return 0;
}
#endif /* NDFS_WITH_STDIO_BACKEND */

void run_backend_stdio_tests(void)
{
    TEST_SUITE_BEGIN("Block-IO Seam / Backend Equivalence Tests");
    RUN_TEST(test_buffer_vs_mock_equivalence);
    RUN_TEST(test_open_block_failure_paths);
#ifdef NDFS_WITH_STDIO_BACKEND
    RUN_TEST(test_hostfile_equivalence);
    RUN_TEST(test_hostfile_write_persists);
    RUN_TEST(test_hostfile_commit_per_mutation);
    RUN_TEST(test_hostfile_subpage_clean_fail);
#endif
}
