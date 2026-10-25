/**
 * ndtool: user management commands.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ndtool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/**
 * Find a user by name (case-insensitive) and return their entry.
 * Returns NDFS_OK and fills out_user, or NDFS_ERR_NOT_FOUND.
 */
static ndfs_error_t find_user_by_name(ndtool_ctx_t *ctx, const char *name,
                                       ndfs_user_entry_t *out_user)
{
    ndfs_user_entry_t *users = NULL;
    size_t count = 0;
    ndfs_error_t err = ndfs_get_users(ctx->fs, &users, &count);
    if (err != NDFS_OK) return err;

    /* Uppercase the search name */
    char upper[NDFS_NAME_MAX + 1];
    size_t i;
    for (i = 0; i < NDFS_NAME_MAX && name[i]; i++) {
        upper[i] = (char)toupper((unsigned char)name[i]);
    }
    upper[i] = '\0';

    for (i = 0; i < count; i++) {
        if (strcmp(users[i].user_name, upper) == 0) {
            if (out_user) *out_user = users[i];
            ndfs_free_users(users);
            return NDFS_OK;
        }
    }

    ndfs_free_users(users);
    return NDFS_ERR_NOT_FOUND;
}

int cmd_useradd(ndtool_ctx_t *ctx, const char *name, uint32_t quota)
{
    ndfs_error_t err;

    if (ctx->dry_run) {
        printf("Would add user '%s' with quota %u [dry run]\n", name, quota);
        return 0;
    }

    err = ndfs_add_user(ctx->fs, name, quota);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error adding user '%s': %s\n", name, ndfs_strerror(err));
        return -1;
    }

    if (ndtool_save_image(ctx) != 0) return -1;

    printf("Added user '%s' with quota %u pages\n", name, quota);
    ctx->modified = false;
    return 0;
}

int cmd_userdel(ndtool_ctx_t *ctx, const char *name)
{
    ndfs_error_t err;
    ndfs_user_entry_t user;

    /* Find user by name */
    err = find_user_by_name(ctx, name, &user);
    if (err != NDFS_OK) {
        fprintf(stderr, "User '%s' not found\n", name);
        return -1;
    }

    if (ctx->dry_run) {
        printf("Would remove user '%s' (index %u) [dry run]\n",
               user.user_name, user.user_index);
        return 0;
    }

    err = ndfs_remove_user(ctx->fs, user.user_index);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error removing user '%s': %s\n", name, ndfs_strerror(err));
        return -1;
    }

    if (ndtool_save_image(ctx) != 0) return -1;

    printf("Removed user '%s' (index %u)\n", user.user_name, user.user_index);
    ctx->modified = false;
    return 0;
}

int cmd_addquota(ndtool_ctx_t *ctx, const char *name, uint32_t pages)
{
    ndfs_error_t err;
    ndfs_user_entry_t user;
    uint32_t free_on_disk = 0;

    /* Find user by name */
    err = find_user_by_name(ctx, name, &user);
    if (err != NDFS_OK) {
        fprintf(stderr, "User '%s' not found\n", name);
        return -1;
    }

    /* Check disk space */
    ndfs_get_free_pages(ctx->fs, &free_on_disk);
    if (pages > free_on_disk) {
        fprintf(stderr, "Cannot add %u pages: only %u free on disk\n",
                pages, free_on_disk);
        return -1;
    }

    uint32_t new_quota = user.pages_reserved + pages;

    if (ctx->dry_run) {
        printf("Would add %u pages to '%s' quota (%u -> %u) [dry run]\n",
               pages, user.user_name, user.pages_reserved, new_quota);
        return 0;
    }

    err = ndfs_update_user_quota(ctx->fs, user.user_index, new_quota);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error updating quota: %s\n", ndfs_strerror(err));
        return -1;
    }

    if (ndtool_save_image(ctx) != 0) return -1;

    printf("User '%s' quota: %u -> %u pages (+%u)\n",
           user.user_name, user.pages_reserved, new_quota, pages);
    ctx->modified = false;
    return 0;
}

