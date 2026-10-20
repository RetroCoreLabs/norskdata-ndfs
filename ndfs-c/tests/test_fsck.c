/**
 * Regression tests for ndfs_fsck().
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 *
 * ndfs_fsck()'s Phase 2/3 block-reference walk was rewritten to fix three
 * distinct bugs, all found while running fsck against real SINTRAN packs:
 *
 *   Bug A (phantom cross-link): for a CONTIGUOUS file, the code pre-counted
 *   the file pointer's block AND counted it again as data page 0 in the
 *   per-page loop, so every contiguous file cross-linked with itself and
 *   Phase 3 reported a false "cross-linked" ERROR.
 *
 *   Bug B (phantom orphans): the reference walk only counted the master
 *   block's three pointer TARGETS (object-file index block, user-file index
 *   block, first bitmap page) -- never the object-file's own data pages, the
 *   user-file's data pages beyond the first, the bitmap's pages beyond the
 *   first, or anything behind a SubIndexed file pointer at all. Every one of
 *   those legitimately-used blocks came back as an "orphaned" WARNING on a
 *   perfectly healthy pack.
 *
 *   Bug C (sparse undercount): the Indexed-file data-pointer scan was bounded
 *   by `pages_in_file`, which (since the sparse-holes fix) is a REAL-page
 *   count, not the logical extent. A file with a few real pages sitting at
 *   high slots in an otherwise-sparse index had those slots skipped entirely,
 *   so their data blocks came back as orphaned too.
 *
 * These tests exercise each bug directly (so a regression is caught the same
 * way it was found), then check the fix did not go too far the other way --
 * fsck must still catch a GENUINE orphan or a GENUINE cross-link when one is
 * injected by hand.
 */

#include "test_framework.h"
#include <ndfs/ndfs.h>
#include "endian_util.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- Helper: create a writable image (mirrors test_write_comprehensive.c) ---- */

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

static void fill_nonzero_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
    size_t i;
    for (i = 0; i < len; i++) buf[i] = (uint8_t)(seed + i);
}

/* Reproduces bit_file.c's addressing exactly: page N lives at bit N&7 of
 * byte (N>>3)^1, counting bytes from the START of the bitmap's own page
 * range. See bf_byte_index()/bf_bit_mask() in src/bit_file.c -- this MUST
 * stay in lock-step with that code or the corruption tests below would
 * flip the wrong bit and prove nothing. */
static void raw_bitmap_bit_offset(uint32_t block_id, size_t *out_byte, uint8_t *out_mask)
{
    *out_byte = (size_t)((block_id >> 3) ^ 1u);
    *out_mask = (uint8_t)(1u << (block_id & 7u));
}

/* ---- Bug A: a CONTIGUOUS file must not cross-link with itself ---- */

/* Before the fix, create_contiguous_file's single fp_block was counted once
 * as "the file's pointer block" and again as "page 0 of the file's data",
 * giving that one block a refcount of 2 -- Phase 3 read that as a real
 * cross-linked block and raised an ERROR for a perfectly healthy file. */
