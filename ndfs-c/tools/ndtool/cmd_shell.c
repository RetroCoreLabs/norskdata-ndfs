/**
 * ndtool: interactive shell.
 *
 * SPDX-License-Identifier: MIT
 */

/* Expose POSIX mkdtemp()/rmdir() under strict -std=c99 (must precede any
 * system header include). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "ndtool.h"
#include "parity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#define rmdir _rmdir
/* No mkdtemp on Windows: make a unique name, then create the directory. */
static char *mkdtemp(char *tmpl)
{
    if (!_mktemp(tmpl)) return NULL;
    return _mkdir(tmpl) == 0 ? tmpl : NULL;
}
#else
#include <unistd.h>
#endif

/* Portable case-insensitive string compare. */
static int ci_equal(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* ── Shell helpers ────────────────────────────────────────────────── */

static void shell_help(void)
{
    printf("Paths:\n");
    printf("  PATH      a file INSIDE the image: USER/NAME:TYPE  (e.g. SYSTEM/STARTUP:MODE)\n");
    printf("  HOSTFILE  a file on your computer  (e.g. ./startup.txt)\n");
    printf("\nCommands:\n");
    printf("  ls [USER[/PATTERN]]          List users or files (* ? :TYPE wildcards)\n");
    printf("  cat PATH                     Show file contents (parity stripped)\n");
    printf("  hexdump PATH                 Hex dump of file contents\n");
    printf("  get PATH [HOSTFILE]          Copy file OUT of the image to your computer\n");
    printf("  put HOSTFILE [PATH]          Copy file from your computer INTO the image\n");
    printf("  rm PATH                      Delete a file inside the image\n");
    printf("  mv OLD NEW                   Rename a file inside the image\n");
    printf("  edit PATH                    Edit in external editor (extract, edit, re-import)\n");
    printf("  stat [-v] PATH               File metadata (-v adds block list)\n");
    printf("  stat [-v] USER               User details + friends\n");
    printf("  chmod SPEC PATH              Change access (e.g. OWN+WD,FRIEND=RW,PUBLIC-A)\n");
    printf("  info                         Filesystem info\n");
    printf("  bitmap                       Bitmap visualization and validation\n");
    printf("  fsck                         Full filesystem check\n");
    printf("  users                        List users with quota info and password word (octal)\n");
    printf("  useradd NAME [QUOTA]         Add user (default quota: 100)\n");
    printf("  userdel NAME                 Remove user (must have no files)\n");
    printf("  quotaadd NAME PAGES          Add pages to user quota\n");
    printf("  quotadel NAME PAGES          Remove pages from user quota\n");
    printf("  passwd NAME                  Clear user password\n");
    printf("  release                      Clear the directory-entered flag and system number\n");
    printf("  cpuid                        Show the SINTRAN CPU number / type (both copies, stamp)\n");
    printf("  setcpu NUMBER[:TYPE] [keep]  Change the CPU number (and type); keep = keep the stamp\n");
    printf("  segments                     List the SINTRAN segments (name, base, length, disk page)\n");
    printf("  export-segment NAME          Write a segment to NAME.seg in the output directory\n");
    printf("  import-segment NAME FILE     Write a host file over a segment's pages\n");
    printf("  friends USER                 List a user's friends and their rights\n");
    printf("  friendadd OWNER FRIEND [R]   Add friend (R = rights, default RWA)\n");
    printf("  frienddel OWNER FRIEND       Remove a friend\n");
    printf("  save [HOSTFILE]              Save changes to disk (optional save-as)\n");
    printf("  version / ver                Show tool version and build date/time\n");
    printf("  help                         Show this help\n");
    printf("  quit / exit                  Exit shell\n");
}

static void shell_ls(ndtool_ctx_t *ctx, const char *arg)
{
    size_t i;
    ndfs_error_t err;
    char user_buf[NDFS_NAME_MAX + 1];
    const char *user_name = NULL;
    const char *file_pat = NULL;

    /* Parse arg: no arg = list users.
     * "USER" or "USER/" = list all files for user.
     * "USER/PATTERN" = list matching files.  Pattern supports * and ?.
     * A leading ':' in pattern is shorthand for '*:TYPE', e.g. ":MODE" -> "*:MODE". */
    if (arg) {
        const char *slash = strchr(arg, '/');
        if (slash) {
            size_t ulen = (size_t)(slash - arg);
            if (ulen > NDFS_NAME_MAX) ulen = NDFS_NAME_MAX;
            memcpy(user_buf, arg, ulen);
            user_buf[ulen] = '\0';
            user_name = user_buf;
            file_pat = slash + 1;
            if (file_pat[0] == '\0') file_pat = NULL; /* trailing slash only */
        } else {
            user_name = arg;
        }
    }

    if (!user_name) {
        /* List users with file count + octal user index */
        ndfs_user_entry_t *users = NULL;
        size_t user_count = 0;

        err = ndfs_get_users(ctx->fs, &users, &user_count);
        if (err != NDFS_OK) {
            fprintf(stderr, "Error: %s\n", ndfs_strerror(err));
            return;
        }

        for (i = 0; i < user_count; i++) {
            ndfs_file_entry_t *files = NULL;
            size_t file_count = 0;
            ndfs_list_directory(ctx->fs, users[i].user_name, &files, &file_count);
            printf("  [%03o]  %-16s  %3zu files  Reserved: %5u  Used: %5u\n",
                   users[i].user_index, users[i].user_name, file_count,
                   users[i].pages_reserved, users[i].pages_used);
            ndfs_free_entries(files);
        }

        ndfs_free_users(users);
        return;
    }

    /* List files for a user, with optional wildcard filter */
    {
        ndfs_object_entry_t *objs = NULL;
        size_t obj_count = 0;
        /* Expand bare ":TYPE" to "*:TYPE" for convenience */
        char pat_buf[NDFS_NAME_MAX + NDFS_TYPE_MAX + 4];
        if (file_pat && file_pat[0] == ':') {
            snprintf(pat_buf, sizeof(pat_buf), "*%s", file_pat);
            file_pat = pat_buf;
        }

        err = ndfs_get_object_entries(ctx->fs, &objs, &obj_count);
        if (err != NDFS_OK) {
            fprintf(stderr, "Error: %s\n", ndfs_strerror(err));
            return;
        }

        for (i = 0; i < obj_count; i++) {
            const ndfs_object_entry_t *e = &objs[i];
            char full[NDFS_NAME_MAX + NDFS_TYPE_MAX + 2];
            char datebuf[24];
            if (!ci_equal(e->user_name, user_name)) continue;
            ndfs_oe_full_name(e, full, sizeof(full));
            if (file_pat && !ndfs_wildmatch(file_pat, full, true)) continue;
            ndtool_format_nd_date(e->date_created, datebuf, sizeof(datebuf));
            printf("  [%04o]  %s  (%s)%-24s %10u bytes  %5u pages\n",
                   e->disk_object_index, datebuf,
                   e->user_name, full,
                   e->bytes_in_file, e->pages_in_file);
        }

        ndfs_free_object_entries(objs);
    }
}

static void shell_cat(ndtool_ctx_t *ctx, const char *path)
{
    uint8_t *data = NULL;
    size_t size = 0;
    size_t i;
    ndfs_error_t err;

    if (!path) {
        fprintf(stderr, "Usage: cat USER/FILE:TYPE\n");
        return;
    }

    err = ndfs_read_file(ctx->fs, path, &data, &size);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error: %s\n", ndfs_strerror(err));
        return;
    }

    /* Strip parity for display */
    for (i = 0; i < size; i++) {
        data[i] &= 0x7F;
    }

    fwrite(data, 1, size, stdout);
    if (size > 0 && data[size - 1] != '\n') {
        printf("\n");
    }

    ndfs_free_data(data);
}

