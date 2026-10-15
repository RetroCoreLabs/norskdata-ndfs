/**
 * NDFS image creation: create new NDFS disk images from templates.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_IMAGE_CREATOR_H
#define NDFS_IMAGE_CREATOR_H

#include "filesystem.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Pre-defined disk image templates. */
typedef enum {
    NDFS_TMPL_FLOPPY_360KB,
    NDFS_TMPL_FLOPPY_12MB,
    NDFS_TMPL_SMD_75MB,
    NDFS_TMPL_WINCHESTER_74MB,
    NDFS_TMPL_CUSTOM
} ndfs_image_template_t;

/** Options for image creation. */
typedef struct {
    ndfs_image_template_t template_type;
    char                  directory_name[NDFS_NAME_MAX + 1];
    uint32_t              custom_pages;       /**< Only used with NDFS_TMPL_CUSTOM. */
    bool                  include_extended_info;
    uint16_t              system_number;
    uint16_t              flag_word;
} ndfs_image_options_t;

/**
 * Initialize image options with sensible defaults.
 * @param opts  Options struct to initialize.
 */
void ndfs_image_options_init(ndfs_image_options_t *opts);

/**
 * Create a new NDFS disk image from a template or custom size.
 *
 * The resulting filesystem handle owns the image buffer and is writable.
 * The caller must close it with ndfs_close() when done.
 *
 * @param out_fs   Receives the new filesystem handle.
 * @param options  Image creation options.
 * @return NDFS_OK or error code.
 */
ndfs_error_t ndfs_create_image(ndfs_filesystem_t **out_fs,
                               const ndfs_image_options_t *options);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_IMAGE_CREATOR_H */
