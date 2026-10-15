/**
 * Minimal test framework for NDFS C library tests.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern int tf_tests_run;
extern int tf_tests_passed;
extern int tf_tests_failed;

#define TEST_ASSERT(condition) do { \
    if (!(condition)) { \
        printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL(expected, actual) do { \
    long long _e = (long long)(expected); \
    long long _a = (long long)(actual); \
    if (_e != _a) { \
        printf("  FAIL: %s:%d: expected %lld, got %lld\n", \
               __FILE__, __LINE__, _e, _a); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_UINT(expected, actual) do { \
    unsigned long long _e = (unsigned long long)(expected); \
    unsigned long long _a = (unsigned long long)(actual); \
    if (_e != _a) { \
        printf("  FAIL: %s:%d: expected %llu, got %llu\n", \
               __FILE__, __LINE__, _e, _a); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) do { \
    const char *_e = (expected); \
    const char *_a = (actual); \
    if (strcmp(_e, _a) != 0) { \
        printf("  FAIL: %s:%d: expected \"%s\", got \"%s\"\n", \
               __FILE__, __LINE__, _e, _a); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        printf("  FAIL: %s:%d: expected NULL\n", __FILE__, __LINE__); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        printf("  FAIL: %s:%d: expected non-NULL\n", __FILE__, __LINE__); \
        return 1; \
    } \
} while(0)

#define TEST_ASSERT_OK(err) do { \
    ndfs_error_t _err = (err); \
    if (_err != NDFS_OK) { \
        printf("  FAIL: %s:%d: expected NDFS_OK, got %d\n", \
               __FILE__, __LINE__, _err); \
        return 1; \
    } \
} while(0)

#define RUN_TEST(func) do { \
    tf_tests_run++; \
    printf("  %s ... ", #func); \
    if (func() == 0) { \
        tf_tests_passed++; \
        printf("OK\n"); \
    } else { \
        tf_tests_failed++; \
    } \
} while(0)

#define TEST_SUITE_BEGIN(name) \
    printf("\n=== %s ===\n", name)

#define TEST_SUITE_REPORT() do { \
    printf("\n========================================\n"); \
    printf("Tests run: %d, Passed: %d, Failed: %d\n", \
           tf_tests_run, tf_tests_passed, tf_tests_failed); \
    if (tf_tests_failed > 0) { \
        printf("SOME TESTS FAILED\n"); \
    } else { \
        printf("ALL TESTS PASSED\n"); \
    } \
    printf("========================================\n"); \
} while(0)

#endif /* TEST_FRAMEWORK_H */