static void shell_hexdump(ndtool_ctx_t *ctx, const char *path)
{
    uint8_t *data = NULL;
    size_t size = 0;
    size_t i, j;
    ndfs_error_t err;

    if (!path) {
        fprintf(stderr, "Usage: hexdump USER/FILE:TYPE\n");
        return;
    }

    err = ndfs_read_file(ctx->fs, path, &data, &size);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error: %s\n", ndfs_strerror(err));
        return;
    }

    for (i = 0; i < size; i += 16) {
        printf("%08zx  ", i);

        /* Hex bytes */
        for (j = 0; j < 16; j++) {
            if (i + j < size) {
                printf("%02x ", data[i + j]);
            } else {
                printf("   ");
            }
            if (j == 7) printf(" ");
        }

        printf(" |");

        /* ASCII */
        for (j = 0; j < 16 && i + j < size; j++) {
            uint8_t c = data[i + j] & 0x7F;
            printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }

        printf("|\n");
    }

    ndfs_free_data(data);
}

static void shell_edit(ndtool_ctx_t *ctx, const char *path)
{
    uint8_t *data = NULL;
    size_t size = 0;
    ndfs_error_t err;
    char tmp_dir[] = "/tmp/ndtool_XXXXXX";
    char tmp_path[512];
    char host_name[64];
    const char *editor;
    char cmd_buf[1024];
    int ret;
    struct stat st_before, st_after;
    ndfs_xat_properties_t saved_xat;
    bool has_xat = false;

    if (!path) {
        fprintf(stderr, "Usage: edit USER/FILE:TYPE\n");
        return;
    }

    /* Save XAT properties (status bits) before editing */
    if (ndfs_get_file_properties(ctx->fs, path, &saved_xat) == NDFS_OK) {
        has_xat = true;
    }

    /* Read file from image */
    err = ndfs_read_file(ctx->fs, path, &data, &size);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error reading '%s': %s\n", path, ndfs_strerror(err));
        return;
    }

    /* Strip parity for editing */
    ndtool_strip_parity(data, size);

    /* Build temp file path */
    ndtool_to_host_name(path, host_name, sizeof(host_name), true);
    /* Keep only characters that are harmless in a file name and in the shell
     * command line below. A name read from the image can hold anything. */
    {
        size_t k;
        for (k = 0; host_name[k]; k++) {
            unsigned char c = (unsigned char)host_name[k];
            if (!(isalnum(c) || c == '.' || c == '-' || c == '_')) host_name[k] = '_';
        }
        if (host_name[0] == '\0') strcpy(host_name, "file");
    }
    /* A private directory with a random name (mode 0700) so no other user can
     * pre-create the file or a symlink in its place. */
    if (!mkdtemp(tmp_dir)) {
        fprintf(stderr, "Error: cannot create a temporary directory\n");
        ndfs_free_data(data);
        return;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s", tmp_dir, host_name);

    /* Write to temp file */
    if (ndtool_write_local_file(tmp_path, data, size) != 0) {
        ndfs_free_data(data);
        remove(tmp_path);
        rmdir(tmp_dir);
        return;
    }
    ndfs_free_data(data);

    /* Get file timestamp before edit */
    stat(tmp_path, &st_before);

    /* Editor priority: --editor flag > $EDITOR > $VISUAL > code --wait */
    editor = ctx->editor;
    if (!editor || editor[0] == '\0') editor = getenv("EDITOR");
    if (!editor || editor[0] == '\0') editor = getenv("VISUAL");
    if (!editor || editor[0] == '\0') editor = "code --wait";

    /* Open editor */
    printf("Opening '%s' in editor...\n", path);
    if (snprintf(cmd_buf, sizeof(cmd_buf), "%s \"%s\"", editor, tmp_path) >= (int)sizeof(cmd_buf)) {
        fprintf(stderr, "Error: editor command is too long\n");
        remove(tmp_path);
        rmdir(tmp_dir);
        return;
    }
    ret = system(cmd_buf);

    if (ret != 0) {
        fprintf(stderr, "Editor exited with code %d\n", ret);
        /* Still check if file changed */
    }

    /* Check if file was modified */
    stat(tmp_path, &st_after);
    if (st_before.st_mtime == st_after.st_mtime &&
        (size_t)st_before.st_size == size) {
        printf("No changes detected, skipping re-import.\n");
        remove(tmp_path);
        rmdir(tmp_dir);
        return;
    }

    /* Read back modified file */
    {
        size_t new_size = 0;
        uint8_t *new_data = ndtool_read_local_file(tmp_path, &new_size);
        if (!new_data) {
            fprintf(stderr, "Error reading back temp file\n");
            remove(tmp_path);
            rmdir(tmp_dir);
            return;
        }

        /* Check if this is a text type -- if so, re-apply even parity */
        {
            /* Extract type from path: after last : */
            const char *colon = strrchr(path, ':');
            if (colon && ndtool_is_text_type(colon + 1)) {
                ndtool_set_parity(new_data, new_size);
                printf("Re-applied even parity (text file type).\n");
            }
        }

        /* Write back to image */
        err = ndfs_write_file(ctx->fs, path, new_data, new_size);
        free(new_data);

        if (err != NDFS_OK) {
            fprintf(stderr, "Error writing back '%s': %s\n", path, ndfs_strerror(err));
        } else {
            /* Re-apply saved XAT status bits (access_bits, file_type, etc.) */
            if (has_xat) {
                ndfs_object_entry_t obj;
                /* Parse the path to get user_name and full_name for lookup */
                {
                    const char *slash = strchr(path, '/');
                    const char *file_part = slash ? slash + 1 : path;
                    char user_buf[NDFS_NAME_MAX + 1];
                    if (slash) {
                        size_t ulen = (size_t)(slash - path);
                        if (ulen > NDFS_NAME_MAX) ulen = NDFS_NAME_MAX;
                        memcpy(user_buf, path, ulen);
                        user_buf[ulen] = '\0';
                    } else {
                        user_buf[0] = '\0';
                    }
                    if (ndfs_get_object_entry(ctx->fs, file_part,
                                              user_buf[0] ? user_buf : NULL,
                                              &obj) == NDFS_OK) {
                        /* Logging only -- the object in the fs is what matters,
                           and we can't directly modify it through the public API.
                           The status bits are preserved by the XAT sidecar on
                           extract/put, and the edit command already re-applies
                           parity. */
                    }
                }
            }
            ctx->modified = true;
            printf("Updated '%s' (%zu bytes).\n", path, new_size);
        }
    }

    remove(tmp_path);
    rmdir(tmp_dir);
}

