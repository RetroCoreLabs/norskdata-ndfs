/**
 * NDFS host-file block backend (PC-only).
 *
 * Binds the ndfs_block_io seam to a stdio FILE*, so the library streams an
 * image a page at a time (fseek+fread / fseek+fwrite) instead of slurping the
 * whole thing into RAM.  This is the real-world path for big images on a host,
 * and it makes the block-IO seam exercised by the host test-suite on every
 * commit (see tests/test_backend_stdio.c).
 *
 * Compiled only when NDFS_WITH_STDIO_BACKEND is defined (default ON for host
 * builds, OFF for freestanding/embedded targets, which supply their own
 * backend via ndfs_open_block()).  This is the ONLY translation unit in the
 * core that pulls in the fopen family; the rest of the library's stdio use is
 * limited to snprintf.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifdef NDFS_WITH_STDIO_BACKEND

#include <ndfs/block_io.h>
#include <ndfs/filesystem.h>   /* ndfs_open_block */
#include <stdio.h>
#include <stdlib.h>

/* Backend state: the open file plus its size in whole pages (used to bound
 * reads/writes so an out-of-range page_id fails cleanly rather than fseek'ing
 * past EOF). */
typedef struct {
    FILE    *fp;
    uint32_t total_pages;
} ndfs_stdio_ctx;

/* Position the stream at the start of page `page_id`. Returns false if the page
 * is out of range or the seek fails. */
static bool stdio_seek_page(ndfs_stdio_ctx *c, uint32_t page_id)
{
    long off;
    if (page_id >= c->total_pages) return false;
    off = (long)page_id * (long)NDFS_PAGE_SIZE;
    return fseek(c->fp, off, SEEK_SET) == 0;
}

static bool stdio_read_block(void *ctx, uint32_t page_id, uint8_t *out)
{
    ndfs_stdio_ctx *c = (ndfs_stdio_ctx *)ctx;
    if (!stdio_seek_page(c, page_id)) return false;
    return fread(out, 1, NDFS_PAGE_SIZE, c->fp) == (size_t)NDFS_PAGE_SIZE;
}

static bool stdio_write_block(void *ctx, uint32_t page_id, const uint8_t *in)
{
    ndfs_stdio_ctx *c = (ndfs_stdio_ctx *)ctx;
    if (!stdio_seek_page(c, page_id)) return false;
    if (fwrite(in, 1, NDFS_PAGE_SIZE, c->fp) != (size_t)NDFS_PAGE_SIZE) return false;
    /* Flush each surgical write so the on-disk image tracks the cache the way
     * the RetroCore reference does (immediate, block-granular writes). */
    return fflush(c->fp) == 0;
}

static void stdio_destroy(void *ctx)
{
    ndfs_stdio_ctx *c = (ndfs_stdio_ctx *)ctx;
    if (!c) return;
    if (c->fp) fclose(c->fp);
    free(c);
}

ndfs_error_t ndfs_open_file(const char *path, bool read_only,
                            ndfs_filesystem_t **out_fs)
{
    ndfs_stdio_ctx *c;
    ndfs_block_io   io;
    long            bytes;
    uint32_t        total_pages;

    if (!path || !out_fs) return NDFS_ERR_NULL_PTR;

    c = (ndfs_stdio_ctx *)malloc(sizeof(*c));
    if (!c) return NDFS_ERR_ALLOC;

    /* Read-only opens "rb"; read/write opens "r+b" -- the image must already
     * exist (creating/formatting an image is image_creator's job, not this). */
    c->fp = fopen(path, read_only ? "rb" : "r+b");
    if (!c->fp) { free(c); return NDFS_ERR_IO; }

    /* Size the image in whole pages (drop any trailing partial page, matching
     * the buffer backend's floor(size/pagesize)). */
    if (fseek(c->fp, 0, SEEK_END) != 0) { fclose(c->fp); free(c); return NDFS_ERR_IO; }
    bytes = ftell(c->fp);
    if (bytes < 0) { fclose(c->fp); free(c); return NDFS_ERR_IO; }
    total_pages = (uint32_t)((size_t)bytes / NDFS_PAGE_SIZE);
    c->total_pages = total_pages;

    io.read_block  = stdio_read_block;
    io.write_block = read_only ? NULL : stdio_write_block;
    io.destroy     = stdio_destroy;   /* library closes fp + frees ctx on close/fail */
    io.ctx         = c;

    /* ndfs_open_block invokes io.destroy on failure, so `c`/`fp` are cleaned up
     * even if the image turns out to be invalid. */
    return ndfs_open_block(&io, total_pages, read_only, out_fs);
}

#endif /* NDFS_WITH_STDIO_BACKEND */
