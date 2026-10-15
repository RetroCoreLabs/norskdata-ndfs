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
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ---- Template specifications ----------------------------------------- */

typedef struct {
    uint32_t ndfs_pages;
    uint32_t file_blocks;
    uint32_t object_file_block;
    uint32_t user_file_block;
    uint32_t bit_file_block;
    uint32_t unreserved_pages;
    bool     has_flomon;
    bool     ext_valid;
} ndfs_template_spec_t;

static const ndfs_template_spec_t spec_floppy_360kb = {
    154, 154, 149, 151, 153, 1, true, false
};
static const ndfs_template_spec_t spec_floppy_12mb = {
    616, 616, 611, 613, 615, 1, true, false
};
static const ndfs_template_spec_t spec_smd_75mb = {
    36945, 38400, 18684, 18686, 18472, 36945, false, true
};
static const ndfs_template_spec_t spec_winchester_74mb = {
    36396, 36360, 32771, 32769, 18198, 36396, false, true
};

static const ndfs_template_spec_t *get_spec(ndfs_image_template_t tmpl)
{
    switch (tmpl) {
        case NDFS_TMPL_FLOPPY_360KB:    return &spec_floppy_360kb;
        case NDFS_TMPL_FLOPPY_12MB:     return &spec_floppy_12mb;
        case NDFS_TMPL_SMD_75MB:        return &spec_smd_75mb;
        case NDFS_TMPL_WINCHESTER_74MB: return &spec_winchester_74mb;
        default:                        return NULL;
    }
}

/* ---- Build custom spec from page count ------------------------------- */

static void build_custom_spec(uint32_t pages, ndfs_template_spec_t *out)
{
    uint32_t bf_block;

    memset(out, 0, sizeof(*out));
    out->ndfs_pages  = pages;
    out->file_blocks = pages;
    out->has_flomon  = false;
    out->ext_valid   = (pages > 1000);

    /* Placement.
     *
     * KERNEL-DERIVED from ALBIT (137517B). The old ">1000 pages = big disk" branch was
     * WRONG: SINTRAN does not switch layout on device size at all. It branches on whether
     * @CREATE-DIRECTORY was given an EXPLICIT bit-file address. A small floppy created on
     * the default path still lands mid-disk. (Real proof: two same-size 616-page floppies
     * sit in opposite layouts because one used the override path.)
     *
     * DEFAULT path (no address supplied):
     *     bit_file = 9 * floor(floor(declared_pages / 2) / 9)
     * i.e. floor(pages/2) rounded DOWN to a multiple of 9.
     * ALBIT 137526B-137532B: SAD ZIN SHR 1 (/2) -> SAT 11 (=9) -> RDIV ST (/9)
     *                        -> RMPY ST DA (*9).
     * Checks out on every real disk: 36945 -> 18472 -> 18468 (the true PACK-ONE
     * bit_file_ptr; plain pages/2 gives 18472, off by 4); 616 -> 308 -> 306; and
     * 61036 -> 30518 -> 30510 on the real SCSI pack. Plain pages/2 is simply wrong.
     *
     * The "9" is PAGES PER TRACK (SMD: 18 sectors/track x 1024 B = 18432 B = 9 pages of
     * 2048), which is why @CREATE-DIRECTORY says the bit file starts "at a track
     * boundary". Note SINTRAN uses the fixed immediate 9 regardless of drive.
     *
     * The object/user base is APPROXIMATE and NOT statically derivable: it comes from the
     * CRDIR scan loop (137173B-137352B) and has no clean closed form - empirically
     * bit+216 on SMD, bit+206 on SCSI, bit+202 on floppy. We use bit + bitmap_pages
     * rounded up so the object/user index blocks always clear the bitmap's own page span
     * (the bitmap can be several pages on a large image; overlapping it corrupted the
     * object directory). Only bit-exactness with a SINTRAN-created image is affected -
     * the reader is pointer-driven and reads any placement correctly.
     *
     * user_file = object_file + 2 holds on floppies and SMD/SCSI. (On the real Winchester
     * WD0.img it is object - 2; the invariant that actually holds everywhere is
     * |user - object| == 2. We emit +2 here.) */
    {
        uint32_t bitmap_bytes = (((pages + 7) / 8) + 1) & ~1u;
        uint32_t bitmap_pages = (bitmap_bytes + NDFS_PAGE_SIZE - 1) / NDFS_PAGE_SIZE;
        uint32_t half         = pages / 2;

        bf_block = (half / 9u) * 9u;                          /* ALBIT: round to a track */

        out->bit_file_block    = bf_block;
        out->object_file_block = bf_block + bitmap_pages;     /* clear the bitmap span */
        out->user_file_block   = out->object_file_block + 2;
    }
    out->unreserved_pages = pages;
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
}

/* Mark one page as allocated in a raw bit-file image.
 *
 * The addressing MUST match ndfs_bf_* in bit_file.c: page N is bit N%16 of the 16-bit
 * big-endian word N/16, which on a byte array means byte (N>>3)^1, bit N&7. See the
 * comment above bf_byte_index() for the SINTRAN manual reference.
 *
 * This used to be six inline copies of "bm[b/8] |= 1u << (b%8)". When the bit order was
 * corrected, copies like that are exactly what gets left behind - and a freshly created
 * image would then have its reserved pages marked in one convention and read back in the
 * other, so the allocator would hand out the object file's own index page to the first
 * file written. One function, one place to be wrong.
 */
static void mark_page_used(uint8_t *bitmap, uint32_t page)
{
    bitmap[(page >> 3) ^ 1u] |= (uint8_t)(1u << (page & 7u));
}

