/**
 * NDFS image creation implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/image_creator.h>
#include "endian_util.h"
#include "ndfs_name.h"
#include "boot_blobs.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ---- Template specifications ----------------------------------------- */

/* What a template fixes. Everything else (where the bit file, the user file and the object
 * file go, the bitmap, the Unreserved Pages count) is worked out from these numbers by the
 * rules measured with the real SINTRAN M @CREATE-DIRECTORY in the nd100x emulator; see
 * docs/CREATE-DIRECTORY-ORACLE.md. */
typedef struct {
    uint32_t            capacity;      /* declared directory size in pages (Pages Available) */
    uint32_t            device_pages;  /* length of the image in pages; never less than capacity */
    bool                ext_valid;     /* hard disk: write the Extended Info Block (floppies have none) */
    ndfs_boot_choice_t  boot;          /* boot area written when the caller says NDFS_BOOTCODE_AUTO */
    bool                scsi_last_page;/* copy the real SCSI last page (SINTRAN needs it) */
} ndfs_template_spec_t;

static const ndfs_template_spec_t spec_floppy_360kb = { 154,   154,   false, NDFS_BOOTCODE_FLOPPY360, false };
static const ndfs_template_spec_t spec_floppy_12mb  = { 616,   616,   false, NDFS_BOOTCODE_FLOPPY616, false };
/* SMD 75MB: capacity 36945 is DISC-75MB in SINTRAN's name table. 38400 pages is the file
 * size used by the real SMD0.IMG; no source for it was found. */
static const ndfs_template_spec_t spec_smd_75mb     = { 36945, 38400, true,  NDFS_BOOTCODE_SMD,        false };
/* Winchester 74MB. Values read from the real Micropolis 1325 image 1325.img: file is
 * 36864 pages (75497472 bytes), declared capacity 36396 (DISC-74MB). The device is 468
 * pages LARGER than the capacity (spare area), never smaller. The earlier file length
 * 36360 ended 36 pages before the pages the label says exist. */
static const ndfs_template_spec_t spec_winchester_74mb = { 36396, 36864, true, NDFS_BOOTCODE_WINCHESTER, false };
/* SCSI 126MB. Values read from the real SCSI image scsi-1.img: file is 64656 pages
 * (132415488 bytes), declared capacity 61036. SINTRAN accepts a SCSI disk only if its
 * length is exactly this and its LAST page holds the real, undecoded 53 bytes (measured:
 * an all-zero last page, or another length with that page, gives a disk transfer error). */
static const ndfs_template_spec_t spec_scsi_126mb   = { 61036, 64656, true,  NDFS_BOOTCODE_SCSI,       true  };

static const ndfs_template_spec_t *get_spec(ndfs_image_template_t tmpl)
{
    switch (tmpl) {
        case NDFS_TMPL_FLOPPY_360KB:    return &spec_floppy_360kb;
        case NDFS_TMPL_FLOPPY_12MB:     return &spec_floppy_12mb;
        case NDFS_TMPL_SMD_75MB:        return &spec_smd_75mb;
        case NDFS_TMPL_WINCHESTER_74MB: return &spec_winchester_74mb;
        case NDFS_TMPL_SCSI_126MB:      return &spec_scsi_126mb;
        default:                        return NULL;
    }
}

/* ---- SINTRAN disk types (fixed SMD / Winchester sizes) ---------------- */

