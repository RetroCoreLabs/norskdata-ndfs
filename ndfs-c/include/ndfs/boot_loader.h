/**
 * NDFS boot loader detection: detect and load boot code from NDFS page 0.
 *
 * Supports BPUN, FLOMON, and raw binary boot formats.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_BOOT_LOADER_H
#define NDFS_BOOT_LOADER_H

#include "filesystem.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Loaded boot code information. */
typedef struct {
    ndfs_boot_format_t          format;
    ndfs_boot_controller_type_t controller_type;
    uint16_t           start_address;
    uint16_t           boot_address;
    uint16_t           load_address;
    uint16_t           word_count;
    uint8_t           *data;       /**< Malloc'd code data (caller frees via ndfs_boot_code_destroy). */
    size_t             data_size;
    bool               checksum_valid;
} ndfs_boot_code_t;

/**
 * Detect the boot format in an NDFS filesystem's page 0.
 * @param fs      Open filesystem handle.
 * @param format  Receives the detected format.
 */
ndfs_error_t ndfs_detect_boot_format(ndfs_filesystem_t *fs,
                                     ndfs_boot_format_t *format);

/**
 * Load boot code from an NDFS filesystem's page 0.
 * @param fs    Open filesystem handle.
 * @param code  Receives the loaded boot code. Caller must call
 *              ndfs_boot_code_destroy() when done.
 */
ndfs_error_t ndfs_load_boot_code(ndfs_filesystem_t *fs,
                                 ndfs_boot_code_t *code);

/**
 * Check if an NDFS filesystem is bootable (has valid boot code).
 * @param fs        Open filesystem handle.
 * @param bootable  Receives true if bootable.
 */
ndfs_error_t ndfs_is_bootable(ndfs_filesystem_t *fs, bool *bootable);

/**
 * Detect the hard-disk controller family (SMD/ECC, Winchester, SCSI) targeted by
 * a raw-binary bootstrap. Result is NDFS_CONTROLLER_UNKNOWN for BPUN/FLOMON
 * (floppy) boots or non-bootable disks.
 * @param fs    Open filesystem handle.
 * @param type  Receives the detected controller type.
 */
ndfs_error_t ndfs_detect_boot_controller_type(ndfs_filesystem_t *fs,
                                              ndfs_boot_controller_type_t *type);

/**
 * Free resources in a boot code struct.
 * Safe to call on zeroed struct or with NULL data.
 */
void ndfs_boot_code_destroy(ndfs_boot_code_t *bc);

#ifdef __cplusplus
}
#endif

#endif /* NDFS_BOOT_LOADER_H */