int cmd_remquota(ndtool_ctx_t *ctx, const char *name, uint32_t pages)
{
    ndfs_error_t err;
    ndfs_user_entry_t user;

    /* Find user by name */
    err = find_user_by_name(ctx, name, &user);
    if (err != NDFS_OK) {
        fprintf(stderr, "User '%s' not found\n", name);
        return -1;
    }

    /* Check we're not going below used pages */
    if (pages > user.pages_reserved) {
        fprintf(stderr, "Cannot remove %u pages: user only has %u reserved\n",
                pages, user.pages_reserved);
        return -1;
    }

    uint32_t new_quota = user.pages_reserved - pages;
    if (new_quota < user.pages_used) {
        fprintf(stderr, "Cannot reduce quota to %u: user is using %u pages\n",
                new_quota, user.pages_used);
        return -1;
    }

    if (ctx->dry_run) {
        printf("Would remove %u pages from '%s' quota (%u -> %u) [dry run]\n",
               pages, user.user_name, user.pages_reserved, new_quota);
        return 0;
    }

    err = ndfs_update_user_quota(ctx->fs, user.user_index, new_quota);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error updating quota: %s\n", ndfs_strerror(err));
        return -1;
    }

    if (ndtool_save_image(ctx) != 0) return -1;

    printf("User '%s' quota: %u -> %u pages (-%u)\n",
           user.user_name, user.pages_reserved, new_quota, pages);
    ctx->modified = false;
    return 0;
}

int cmd_passwd(ndtool_ctx_t *ctx, const char *name)
{
    ndfs_error_t err;

    if (ctx->dry_run) {
        printf("Would clear password for '%s' [dry run]\n", name);
        return 0;
    }

    err = ndfs_clear_user_password(ctx->fs, name);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error clearing password for '%s': %s\n", name, ndfs_strerror(err));
        return -1;
    }

    if (ndtool_save_image(ctx) != 0) return -1;

    printf("Password cleared for '%s'\n", name);
    ctx->modified = false;
    return 0;
}

/* --release: clear the "directory entered" flag and the system number that SINTRAN left in
 * the Extended Info Block when the emulator stopped without releasing the directory. The
 * boot then no longer prints DIRECTORY ENTERED BY ANOTHER SYSTEM. Stop the emulator first:
 * ndtool cannot see whether SINTRAN has the directory entered right now. */
int cmd_release(ndtool_ctx_t *ctx)
{
    ndfs_error_t err;
    bool entered = false;
    uint16_t sys = 0;
    const ndfs_master_block_t *mb = NULL;

    if (ndfs_get_master_block(ctx->fs, &mb) != NDFS_OK || !mb) {
        fprintf(stderr, "Error reading the master block\n");
        return -1;
    }
    entered = (mb->ext_flag_word & NDFS_EXT_FLAG_ENTERED) != 0;
    sys = mb->ext_last_system_number;

    if (!entered && sys == 0) {
        printf("Directory is not marked entered and has no system number: nothing to do\n");
        ctx->modified = false;
        return 0;
    }
    if (ctx->dry_run) {
        printf("Would release the directory: entered=%s, system number %o (octal) [dry run]\n",
               entered ? "yes" : "no", (unsigned)sys);
        return 0;
    }

    err = ndfs_release_directory(ctx->fs, NULL, NULL);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error releasing the directory: %s\n", ndfs_strerror(err));
        return -1;
    }
    if (ndtool_save_image(ctx) != 0) return -1;

    printf("Directory released: entered flag cleared, system number %o (octal) -> 0\n", (unsigned)sys);
    ctx->modified = false;
    return 0;
}

/* ── Friends ──────────────────────────────────────────────────────────
 * Friend commands take colon-packed args so each maps to one CLI option:
 *   --friendadd OWNER:FRIEND[:RWACD]   --frienddel OWNER:FRIEND
 * OWNER/FRIEND may be a user name or a decimal index (0-255). The shell
 * passes the same packed forms. */

/* Split "a:b[:c]" in place into up to 3 fields. Returns the field count. */
static int split_colon(char *s, char *fields[3])
{
    int n = 0;
    char *p = s;
    fields[0] = fields[1] = fields[2] = NULL;
    if (!s || !*s) return 0;
    fields[n++] = p;
    for (; *p && n < 3; p++) {
        if (*p == ':') {
            *p = '\0';
            fields[n++] = p + 1;
        }
    }
    return n;
}

int cmd_friend_add(ndtool_ctx_t *ctx, const char *arg)
{
    char buf[96];
    char *f[3];
    const char *owner, *friend, *perms;
    ndfs_error_t err;
    int n;

    strncpy(buf, arg ? arg : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    n = split_colon(buf, f);
    if (n < 2 || !f[0][0] || !f[1][0]) {
        fprintf(stderr, "Usage: --friendadd OWNER:FRIEND[:RWACD]\n");
        return -1;
    }
    owner = f[0];
    friend = f[1];
    perms = (n >= 3 && f[2][0]) ? f[2] : "RWA";

    if (ctx->dry_run) {
        printf("Would add friend '%s' to '%s' with rights %s [dry run]\n",
               friend, owner, perms);
        return 0;
    }

    err = ndfs_add_friend(ctx->fs, owner, friend, perms);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error adding friend: %s\n", ndfs_strerror(err));
        return -1;
    }
    if (ndtool_save_image(ctx) != 0) return -1;
    printf("Added friend '%s' to '%s' (%s)\n", friend, owner, perms);
    ctx->modified = false;
    return 0;
}

