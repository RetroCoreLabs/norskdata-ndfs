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
    NDFS_TMPL_CUSTOM,
    NDFS_TMPL_SCSI_126MB    /**< Added after NDFS_TMPL_CUSTOM so existing values stay the same. */
} ndfs_image_template_t;

/** Controller family of a SINTRAN disk type, from the generation source. */
typedef enum {
    NDFS_DISK_FAMILY_UNKNOWN = 0,   /**< Named in the manual only; not in the carved name table. */
    NDFS_DISK_FAMILY_WINCHESTER,    /**< ST506 style controller (IOX 500 family). */
    NDFS_DISK_FAMILY_SMD            /**< Big disc controller (IOX 1540 family). */
} ndfs_disk_family_t;

/** One SINTRAN disk type name and the directory size SINTRAN gives it, in pages. */
typedef struct {
    const char        *name;    /**< Name without unit number, e.g. "DISC-75MB". */
    uint32_t           pages;   /**< Directory size in pages (the declared capacity); 0 = size not known. */
    ndfs_disk_family_t family;  /**< Controller family. */
    bool               tested;  /**< True if real SINTRAN M created a directory on this type, or entered an ndtool image of it, in the emulator. */
    const char        *drive;   /**< Drive model from the SINTRAN manual. */
    uint32_t           layout_pages; /**< Pages in the full SINTRAN disk layout (cylinders * sectors per cylinder / 2,
                                          from the DT table); 0 = not known for this name. A disk image shorter than
                                          this is not accepted by the emulator's disk controllers. */
} ndfs_disk_type_t;

/** Number of SINTRAN SMD/Winchester disk types in the table. */
size_t ndfs_disk_type_count(void);

/** Get disk type number index (0 .. ndfs_disk_type_count()-1), or NULL if out of range. */
const ndfs_disk_type_t *ndfs_disk_type_get(size_t index);

/**
 * Look up a SINTRAN disk type by name (case-insensitive, e.g. "disc-75mb").
 *
 * The sizes are the fixed numbers in SINTRAN's device-name table (segment 101-S3DNAM in
 * the L and M carves). They are the declared capacity SINTRAN stores in the directory
 * label; they are not file sizes of real drives (a real drive also has a spare area,
 * whose size is not known per type). Pass the result to NDFS_TMPL_CUSTOM as custom_pages.
 * SCSI disks are not in the table: SINTRAN sets their size from the drive.
 *
 * @param name       Disk type name.
 * @param out_pages  Receives the size in pages.
 * @return NDFS_OK (out_pages may be 0 if the name is valid but its size is not known) or NDFS_ERR_NOT_FOUND.
 */
ndfs_error_t ndfs_disk_type_find(const char *name, uint32_t *out_pages);

/** The whole table entry for a disk type name (case-insensitive), or NULL. */
const ndfs_disk_type_t *ndfs_disk_type_by_name(const char *name);

/** Which boot area ndfs_create_image writes into page 0, bytes 0 to 1999. */
typedef enum {
    NDFS_BOOTCODE_AUTO = 0,     /**< The boot code that belongs to the template. */
    NDFS_BOOTCODE_NONE,         /**< Leave the boot area all zero (a data disk). */
    NDFS_BOOTCODE_SMD,          /**< Real SMD boot area (K-like layout). */
    NDFS_BOOTCODE_WINCHESTER,   /**< Real Winchester boot area (L-like layout). */
    NDFS_BOOTCODE_SCSI,         /**< Real SCSI boot area (K-like layout). */
    NDFS_BOOTCODE_FLOPPY360,    /**< Real floppy loader stream, 154 page floppy. */
    NDFS_BOOTCODE_FLOPPY616     /**< Real floppy loader stream, 616 page floppy. */
} ndfs_boot_choice_t;

/** Options for image creation. */
typedef struct {
    ndfs_image_template_t template_type;
    char                  directory_name[NDFS_NAME_MAX + 1];
    uint32_t              custom_pages;       /**< Only used with NDFS_TMPL_CUSTOM. */
    bool                  include_extended_info;
    uint16_t              system_number;
    uint16_t              flag_word;
    ndfs_boot_choice_t    boot_choice;        /**< Default NDFS_BOOTCODE_AUTO. Not verified: nobody has booted a created image. */
    const uint8_t        *boot_data;          /**< Optional own boot area (bytes 0..1999 of page 0); wins over boot_choice. */
    size_t                boot_size;          /**< Length of boot_data, at most 2000. */
    uint32_t              device_pages;       /**< Image length in pages; 0 = the template size. Used with NDFS_TMPL_CUSTOM; ignored if less than the capacity. */
    int32_t               bit_file_answer;    /**< SINTRAN's bit file address prompt: 0 = default, -1 = end of disc, n = octal address as a number. */
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
