/**
 * Main test runner for NDFS C library.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 */

#include "test_framework.h"

int tf_tests_run    = 0;
int tf_tests_passed = 0;
int tf_tests_failed = 0;

/* Suite declarations */
extern void run_endian_tests(void);
extern void run_nd_time_tests(void);
extern void run_block_pointer_tests(void);
extern void run_master_block_tests(void);
extern void run_bit_file_tests(void);
extern void run_user_entry_tests(void);
extern void run_object_entry_tests(void);
extern void run_object_block_tests(void);
extern void run_filesystem_tests(void);
extern void run_image_creator_tests(void);
extern void run_boot_loader_tests(void);
extern void run_sintran_tests(void);
extern void run_write_comprehensive_tests(void);
extern void run_fsck_tests(void);
extern void run_contiguous_file_tests(void);
extern void run_xat_tests(void);
extern void run_xat_copy_tests(void);
extern void run_parity_tests(void);
extern void run_wildmatch_tests(void);
extern void run_golden_tests(void);
extern void run_backend_stdio_tests(void);
extern void run_damaged_media_tests(void);

int main(void)
{
    printf("NDFS C Library Tests\n");
    printf("========================================\n");

    run_endian_tests();
    run_nd_time_tests();
    run_block_pointer_tests();
    run_master_block_tests();
    run_bit_file_tests();
    run_user_entry_tests();
    run_object_entry_tests();
    run_object_block_tests();
    run_filesystem_tests();
    run_image_creator_tests();
    run_boot_loader_tests();
    run_sintran_tests();
    run_write_comprehensive_tests();
    run_fsck_tests();
    run_contiguous_file_tests();
    run_xat_tests();
    run_xat_copy_tests();
    run_parity_tests();
    run_wildmatch_tests();
    run_golden_tests();
    run_backend_stdio_tests();
    run_damaged_media_tests();

    TEST_SUITE_REPORT();

    return tf_tests_failed > 0 ? 1 : 0;
}