/* Names and sizes come from the device-name table (segment 101-S3DNAM) of the SINTRAN L
 * and M carves: each disk unit name carries one fixed 32-bit number, the directory size
 * in pages (identical in L and M). Names with a unit number (DISC-75MB-1) use the base
 * name here. DISC-30MB, DISC-60MB and DISC-90MB share 7389.
 *
 * family  - the controller family from the generation source (NPL s3vs-4.symb): the
 *           layout entries DT014 to DT074 are under the Winchester (ST506, IOX 500)
 *           conditional, all others under the big disc (SMD, IOX 1540) conditional.
 * drive   - the drive model the SINTRAN III System Supervisor manual (ND-30.003.7,
 *           Appendix C, table 27) gives for the name. "not named in the manual" = the
 *           carved name table has the name, the manual edition does not.
 * tested  - true only if, in the nd100x emulator, the real SINTRAN M either created a
 *           directory on a blank image of this type (DISC-38MB, 70MB, 74MB, 75MB), or did
 *           ENTER-DIRECTORY, DIR and CREATE-FILE on an image made by ndtool with the
 *           full layout length (the others marked yes), and the result was read back
 *           (docs/CREATE-DIRECTORY-ORACLE.md). false = valid name, never run here. The
 *           emulator could not create DISC-30MB (SINTRAN halted in ERRFATAL).
 * layout  - the last number: pages in the full layout of the DT entry whose comment names
 *           this type (DT table, DISK-LAYOUT-TABLE-REPORT). Only names that a DT comment
 *           gives directly have one; the others are 0.
 * pages 0 - named in the manual but not in the carved name table: size unknown. */
static const ndfs_disk_type_t disk_types[] = {
    { "DISC-14MB",          6912, NDFS_DISK_FAMILY_WINCHESTER, true, "CDC 9410 Finch", 6912 },
    { "DISC-16MB",          8000, NDFS_DISK_FAMILY_WINCHESTER, false, "Micropolis 1304 (ST506)", 0 },
    { "DISC-21MB",         10728, NDFS_DISK_FAMILY_WINCHESTER, true, "CDC 9410 Finch", 10890 },
    { "DISC-23MB",         11016, NDFS_DISK_FAMILY_WINCHESTER, true, "Micropolis 1304 (ST506)", 11205 },
    { "DISC-28MB",         13648, NDFS_DISK_FAMILY_WINCHESTER, true, "Micropolis 1325 (ST506)", 13797 },
    { "DISC-45MB",         22032, NDFS_DISK_FAMILY_WINCHESTER, true, "Micropolis 1304/1325 (ST506)", 22410 },
    { "DISC-74MB",         36396, NDFS_DISK_FAMILY_WINCHESTER, true, "Micropolis 1325 (ST506)", 36792 },
    { "DISC-30MB",          7389, NDFS_DISK_FAMILY_SMD,        false, "CDC 9448 CMD big cartridge", 0 },
    { "DISC-60MB",          7389, NDFS_DISK_FAMILY_SMD,        false, "CDC 9762 SMD (ND-IO)", 0 },
    { "DISC-90MB",          7389, NDFS_DISK_FAMILY_SMD,        false, "CDC 9448 CMD", 0 },
    { "DISC-38MB",         18486, NDFS_DISK_FAMILY_SMD,        true, "CDC 9762 SMD (ND-IO)", 18495 },
    { "DISC-70MB",         34765, NDFS_DISK_FAMILY_SMD,        true, "CDC 9710 RSD", 37035 },
    { "DISC-75MB",         36945, NDFS_DISK_FAMILY_SMD,        true, "CDC 9762 SMD", 37035 },
    { "DISC-2-75MB",       36945, NDFS_DISK_FAMILY_SMD,        false, "CDC 9730 MMD (150 MB, sub-divided)", 0 },
    { "DISC-3-75MB",       36945, NDFS_DISK_FAMILY_SMD,        false, "CDC 9766 SMD (225 MB, sub-divided)", 0 },
    { "DISC-140MB-F",      69530, NDFS_DISK_FAMILY_SMD,        false, "Fujitsu M2322K", 0 },
    { "DISC-2-70MB-F",     34765, NDFS_DISK_FAMILY_SMD,        false, "Fujitsu M2322K (140 MB, sub-divided)", 0 },
    { "DISC-225MB-R",     110292, NDFS_DISK_FAMILY_SMD,        false, "CDC 9766 SMD", 0 },
    { "DISC-225MB-F",     110292, NDFS_DISK_FAMILY_SMD,        false, "not named in the manual", 0 },
    { "DISC-225MB-E",     110292, NDFS_DISK_FAMILY_SMD,        false, "not named in the manual", 0 },
    { "DISC-225MB-N",     110292, NDFS_DISK_FAMILY_SMD,        false, "not named in the manual", 0 },
    { "DISC-288MB-R",     140391, NDFS_DISK_FAMILY_SMD,        true, "CDC 9766 SMD", 140733 },
    { "DISC-288MB-F",     140391, NDFS_DISK_FAMILY_SMD,        true, "CDC 9715 FSD 340", 153576 },
    { "DISC-288MB-E",     140391, NDFS_DISK_FAMILY_SMD,        false, "CDC 9720 EMD 368", 158210 },
    { "DISC-288MB-N",     140391, NDFS_DISK_FAMILY_SMD,        false, "not named in the manual", 144025 },
    { "DISC-4-70MB-R",     34765, NDFS_DISK_FAMILY_SMD,        false, "CDC 9766 SMD (280 MB, sub-divided)", 0 },
    { "DISC-4-70MB-F",     34765, NDFS_DISK_FAMILY_SMD,        false, "CDC 9715 FSD 340 (280 MB, sub-divided)", 0 },
    { "DISC-4-70MB-E",     34765, NDFS_DISK_FAMILY_SMD,        false, "CDC 9720 EMD 368 (280 MB, sub-divided)", 0 },
    { "DISC-4-70MB-N",     34765, NDFS_DISK_FAMILY_SMD,        false, "not named in the manual", 0 },
    { "DISC-6-70MB-F",     34765, NDFS_DISK_FAMILY_SMD,        false, "CDC 9715 FSD 515 (450 MB, sub-divided)", 0 },
    { "DISC-6-70MB-N",     34765, NDFS_DISK_FAMILY_SMD,        false, "not named in the manual", 0 },
    { "DISC-450MB-F",     220584, NDFS_DISK_FAMILY_SMD,        true, "CDC 9715 FSD 515", 230364 },
    { "DISC-450MB-N",     220584, NDFS_DISK_FAMILY_SMD,        true, "not named in the manual", 223820 },
    { "DISC-10MB",             0, NDFS_DISK_FAMILY_UNKNOWN,    false, "CDC 9427 Hawk cartridge (manual only)", 0 },
    { "DISC-33MB",             0, NDFS_DISK_FAMILY_UNKNOWN,    false, "CDC 9762 SMD, ND-IO (manual only)", 0 },
    { "DISC-50MB",             0, NDFS_DISK_FAMILY_UNKNOWN,    false, "CDC 9448 CMD (manual only)", 0 },
    { "DISC-2-225MB-F",        0, NDFS_DISK_FAMILY_UNKNOWN,    false, "CDC 9715 FSD 515 (450 MB, sub-divided; manual only)", 0 },
    { "DISC-2-225MB-E",        0, NDFS_DISK_FAMILY_UNKNOWN,    false, "NEC D3525-H (450 MB, sub-divided; manual only)", 0 },
    { "DISC-6-70MB-E",         0, NDFS_DISK_FAMILY_UNKNOWN,    false, "NEC D3525-H (450 MB, sub-divided; manual only)", 0 },
    { "DISC-450MB-E",          0, NDFS_DISK_FAMILY_UNKNOWN,    false, "NEC D3525-H (manual only)", 0 }
};