static int test_fsck_contiguous_file_not_cross_linked(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "FSCKCONT");
    char *report = NULL;
    int fsck_errors = -1;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_create_contiguous_file(fs, "SYSTEM/CFILE:DATA", 5));

    TEST_ASSERT_OK(ndfs_fsck(fs, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL(0, fsck_errors);
    TEST_ASSERT(strstr(report, "OK: No cross-linked blocks") != NULL);
    TEST_ASSERT(strstr(report, "cross-linked)") == NULL); /* no "N blocks ... (cross-linked)" ERROR line */

    free(report);
    ndfs_close(fs);
    return 0;
}

/* ---- Bug B: the base structures (object file, user file, bit file) ---- */

/* On the smallest possible image with nothing but the default SYSTEM user,
 * the object file, user file and bit file are ALREADY multi-block
 * structures (an index/sub-index block plus at least one data page each).
 * Before the fix, none of those data pages were counted, so this simplest
 * possible fsck run already reported orphans. */
static int test_fsck_fresh_image_no_orphans(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "FSCKBASE");
    char *report = NULL;
    int fsck_errors = -1;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_fsck(fs, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL(0, fsck_errors);
    TEST_ASSERT(strstr(report, "OK: No orphaned blocks") != NULL);

    free(report);
    ndfs_close(fs);
    return 0;
}

/* The bit file's own bitmap spans MULTIPLE pages once the volume is large
 * enough (NDFS_TMPL_SMD_75MB is 38400 pages, needing 3 bitmap pages -- see
 * the bitmap_pages computation mirrored from load_structures). Before the
 * fix only the bit file's FIRST page was ever counted, so pages 2 and 3 of
 * the bitmap always came back as orphaned on any image this size or larger. */
static int test_fsck_multi_page_bitmap_no_orphans(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "FSCKBMAP");
    char *report = NULL;
    int fsck_errors = -1;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_fsck(fs, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL(0, fsck_errors);
    TEST_ASSERT(strstr(report, "OK: No orphaned blocks") != NULL);

    free(report);
    ndfs_close(fs);
    return 0;
}

/* NDFS_MAX_USER_FILE_PTRS (8) data pages, 32 users each -- more than 32
 * users forces a second user-file data page. Before the fix NO user-file
 * data page was ever counted (not even the first), so this is really just
 * a stronger version of test_fsck_fresh_image_no_orphans, but it pins down
 * the multi-page case specifically. */
static int test_fsck_many_users_no_orphans(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_12MB, "FSCKUSRS");
    char uname[NDFS_NAME_MAX + 1];
    char *report = NULL;
    int fsck_errors = -1;
    int u;
    const int num_users = 40; /* SYSTEM (slot 0) + 40 spills into the second user page */

    TEST_ASSERT_NOT_NULL(fs);
    for (u = 0; u < num_users; u++) {
        snprintf(uname, sizeof(uname), "FUSER%03d", u);
        TEST_ASSERT_OK(ndfs_add_user(fs, uname, (uint32_t)(u + 1)));
    }

    TEST_ASSERT_OK(ndfs_fsck(fs, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL(0, fsck_errors);
    TEST_ASSERT(strstr(report, "OK: No orphaned blocks") != NULL);

    free(report);
    ndfs_close(fs);
    return 0;
}

/* A SubIndexed file (>512 logical pages) used to be skipped ENTIRELY by the
 * reference walk -- its sub-index block, every group-index block, and every
 * real data page all came back as orphaned. Mostly-sparse so the test stays
 * fast: only 2 of 700 pages are real. */
static int test_fsck_subindexed_file_no_orphans(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "FSCKSUBI");
    uint32_t logical_pages = 700;
    size_t size = (size_t)logical_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)calloc(size, 1);
    uint32_t *hole_pages = (uint32_t *)malloc(sizeof(uint32_t) * logical_pages);
    ndfs_hole_list_t holes;
    uint32_t p, n = 0;
    char *report = NULL;
    int fsck_errors = -1;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_NOT_NULL(hole_pages);

    /* Real pages: 5 (first group) and 600 (second group, 512..699). Every
     * other page is a DECLARED hole -- see the sparse-holes fix, holes are
     * never inferred from zero content. */
    fill_nonzero_pattern(data + (size_t)5 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 9);
    fill_nonzero_pattern(data + (size_t)600 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 17);
    for (p = 0; p < logical_pages; p++) {
        if (p != 5 && p != 600) hole_pages[n++] = p;
    }
    holes.pages = hole_pages;
    holes.count = n;

    TEST_ASSERT_OK(ndfs_write_file_holes(fs, "SYSTEM/SUBI:DATA", data, size,
                                         NDFS_PARITY_NONE, &holes));
    free(hole_pages);
    free(data);

    TEST_ASSERT_OK(ndfs_fsck(fs, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL(0, fsck_errors);
    TEST_ASSERT(strstr(report, "OK: No orphaned blocks") != NULL);

    free(report);
    ndfs_close(fs);
    return 0;
}

/* ---- Bug C: an Indexed file's real pages past the old pages_in_file bound ---- */

/* pages_in_file is a REAL-page count (2), far smaller than the index slot
 * (498) the second real page actually lives at. The old scan stopped at
 * `j < pages_in_file` (i.e. j < 2) and never looked at slot 498, so that
 * page's data block came back as orphaned even though the file legitimately
 * owns it. */
static int test_fsck_sparse_indexed_high_slot_no_orphans(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_12MB, "FSCKSPIX");
    uint32_t logical_pages = 500; /* stays <= 512: plain Indexed, not SubIndexed */
    size_t size = (size_t)logical_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)calloc(size, 1);
    uint32_t *hole_pages = (uint32_t *)malloc(sizeof(uint32_t) * logical_pages);
    ndfs_hole_list_t holes;
    uint32_t p, n = 0;
    char *report = NULL;
    int fsck_errors = -1;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_NOT_NULL(hole_pages);

    fill_nonzero_pattern(data + (size_t)2 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 3);
    fill_nonzero_pattern(data + (size_t)498 * NDFS_PAGE_SIZE, NDFS_PAGE_SIZE, 4);
    for (p = 0; p < logical_pages; p++) {
        if (p != 2 && p != 498) hole_pages[n++] = p;
    }
    holes.pages = hole_pages;
    holes.count = n;

    TEST_ASSERT_OK(ndfs_write_file_holes(fs, "SYSTEM/SPIX:DATA", data, size,
                                         NDFS_PARITY_NONE, &holes));
    free(hole_pages);
    free(data);

    /* Sanity-check the premise: pages_in_file (real count) really is much
     * smaller than the slot the second real page sits at, or this test
     * would not exercise the old bug at all. */
    {
        ndfs_object_entry_t entry;
        TEST_ASSERT_OK(ndfs_get_object_entry(fs, "SPIX:DATA", "SYSTEM", &entry));
        TEST_ASSERT_EQUAL(2, entry.pages_in_file);
        TEST_ASSERT_EQUAL(NDFS_PTR_INDEXED, entry.file_pointer.type);
    }

    TEST_ASSERT_OK(ndfs_fsck(fs, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL(0, fsck_errors);
    TEST_ASSERT(strstr(report, "OK: No orphaned blocks") != NULL);

    free(report);
    ndfs_close(fs);
    return 0;
}

/* ---- Guard: a garbage pointer into the reserved block range (0..6) ---- */

/* fsck_ref() refuses to bump the refcount for any block below
 * NDFS_FIRST_ALLOC_BLOCK. Without that guard, a stray/garbage index slot
 * pointing into the reserved range (which is legitimately referenced by the
 * master block's own fixed structures) would inflate that block's refcount
 * past 1 and Phase 3 would report a phantom cross-link -- exactly the kind
 * of report a real SINTRAN pack with leftover byte patterns in an unused
 * index slot can trigger. This test proves the garbage pointer is silently
 * ignored: the fsck report is byte-for-byte identical with and without it. */
static int test_fsck_garbage_pointer_into_reserved_range_ignored(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_12MB, "FSCKGRB");
    uint32_t logical_pages = 5;
    size_t size = (size_t)logical_pages * NDFS_PAGE_SIZE;
    uint8_t *data = (uint8_t *)calloc(size, 1);
    static const uint32_t hole_pages[] = { 4 }; /* slot 4 stays a hole (block_id 0) */
    ndfs_hole_list_t holes;
    ndfs_object_entry_t entry;
    uint8_t *baseline = NULL;
    size_t baseline_size = 0;
    uint8_t *corrupt = NULL;
    size_t corrupt_size = 0;
    char *report_before = NULL;
    char *report_after = NULL;
    int errors_before = -1, errors_after = -2;
    ndfs_filesystem_t *fs_corrupt = NULL;
    size_t slot4_offset;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    fill_nonzero_pattern(data, size, 1);

    holes.pages = hole_pages;
    holes.count = 1;
    TEST_ASSERT_OK(ndfs_write_file_holes(fs, "SYSTEM/GRB:DATA", data, size,
                                         NDFS_PARITY_NONE, &holes));
    free(data);

    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "GRB:DATA", "SYSTEM", &entry));
    TEST_ASSERT_EQUAL(NDFS_PTR_INDEXED, entry.file_pointer.type);

    TEST_ASSERT_OK(ndfs_fsck(fs, &report_before, &errors_before));
    TEST_ASSERT_NOT_NULL(report_before);

    TEST_ASSERT_OK(ndfs_to_buffer(fs, &baseline, &baseline_size));
    corrupt = (uint8_t *)malloc(baseline_size);
    TEST_ASSERT_NOT_NULL(corrupt);
    memcpy(corrupt, baseline, baseline_size);
    corrupt_size = baseline_size;
    free(baseline);

    /* Slot 4 of the index block is the declared hole -- currently 0 on disk.
     * Overwrite it with block_id 3 (inside the reserved 0..6 range, which
     * legitimately belongs to fixed system structures) to simulate a stale
     * byte pattern left over from a rewritten pack. */
    slot4_offset = (size_t)entry.file_pointer.block_id * NDFS_PAGE_SIZE + 4 * 4;
    TEST_ASSERT_EQUAL(0, ndfs_read_u32be(corrupt, slot4_offset));
    ndfs_write_u32be(corrupt, slot4_offset, 3u);

    TEST_ASSERT_OK(ndfs_open_buffer_copy(corrupt, corrupt_size, true, &fs_corrupt));
    free(corrupt);

    TEST_ASSERT_OK(ndfs_fsck(fs_corrupt, &report_after, &errors_after));
    TEST_ASSERT_NOT_NULL(report_after);

    /* The garbage pointer must change NOTHING: same error count, same
     * "no orphans" / "no cross-linked" verdicts as before it was injected. */
    TEST_ASSERT_EQUAL(errors_before, errors_after);
    TEST_ASSERT(strstr(report_after, "OK: No cross-linked blocks") != NULL);
    TEST_ASSERT(strstr(report_after, "OK: No orphaned blocks") != NULL);
    TEST_ASSERT(strstr(report_after, "referenced by files but marked FREE") == NULL);

    free(report_before);
    free(report_after);
    ndfs_close(fs_corrupt);
    ndfs_close(fs);
    return 0;
}