int cmd_friend_del(ndtool_ctx_t *ctx, const char *arg)
{
    char buf[96];
    char *f[3];
    const char *owner, *friend;
    ndfs_error_t err;
    int n;

    strncpy(buf, arg ? arg : "", sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    n = split_colon(buf, f);
    if (n < 2 || !f[0][0] || !f[1][0]) {
        fprintf(stderr, "Usage: --frienddel OWNER:FRIEND\n");
        return -1;
    }
    owner = f[0];
    friend = f[1];

    if (ctx->dry_run) {
        printf("Would remove friend '%s' from '%s' [dry run]\n", friend, owner);
        return 0;
    }

    err = ndfs_remove_friend(ctx->fs, owner, friend);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error removing friend: %s\n", ndfs_strerror(err));
        return -1;
    }
    if (ndtool_save_image(ctx) != 0) return -1;
    printf("Removed friend '%s' from '%s'\n", friend, owner);
    ctx->modified = false;
    return 0;
}

int cmd_friend_list(ndtool_ctx_t *ctx, const char *user_ref)
{
    ndfs_friend_info_t *friends = NULL;
    size_t count = 0, i;
    ndfs_error_t err;

    err = ndfs_list_friends(ctx->fs, user_ref, &friends, &count);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error listing friends for '%s': %s\n",
                user_ref, ndfs_strerror(err));
        return -1;
    }

    if (count == 0) {
        printf("User '%s' has no friends.\n", user_ref);
        return 0;
    }

    printf("Friends of '%s': %zu\n", user_ref, count);
    for (i = 0; i < count; i++) {
        if (friends[i].name[0]) {
            printf("  [%3u]  %-16s  %s\n",
                   friends[i].index, friends[i].name, friends[i].perms);
        } else {
            printf("  [%3u]  %-16s  %s\n",
                   friends[i].index, "(no such user)", friends[i].perms);
        }
    }
    ndfs_free_friends(friends);
    return 0;
}

/*
 * --objblocks NAME  -- report a user's object-block position.
 *
 * A user's file entries live in OBJECT BLOCKS of 256 files each: one when the
 * user is created, up to 16 (4096 files) once granted. This is the command that
 * answers "why can I not add another file to this user".
 *
 * See docs/NDFS-OBJECT-BLOCKS-SPEC.md.
 */
int cmd_objblocks(ndtool_ctx_t *ctx, const char *name)
{
    ndfs_error_t err;
    ndfs_object_block_info_t info;

    err = ndfs_get_object_block_info(ctx->fs, name, &info);
    if (err != NDFS_OK) {
        fprintf(stderr, "User '%s' not found\n", name);
        return -1;
    }

    printf("User               : %s (index %u)\n", info.user_name, info.user_index);
    printf("Object blocks      : %u allocated of %u max\n",
           info.allocated_object_blocks, info.max_object_blocks);
    printf("Files in use       : %u of %u maximum\n", info.files_in_use, info.max_files);
    printf("Without new block  : %u files\n", info.allocated_files);

    if (info.files_in_use >= info.max_files) {
        if (info.grantable_blocks == 0) {
            /* 16 blocks is the SINTRAN ceiling - do not send the operator to a
             * command that can only refuse. */
            printf("\n*** FULL. This is the SINTRAN maximum of %u files;"
                   " no more object blocks can be granted.\n"
                   "    Delete files, or use another user area.\n",
                   (unsigned)(NDFS_MAX_OBJECT_BLOCKS * NDFS_FILES_PER_OBJECT_BLOCK));
        } else {
            printf("\n*** FULL. Grant more with:  ndtool --giveobjblocks %s N IMAGE\n"
                   "    (up to %u more block(s), %u files each)\n",
                   info.user_name, info.grantable_blocks,
                   (unsigned)NDFS_FILES_PER_OBJECT_BLOCK);
        }
    } else if (info.grantable_blocks > 0) {
        printf("Can still be given : %u more block(s)\n", info.grantable_blocks);
    } else {
        printf("Can still be given : none - already at the SINTRAN maximum\n");
    }

    return 0;
}