size_t ndfs_disk_type_count(void)
{
    return sizeof(disk_types) / sizeof(disk_types[0]);
}

const ndfs_disk_type_t *ndfs_disk_type_get(size_t index)
{
    if (index >= ndfs_disk_type_count()) return NULL;
    return &disk_types[index];
}

const ndfs_disk_type_t *ndfs_disk_type_by_name(const char *name)
{
    size_t i, k;

    if (!name) return NULL;
    for (i = 0; i < ndfs_disk_type_count(); i++) {
        const char *want = disk_types[i].name;
        for (k = 0; want[k] && name[k]; k++) {
            if (toupper((unsigned char)name[k]) != want[k]) break;
        }
        if (want[k] == '\0' && name[k] == '\0') return &disk_types[i];
    }
    return NULL;
}

ndfs_error_t ndfs_disk_type_find(const char *name, uint32_t *out_pages)
{
    const ndfs_disk_type_t *t;

    if (!name || !out_pages) return NDFS_ERR_NULL_PTR;
    t = ndfs_disk_type_by_name(name);
    if (!t) return NDFS_ERR_NOT_FOUND;
    *out_pages = t->pages;
    return NDFS_OK;
}

/* ---- Custom size ------------------------------------------------------ */

static void build_custom_spec(uint32_t pages, ndfs_template_spec_t *out)
{
    memset(out, 0, sizeof(*out));
    out->capacity     = pages;
    out->device_pages = pages;
    out->ext_valid    = (pages > 1000);          /* floppy sized images carry no Extended Info Block */
    out->boot         = (pages == 154) ? NDFS_BOOTCODE_FLOPPY360
                      : (pages == 616) ? NDFS_BOOTCODE_FLOPPY616 : NDFS_BOOTCODE_NONE;
}

