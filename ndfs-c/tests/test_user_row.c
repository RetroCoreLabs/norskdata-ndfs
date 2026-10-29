/**
 * Tests for the ndtool user-listing row (tools/ndtool/user_row.c), the text
 * printed by `ndtool -u` and by the shell `users` command.
 *
 * SPDX-License-Identifier: MIT
 */

#include "test_framework.h"
#include "user_row.h"
#include <ndfs/ndfs.h>
#include <string.h>

/* Build a user entry with the fields the row prints. */
static void make_user(ndfs_user_entry_t *u, uint8_t index, const char *name,
                      uint32_t reserved, uint32_t used, uint16_t password)
{
    ndfs_ue_init(u);
    u->user_index = index;
    strncpy(u->user_name, name, NDFS_NAME_MAX);
    u->user_name[NDFS_NAME_MAX] = '\0';
    u->pages_reserved = reserved;
    u->pages_used = used;
    u->password = password;
}

/* A user with a password: the word prints as 6 octal digits + B. */
static int test_row_with_password(void)
{
    ndfs_user_entry_t u;
    char line[NDTOOL_USER_ROW_MAX];

    make_user(&u, 6, "RONNY", 1000, 87, 05325);
    ndtool_format_user_row(&u, 13, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING(
        "  [006]  RONNY              13 files  Reserved:  1000  Used:    87  "
        "Free:   913  Password: 005325B",
        line);
    return 0;
}

/* No password is the word 0 and prints as 000000B, not blank. */
static int test_row_without_password(void)
{
    ndfs_user_entry_t u;
    char line[NDTOOL_USER_ROW_MAX];

    make_user(&u, 0, "SYSTEM", 15000, 11774, 0);
    ndtool_format_user_row(&u, 85, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING(
        "  [000]  SYSTEM             85 files  Reserved: 15000  Used: 11774  "
        "Free:  3226  Password: 000000B",
        line);
    return 0;
}

/* The largest 16-bit word is 177777B: still 6 digits, no overflow. */
static int test_row_max_password(void)
{
    ndfs_user_entry_t u;
    char line[NDTOOL_USER_ROW_MAX];
    const char *tail;

    make_user(&u, 017, "X", 0, 0, 0xFFFF);
    ndtool_format_user_row(&u, 0, line, sizeof(line));
    tail = strstr(line, "Password: ");
    TEST_ASSERT_NOT_NULL(tail);
    TEST_ASSERT_EQUAL_STRING("Password: 177777B", tail);
    return 0;
}

/* Over quota: Free goes negative and the password column still follows. */
static int test_row_over_quota(void)
{
    ndfs_user_entry_t u;
    char line[NDTOOL_USER_ROW_MAX];

    make_user(&u, 2, "UTILITY", 100, 150, 1);
    ndtool_format_user_row(&u, 17, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING(
        "  [002]  UTILITY            17 files  Reserved:   100  Used:   150  "
        "Free:   -50  Password: 000001B",
        line);
    return 0;
}

/* Every header label must start in the same column as the value it names,
 * so the header and the rows line up when printed one above the other. */
static int test_header_lines_up_with_row(void)
{
    ndfs_user_entry_t u;
    char header[NDTOOL_USER_ROW_MAX];
    char line[NDTOOL_USER_ROW_MAX];
    const char *p;

    make_user(&u, 6, "RONNY", 1000, 87, 05325);
    ndtool_format_user_header(header, sizeof(header));
    ndtool_format_user_row(&u, 13, line, sizeof(line));

    /* Index over "[006]" */
    p = strstr(header, "Index");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(p - header, strstr(line, "[006]") - line);

    /* Name over "RONNY" */
    p = strstr(header, "Name");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(p - header, strstr(line, "RONNY") - line);

    /* Files over the right-aligned 3-digit count */
    p = strstr(header, "Files");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(p - header, strstr(line, " 13 files") - line);

    /* Reserved / Used / Free / Password over their labels in the row */
    p = strstr(header, "Reserved");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(p - header, strstr(line, "Reserved:") - line);

    p = strstr(header, "Used");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(p - header, strstr(line, "Used:") - line);

    p = strstr(header, "Free");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(p - header, strstr(line, "Free:") - line);

    p = strstr(header, "Password");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL(p - header, strstr(line, "Password:") - line);

    /* Password is the LAST column: nothing follows the B. */
    p = strstr(line, "Password:");
    TEST_ASSERT_EQUAL('B', p[strlen("Password: 005325")]);
    TEST_ASSERT_EQUAL('\0', p[strlen("Password: 005325B")]);
    return 0;
}

/* The row fits the buffer size the header promises. */
static int test_row_fits_buffer(void)
{
    ndfs_user_entry_t u;
    char line[NDTOOL_USER_ROW_MAX];
    int n;

    make_user(&u, 0377, "SIXTEEN-CHAR-NAM", 0xFFFFFFFFu, 0, 0xFFFF);
    n = ndtool_format_user_row(&u, 4096, line, sizeof(line));
    TEST_ASSERT(n > 0);
    TEST_ASSERT(n < NDTOOL_USER_ROW_MAX);
    return 0;
}

void run_user_row_tests(void)
{
    TEST_SUITE_BEGIN("ndtool User Row Tests");

    RUN_TEST(test_row_with_password);
    RUN_TEST(test_row_without_password);
    RUN_TEST(test_row_max_password);
    RUN_TEST(test_row_over_quota);
    RUN_TEST(test_header_lines_up_with_row);
    RUN_TEST(test_row_fits_buffer);
}