/*
 * --giveobjblocks NAME COUNT  -- the @GIVE-OBJECT-BLOCKS equivalent.
 *
 * ADDS object blocks to a user's maximum, exactly as the SINTRAN operator
 * command does. One block is 256 files; the ceiling is 16 blocks = 4096 files.
 *
 * This raises only the MAXIMUM. Blocks are allocated on demand as files are
 * created, and no disk pages are reserved - object blocks cap the NUMBER of
 * files, not their size (for that, see --quotaadd).
 */
int cmd_giveobjblocks(ndtool_ctx_t *ctx, const char *name, uint32_t count)
{
    ndfs_error_t err;
    ndfs_object_block_info_t info;
    uint8_t new_max = 0;

    err = ndfs_get_object_block_info(ctx->fs, name, &info);
    if (err != NDFS_OK) {
        fprintf(stderr, "User '%s' not found\n", name);
        return -1;
    }

    if (count < 1) {
        fprintf(stderr, "The number of object blocks to give must be 1 or more\n");
        return -1;
    }

    if (info.grantable_blocks == 0) {
        fprintf(stderr,
                "User '%s' already has all %u object blocks (%u files).\n"
                "This is the SINTRAN maximum - no further object blocks can be granted.\n",
                info.user_name, (unsigned)NDFS_MAX_OBJECT_BLOCKS, info.max_files);
        return -1;
    }

    if (count > info.grantable_blocks) {
        fprintf(stderr,
                "Cannot give user '%s' %u more object block(s): they have %u and "
                "SINTRAN allows at most %u (%u files). At most %u more can be granted.\n",
                info.user_name, count, info.max_object_blocks,
                (unsigned)NDFS_MAX_OBJECT_BLOCKS,
                (unsigned)(NDFS_MAX_OBJECT_BLOCKS * NDFS_FILES_PER_OBJECT_BLOCK),
                info.grantable_blocks);
        return -1;
    }

    if (ctx->dry_run) {
        printf("Would give '%s' %u more object block(s): max files %u -> %u [dry run]\n",
               info.user_name, count, info.max_files,
               (unsigned)((info.max_object_blocks + count) * NDFS_FILES_PER_OBJECT_BLOCK));
        return 0;
    }

    err = ndfs_give_object_blocks(ctx->fs, name, count, &new_max);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error giving object blocks: %s\n", ndfs_strerror(err));
        return -1;
    }

    if (ndtool_save_image(ctx) != 0) return -1;

    printf("User '%s' object blocks: %u -> %u  (max files %u -> %u)\n",
           info.user_name, info.max_object_blocks, new_max, info.max_files,
           (unsigned)(new_max * NDFS_FILES_PER_OBJECT_BLOCK));
    ctx->modified = false;
    return 0;
}

/*
 * --takeobjblocks NAME COUNT  -- lower a user's object-block maximum.
 *
 * THIS HAS NO SINTRAN EQUIVALENT: no command to remove object blocks is
 * documented, which is consistent with the structure since a file's number is
 * derived from the block it sits in. It exists only to undo a grant on an image
 * being built, and refuses to drop the maximum below the allocated count.
 */
int cmd_takeobjblocks(ndtool_ctx_t *ctx, const char *name, uint32_t count)
{
    ndfs_error_t err;
    ndfs_object_block_info_t info;
    uint8_t new_max = 0;

    err = ndfs_get_object_block_info(ctx->fs, name, &info);
    if (err != NDFS_OK) {
        fprintf(stderr, "User '%s' not found\n", name);
        return -1;
    }

    if (count < 1) {
        fprintf(stderr, "The number of object blocks to take must be 1 or more\n");
        return -1;
    }

    if (ctx->dry_run) {
        printf("Would take %u object block(s) from '%s' [dry run]\n", count, info.user_name);
        return 0;
    }

    err = ndfs_take_object_blocks(ctx->fs, name, count, &new_max);
    if (err == NDFS_ERR_OUT_OF_RANGE) {
        fprintf(stderr,
                "Cannot take %u object block(s) from '%s': %u of %u are allocated and "
                "hold files. Lowering the maximum below the allocated count would orphan them.\n",
                count, info.user_name, info.allocated_object_blocks, info.max_object_blocks);
        return -1;
    }
    if (err != NDFS_OK) {
        fprintf(stderr, "Error taking object blocks: %s\n", ndfs_strerror(err));
        return -1;
    }

    if (ndtool_save_image(ctx) != 0) return -1;

    printf("User '%s' object blocks: %u -> %u  (max files %u -> %u)\n",
           info.user_name, info.max_object_blocks, new_max, info.max_files,
           (unsigned)(new_max * NDFS_FILES_PER_OBJECT_BLOCK));
    ctx->modified = false;
    return 0;
}