/* ---- Helpers --------------------------------------------------------- */

static void str_toupper_ic(char *s)
{
    while (*s) {
        *s = (char)toupper((unsigned char)*s);
        s++;
    }
}

/* ---- Public API ------------------------------------------------------ */

void ndfs_image_options_init(ndfs_image_options_t *opts)
{
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->template_type = NDFS_TMPL_FLOPPY_360KB;
    strcpy(opts->directory_name, "NDFS-DISK");
    opts->include_extended_info = false;
    opts->system_number = 0;
    opts->flag_word = 0;
    opts->boot_choice = NDFS_BOOTCODE_AUTO;
    opts->boot_data = NULL;
    opts->boot_size = 0;
    opts->bit_file_answer = 0;
}

/* Bitmap bit for one page in a raw bit file image.
 *
 * The addressing MUST match ndfs_bf_* in bit_file.c: page N is bit N%16 of the 16-bit
 * big-endian word N/16, which on a byte array means byte (N>>3)^1, bit N&7. See the
 * comment above bf_byte_index() for the SINTRAN manual reference. One function per
 * operation, one place to be wrong. */
static void mark_page_used(uint8_t *bitmap, uint32_t page)
{
    bitmap[(page >> 3) ^ 1u] |= (uint8_t)(1u << (page & 7u));
}

static bool page_is_used(const uint8_t *bitmap, uint32_t page)
{
    return (bitmap[(page >> 3) ^ 1u] >> (page & 7u)) & 1u;
}

/* The page allocator SINTRAN M uses for the first user file and the first object file
 * (routine RSBPG in segment 006-S3FS, read in the M carve and checked against 12 measured
 * images, see docs/CREATE-DIRECTORY-ORACLE.md):
 *
 *   hint H = BA - 1; the search starts at the TOP page of the 16-page bitmap word that
 *   holds H (page H OR 15) and goes DOWN to the first free page (bit 0). If that word has
 *   no free page the search spirals outwards over the neighbouring words: word+1 (upwards,
 *   lowest page first), word-1 (downwards, highest page first), word+2, word-2, ...
 *
 * The spiral part was read in the code but never exercised by a measurement. Pages must
 * be below the declared capacity. */
static bool alloc_near_hint(uint8_t *bitmap, uint32_t capacity, uint32_t hint, uint32_t *out)
{
    uint32_t word = hint >> 4;
    uint32_t nwords = (capacity + 15u) >> 4;
    uint32_t step, bit, w;

    for (bit = 16; bit-- > 0; ) {
        uint32_t p = (word << 4) + bit;
        if (p < capacity && !page_is_used(bitmap, p)) { mark_page_used(bitmap, p); *out = p; return true; }
    }
    for (step = 1; step <= nwords; step++) {
        w = word + step;
        if (w < nwords) {
            for (bit = 0; bit < 16; bit++) {
                uint32_t p = (w << 4) + bit;
                if (p < capacity && !page_is_used(bitmap, p)) { mark_page_used(bitmap, p); *out = p; return true; }
            }
        }
        if (word >= step) {
            w = word - step;
            for (bit = 16; bit-- > 0; ) {
                uint32_t p = (w << 4) + bit;
                if (p < capacity && !page_is_used(bitmap, p)) { mark_page_used(bitmap, p); *out = p; return true; }
            }
        }
    }
    return false;
}