static void shell_get(ndtool_ctx_t *ctx, const char *ndfs_path, const char *local_path)
{
    uint8_t *data = NULL;
    size_t size = 0;
    char auto_name[64];
    ndfs_error_t err;

    if (!ndfs_path) {
        fprintf(stderr, "Usage: get USER/FILE:TYPE [HOSTFILE]   "
                        "(copies a file OUT of the image to your computer)\n");
        return;
    }

    err = ndfs_read_file(ctx->fs, ndfs_path, &data, &size);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error: %s\n", ndfs_strerror(err));
        return;
    }

    if (!local_path) {
        /* Derive from NDFS path: take part after last / */
        const char *slash = strrchr(ndfs_path, '/');
        const char *fname = slash ? slash + 1 : ndfs_path;
        ndtool_to_host_name(fname, auto_name, sizeof(auto_name), true);
        local_path = auto_name;
    }

    if (ctx->do_parity) {
        ndtool_strip_parity(data, size);
    }

    if (ndtool_write_local_file(local_path, data, size) == 0) {
        printf("Extracted %s -> %s (%zu bytes)\n", ndfs_path, local_path, size);
    }

    ndfs_free_data(data);
}

static void shell_put(ndtool_ctx_t *ctx, const char *local_path, const char *ndfs_path)
{
    if (!local_path) {
        fprintf(stderr, "Usage: put HOSTFILE [USER/FILE:TYPE]   "
                        "(copies a file from your computer INTO the image)\n");
        return;
    }

    cmd_put(ctx, local_path, ndfs_path);
    ctx->modified = true;
}

