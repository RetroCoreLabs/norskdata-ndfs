/**
 * ndtool: list, export and import SINTRAN segments.
 *
 *   --segments                    the segment table with names, bases, lengths and disk pages
 *   --export-segment NAME         NAME.seg and NAME.seg.txt in the -o directory (default ".")
 *   --import-segment NAME=FILE    write FILE over the segment's pages (same length or shorter)
 *
 * NAME is the RT-loader name (S3FS, S3SDPIT) or the number: octal with a trailing B
 * (53B) or decimal. Stop the emulator first: the image is patched in place.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ndtool.h"
#include <ndfs/sintran_segments.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_segfiles(const ndfs_segment_list_t *l)
{
    int n;
    printf("Segment files (from %s):\n", l->have_symbols ? "DBLST / ABLPAGE / MASSNO in S3IDPIT, symbols BLST/ABLPA/MASSN"
                                                          : "the SEGFIL0:DATA file only, no symbol list");
    for (n = 0; n < NDFS_SEGFILE_COUNT; n++) {
        const ndfs_segfile_t *sf = &l->segfiles[n];
        if (!sf->defined) {
            printf("  %d: not defined\n", n);
            continue;
        }
        printf("  %d: disk page %ld", n, sf->base_page);
        if (sf->blocks_per_page > 0)
            printf(" (DBLST %u blocks, %d blocks per page)", (unsigned)sf->dblst, sf->blocks_per_page);
        if (sf->file_path[0]) printf(", file %s", sf->file_path);
        if (!sf->on_this_disk) printf(", ON ANOTHER DISK UNIT (MASSNO %o), not reachable here", (unsigned)sf->massno);
        printf(" [%s]\n", sf->source);
    }
}

int cmd_segments(ndtool_ctx_t *ctx)
{
    ndfs_segment_list_t *l = (ndfs_segment_list_t *)malloc(sizeof(*l));
    ndfs_error_t err;
    int i;

    if (!l) return -1;
    err = ndfs_list_segments(ctx->fs, l);
    if (err != NDFS_OK) {
        fprintf(stderr, "No SINTRAN segment table found (%s)\n", ndfs_strerror(err));
        free(l);
        return -1;
    }
    printf("SINTRAN version %c, segment table at page %d of %s, names from %s\n",
           l->version, l->segment_table_page, l->segfil_path,
           l->names_path[0] ? l->names_path : "(none found)");
    print_segfiles(l);
    printf("\n seg  name      base   pages  sf  MADR   disk page  flags\n");
    for (i = 0; i < l->count; i++) {
        const ndfs_segment_t *s = &l->segments[i];
        printf(" %03o  %-8s  %06o %5d  %d  %5d  ", (unsigned)s->number, s->name[0] ? s->name : "-",
               (unsigned)((s->logad & 077) * 02000), s->segle, s->segfile, s->madr);
        if (s->memory_only) printf("%-10s", "memory");
        else if (s->disk_page < 0) printf("%-10s", "?");
        else printf("%-10ld", s->disk_page);
        printf("%s%s", s->ok5 ? " 5OK" : "", (s->flag & 0x1FFE) ? "" : "");
        if (ctx->verbose)
            printf("  SEGLI %06o PRESE %06o LOGAD %06o FLAG %06o SGSTA %06o BPAGL %06o",
                   (unsigned)s->segli, (unsigned)s->prese, (unsigned)s->logad,
                   (unsigned)s->flag, (unsigned)s->sgsta, (unsigned)s->bpagl);
        printf("\n");
    }
    printf("%d segments. 'base' is the logical address, 'disk page' the first page of the\n"
           "segment on this image (base of its segment file + MADR); memory = no disk copy.\n", l->count);
    free(l);
    return 0;
}

/* Host file names for a segment: NAME.seg and NAME.seg.txt in the output directory. */
static void seg_file_names(const ndtool_ctx_t *ctx, const ndfs_segment_t *s,
                           char *bin, size_t bin_sz, char *txt, size_t txt_sz)
{
    const char *dir = ctx->output_dir ? ctx->output_dir : ".";
    char base[32];
    if (s->name[0]) snprintf(base, sizeof(base), "%s", s->name);
    else snprintf(base, sizeof(base), "SEG%03o", (unsigned)s->number);
    snprintf(bin, bin_sz, "%s/%s.seg", dir, base);
    snprintf(txt, txt_sz, "%s/%s.seg.txt", dir, base);
}

