/**
 * ndtool: one text row of the user listing.
 *
 * See user_row.h for the column layout. The header and the row are written
 * with matching field widths so the labels sit over the values:
 *
 *   col  0  "  "
 *   col  2  [ooo]      5 wide, then 2 spaces
 *   col  9  name      16 wide, then 2 spaces
 *   col 27  NNN files  9 wide, then 2 spaces
 *   col 38  Reserved: NNNNN  15 wide, then 2 spaces
 *   col 55  Used: NNNNN      11 wide, then 2 spaces
 *   col 68  Free: NNNNN      11 wide, then 2 spaces
 *   col 81  Password: ooooooB
 *
 * If you change a width here, change it in both functions, and the alignment
 * test in tests/test_user_row.c will tell you if they drifted apart.
 *
 * SPDX-License-Identifier: MIT
 */

#include "user_row.h"

#include <stdio.h>

int ndtool_format_user_header(char *buf, size_t buf_len)
{
    /* Each label is padded to the width of the whole field it sits over,
     * including the two separating spaces that follow it in a row. */
    return snprintf(buf, buf_len, "  %-7s%-18s%-11s%-17s%-13s%-13s%s",
                    "Index", "Name", "Files", "Reserved", "Used", "Free",
                    "Password");
}

int ndtool_format_user_row(const ndfs_user_entry_t *user, size_t file_count,
                           char *buf, size_t buf_len)
{
    int32_t free_pages = ndfs_ue_free_pages(user);

    /* Password: the 16-bit word at bytes 18-19 of the user entry, printed
     * as 6 octal digits with a trailing B (SINTRAN's own octal notation).
     * We only know it as a 16-bit word; how SINTRAN makes it from the typed
     * password is not documented here. A cleared password (`passwd NAME` /
     * --passwd) is the word 0, which prints as 000000B. */
    return snprintf(buf, buf_len,
                    "  [%03o]  %-16s  %3zu files  Reserved: %5u  Used: %5u  "
                    "Free: %5d  Password: %06oB",
                    user->user_index,
                    user->user_name,
                    file_count,
                    user->pages_reserved,
                    user->pages_used,
                    free_pages,
                    (unsigned)user->password);
}
