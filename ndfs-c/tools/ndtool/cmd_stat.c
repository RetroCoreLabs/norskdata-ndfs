/**
 * ndtool: detailed file stat (RetroCommander-style), with -v for the full
 * record including the data-block list.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ndtool.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* Portable case-insensitive string compare (avoids strcasecmp/_stricmp). */
static int ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Format an ND-100 packed timestamp (32-bit) as "YYYY-MM-DD HH:MM:SS", or
 * "(not set)" for a raw value of 0. Thin wrapper over the library's
 * ndfs_nd_time_format (ndfs/nd_time.h) — the actual bit-layout decode lives
 * there in exactly one place, shared with the RetroFS.NDFS (C#) and
 * ndfs-py/ndfs-ts ports of this same format. */
void ndtool_format_nd_date(uint32_t v, char *out, size_t len)
{
    ndfs_nd_time_format(v, out, len);
}

/* Decode one 5-bit access tier into a human list, e.g. "READ, WRITE". */
void ndtool_access_tier_str(uint16_t access_bits, unsigned shift,
                            char *out, size_t len)
{
    unsigned t = (access_bits >> shift) & NDFS_ACC_TIER_MASK;
    size_t n = 0;
    out[0] = '\0';
    if (t == 0) {
        snprintf(out, len, "NONE");
        return;
    }
    #define APPEND_RIGHT(bit, name) \
        do { \
            if (t & (bit)) { \
                int w = snprintf(out + n, len > n ? len - n : 0, \
                                 "%s%s", n ? ", " : "", name); \
                if (w > 0) n += (size_t)w; \
            } \
        } while (0)
    APPEND_RIGHT(NDFS_ACC_READ,      "READ");
    APPEND_RIGHT(NDFS_ACC_WRITE,     "WRITE");
    APPEND_RIGHT(NDFS_ACC_APPEND,    "APPEND");
    APPEND_RIGHT(NDFS_ACC_COMMON,    "COMMON");
    APPEND_RIGHT(NDFS_ACC_DIRECTORY, "DIRECTORY");
    #undef APPEND_RIGHT
}

/* Decode file_type_flags into the SINTRAN letter set, e.g. "I" or "It". */
static void file_type_flags_str(uint16_t f, char *out, size_t len)
{
    size_t n = 0;
    out[0] = '\0';
    #define FLAG(bit, ch) do { if ((f & (bit)) && n + 1 < len) out[n++] = (ch); } while (0)
    FLAG(NDFS_FT_LIBRARY,    'L');
    FLAG(NDFS_FT_MAGTAPE,    'M');
    FLAG(NDFS_FT_ALLOCATED,  'A');
    FLAG(NDFS_FT_CONTIGUOUS, 'C');
    FLAG(NDFS_FT_INDEXED,    'I');
    FLAG(NDFS_FT_SPOOLING,   'S');
    FLAG(NDFS_FT_PERIPHERAL, 'P');
    FLAG(NDFS_FT_TERMINAL,   'T');
    #undef FLAG
    out[n] = '\0';
    if (n == 0) snprintf(out, len, "-");
}

static const char *ptr_type_str(ndfs_pointer_type_t t)
{
    switch (t) {
    case NDFS_PTR_CONTIGUOUS: return "Contiguous";
    case NDFS_PTR_INDEXED:    return "Indexed";
    case NDFS_PTR_SUBINDEXED: return "SubIndexed";
    default:                  return "Reserved";
    }
}

/* Show user details + friend list. Used when 'stat NAME' names a user
 * (an argument with no '/'). NAME may be a user name or index (0-255). */
static int cmd_stat_user(ndtool_ctx_t *ctx, const char *user_ref, bool verbose)
{
    ndfs_user_entry_t *users = NULL;
    size_t count = 0, i;
    const ndfs_user_entry_t *u = NULL;
    char own[64], frnd[64], pub[64], buf[64];
    ndfs_friend_info_t *friends = NULL;
    size_t fcount = 0;
    long as_index = -1;
    const char *p;

    if (ndfs_get_users(ctx->fs, &users, &count) != NDFS_OK || count == 0) {
        fprintf(stderr, "No users found\n");
        return -1;
    }

    /* A purely-numeric ref matches by index, otherwise by name. */
    as_index = 0;
    for (p = user_ref; *p; p++) {
        if (*p < '0' || *p > '9') { as_index = -1; break; }
    }
    if (as_index == 0) {
        as_index = strtol(user_ref, NULL, 10); /* digits only; overflow saturates */
        if (as_index > 255) as_index = 256;    /* no user has this index */
    }

    for (i = 0; i < count; i++) {
        if (as_index >= 0 ? ((long)users[i].user_index == as_index)
                          : ci_equal(users[i].user_name, user_ref)) {
            u = &users[i];
            break;
        }
    }
    if (!u) {
        fprintf(stderr, "User not found: %s\n", user_ref);
        ndfs_free_users(users);
        return -1;
    }

    printf("UserName                         : %s\n", u->user_name);
    printf("UserIndex                        : %u\n", u->user_index);
    printf("PagesReserved                    : %u\n", u->pages_reserved);
    printf("PagesUsed                        : %u\n", u->pages_used);
    ndtool_format_nd_date(u->date_created, buf, sizeof(buf));
    printf("DateCreated                      : %s\n", buf);
    ndtool_format_nd_date(u->last_date_entered, buf, sizeof(buf));
    printf("LastDateEntered                  : %s\n", buf);

    ndtool_access_tier_str(u->default_file_access, NDFS_ACC_OWN_SHIFT, own, sizeof(own));
    ndtool_access_tier_str(u->default_file_access, NDFS_ACC_FRIEND_SHIFT, frnd, sizeof(frnd));
    ndtool_access_tier_str(u->default_file_access, NDFS_ACC_PUBLIC_SHIFT, pub, sizeof(pub));
    printf("DefaultFileAccess OWN            : %s\n", own);
    printf("DefaultFileAccess FRIEND         : %s\n", frnd);
    printf("DefaultFileAccess PUBLIC         : %s\n", pub);

    /* Friends. */
    if (ndfs_list_friends(ctx->fs, user_ref, &friends, &fcount) == NDFS_OK) {
        printf("\nFriends: %zu\n", fcount);
        for (i = 0; i < fcount; i++) {
            const char *fname = friends[i].name[0] ? friends[i].name : "(no such user)";
            if (verbose) {
                printf("  [%3u]  %-16s  %s  (R=read W=write A=append C=common D=directory)\n",
                       friends[i].index, fname, friends[i].perms);
            } else {
                printf("  [%3u]  %-16s  %s\n",
                       friends[i].index, fname, friends[i].perms);
            }
        }
        ndfs_free_friends(friends);
    }

    ndfs_free_users(users);
    return 0;
}

int cmd_stat(ndtool_ctx_t *ctx, const char *path, bool verbose)
{
    ndfs_file_entry_t meta;
    ndfs_object_entry_t obj;
    char buf[64];
    char own[64], frnd[64], pub[64];

    /* A name with no '/' is a user, not a file path: show user/friend info. */
    if (path && !strchr(path, '/')) {
        return cmd_stat_user(ctx, path, verbose);
    }

    if (ndfs_get_metadata(ctx->fs, path, &meta) != NDFS_OK) {
        fprintf(stderr, "File not found: %s\n", path);
        return -1;
    }
    if (ndfs_get_object_entry(ctx->fs, meta.full_name, meta.user_name, &obj) != NDFS_OK) {
        fprintf(stderr, "Cannot read object entry: %s\n", path);
        return -1;
    }

    file_type_flags_str(obj.file_type_flags, buf, sizeof(buf));

    printf("ObjectName                       : %s\n", obj.object_name);
    printf("ObjectIndexOfThisObjectEntry     : %u\n", obj.disk_object_index);
    /* The number SINTRAN itself shows - the "n" in "FILE n" - which is NOT the
     * physical position and NOT the raw on-disk word above. A user's files can
     * span up to 16 object blocks of 256, each a whole index block apart, and
     * only inside the first block do the three coincide. The on-disk word can
     * only carry the low 8 bits of the slot, which is why file numbers above
     * 255 were wrong before this was computed properly.
     * See docs/NDFS-OBJECT-BLOCKS-SPEC.md section 4. */
    printf("FileNumber (SINTRAN 'FILE n')    : %d  [block %u, slot %u]\n",
           (int)obj.file_number,
           (unsigned)(obj.file_number / NDFS_FILES_PER_OBJECT_BLOCK),
           (unsigned)(obj.file_number % NDFS_FILES_PER_OBJECT_BLOCK));
    printf("\n");
    printf("Type                             : %s\n", obj.type);
    printf("FileType                         : %s\n", ptr_type_str(obj.file_pointer.type));
    printf("FileTypeAsText                   : %s\n", buf);
    printf("\n");
    printf("PagesInFile                      : %u [%u bytes]\n",
           obj.pages_in_file, obj.pages_in_file * 2048u);
    printf("BytesInFile                      : %u\n", obj.bytes_in_file);
    printf("FilePointer Type                 : %s\n", ptr_type_str(obj.file_pointer.type));
    printf("FilePointer BlockID              : %u\n", obj.file_pointer.block_id);
    printf("\n");
    printf("UserIndexOfReservingUser         : %u\n", obj.user_index);
    printf("UserName                         : %s\n", obj.user_name);
    printf("\n");
    printf("ObjectEntryNextVersion           : %u\n", obj.next_version);
    printf("ObjectEntryPrevVersion           : %u\n", obj.prev_version);
    printf("\n");

    ndtool_access_tier_str(obj.access_bits, NDFS_ACC_PUBLIC_SHIFT, pub, sizeof(pub));
    ndtool_access_tier_str(obj.access_bits, NDFS_ACC_FRIEND_SHIFT, frnd, sizeof(frnd));
    ndtool_access_tier_str(obj.access_bits, NDFS_ACC_OWN_SHIFT, own, sizeof(own));
    printf("AccessBits PUBLIC                : %s\n", pub);
    printf("AccessBits FRIEND                : %s\n", frnd);
    printf("AccessBits OWN                   : %s\n", own);
    printf("\n");
    printf("DeviceNumber                     : %u\n", obj.device_number);
    printf("\n");
    printf("CurrentOpenCount                 : %u\n", obj.current_open_count);
    printf("TotalOpenCount                   : %u\n", obj.total_open_count);

    ndtool_format_nd_date(obj.date_created, buf, sizeof(buf));
    printf("DateCreated                      : %s\n", buf);
    ndtool_format_nd_date(obj.last_read_date, buf, sizeof(buf));
    printf("LastDateOpenedForRead            : %s\n", buf);
    ndtool_format_nd_date(obj.last_write_date, buf, sizeof(buf));
    printf("LastDateOpenedForWrite           : %s\n", buf);

    if (verbose) {
        uint32_t *blocks = NULL;
        size_t count = 0, i;
        if (ndfs_get_file_blocks(ctx->fs, meta.full_name, &blocks, &count) == NDFS_OK) {
            printf("\nFileBlocks: %zu\n", count);
            for (i = 0; i < count; i++) {
                if (blocks[i] == 0) {
                    printf("* (sparse hole)\n");
                } else {
                    printf("* %u (%s)\n", blocks[i],
                           ptr_type_str(obj.file_pointer.type));
                }
            }
            free(blocks);
        }
    }

    return 0;
}