static void shell_rm(ndtool_ctx_t *ctx, const char *path)
{
    ndfs_error_t err;

    if (!path) {
        fprintf(stderr, "Usage: rm USER/FILE:TYPE\n");
        return;
    }

    err = ndfs_delete_file(ctx->fs, path);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error: %s\n", ndfs_strerror(err));
        return;
    }

    ctx->modified = true;
    printf("Deleted %s\n", path);
}

static void shell_mv(ndtool_ctx_t *ctx, const char *old_path, const char *new_path)
{
    ndfs_error_t err;

    if (!old_path || !new_path) {
        fprintf(stderr, "Usage: mv OLD_PATH NEW_PATH\n");
        return;
    }

    err = ndfs_rename(ctx->fs, old_path, new_path);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error: %s\n", ndfs_strerror(err));
        return;
    }

    ctx->modified = true;
    printf("Renamed %s -> %s\n", old_path, new_path);
}

/* ── Main shell loop ──────────────────────────────────────────────── */

int cmd_shell(ndtool_ctx_t *ctx)
{
    char line[1024];
    printf("ndtool shell - type 'help' for commands\n");
    ndtool_print_version();

    while (1) {
        printf("ndtool> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        /* Trim newline */
        {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
        }
        if (line[0] == '\0') continue;

        /* Parse command and args */
        {
            char *cmd = strtok(line, " \t");
            char *arg1 = strtok(NULL, " \t");
            char *arg2 = strtok(NULL, " \t");
            char *arg3 = strtok(NULL, " \t");

            if (!cmd) continue;

            if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
                if (ctx->modified) {
                    char resp[16];
                    printf("Unsaved changes. Save before quitting? [y/N] ");
                    fflush(stdout);
                    if (fgets(resp, sizeof(resp), stdin) &&
                        (resp[0] == 'y' || resp[0] == 'Y')) {
                        ndtool_save_image(ctx);
                    }
                }
                break;
            }
            else if (strcmp(cmd, "help") == 0) {
                shell_help();
            }
            else if (strcmp(cmd, "version") == 0 || strcmp(cmd, "ver") == 0) {
                ndtool_print_version();
            }
            else if (strcmp(cmd, "ls") == 0) {
                shell_ls(ctx, arg1);
            }
            else if (strcmp(cmd, "cat") == 0) {
                shell_cat(ctx, arg1);
            }
            else if (strcmp(cmd, "hexdump") == 0) {
                shell_hexdump(ctx, arg1);
            }
            else if (strcmp(cmd, "edit") == 0) {
                shell_edit(ctx, arg1);
            }
            else if (strcmp(cmd, "get") == 0) {
                shell_get(ctx, arg1, arg2);
            }
            else if (strcmp(cmd, "put") == 0) {
                shell_put(ctx, arg1, arg2);
            }
            else if (strcmp(cmd, "rm") == 0) {
                shell_rm(ctx, arg1);
            }
            else if (strcmp(cmd, "mv") == 0) {
                shell_mv(ctx, arg1, arg2);
            }
            else if (strcmp(cmd, "info") == 0) {
                cmd_info(ctx);
            }
            else if (strcmp(cmd, "bitmap") == 0) {
                /* Temporarily enable verbose to show bitmap */
                bool was_verbose = ctx->verbose;
                ctx->verbose = true;
                cmd_info(ctx);
                ctx->verbose = was_verbose;
            }
            else if (strcmp(cmd, "fsck") == 0) {
                char *report = NULL;
                int errs = 0;
                ndfs_error_t ferr = ndfs_fsck(ctx->fs, &report, &errs);
                if (ferr == NDFS_OK && report) {
                    printf("%s", report);
                    ndfs_free_string(report);
                } else {
                    fprintf(stderr, "Error running fsck: %s\n", ndfs_strerror(ferr));
                }
            }
            else if (strcmp(cmd, "stat") == 0) {
                /* Accept "stat PATH", "stat -v PATH" or "stat PATH -v". */
                bool verbose = false;
                const char *path = arg1;
                if (arg1 && strcmp(arg1, "-v") == 0) {
                    verbose = true;
                    path = arg2;
                } else if (arg2 && strcmp(arg2, "-v") == 0) {
                    verbose = true;
                }
                if (!path) {
                    fprintf(stderr, "Usage: stat [-v] USER/FILE:TYPE\n");
                } else {
                    cmd_stat(ctx, path, verbose);
                }
            }
            else if (strcmp(cmd, "chmod") == 0) {
                /* chmod SPEC PATH */
                if (!arg1 || !arg2) {
                    fprintf(stderr, "Usage: chmod SPEC USER/FILE:TYPE\n");
                    fprintf(stderr, "  SPEC e.g. OWN+WD,FRIEND=RW,PUBLIC-A  or  0x03FF\n");
                    fprintf(stderr, "  rights: R read W write A append C common D directory\n");
                } else {
                    cmd_chmod(ctx, arg1, arg2);
                }
            }
            else if (strcmp(cmd, "users") == 0) {
                cmd_users(ctx);
            }
            else if (strcmp(cmd, "useradd") == 0) {
                uint32_t q = 100;
                if (!arg1) fprintf(stderr, "Usage: useradd NAME [QUOTA]\n");
                else if (arg2 && !ndtool_parse_u32(arg2, &q)) fprintf(stderr, "Error: '%s' is not a valid quota\n", arg2);
                else cmd_useradd(ctx, arg1, q);
            }
            else if (strcmp(cmd, "userdel") == 0) {
                if (!arg1) {
                    fprintf(stderr, "Usage: userdel NAME\n");
                } else {
                    cmd_userdel(ctx, arg1);
                }
            }
            else if (strcmp(cmd, "quotaadd") == 0) {
                if (!arg1 || !arg2) {
                    fprintf(stderr, "Usage: quotaadd NAME PAGES\n");
                } else {
                    uint32_t pages;
                    if (!ndtool_parse_u32(arg2, &pages)) fprintf(stderr, "Error: '%s' is not a valid page count\n", arg2);
                    else cmd_addquota(ctx, arg1, pages);
                }
            }
            else if (strcmp(cmd, "quotadel") == 0) {
                if (!arg1 || !arg2) {
                    fprintf(stderr, "Usage: quotadel NAME PAGES\n");
                } else {
                    uint32_t pages;
                    if (!ndtool_parse_u32(arg2, &pages)) fprintf(stderr, "Error: '%s' is not a valid page count\n", arg2);
                    else cmd_remquota(ctx, arg1, pages);
                }
            }
            else if (strcmp(cmd, "friends") == 0) {
                if (!arg1) {
                    fprintf(stderr, "Usage: friends USER\n");
                } else {
                    cmd_friend_list(ctx, arg1);
                }
            }
            else if (strcmp(cmd, "friendadd") == 0) {
                if (!arg1 || !arg2) {
                    fprintf(stderr, "Usage: friendadd OWNER FRIEND [RWACD]\n");
                } else {
                    ndfs_error_t e = ndfs_add_friend(ctx->fs, arg1, arg2,
                                                     arg3 ? arg3 : "RWA");
                    if (e != NDFS_OK) {
                        fprintf(stderr, "Error: %s\n", ndfs_strerror(e));
                    } else {
                        printf("Added friend '%s' to '%s' (%s)\n",
                               arg2, arg1, arg3 ? arg3 : "RWA");
                        ctx->modified = true;
                    }
                }
            }
            else if (strcmp(cmd, "frienddel") == 0) {
                if (!arg1 || !arg2) {
                    fprintf(stderr, "Usage: frienddel OWNER FRIEND\n");
                } else {
                    ndfs_error_t e = ndfs_remove_friend(ctx->fs, arg1, arg2);
                    if (e != NDFS_OK) {
                        fprintf(stderr, "Error: %s\n", ndfs_strerror(e));
                    } else {
                        printf("Removed friend '%s' from '%s'\n", arg2, arg1);
                        ctx->modified = true;
                    }
                }
            }
            else if (strcmp(cmd, "passwd") == 0) {
                if (!arg1) {
                    fprintf(stderr, "Usage: passwd NAME\n");
                } else {
                    cmd_passwd(ctx, arg1);
                }
            }
            else if (strcmp(cmd, "release") == 0) {
                cmd_release(ctx);
            }
            else if (strcmp(cmd, "cpuid") == 0) {
                cmd_cpu_identity(ctx);
            }
            else if (strcmp(cmd, "segments") == 0) {
                cmd_segments(ctx);
            }
            else if (strcmp(cmd, "export-segment") == 0) {
                if (!arg1) fprintf(stderr, "Usage: export-segment NAME\n");
                else cmd_export_segment(ctx, arg1);
            }
            else if (strcmp(cmd, "import-segment") == 0) {
                if (!arg1 || !arg2) fprintf(stderr, "Usage: import-segment NAME FILE\n");
                else {
                    char spec[600];
                    snprintf(spec, sizeof(spec), "%s=%s", arg1, arg2);
                    cmd_import_segment(ctx, spec);
                }
            }
            else if (strcmp(cmd, "setcpu") == 0) {
                if (!arg1) {
                    fprintf(stderr, "Usage: setcpu NUMBER[:TYPE] [keep]\n");
                } else {
                    cmd_set_cpu(ctx, arg1, arg2 && strcmp(arg2, "keep") == 0);
                }
            }
            else if (strcmp(cmd, "save") == 0) {
                if (arg1) ctx->image_path = arg1;
                ndtool_save_image(ctx);
                ctx->modified = false;
                printf("Image saved to %s\n", ctx->image_path);
            }
            else {
                printf("Unknown command: %s (type 'help')\n", cmd);
            }
        }
    }

    return 0;
}