ndfs_error_t ndfs_create_image(ndfs_filesystem_t **out_fs,
                               const ndfs_image_options_t *options)
{
    const ndfs_template_spec_t *spec;
    ndfs_template_spec_t custom_spec;
    uint8_t *img;
    size_t img_size;
    ndfs_master_block_t mb;
    ndfs_user_entry_t ue;
    ndfs_block_pointer_t bp;
    uint32_t bitmap_bytes, bitmap_pages;
    uint32_t i;
    size_t b;
    ndfs_error_t err;

    if (!out_fs || !options) return NDFS_ERR_NULL_PTR;

    /* Get template spec */
    if (options->template_type == NDFS_TMPL_CUSTOM) {
        if (options->custom_pages < 32) return NDFS_ERR_TOO_SMALL;
        build_custom_spec(options->custom_pages, &custom_spec);
        spec = &custom_spec;
    } else {
        spec = get_spec(options->template_type);
        if (!spec) return NDFS_ERR_INVALID_ARG;
    }

    /* Allocate image buffer */
    img_size = (size_t)spec->file_blocks * NDFS_PAGE_SIZE;
    img = (uint8_t *)calloc(img_size, 1);
    if (!img) return NDFS_ERR_ALLOC;

    /* ---- Page 0: Master block ---- */
    memset(&mb, 0, sizeof(mb));
    {
        size_t nlen = strlen(options->directory_name);
        if (nlen > NDFS_NAME_MAX) nlen = NDFS_NAME_MAX;
        memcpy(mb.directory_name, options->directory_name, nlen);
        mb.directory_name[nlen] = '\0';
        str_toupper_ic(mb.directory_name);
    }

    mb.object_file_ptr.block_id = spec->object_file_block;
    mb.object_file_ptr.type     = NDFS_PTR_INDEXED;
    mb.user_file_ptr.block_id   = spec->user_file_block;
    mb.user_file_ptr.type       = NDFS_PTR_INDEXED;
    mb.bit_file_ptr.block_id    = spec->bit_file_block;
    mb.bit_file_ptr.type        = NDFS_PTR_CONTIGUOUS;
    mb.unreserved_pages         = spec->unreserved_pages;
    mb.image_size               = spec->ndfs_pages;
    mb.has_flomon               = spec->has_flomon;

    ndfs_mb_write(&mb, img);

    /* Write extended info if requested or if spec says so */
    if (options->include_extended_info || spec->ext_valid) {
        mb.ext_valid = true;
        mb.ext_flag_word = options->flag_word;
        mb.ext_last_system_number = options->system_number;
        mb.ext_pages_available = spec->ndfs_pages;
        ndfs_mb_write_extended(&mb, img);
    }

    /* ---- User file index block ---- */
    /* The user file index block contains up to 8 pointers.
       First pointer -> user data page (user_file_block + 1) */
    {
        uint32_t user_data_page = spec->user_file_block + 1;
        bp.block_id = user_data_page;
        bp.type = NDFS_PTR_CONTIGUOUS;
        ndfs_bp_to_bytes(&bp, img + (size_t)spec->user_file_block * NDFS_PAGE_SIZE, 0);

        /* ---- User data page: SYSTEM user ---- */
        ndfs_ue_init(&ue);
        strcpy(ue.user_name, "SYSTEM");
        ue.user_index = 0;
        ue.pages_reserved = spec->ndfs_pages > 1000 ? spec->ndfs_pages / 2 : 500;
        ue.pages_used = 0;
        ndfs_ue_to_bytes(&ue, img + (size_t)user_data_page * NDFS_PAGE_SIZE);
    }

    /* ---- Object file index block ---- */
    /* First pointer -> object data page (object_file_block + 1) */
    {
        uint32_t obj_data_page = spec->object_file_block + 1;
        bp.block_id = obj_data_page;
        bp.type = NDFS_PTR_CONTIGUOUS;
        ndfs_bp_to_bytes(&bp, img + (size_t)spec->object_file_block * NDFS_PAGE_SIZE, 0);
        /* Object data page is all zeros (no files yet) - already calloc'd */
    }

    /* ---- Bitmap ---- */
    bitmap_bytes = (((spec->ndfs_pages + 7) / 8) + 1) & ~1u;
    bitmap_pages = (bitmap_bytes + NDFS_PAGE_SIZE - 1) / NDFS_PAGE_SIZE;

    /* Mark system pages as used in the bitmap.
       System pages: 0 (master), user file index, user data page,
       object file index, object data page, bitmap pages */
    {
        uint8_t *bm = img + (size_t)spec->bit_file_block * NDFS_PAGE_SIZE;

        /* Mark page 0 */
        mark_page_used(bm, 0);

        /* Mark user file index block and its data page */
        b = spec->user_file_block;
        mark_page_used(bm, b);
        b = spec->user_file_block + 1;
        if (b < spec->ndfs_pages) {
            mark_page_used(bm, b);
        }

        /* Mark object file index block and its data page */
        b = spec->object_file_block;
        mark_page_used(bm, b);
        b = spec->object_file_block + 1;
        if (b < spec->ndfs_pages) {
            mark_page_used(bm, b);
        }

        /* Mark bitmap pages */
        for (i = 0; i < bitmap_pages; i++) {
            b = spec->bit_file_block + i;
            if (b < spec->ndfs_pages) {
                mark_page_used(bm, b);
            }
        }
    }

    /* ---- Open the image as a filesystem ---- */
    err = ndfs_open_buffer_copy(img, img_size, false, out_fs);
    free(img);

    return err;
}
