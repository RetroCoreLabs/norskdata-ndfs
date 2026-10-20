/**
 * Boot code for new images (see boot_blobs.c for where the bytes come from).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#ifndef NDFS_BOOT_BLOBS_H
#define NDFS_BOOT_BLOBS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    NDFS_BOOT_BLOB_SMD = 1,
    NDFS_BOOT_BLOB_WINCHESTER,
    NDFS_BOOT_BLOB_SCSI,
    NDFS_BOOT_BLOB_FLOPPY360,
    NDFS_BOOT_BLOB_FLOPPY616
} ndfs_boot_blob_id_t;

/** The 2000 byte boot area for the given blob, or NULL. *size receives the length. */
const uint8_t *ndfs_boot_blob(ndfs_boot_blob_id_t id, size_t *size);

/** The 2048 byte last page of the real SCSI image (needed for SINTRAN to accept a SCSI disk). */
const uint8_t *ndfs_scsi_last_page(void);

#endif /* NDFS_BOOT_BLOBS_H */
