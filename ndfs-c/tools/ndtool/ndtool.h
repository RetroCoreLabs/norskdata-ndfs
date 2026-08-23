/**
 * ndtool: CLI tool for working with NDFS disk images.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef NDTOOL_H
#define NDTOOL_H

#include <ndfs/ndfs.h>
#include <stdbool.h>

#define NDTOOL_VERSION "0.0.7"

/* Policy when a target file already exists (import into image, or extract to
 * host). Default is DENY: skip the file and keep going. */
typedef enum {
    NDTOOL_ON_EXIST_DENY = 0,   /* skip existing targets (safe default) */
    NDTOOL_ON_EXIST_OVERWRITE,  /* replace existing targets (--overwrite/-f) */
    NDTOOL_ON_EXIST_PROMPT      /* ask per file (-i), falls back to deny w/o TTY */
} ndtool_on_exist_t;

typedef struct {
    const char *image_path;
    ndfs_filesystem_t *fs;
    bool verbose;
    bool lowercase;
    bool do_parity;     /* strip on extract, set on put */
    bool dry_run;
    bool force;
    bool with_dirs;     /* -d flag */
    const char *output_dir;
    const char *filter_user;  /* -u USER filter */
    const char *filter_file;  /* -F FILE/glob filter (wildcards on extract) */
    const char *dest_user;    /* --dest USER: destination for multi-file --put */
    ndtool_on_exist_t on_exist; /* collision policy for existing targets */
    bool modified;
    bool use_xat;       /* --xat: write/read .xat sidecar files */
    const char *editor;     /* custom editor command, NULL = auto-detect */
} ndtool_ctx_t;

/* Helper: convert NDFS name to host filename (: -> ., optional lowercase) */
void ndtool_to_host_name(const char *ndfs_name, char *out, size_t out_len, bool lowercase);

/* Helper: convert host filename to NDFS name (. -> :, uppercase) */
void ndtool_to_ndfs_name(const char *host_name, char *out, size_t out_len);

/* Helper: read entire local file into malloc'd buffer */
uint8_t *ndtool_read_local_file(const char *path, size_t *out_size);

/* Helper: write buffer to local file */
int ndtool_write_local_file(const char *path, const uint8_t *data, size_t size);

/* Helper: save modified image back to disk */
int ndtool_save_image(ndtool_ctx_t *ctx);

/* Print "ndtool <version> (built <date> <time>)". */
void ndtool_print_version(void);

/* Helper: true if the string contains a '*' or '?' wildcard. */
bool ndtool_has_wildcard(const char *s);

/* Decide whether to write a target that already exists, per ctx->on_exist.
 * Prints a skip/overwrite notice. 'what' describes the target for messages
 * (e.g. the destination path). Returns true to proceed, false to skip. */
bool ndtool_confirm_overwrite(ndtool_ctx_t *ctx, const char *what, bool exists);

/* Commands */
int cmd_list(ndtool_ctx_t *ctx);
int cmd_users(ndtool_ctx_t *ctx);
int cmd_info(ndtool_ctx_t *ctx);
int cmd_extract(ndtool_ctx_t *ctx);
int cmd_put(ndtool_ctx_t *ctx, const char *local_path, const char *ndfs_path);
int cmd_delete(ndtool_ctx_t *ctx, const char *ndfs_path);
int cmd_useradd(ndtool_ctx_t *ctx, const char *name, uint32_t quota);
int cmd_userdel(ndtool_ctx_t *ctx, const char *name);
int cmd_addquota(ndtool_ctx_t *ctx, const char *name, uint32_t pages);

/* Object blocks: files per user. A user holds 256 files per object block, one
 * by default and up to 16 (4096 files). See docs/NDFS-OBJECT-BLOCKS-SPEC.md. */

/** Report a user's object-block position: files held, ceiling, room to grant. */
int cmd_objblocks(ndtool_ctx_t *ctx, const char *name);

/** Grant a user COUNT more object blocks - the @GIVE-OBJECT-BLOCKS equivalent. */
int cmd_giveobjblocks(ndtool_ctx_t *ctx, const char *name, uint32_t count);

/** Lower a user's object-block maximum. No SINTRAN equivalent; undoes a grant. */
int cmd_takeobjblocks(ndtool_ctx_t *ctx, const char *name, uint32_t count);
int cmd_remquota(ndtool_ctx_t *ctx, const char *name, uint32_t pages);
int cmd_passwd(ndtool_ctx_t *ctx, const char *name);
/* Friend commands. add/del take "OWNER:FRIEND[:RWACD]" / "OWNER:FRIEND";
 * list takes a user name or index. */
int cmd_friend_add(ndtool_ctx_t *ctx, const char *arg);
int cmd_friend_del(ndtool_ctx_t *ctx, const char *arg);
int cmd_friend_list(ndtool_ctx_t *ctx, const char *user_ref);
int cmd_create(ndtool_ctx_t *ctx, const char *template_name, uint32_t custom_pages, const char *dir_name);
int cmd_shell(ndtool_ctx_t *ctx);
int cmd_stat(ndtool_ctx_t *ctx, const char *path, bool verbose);
int cmd_chmod(ndtool_ctx_t *ctx, const char *spec, const char *path);

/* stat/chmod helpers (also used by the access-edit command) */
void ndtool_format_nd_date(uint32_t v, char *out, size_t len);
void ndtool_access_tier_str(uint16_t access_bits, unsigned shift, char *out, size_t len);

#endif /* NDTOOL_H */
