/**
 * NDFS block-IO seam: the library's single storage path.
 *
 * Every page the filesystem touches is fetched and stored through an
 * ndfs_block_io vtable, never by pointer arithmetic into a flat image
 * buffer.  This lets the exact same filesystem/traversal logic run over
 *   - an in-RAM buffer   (the historical behaviour; ndfs_open_buffer*),
 *   - a host stdio file  (ndfs_open_file, streams instead of slurping),
 *   - a consumer-supplied embedded backend (e.g. a Pico reading an SD
 *     card through ndm_media_read/write) via ndfs_open_block().
 *
 * A page is NDFS_PAGE_SIZE (2048) bytes.  Byte offset of page N in the
 * image is N * NDFS_PAGE_SIZE.  Everything is whole pages.
 *
 * THREAD-SAFETY: an ndfs_filesystem_t handle carries a mutable page cache, so a
 * single handle is NOT safe for concurrent use -- not even for concurrent
 * readers (a read updates cache LRU/pin state). Give each thread/task its own
 * handle by opening the image independently.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_BLOCK_IO_H
#define NDFS_BLOCK_IO_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Storage backend vtable: fetch/store exactly one NDFS page.
 *
 * read_block  fills `out` (NDFS_PAGE_SIZE bytes) with the contents of page
 *             `page_id`.  Returns true on success; a false aborts the current
 *             filesystem operation with NDFS_ERR_IO.
 * write_block stores `in` (NDFS_PAGE_SIZE bytes) as page `page_id`.  May be
 *             NULL for a read-only backend; the library then rejects mutation.
 * destroy     optional cleanup for `ctx`, called once from ndfs_close() (and
 *             on a failed open).  Leave NULL when the caller owns `ctx` and
 *             does not want the library to free it (the usual embedded case).
 * ctx         opaque backend state passed back to every callback.
 */
typedef struct ndfs_block_io {
    bool (*read_block) (void *ctx, uint32_t page_id, uint8_t *out /* NDFS_PAGE_SIZE */);
    bool (*write_block)(void *ctx, uint32_t page_id, const uint8_t *in /* NDFS_PAGE_SIZE */);
    void (*destroy)    (void *ctx);
    void  *ctx;
} ndfs_block_io;

/* Forward-declared opaque handle (full definition lives in filesystem.h).
 * Guarded so C99 does not see a duplicate typedef when both headers are
 * included together (C99 forbids redefining a typedef, unlike C11). */
#ifndef NDFS_FILESYSTEM_T_DEFINED
#define NDFS_FILESYSTEM_T_DEFINED
typedef struct ndfs_filesystem ndfs_filesystem_t;
#endif

/**
 * Open an NDFS image over an arbitrary block backend.
 *
 * The vtable is copied by value into the handle; `io->ctx` and the function
 * pointers must stay valid for the lifetime of the handle.  On failure the
 * backend's destroy hook (if any) is invoked so the caller need not clean up.
 *
 * @param io          Backend vtable (read_block required; write_block may be NULL).
 * @param total_pages Image size in whole pages (image_bytes / NDFS_PAGE_SIZE).
 * @param read_only   If true (or write_block is NULL) mutations are rejected.
 * @param out_fs      Receives the filesystem handle on success.
 */
ndfs_error_t ndfs_open_block(const ndfs_block_io *io, uint32_t total_pages,
                             bool read_only, ndfs_filesystem_t **out_fs);

#ifdef NDFS_WITH_STDIO_BACKEND
/**
 * Open an NDFS image straight from a host file (PC-only convenience).
 *
 * Uses the stdio host-file backend (fopen/fseek/fread/fwrite): the image is
 * streamed a page at a time through the page cache rather than slurped whole.
 * Compiled only when NDFS_WITH_STDIO_BACKEND is defined (default ON for host
 * builds, OFF for freestanding/embedded targets).
 *
 * @param path       Path to the disk-image file.
 * @param read_only  If true the file is opened "rb" and writes are rejected;
 *                   otherwise "r+b" (the file must already exist).
 * @param out_fs     Receives the filesystem handle on success.
 */
ndfs_error_t ndfs_open_file(const char *path, bool read_only,
                            ndfs_filesystem_t **out_fs);
#endif /* NDFS_WITH_STDIO_BACKEND */

#ifdef __cplusplus
}
#endif

#endif /* NDFS_BLOCK_IO_H */
