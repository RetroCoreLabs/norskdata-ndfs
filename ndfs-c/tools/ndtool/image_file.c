/**
 * ndtool: reading and writing the image file on the host.
 *
 * Kept apart from ndtool.c (which holds main) so the unit tests can link it
 * and check the shell's save rule directly.
 *
 * The rule: outside the shell, a command that changes the image writes the
 * file at once (ndtool_save_image). Inside the shell (ctx->in_shell) the
 * same call only marks the in-memory copy as changed; the file is written by
 * the shell's `save`, or by answering yes when `quit` asks. That is what the
 * ndtool README promises ("nothing is written to the .ndfs file until you
 * run save"). Until 2026-11-04 every cmd_* that called ndtool_save_image
 * (chmod, useradd, rm, passwd, ...) wrote the file from inside the shell
 * too, so `quit` without saving did not throw the change away.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ndtool.h"

#include <stdio.h>
#include <stdlib.h>

/* ── Helper: read local file ──────────────────────────────────────── */

uint8_t *ndtool_read_local_file(const char *path, size_t *out_size)
{
    FILE *f;
    uint8_t *data;
    long fsize;

    f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0) {
        fclose(f);
        return NULL;
    }

    data = (uint8_t *)malloc((size_t)fsize);
    if (!data) {
        fclose(f);
        return NULL;
    }

    if (fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(data);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = (size_t)fsize;
    return data;
}

/* ── Helper: write local file ─────────────────────────────────────── */

int ndtool_write_local_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", path);
        return -1;
    }

    if (fwrite(data, 1, size, f) != size) {
        fprintf(stderr, "Error: write failed for '%s'\n", path);
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

/* ── Helper: save image ───────────────────────────────────────────── */

int ndtool_save_image(ndtool_ctx_t *ctx)
{
    if (ctx->dry_run) {
        printf("(dry run: not saving)\n");
        return 0;
    }
    /* In the shell: change the in-memory copy only, `save` writes it. */
    if (ctx->in_shell) {
        ctx->shell_unsaved = true;
        return 0;
    }
    return ndtool_write_image(ctx);
}

int ndtool_write_image(ndtool_ctx_t *ctx)
{
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    ndfs_error_t err;
    int ret;

    err = ndfs_to_buffer(ctx->fs, &buf, &buf_size);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error: cannot export image: %s\n", ndfs_strerror(err));
        return -1;
    }

    ret = ndtool_write_local_file(ctx->image_path, buf, buf_size);
    ndfs_free_data(buf);
    if (ret == 0) ctx->shell_unsaved = false;
    return ret;
}