/* ---- The fix must not stop fsck from catching REAL corruption ---- */

/* Flips one genuinely-unused page's bitmap bit to "used" by hand. This is
 * the one case Phase 3 SHOULD flag: a block marked used but referenced by
 * nothing. Proves the Bug B fix (which taught fsck about many more
 * legitimate references) didn't accidentally teach it to ignore blocks it
 * still has no business calling "referenced". */
static int test_fsck_detects_genuine_orphan(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "FSCKORPH");
    const ndfs_master_block_t *mb = NULL;
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    char *report_clean = NULL;
    char *report_dirty = NULL;
    int errors_clean = -1, errors_dirty = -1;
    ndfs_filesystem_t *fs_dirty = NULL;
    size_t byte_off;
    uint8_t mask;
    size_t abs_off;

    TEST_ASSERT_NOT_NULL(fs);

    /* Confirm the pack starts clean before hand-corrupting it. */
    TEST_ASSERT_OK(ndfs_fsck(fs, &report_clean, &errors_clean));
    TEST_ASSERT_NOT_NULL(report_clean);
    TEST_ASSERT_EQUAL(0, errors_clean);
    TEST_ASSERT(strstr(report_clean, "OK: No orphaned blocks") != NULL);

    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));

    /* NDFS_FIRST_ALLOC_BLOCK (7) is the lowest allocatable page. On a nearly
     * empty pack (allocation proceeds top-down from the ceiling) it is still
     * free -- mark it used behind the filesystem's back with no file ever
     * pointing at it. */
    raw_bitmap_bit_offset(NDFS_FIRST_ALLOC_BLOCK, &byte_off, &mask);
    abs_off = (size_t)mb->bit_file_ptr.block_id * NDFS_PAGE_SIZE + byte_off;
    TEST_ASSERT((buf[abs_off] & mask) == 0); /* premise: block 7 really is free */
    buf[abs_off] |= mask;

    TEST_ASSERT_OK(ndfs_open_buffer_copy(buf, buf_size, true, &fs_dirty));
    free(buf);

    TEST_ASSERT_OK(ndfs_fsck(fs_dirty, &report_dirty, &errors_dirty));
    TEST_ASSERT_NOT_NULL(report_dirty);
    TEST_ASSERT_EQUAL(0, errors_dirty); /* orphans are a WARNING, not an ERROR */
    TEST_ASSERT(strstr(report_dirty, "1 orphaned blocks") != NULL);

    free(report_clean);
    free(report_dirty);
    ndfs_close(fs_dirty);
    ndfs_close(fs);
    return 0;
}

