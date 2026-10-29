/**
 * ndtool: list files and users.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ndtool.h"
#include "user_row.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Version chain ────────────────────────────────────────────────── */

/* Compute the SINTRAN-style version ordinal (the ";N" suffix). A single-
 * version file points its next/prev version at itself -> version 1. For a
 * chained file we count predecessors via prev_version. Best-effort for the
 * rare multi-version case; exact (=1) for the universal single-version case. */
static unsigned file_version(const ndfs_object_entry_t *objs, size_t n,
                             const ndfs_object_entry_t *e)
{
    unsigned v = 1;
    uint16_t cur;
    size_t guard;

    if (e->next_version == e->disk_object_index &&
        e->prev_version == e->disk_object_index) {
        return 1;
    }
    cur = e->prev_version;
    for (guard = 0; guard < n; guard++) {
        const ndfs_object_entry_t *p = NULL;
        size_t k;
        if (cur == e->disk_object_index) break;
        for (k = 0; k < n; k++) {
            if (objs[k].disk_object_index == cur) { p = &objs[k]; break; }
        }
        if (!p) break;
        v++;
        if (p->prev_version == cur) break;
        cur = p->prev_version;
    }
    return v;
}

/* ── List files for one user ──────────────────────────────────────── */

/* List a user's files from the object-entry table, in object-id order, with
 * the SINTRAN-style "NNNN NAME:TYPE;V" layout (object id in 4-digit octal). */
static void list_user_files(const char *user_name, const char *file_pat,
                            const ndfs_object_entry_t *objs, size_t n)
{
    size_t i;
    char full[NDFS_NAME_MAX + NDFS_TYPE_MAX + 2];
    char named[40];
    char datebuf[24];

    for (i = 0; i < n; i++) {
        const ndfs_object_entry_t *e = &objs[i];
        if (strcmp(e->user_name, user_name) != 0) continue;
        ndfs_oe_full_name(e, full, sizeof(full));
        if (file_pat && !ndfs_wildmatch(file_pat, full, true)) continue;

        /* "NAME:TYPE;V" joined tightly, like SINTRAN's investigator. */
        snprintf(named, sizeof(named), "%s;%u", full, file_version(objs, n, e));
        ndtool_format_nd_date(e->date_created, datebuf, sizeof(datebuf));
        printf("  [%04o]  %s  (%s)%-28s %10u bytes  %5u pages\n",
               e->disk_object_index, datebuf,
               e->user_name, named,
               e->bytes_in_file, e->pages_in_file);
    }
}

/* ── Volume header ────────────────────────────────────────────────── */

/* Print the volume (directory) name from the master block, if present. */
static void print_volume_header(ndtool_ctx_t *ctx)
{
    const ndfs_master_block_t *mb = NULL;

    if (ndfs_get_master_block(ctx->fs, &mb) != NDFS_OK || !mb) return;
    if (mb->directory_name[0] == '\0') return;

    printf("Volume: %s\n\n", mb->directory_name);
}

/* ── cmd_list ─────────────────────────────────────────────────────── */

int cmd_list(ndtool_ctx_t *ctx)
{
    char user_pat_buf[NDFS_NAME_MAX + 1];
    const char *user_pat = NULL;
    const char *file_pat = ctx->filter_file;
    ndfs_file_entry_t *users = NULL;
    size_t user_count = 0;
    ndfs_object_entry_t *objs = NULL;
    size_t obj_count = 0;
    size_t i;
    ndfs_error_t err;

    print_volume_header(ctx);

    /* A -F pattern of the form USER/FILE splits into a user glob and a file
     * glob; otherwise it filters file names within the selected user(s). */
    if (file_pat) {
        const char *slash = strchr(file_pat, '/');
        if (slash) {
            size_t ulen = (size_t)(slash - file_pat);
            if (ulen >= sizeof(user_pat_buf)) ulen = sizeof(user_pat_buf) - 1;
            memcpy(user_pat_buf, file_pat, ulen);
            user_pat_buf[ulen] = '\0';
            user_pat = user_pat_buf;
            file_pat = slash + 1;
        }
    }
    if (!user_pat && ctx->filter_user) user_pat = ctx->filter_user;

    err = ndfs_list_directory(ctx->fs, "/", &users, &user_count);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error listing root: %s\n", ndfs_strerror(err));
        return -1;
    }

    /* All object entries up front, so per-file object id + version are shown
     * without re-reading the object file for each user. */
    ndfs_get_object_entries(ctx->fs, &objs, &obj_count);

    for (i = 0; i < user_count; i++) {
        if (!users[i].is_directory) continue;
        if (user_pat && !ndfs_wildmatch(user_pat, users[i].name, true)) continue;
        printf("USER: %s\n", users[i].name);
        list_user_files(users[i].name, file_pat, objs, obj_count);
    }

    ndfs_free_object_entries(objs);
    ndfs_free_entries(users);
    return 0;
}

/* ── cmd_users ────────────────────────────────────────────────────── */

int cmd_users(ndtool_ctx_t *ctx)
{
    ndfs_user_entry_t *users = NULL;
    size_t count = 0;
    size_t i;
    ndfs_error_t err;

    err = ndfs_get_users(ctx->fs, &users, &count);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error getting users: %s\n", ndfs_strerror(err));
        return -1;
    }

    printf("Users: %zu\n", count);
    /* Column header, then one row per user. The text of both comes from
     * user_row.c so the unit tests can check it; the password column was
     * added there (6-digit octal with a trailing B, 000000B = no password). */
    {
        char line[NDTOOL_USER_ROW_MAX];
        ndtool_format_user_header(line, sizeof(line));
        printf("%s\n", line);
    }
    for (i = 0; i < count; i++) {
        char line[NDTOOL_USER_ROW_MAX];
        ndfs_file_entry_t *files = NULL;
        size_t file_count = 0;
        ndfs_list_directory(ctx->fs, users[i].user_name, &files, &file_count);
        ndfs_free_entries(files);
        /* User index shown in 3-digit octal, matching SINTRAN's
         * file-system-investigator (LIST-USERS) e.g. "011 BUILD". */
        ndtool_format_user_row(&users[i], file_count, line, sizeof(line));
        printf("%s\n", line);

        if (ctx->verbose) {
            int j;
            printf("         Friends:");
            for (j = 0; j < NDFS_MAX_FRIENDS; j++) {
                if (ndfs_uf_is_active(&users[i].friends[j])) {
                    printf(" [%u]", ndfs_uf_friend_index(&users[i].friends[j]));
                }
            }
            printf("\n");
        }
    }

    ndfs_free_users(users);
    return 0;
}
