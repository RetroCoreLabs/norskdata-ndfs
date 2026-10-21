/*
 * cmd_fsck.c - ndtool --fsck: check, safe repairs, salvage, undo.
 *
 * The check and the repairs are in the library (ndfs_fsck_ex). This file only reads the options,
 * prints the report, keeps the undo file next to the image and copies out failed files.
 * See docs/FSCK-FIX-PLAN.md.
 */

#include "ndtool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define ndtool_mkdir(path) _mkdir(path)
#else
#include <sys/stat.h>
#define ndtool_mkdir(path) mkdir((path), 0755)
#endif

/* Exit codes: 0 CLEAN, 1 DAMAGED, 2 FAILED (the verdict of the last check), 3 ndtool could not do what was asked. */
#define FSCK_EXIT_TOOL_ERROR 3

static void undo_path(const ndtool_ctx_t *ctx, char *out, size_t n)
{
    snprintf(out, n, "%s.fsck-undo", ctx->image_path);
}

/* Host file name for a failed file: USER/NAME:TYPE -> USER_NAME.TYPE, only safe characters. */
static void salvage_name(const char *path, char *out, size_t n)
{
    size_t i, o = 0;
    for (i = 0; path[i] && o + 1 < n; i++) {
        char c = path[i];
        if (c == '/') c = '_';
        else if (c == ':') c = '.';
        else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '_' || c == '.'))
            c = '-';
        out[o++] = c;
    }
    out[o] = '\0';
}

/* --salvage DIR: copy out every file named in the failures, and write SALVAGE-REPORT.txt with
 * every finding (which blocks are shared with which file). Nothing in the image is changed. */
static int salvage(ndtool_ctx_t *ctx, const char *dir, const ndfs_fsck_result_t *r)
{
    char report_path[1024];
    FILE *rep;
    size_t i, j, copied = 0, failed = 0;

    ndtool_mkdir(dir);
    snprintf(report_path, sizeof(report_path), "%s/SALVAGE-REPORT.txt", dir);
    rep = fopen(report_path, "w");
    if (!rep) {
        fprintf(stderr, "Error: cannot write '%s'\n", report_path);
        return -1;
    }
    fprintf(rep, "Findings of the check of %s\n", ctx->image_path);
    fprintf(rep, "A file that shares a page with another file may hold the other file's bytes there.\n\n");
    for (i = 0; i < r->n_failures; i++) {
        const ndfs_fsck_failure_t *f = &r->failures[i];
        if (f->page_in_file == 0xFFFFFFFFu)
            fprintf(rep, "%s  index block, block %u: %s\n", f->path, f->block, f->reason);
        else
            fprintf(rep, "%s  page %u, block %u: %s\n", f->path, f->page_in_file, f->block, f->reason);
    }
    fprintf(rep, "\nFiles copied:\n");

    for (i = 0; i < r->n_failures; i++) {
        const ndfs_fsck_failure_t *f = &r->failures[i];
        char host[256], full[1280];
        uint8_t *data = NULL;
        size_t size = 0;
        bool done_before = false;
        ndfs_error_t e;

        if (f->path[0] == '(' || !strchr(f->path, '/')) continue;   /* a structure, not a file */
        for (j = 0; j < i; j++)
            if (strcmp(r->failures[j].path, f->path) == 0) { done_before = true; break; }
        if (done_before) continue;

        salvage_name(f->path, host, sizeof(host));
        snprintf(full, sizeof(full), "%s/%s", dir, host);
        e = ndfs_read_file(ctx->fs, f->path, &data, &size);
        if (e != NDFS_OK) {
            fprintf(rep, "  %s: not copied (%s)\n", f->path, ndfs_strerror(e));
            failed++;
            continue;
        }
        if (ndtool_write_local_file(full, data, size) != 0) {
            fprintf(rep, "  %s: not copied (cannot write %s)\n", f->path, full);
            failed++;
        } else {
            fprintf(rep, "  %s -> %s (%zu bytes)\n", f->path, host, size);
            copied++;
        }
        ndfs_free_data(data);
    }
    fclose(rep);
    printf("Salvage: %zu file(s) copied to %s, %zu not copied. Report: %s\n", copied, dir, failed, report_path);
    return 0;
}

int cmd_fsck_undo(ndtool_ctx_t *ctx)
{
    char path[1024];
    size_t size = 0;
    uint32_t pages = 0;
    uint8_t *data;
    ndfs_error_t e;

    undo_path(ctx, path, sizeof(path));
    data = ndtool_read_local_file(path, &size);
    if (!data) {
        fprintf(stderr, "Error: cannot read the undo file '%s'\n", path);
        return FSCK_EXIT_TOOL_ERROR;
    }
    e = ndfs_fsck_undo(ctx->fs, data, size, &pages);
    free(data);
    if (e != NDFS_OK) {
        fprintf(stderr, "Error: cannot put back the saved pages: %s\n", ndfs_strerror(e));
        return FSCK_EXIT_TOOL_ERROR;
    }
    if (ndtool_save_image(ctx) != 0) return FSCK_EXIT_TOOL_ERROR;
    printf("Put back %u saved page(s) from %s\n", (unsigned)pages, path);
    return 0;
}

int cmd_fsck(ndtool_ctx_t *ctx, bool fix, const char *recover_user, const char *salvage_dir)
{
    ndfs_fsck_options_t o;
    ndfs_fsck_result_t r;
    ndfs_error_t e;
    int verdict;

    if (salvage_dir) {
        memset(&o, 0, sizeof(o));
        e = ndfs_fsck_ex(ctx->fs, &o, &r);
        if (e != NDFS_OK) {
            fprintf(stderr, "Error running fsck: %s\n", ndfs_strerror(e));
            return FSCK_EXIT_TOOL_ERROR;
        }
        if (salvage(ctx, salvage_dir, &r) != 0) {
            ndfs_fsck_result_free(&r);
            return FSCK_EXIT_TOOL_ERROR;
        }
        ndfs_fsck_result_free(&r);
    }

    memset(&o, 0, sizeof(o));
    o.verbose = ctx->verbose;
    o.fix = fix;
    o.dry_run = ctx->dry_run;
    o.force = ctx->force;
    o.recover_orphans_user = recover_user;

    e = ndfs_fsck_ex(ctx->fs, &o, &r);
    if (e != NDFS_OK) {
        fprintf(stderr, "Error running fsck: %s\n", ndfs_strerror(e));
        return FSCK_EXIT_TOOL_ERROR;
    }
    printf("%s", r.report);

    if (fix && !o.dry_run && !r.refused && r.undo_data) {
        char path[1024];
        undo_path(ctx, path, sizeof(path));
        /* The undo file goes to disk BEFORE the image: the image file is not touched until this worked. */
        if (ndtool_write_local_file(path, r.undo_data, r.undo_size) != 0) {
            fprintf(stderr, "Error: cannot write the undo file '%s'; the image was NOT changed.\n", path);
            ndfs_fsck_result_free(&r);
            return FSCK_EXIT_TOOL_ERROR;
        }
        if (ndtool_save_image(ctx) != 0) {
            ndfs_fsck_result_free(&r);
            return FSCK_EXIT_TOOL_ERROR;
        }
        printf("\nUndo file: %s   (ndtool --fsck-undo %s puts the pages back)\n", path, ctx->image_path);
    }

    verdict = (int)r.verdict;
    if (fix && r.refused) verdict = FSCK_EXIT_TOOL_ERROR;
    ndfs_fsck_result_free(&r);
    return verdict;
}
