/**
 * ndtool: one text row of the user listing.
 *
 * Shared by `ndtool -u` and the shell `users` command (both go through
 * cmd_users in cmd_list.c). Kept in its own file with no ndtool.h dependency
 * so the unit tests in tests/test_user_row.c can build it against the
 * library alone and check the exact text, column by column.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NDTOOL_USER_ROW_H
#define NDTOOL_USER_ROW_H

#include <ndfs/ndfs.h>
#include <stddef.h>

/* Largest row the formatter can produce, including the terminating NUL.
 * A row is about 100 characters; this leaves room for a 16-byte name full of
 * multibyte characters (names are plain ASCII on a real pack, but be safe). */
#define NDTOOL_USER_ROW_MAX 160

/**
 * Write the column header line for the user listing into buf.
 * Every label starts in the same column as the value it names in a row from
 * ndtool_format_user_row, so the two line up when printed one above the other.
 * No trailing newline.
 * @return the number of characters written (not counting the NUL), as snprintf.
 */
int ndtool_format_user_header(char *buf, size_t buf_len);

/**
 * Write one row of the user listing into buf. No trailing newline.
 *
 * Columns, left to right:
 *   [index]   user index in 3-digit octal, matching SINTRAN's
 *             file-system-investigator (LIST-USERS) e.g. "011 BUILD"
 *   name      user name, left-aligned in 16 characters
 *   files     file_count, the number of files the caller counted
 *   Reserved  pages_reserved
 *   Used      pages_used
 *   Free      Reserved - Used (can go negative when a user is over quota)
 *   Password  the 16-bit password word in 6-digit octal with a trailing B,
 *             the way SINTRAN prints octal numbers; 000000B means no password
 *
 * @param user        the user entry to print
 * @param file_count  how many files the user has (counted by the caller)
 * @param buf         receives the text
 * @param buf_len     size of buf; NDTOOL_USER_ROW_MAX is always enough
 * @return the number of characters written (not counting the NUL), as snprintf.
 */
int ndtool_format_user_row(const ndfs_user_entry_t *user, size_t file_count,
                           char *buf, size_t buf_len);

#endif /* NDTOOL_USER_ROW_H */