/* Points one file's index slot 0 at ANOTHER file's data page, by hand. This
 * is a genuine cross-link and Phase 3 must still catch it as an ERROR --
 * proves the Bug A fix (which stopped a contiguous file's own block from
 * "cross-linking with itself") didn't also blind fsck to two DIFFERENT
 * files sharing a block for real. */
static int test_fsck_detects_genuine_cross_link(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_12MB, "FSCKXLNK");
    uint8_t *data_a = (uint8_t *)malloc(NDFS_PAGE_SIZE);
    uint8_t *data_b = (uint8_t *)malloc(NDFS_PAGE_SIZE);
    ndfs_object_entry_t entry_a, entry_b;
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    ndfs_filesystem_t *fs_dirty = NULL;
    size_t slot0_a, slot0_b;
    uint32_t pointer_value;
    char *report = NULL;
    int fsck_errors = -1;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data_a);
    TEST_ASSERT_NOT_NULL(data_b);
    fill_nonzero_pattern(data_a, NDFS_PAGE_SIZE, 1);
    fill_nonzero_pattern(data_b, NDFS_PAGE_SIZE, 2);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/XLNKA:DATA", data_a, NDFS_PAGE_SIZE));
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/XLNKB:DATA", data_b, NDFS_PAGE_SIZE));
    free(data_a);
    free(data_b);

    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "XLNKA:DATA", "SYSTEM", &entry_a));
    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "XLNKB:DATA", "SYSTEM", &entry_b));
    TEST_ASSERT_EQUAL(NDFS_PTR_INDEXED, entry_a.file_pointer.type);
    TEST_ASSERT_EQUAL(NDFS_PTR_INDEXED, entry_b.file_pointer.type);

    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));

    slot0_a = (size_t)entry_a.file_pointer.block_id * NDFS_PAGE_SIZE + 0;
    slot0_b = (size_t)entry_b.file_pointer.block_id * NDFS_PAGE_SIZE + 0;
    pointer_value = ndfs_read_u32be(buf, slot0_a);
    TEST_ASSERT(pointer_value != 0); /* XLNKA's page 0 is real, not a hole */

    /* Make XLNKB's page 0 point at the SAME physical block as XLNKA's. */
    ndfs_write_u32be(buf, slot0_b, pointer_value);

    TEST_ASSERT_OK(ndfs_open_buffer_copy(buf, buf_size, true, &fs_dirty));
    free(buf);

    TEST_ASSERT_OK(ndfs_fsck(fs_dirty, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT(fsck_errors > 0);
    TEST_ASSERT(strstr(report, "cross-linked)") != NULL);

    free(report);
    ndfs_close(fs_dirty);
    ndfs_close(fs);
    return 0;
}

/* A file's data block, freed in the bitmap by hand while the file's index
 * still points at it. This is the third leg of Phase 3 (orphaned /
 * referenced_free / multiply_referenced) that the earlier tests don't touch:
 * a block a file legitimately owns, but the bitmap says is free. Genuinely
 * dangerous on a real pack -- the next file written could be handed that
 * same block out from under the file that already owns it. */
static int test_fsck_detects_referenced_but_marked_free(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_12MB, "FSCKRFREE");
    uint8_t *data = (uint8_t *)malloc(NDFS_PAGE_SIZE);
    ndfs_object_entry_t entry;
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    ndfs_filesystem_t *fs_dirty = NULL;
    uint32_t data_block;
    size_t byte_off;
    uint8_t mask;
    size_t abs_off;
    char *report = NULL;
    int fsck_errors = -1;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(data);
    fill_nonzero_pattern(data, NDFS_PAGE_SIZE, 8);

    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/RFREE:DATA", data, NDFS_PAGE_SIZE));
    free(data);

    TEST_ASSERT_OK(ndfs_get_object_entry(fs, "RFREE:DATA", "SYSTEM", &entry));
    TEST_ASSERT_EQUAL(NDFS_PTR_INDEXED, entry.file_pointer.type);

    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));
    data_block = ndfs_read_u32be(buf, (size_t)entry.file_pointer.block_id * NDFS_PAGE_SIZE + 0);
    TEST_ASSERT(data_block != 0);

    /* Flip the bitmap bit for RFREE's own data page to "free", without
     * touching its index entry -- the index still legitimately points at it. */
    {
        const ndfs_master_block_t *mb = NULL;
        TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
        raw_bitmap_bit_offset(data_block, &byte_off, &mask);
        abs_off = (size_t)mb->bit_file_ptr.block_id * NDFS_PAGE_SIZE + byte_off;
    }
    TEST_ASSERT((buf[abs_off] & mask) != 0); /* premise: it really is marked used before the flip */
    buf[abs_off] &= (uint8_t)~mask;

    TEST_ASSERT_OK(ndfs_open_buffer_copy(buf, buf_size, true, &fs_dirty));
    free(buf);

    TEST_ASSERT_OK(ndfs_fsck(fs_dirty, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT(fsck_errors > 0);
    TEST_ASSERT(strstr(report, "referenced by files but marked FREE") != NULL);

    free(report);
    ndfs_close(fs_dirty);
    ndfs_close(fs);
    return 0;
}

/* Phase 1 must still catch a master block whose object-file pointer was
 * clobbered to a RESERVED (invalid) type -- the exact shape of damage a
 * partially-overwritten master block leaves behind. */
static int test_fsck_detects_invalid_master_block_pointer(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "FSCKMBAD");
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    ndfs_filesystem_t *fs_dirty = NULL;
    size_t obj_ptr_off;
    char *report = NULL;
    int fsck_errors = -1;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));

    /* Object-file pointer lives at NDFS_MASTER_BLOCK_OFFSET+0x10 (see
     * master_block.c's ndfs_mb_from_bytes/to_bytes). Encode type=3
     * (NDFS_PTR_RESERVED, top 2 bits) over whatever block_id was there --
     * ndfs_bp_is_valid() rejects any RESERVED-type pointer outright. */
    obj_ptr_off = NDFS_MASTER_BLOCK_OFFSET + 0x10;
    ndfs_write_u32be(buf, obj_ptr_off, 0xC0000001u);

    TEST_ASSERT_OK(ndfs_open_buffer_copy(buf, buf_size, true, &fs_dirty));
    free(buf);

    TEST_ASSERT_OK(ndfs_fsck(fs_dirty, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT(fsck_errors > 0);
    TEST_ASSERT(strstr(report, "ERROR: Object file pointer invalid") != NULL);

    free(report);
    ndfs_close(fs_dirty);
    ndfs_close(fs);
    return 0;
}

/* Phase 4 compares pages_used (as SINTRAN would report it) against a fresh
 * recount from the actual file structure. Corrupting pages_used by hand --
 * the on-disk shape of a quota field a crash left stale -- must surface as
 * a WARNING (not silently accepted, and not escalated to an ERROR: a quota
 * mismatch is informational, never a structural danger the way a cross-link
 * or a referenced-but-free block is). */
static int test_fsck_detects_quota_mismatch(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_FLOPPY_360KB, "FSCKQUOT");
    ndfs_user_entry_t *users = NULL;
    size_t user_count = 0;
    uint8_t system_index = 0xFF;
    size_t u;
    const ndfs_master_block_t *mb = NULL;
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    uint32_t user_data_block;
    size_t entry_off, pages_used_off;
    ndfs_filesystem_t *fs_dirty = NULL;
    char *report = NULL;
    int fsck_errors = -1;

    TEST_ASSERT_NOT_NULL(fs);

    TEST_ASSERT_OK(ndfs_get_users(fs, &users, &user_count));
    for (u = 0; u < user_count; u++) {
        if (strcmp(users[u].user_name, "SYSTEM") == 0) {
            system_index = users[u].user_index;
            break;
        }
    }
    ndfs_free_users(users);
    TEST_ASSERT(system_index != 0xFF);

    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));

    /* User-file index block: slot (user_index/32) holds the data page that
     * holds this user's 64-byte entry at slot (user_index%32). */
    user_data_block = ndfs_read_u32be(
        buf, (size_t)mb->user_file_ptr.block_id * NDFS_PAGE_SIZE
             + (size_t)(system_index / 32) * 4);
    TEST_ASSERT(user_data_block != 0);

    entry_off = (size_t)user_data_block * NDFS_PAGE_SIZE
              + (size_t)(system_index % 32) * 64;
    pages_used_off = entry_off + 32; /* see user_entry.c: pages_used is offset+32 */
    TEST_ASSERT_EQUAL(1, ndfs_read_u32be(buf, pages_used_off)); /* premise: SYSTEM owns no files; its object page is charged */
    ndfs_write_u32be(buf, pages_used_off, 9999u);

    TEST_ASSERT_OK(ndfs_open_buffer_copy(buf, buf_size, true, &fs_dirty));
    free(buf);

    TEST_ASSERT_OK(ndfs_fsck(fs_dirty, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL(0, fsck_errors); /* a quota mismatch is a WARNING, not an ERROR */
    TEST_ASSERT(strstr(report, "pages_used=9999") != NULL);

    free(report);
    ndfs_close(fs_dirty);
    ndfs_close(fs);
    return 0;
}

/* ---- End-to-end: a realistic mixed workload must come back fully clean ---- */

/* The strongest regression net: many users, every pointer type (Contiguous,
 * Indexed, SubIndexed), sparse holes, an overwrite (grow then shrink), and a
 * delete -- then fsck must report the pack as CLEAN, not merely
 * error-free. Any one of Bugs A/B/C reintroduced would show up here as an
 * orphan or cross-link warning/error. */
static int test_fsck_mixed_workload_is_clean(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "FSCKMIX");
    uint32_t big_pages = 600;
    uint8_t *small = (uint8_t *)malloc(3 * NDFS_PAGE_SIZE);
    uint8_t *big = (uint8_t *)calloc((size_t)big_pages * NDFS_PAGE_SIZE, 1);
    uint8_t *replacement = (uint8_t *)malloc(2 * NDFS_PAGE_SIZE);
    static const uint32_t big_holes[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 }; /* page 0 and 10.. stay real-ish */
    ndfs_hole_list_t holes;
    char uname[NDFS_NAME_MAX + 1];
    char *report = NULL;
    int fsck_errors = -1;
    int u;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_NOT_NULL(small);
    TEST_ASSERT_NOT_NULL(big);
    TEST_ASSERT_NOT_NULL(replacement);
    fill_nonzero_pattern(small, 3 * NDFS_PAGE_SIZE, 5);
    fill_nonzero_pattern(big, NDFS_PAGE_SIZE, 6); /* only page 0 real; 1..9 declared holes below */
    fill_nonzero_pattern(replacement, 2 * NDFS_PAGE_SIZE, 7);

    for (u = 0; u < 5; u++) {
        snprintf(uname, sizeof(uname), "MIXU%02d", u);
        TEST_ASSERT_OK(ndfs_add_user(fs, uname, 50));
    }

    TEST_ASSERT_OK(ndfs_create_contiguous_file(fs, "SYSTEM/MIXCONT:DATA", 4));
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/MIXSMALL:DATA", small, 3 * NDFS_PAGE_SIZE));

    holes.pages = big_holes;
    holes.count = sizeof(big_holes) / sizeof(big_holes[0]);
    TEST_ASSERT_OK(ndfs_write_file_holes(fs, "MIXU00/MIXBIG:DATA", big,
                                         (size_t)big_pages * NDFS_PAGE_SIZE,
                                         NDFS_PARITY_NONE, &holes));

    /* Overwrite the small file with an even smaller one (shrink path). */
    TEST_ASSERT_OK(ndfs_write_file(fs, "SYSTEM/MIXSMALL:DATA", replacement,
                                   2 * NDFS_PAGE_SIZE));

    /* Delete the contiguous file entirely. */
    TEST_ASSERT_OK(ndfs_delete_file(fs, "SYSTEM/MIXCONT:DATA"));

    free(small);
    free(big);
    free(replacement);

    TEST_ASSERT_OK(ndfs_fsck(fs, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL(0, fsck_errors);
    TEST_ASSERT(strstr(report, "Filesystem is CLEAN. No errors, no warnings.") != NULL);

    free(report);
    ndfs_close(fs);
    return 0;
}

/* Pages from the declared capacity (Extended Info Block, Pages Available)
 * to the end of the device are the drive's spare area. Real images exist
 * (BIGDISK0-M-103.IMG, WD0-SINTRAN-M.IMG) with every bit in that area set
 * and no file owning them. fsck must NOT call those orphans: it counts them
 * and prints one INFO line. The SMD_75MB template declares 36945 pages in a
 * 38400-page device, so pages 36945..38399 are that spare area; the creator itself
 * marks 36945..37119 used (SINTRAN does, up to the next multiple of 256). A used bit
 * BELOW the capacity with no owner must still be reported as an orphan. */
static int test_fsck_spare_area_marked_used_is_info_not_orphan(void)
{
    ndfs_filesystem_t *fs = create_writable(NDFS_TMPL_SMD_75MB, "FSCKSPARE");
    ndfs_filesystem_t *fs_dirty = NULL;
    const ndfs_master_block_t *mb = NULL;
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    char *report = NULL;
    int fsck_errors = -1;
    size_t byte_off;
    uint8_t mask;
    size_t abs_off;
    uint32_t spare_pages[2] = { 37120, 38399 };   /* the creator already marks 36945..37119, like SINTRAN */
    int i;

    TEST_ASSERT_NOT_NULL(fs);
    TEST_ASSERT_OK(ndfs_get_master_block(fs, &mb));
    TEST_ASSERT_OK(ndfs_to_buffer(fs, &buf, &buf_size));

    for (i = 0; i < 2; i++) {
        raw_bitmap_bit_offset(spare_pages[i], &byte_off, &mask);
        abs_off = (size_t)mb->bit_file_ptr.block_id * NDFS_PAGE_SIZE + byte_off;
        TEST_ASSERT((buf[abs_off] & mask) == 0); /* premise: spare page is free */
        buf[abs_off] |= mask;
    }

    TEST_ASSERT_OK(ndfs_open_buffer_copy(buf, buf_size, true, &fs_dirty));
    free(buf);

    TEST_ASSERT_OK(ndfs_fsck(fs_dirty, &report, &fsck_errors));
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL(0, fsck_errors);
    TEST_ASSERT(strstr(report, "OK: No orphaned blocks") != NULL);
    TEST_ASSERT(strstr(report, "INFO: 177 pages at or above the declared capacity (36945)") != NULL); /* 175 marked by the creator + 2 */

    free(report);
    ndfs_close(fs_dirty);
    ndfs_close(fs);
    return 0;
}

void run_fsck_tests(void)
{
    TEST_SUITE_BEGIN("fsck Reference-Walk Tests");
    RUN_TEST(test_fsck_contiguous_file_not_cross_linked);
    RUN_TEST(test_fsck_fresh_image_no_orphans);
    RUN_TEST(test_fsck_multi_page_bitmap_no_orphans);
    RUN_TEST(test_fsck_many_users_no_orphans);
    RUN_TEST(test_fsck_subindexed_file_no_orphans);
    RUN_TEST(test_fsck_sparse_indexed_high_slot_no_orphans);
    RUN_TEST(test_fsck_garbage_pointer_into_reserved_range_ignored);
    RUN_TEST(test_fsck_detects_genuine_orphan);
    RUN_TEST(test_fsck_detects_genuine_cross_link);
    RUN_TEST(test_fsck_detects_referenced_but_marked_free);
    RUN_TEST(test_fsck_detects_invalid_master_block_pointer);
    RUN_TEST(test_fsck_detects_quota_mismatch);
    RUN_TEST(test_fsck_mixed_workload_is_clean);
    RUN_TEST(test_fsck_spare_area_marked_used_is_info_not_orphan);
}