ndfs_error_t ndfs_create_image(ndfs_filesystem_t **out_fs,
                               const ndfs_image_options_t *options)
{
    const ndfs_template_spec_t *spec;
    ndfs_template_spec_t custom_spec;
    uint8_t *img;
    uint8_t *bm;
    size_t img_size;
    ndfs_master_block_t mb;
    ndfs_user_entry_t ue;
    ndfs_block_pointer_t bp;
    uint32_t cap, dev, bitmap_pages, ba, avail, quota;
    uint32_t user_data = 0, user_index = 0, obj_data = 0, obj_index = 0;
    ndfs_boot_choice_t boot;
    ndfs_error_t err;

    if (!out_fs || !options) return NDFS_ERR_NULL_PTR;

    /* Get template spec */
    if (options->template_type == NDFS_TMPL_CUSTOM) {
        if (options->custom_pages < 32) return NDFS_ERR_TOO_SMALL;
        build_custom_spec(options->custom_pages, &custom_spec);
        if (options->device_pages > custom_spec.device_pages)
            custom_spec.device_pages = options->device_pages;
        spec = &custom_spec;
    } else {
        spec = get_spec(options->template_type);
        if (!spec) return NDFS_ERR_INVALID_ARG;
    }
    cap = spec->capacity;
    dev = spec->device_pages;

    /* The bit file: one bit per page, 16384 bits per page, sized by the declared capacity. */
    bitmap_pages = (cap + 16383u) / 16384u;

    /* Bit file address, the answer to SINTRAN's prompt "OCTAL BIT FILE ADDRESS (-1 END OF DISC)":
     *   0  -> 9 * floor(floor(capacity / 2) / 9)    (measured on 6 disk types)
     *   -1 -> capacity - bitmap pages               (end of the disc)
     *   n  -> n, used as given. */
    if (options->bit_file_answer == 0)
        ba = 9u * ((cap / 2u) / 9u);
    else if (options->bit_file_answer == -1)
        ba = cap - bitmap_pages;
    else if (options->bit_file_answer > 0)
        ba = (uint32_t)options->bit_file_answer;
    else
        return NDFS_ERR_INVALID_ARG;
    if (ba < 8 || ba + bitmap_pages > cap) return NDFS_ERR_INVALID_ARG;

    /* Pages left for the user quota: capacity - page 0 - the bit file - the user file (index
     * block + data page) - the object file index block. Measured: Unreserved Pages in the
     * master block is exactly this minus the sum of the user quotas (9 runs). */
    if (cap < 40u + bitmap_pages) return NDFS_ERR_TOO_SMALL;
    avail = cap - 1u - bitmap_pages - 2u - 1u;
    quota = cap > 1000 ? cap / 2u : (avail < 500u ? avail : 500u);

    /* Allocate image buffer */
    img_size = (size_t)dev * NDFS_PAGE_SIZE;
    img = (uint8_t *)calloc(img_size, 1);
    if (!img) return NDFS_ERR_ALLOC;

    /* ---- Bitmap, as @CREATE-DIRECTORY leaves it, then the four pages the first user and
     *      the first file take ---- */
    bm = (uint8_t *)calloc((size_t)bitmap_pages, NDFS_PAGE_SIZE);
    if (!bm) { free(img); return NDFS_ERR_ALLOC; }
    {
        uint32_t i;
        mark_page_used(bm, 0);
        for (i = 0; i < bitmap_pages; i++) mark_page_used(bm, ba + i);
        /* Hard disks: when the device is longer than the capacity, SINTRAN marks the pages
         * from the capacity up to the next multiple of 256 as used (measured: SMD 75MB
         * 36945..37119, Winchester 74MB 36396..36607, SCSI 61036..61183). */
        if (spec->ext_valid && dev > cap) {
            uint32_t end = ((cap + 255u) / 256u) * 256u;
            if (end > dev) end = dev;
            for (i = cap; i < end; i++) mark_page_used(bm, i);
        }
    }
    if (!alloc_near_hint(bm, cap, ba - 1u, &user_data) ||
        !alloc_near_hint(bm, cap, ba - 1u, &user_index) ||
        !alloc_near_hint(bm, cap, ba - 1u, &obj_data) ||
        !alloc_near_hint(bm, cap, ba - 1u, &obj_index)) {
        free(bm); free(img);
        return NDFS_ERR_NO_SPACE;
    }

    /* ---- Boot area: bytes 0..1999 of page 0 ---- */
    boot = options->boot_choice == NDFS_BOOTCODE_AUTO ? spec->boot : options->boot_choice;
    if (options->boot_data && options->boot_size > 0) {
        size_t n = options->boot_size > 2000 ? 2000 : options->boot_size;
        memcpy(img, options->boot_data, n);
    } else if (boot != NDFS_BOOTCODE_NONE) {
        ndfs_boot_blob_id_t id = boot == NDFS_BOOTCODE_SMD ? NDFS_BOOT_BLOB_SMD
                               : boot == NDFS_BOOTCODE_WINCHESTER ? NDFS_BOOT_BLOB_WINCHESTER
                               : boot == NDFS_BOOTCODE_SCSI ? NDFS_BOOT_BLOB_SCSI
                               : boot == NDFS_BOOTCODE_FLOPPY360 ? NDFS_BOOT_BLOB_FLOPPY360
                               : NDFS_BOOT_BLOB_FLOPPY616;
        size_t n = 0;
        const uint8_t *blob = ndfs_boot_blob(id, &n);
        if (blob) memcpy(img, blob, n);
    }

    /* ---- Page 0: master block (and Extended Info Block on hard disks) ---- */
    memset(&mb, 0, sizeof(mb));
    {
        size_t nlen = strlen(options->directory_name);
        if (nlen > NDFS_NAME_MAX) nlen = NDFS_NAME_MAX;
        memcpy(mb.directory_name, options->directory_name, nlen);
        mb.directory_name[nlen] = '\0';
        str_toupper_ic(mb.directory_name);
    }

    mb.object_file_ptr.block_id = obj_index;
    mb.object_file_ptr.type     = NDFS_PTR_INDEXED;
    mb.user_file_ptr.block_id   = user_index;
    mb.user_file_ptr.type       = NDFS_PTR_INDEXED;
    mb.bit_file_ptr.block_id    = ba;
    mb.bit_file_ptr.type        = NDFS_PTR_CONTIGUOUS;
    mb.unreserved_pages         = avail - quota;
    mb.image_size               = cap;

    ndfs_mb_write(&mb, img);

    /* Extended Info Block: hard disks only. Floppies get none (measured). */
    if (options->include_extended_info || spec->ext_valid) {
        mb.ext_valid = true;
        mb.ext_flag_word = options->flag_word;
        mb.ext_last_system_number = options->system_number;
        mb.ext_pages_available = cap;
        ndfs_mb_write_extended(&mb, img);
    }

    /* ---- User file: index block -> data page with the SYSTEM entry ---- */
    bp.block_id = user_data;
    bp.type = NDFS_PTR_CONTIGUOUS;
    ndfs_bp_to_bytes(&bp, img + (size_t)user_index * NDFS_PAGE_SIZE, 0);

    ndfs_ue_init(&ue);
    strcpy(ue.user_name, "SYSTEM");
    ue.user_index = 0;
    ue.pages_reserved = quota;
    ue.pages_used = 1;   /* the object data page, charged to its owner like SINTRAN does (NDFS-ALLOCATION-RULES.md section 7) */
    ndfs_ue_to_bytes(&ue, img + (size_t)user_data * NDFS_PAGE_SIZE);

    /* ---- Object file: index block -> data page (no files yet, all zero) ---- */
    bp.block_id = obj_data;
    bp.type = NDFS_PTR_CONTIGUOUS;
    ndfs_bp_to_bytes(&bp, img + (size_t)obj_index * NDFS_PAGE_SIZE, 0);

    /* ---- Bit file ---- */
    memcpy(img + (size_t)ba * NDFS_PAGE_SIZE, bm, (size_t)bitmap_pages * NDFS_PAGE_SIZE);
    free(bm);

    /* ---- SCSI: the last page the real disks carry ---- */
    if (spec->scsi_last_page)
        memcpy(img + (size_t)(dev - 1u) * NDFS_PAGE_SIZE, ndfs_scsi_last_page(), NDFS_PAGE_SIZE);

    /* ---- Open the image as a filesystem ---- */
    err = ndfs_open_buffer_copy(img, img_size, false, out_fs);
    free(img);

    return err;
}