int cmd_export_segment(ndtool_ctx_t *ctx, const char *which)
{
    ndfs_segment_list_t *l = (ndfs_segment_list_t *)malloc(sizeof(*l));
    const ndfs_segment_t *s;
    uint8_t *data = NULL; size_t size = 0;
    char bin[1024], txt[1024];
    ndfs_error_t err;
    int idx;
    FILE *f;

    if (!l) return -1;
    err = ndfs_list_segments(ctx->fs, l);
    if (err != NDFS_OK) { fprintf(stderr, "No SINTRAN segment table found\n"); free(l); return -1; }
    idx = ndfs_find_segment(l, which);
    if (idx < 0) { fprintf(stderr, "No segment named or numbered '%s' (see --segments)\n", which); free(l); return -1; }
    s = &l->segments[idx];

    err = ndfs_export_segment(ctx->fs, l, idx, &data, &size);
    if (err == NDFS_ERR_NOT_FOUND) {
        fprintf(stderr, "Segment %03o %s has no disk copy here (%s)\n", (unsigned)s->number, s->name,
                s->memory_only ? "memory only" : "its segment file is undefined or on another disk");
        free(l); return -1;
    }
    if (err != NDFS_OK) { fprintf(stderr, "Export failed: %s\n", ndfs_strerror(err)); free(l); return -1; }

    seg_file_names(ctx, s, bin, sizeof(bin), txt, sizeof(txt));
    if (ndtool_write_local_file(bin, data, size) != 0) { ndfs_free_data(data); free(l); return -1; }
    f = fopen(txt, "w");
    if (f) {
        fprintf(f, "segment %03o %s\nsintran %c\nlogical-base %06o\npages %d\nsegment-file %d\nmadr %d\n"
                   "disk-page %ld\nflag %06o\nsgsta %06o\nbytes %zu\n"
                   "note the file is the segment's pages in order; load at the logical base\n",
                (unsigned)s->number, s->name, l->version, (unsigned)((s->logad & 077) * 02000), s->segle,
                s->segfile, s->madr, s->disk_page, (unsigned)s->flag, (unsigned)s->sgsta, size);
        fclose(f);
    }
    printf("Exported segment %03o %s: %zu bytes (%d pages from disk page %ld) -> %s (+ .txt)\n",
           (unsigned)s->number, s->name, size, s->segle, s->disk_page, bin);
    ndfs_free_data(data);
    free(l);
    return 0;
}

int cmd_import_segment(ndtool_ctx_t *ctx, const char *spec)
{
    ndfs_segment_list_t *l;
    const ndfs_segment_t *s;
    char which[64];
    const char *eq, *file;
    uint8_t *data; size_t size;
    ndfs_error_t err;
    int idx;

    eq = spec ? strchr(spec, '=') : NULL;
    if (!eq || eq == spec || !eq[1]) {
        fprintf(stderr, "Usage: --import-segment NAME=FILE\n");
        return -1;
    }
    if ((size_t)(eq - spec) >= sizeof(which)) { fprintf(stderr, "Segment name too long\n"); return -1; }
    memcpy(which, spec, (size_t)(eq - spec));
    which[eq - spec] = '\0';
    file = eq + 1;

    l = (ndfs_segment_list_t *)malloc(sizeof(*l));
    if (!l) return -1;
    err = ndfs_list_segments(ctx->fs, l);
    if (err != NDFS_OK) { fprintf(stderr, "No SINTRAN segment table found\n"); free(l); return -1; }
    idx = ndfs_find_segment(l, which);
    if (idx < 0) { fprintf(stderr, "No segment named or numbered '%s' (see --segments)\n", which); free(l); return -1; }
    s = &l->segments[idx];

    data = ndtool_read_local_file(file, &size);
    if (!data) { fprintf(stderr, "Cannot read %s\n", file); free(l); return -1; }
    if (size > (size_t)s->segle * 2048) {
        fprintf(stderr, "%s is %zu bytes; segment %03o %s holds %d pages = %d bytes\n",
                file, size, (unsigned)s->number, s->name, s->segle, s->segle * 2048);
        free(data); free(l); return -1;
    }
    if (s->memory_only || s->disk_page < 0) {
        fprintf(stderr, "Segment %03o %s has no disk copy here\n", (unsigned)s->number, s->name);
        free(data); free(l); return -1;
    }

    printf("Import %zu bytes from %s over segment %03o %s: %d pages from disk page %ld (segment file %d%s%s)",
           size, file, (unsigned)s->number, s->name, (int)((size + 2047) / 2048), s->disk_page, s->segfile,
           l->segfiles[s->segfile].file_path[0] ? ", " : "", l->segfiles[s->segfile].file_path);
    if (s->name[0] == 'S' && s->name[1] == '3' && s->name[2] == 'I')
        printf("\nNOTE: this looks like an image copy; a cold start replaces it from the save copy");
    if (ctx->dry_run) { printf(" [dry run]\n"); free(data); free(l); return 0; }
    printf("\n");

    err = ndfs_import_segment(ctx->fs, l, idx, data, size);
    free(data);
    if (err != NDFS_OK) { fprintf(stderr, "Import failed: %s\n", ndfs_strerror(err)); free(l); return -1; }
    if (ndtool_save_image(ctx) != 0) { free(l); return -1; }
    ctx->modified = false;
    printf("Done.\n");
    free(l);
    return 0;
}
