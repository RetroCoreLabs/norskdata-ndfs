/**
 * ndtool: create new NDFS disk image.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ndtool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cmd_create(ndtool_ctx_t *ctx, const char *template_name,
               uint32_t custom_pages, const char *dir_name)
{
    ndfs_image_options_t opts;
    ndfs_image_template_t tmpl;
    ndfs_error_t err;
    uint32_t disk_type_pages = 0;
    uint32_t disk_layout_pages = 0;
    uint8_t boot_bytes[2000];
    ndfs_disk_family_t disk_family = NDFS_DISK_FAMILY_UNKNOWN;

    /* "list": print every SINTRAN disk type this tool knows, with family, drive and test status */
    if (strcmp(template_name, "list") == 0) {
        size_t k;
        printf("%-16s %9s %9s %9s  %-10s %-6s %s\n", "NAME", "DIR PAGES", "IMG PAGES", "IMG MB", "FAMILY", "TESTED", "DRIVE (SINTRAN manual)");
        for (k = 0; k < ndfs_disk_type_count(); k++) {
            const ndfs_disk_type_t *t = ndfs_disk_type_get(k);
            const char *fam = t->family == NDFS_DISK_FAMILY_WINCHESTER ? "Winchester"
                            : t->family == NDFS_DISK_FAMILY_SMD ? "SMD" : "unknown";
            if (t->pages) {
                uint32_t img = t->layout_pages > t->pages ? t->layout_pages : t->pages;
                printf("%-16s %9u %9u %9.1f  %-10s %-6s %s\n", t->name, (unsigned)t->pages, (unsigned)img,
                       (double)img * 2048.0 / 1048576.0, fam, t->tested ? "yes" : "no", t->drive);
            } else
                printf("%-16s %9s %9s %9s  %-10s %-6s %s\n", t->name, "?", "?", "?", fam, "no", t->drive);
        }
        printf("\nDIR PAGES = the directory size SINTRAN gives the type (its declared capacity).\n");
        printf("IMG PAGES = the length of the image file ndtool makes. Where the SINTRAN disk layout\n");
        printf("            is known it is the full layout (the emulator's disk controllers need it);\n");
        printf("            otherwise it equals DIR PAGES. IMG MB = IMG PAGES * 2048 bytes / 1048576.\n");
        printf("TESTED yes = real SINTRAN M created a directory on this type, or entered an ndtool image of it\n               (ENTER-DIRECTORY, DIR, CREATE-FILE), in the emulator.\n");
        printf("PAGES ? = valid name, size not known: cannot be created.\n");
        printf("Other sizes: floppy360, floppy12, smd75, winchester74, scsi126, custom --pages N.\n");
        return 0;
    }

    /* Map template name to enum */
    if (strcmp(template_name, "floppy360") == 0) {
        tmpl = NDFS_TMPL_FLOPPY_360KB;
    } else if (strcmp(template_name, "floppy12") == 0) {
        tmpl = NDFS_TMPL_FLOPPY_12MB;
    } else if (strcmp(template_name, "smd75") == 0) {
        tmpl = NDFS_TMPL_SMD_75MB;
    } else if (strcmp(template_name, "winchester74") == 0) {
        tmpl = NDFS_TMPL_WINCHESTER_74MB;
    } else if (strcmp(template_name, "scsi126") == 0) {
        tmpl = NDFS_TMPL_SCSI_126MB;
    } else if (strcmp(template_name, "custom") == 0) {
        tmpl = NDFS_TMPL_CUSTOM;
        if (custom_pages == 0) {
            fprintf(stderr, "Error: --pages N required for custom template\n");
            return -1;
        }
    } else if (ndfs_disk_type_find(template_name, &disk_type_pages) == NDFS_OK) {
        /* A SINTRAN disk type name (DISC-75MB, DISC-288MB, ...): the image is exactly the
         * directory size SINTRAN gives that type. No spare area is added; the spare size
         * per type is not known. */
        if (disk_type_pages == 0) {
            fprintf(stderr, "Error: '%s' is a valid SINTRAN disk type name but its size in pages is not known, so no image can be made.\n", template_name);
            fprintf(stderr, "Use --pages N with the custom template, or see: ndtool --create list\n");
            return -1;
        }
        tmpl = NDFS_TMPL_CUSTOM;
        custom_pages = disk_type_pages;
        disk_family = ndfs_disk_type_by_name(template_name)->family;
        disk_layout_pages = ndfs_disk_type_by_name(template_name)->layout_pages;
    } else {
        fprintf(stderr, "Error: unknown template '%s'\n", template_name);
        fprintf(stderr, "Valid templates: floppy360, floppy12, smd75, winchester74, scsi126, custom\n");
        fprintf(stderr, "SINTRAN disk type names: see ndtool --create list\n");
        return -1;
    }

    ndfs_image_options_init(&opts);
    opts.template_type = tmpl;

    if (dir_name) {
        strncpy(opts.directory_name, dir_name, NDFS_NAME_MAX);
        opts.directory_name[NDFS_NAME_MAX] = '\0';
    }

    if (custom_pages) {
        opts.custom_pages = custom_pages;
    }
    /* The image is as long as the SINTRAN layout of the type, not just the directory size:
     * SINTRAN reads and writes above the directory size, and the emulator's disk controllers
     * pick their geometry from the file length. */
    opts.device_pages = disk_layout_pages;

    /* Floppies have no Extended Info Block (measured with real SINTRAN): not the floppy templates,
     * and not a custom image of floppy size. Hard disks always have one. */
    if (tmpl == NDFS_TMPL_FLOPPY_360KB || tmpl == NDFS_TMPL_FLOPPY_12MB ||
        (tmpl == NDFS_TMPL_CUSTOM && custom_pages <= 1000)) {
        opts.include_extended_info = false;
    } else {
        opts.include_extended_info = true;
    }

    /* SINTRAN's "OCTAL BIT FILE ADDRESS (-1 END OF DISC)" prompt */
    if (ctx->create_bit_file) {
        const char *a = ctx->create_bit_file;
        char *end = NULL;
        if (strcmp(a, "-1") == 0) {
            opts.bit_file_answer = -1;
        } else {
            long v = strtol(a, &end, 8);
            if (*a == '\0' || *end != '\0' || v < 0) {
                fprintf(stderr, "Error: --bit-file must be 0, -1 or an octal page address\n");
                return -1;
            }
            opts.bit_file_answer = (int32_t)v;
        }
    }

    /* Boot area. A named SINTRAN disk type gets the boot area of its controller family. */
    if (disk_family == NDFS_DISK_FAMILY_SMD) opts.boot_choice = NDFS_BOOTCODE_SMD;
    else if (disk_family == NDFS_DISK_FAMILY_WINCHESTER) opts.boot_choice = NDFS_BOOTCODE_WINCHESTER;
    if (ctx->create_boot) {
        const char *b = ctx->create_boot;
        if      (strcmp(b, "auto") == 0)       opts.boot_choice = NDFS_BOOTCODE_AUTO;
        else if (strcmp(b, "none") == 0)       opts.boot_choice = NDFS_BOOTCODE_NONE;
        else if (strcmp(b, "smd") == 0)        opts.boot_choice = NDFS_BOOTCODE_SMD;
        else if (strcmp(b, "winchester") == 0) opts.boot_choice = NDFS_BOOTCODE_WINCHESTER;
        else if (strcmp(b, "scsi") == 0)       opts.boot_choice = NDFS_BOOTCODE_SCSI;
        else if (strcmp(b, "floppy360") == 0)  opts.boot_choice = NDFS_BOOTCODE_FLOPPY360;
        else if (strcmp(b, "floppy616") == 0)  opts.boot_choice = NDFS_BOOTCODE_FLOPPY616;
        else {
            fprintf(stderr, "Error: unknown --boot '%s' (auto, none, smd, winchester, scsi, floppy360, floppy616)\n", b);
            return -1;
        }
    }
    {
        const char *src = ctx->create_boot_from ? ctx->create_boot_from : ctx->create_boot_file;
        if (src) {
            FILE *bf = fopen(src, "rb");
            size_t got;
            if (!bf) {
                fprintf(stderr, "Error: cannot open boot source '%s'\n", src);
                return -1;
            }
            got = fread(boot_bytes, 1, sizeof(boot_bytes), bf);
            fclose(bf);
            if (got == 0 || (ctx->create_boot_from && got < sizeof(boot_bytes))) {
                fprintf(stderr, "Error: boot source '%s' is too short (need %u bytes from an image)\n", src, (unsigned)sizeof(boot_bytes));
                return -1;
            }
            opts.boot_data = boot_bytes;
            opts.boot_size = got;
        }
    }

    if (ctx->dry_run) {
        printf("Would create %s image: %s [dry run]\n", template_name, ctx->image_path);
        return 0;
    }

    err = ndfs_create_image(&ctx->fs, &opts);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error creating image: %s\n", ndfs_strerror(err));
        return -1;
    }

    if (ndtool_save_image(ctx) != 0) {
        ndfs_close(ctx->fs);
        ctx->fs = NULL;
        return -1;
    }

    printf("Created %s image: %s\n", template_name, ctx->image_path);
    ndfs_close(ctx->fs);
    ctx->fs = NULL;
    return 0;
}
