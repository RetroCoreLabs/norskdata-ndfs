/**
 * ndtool: show and change the CPU number and CPU type of a SINTRAN system pack.
 *
 *   --cpu-identity            print SYSNO / FCPUN / CPU type / patch level from both
 *                             disk copies of the resident data page, the generation
 *                             symbols 33CPN / 33CPU, and the directory stamp
 *   --set-cpu NUMBER[:TYPE]   write NUMBER into SYSNO and FCPUN of both copies (and
 *                             TYPE into HWINF+2), update 33CPN / 33CPU, clear the
 *                             "entered by system N" stamp unless --keep-stamp
 *
 * Replaces the DMAC session ")CLOAD S3IDPIT ... SYSNO/old new ..." on a running
 * system. Stop the emulator first: the image is patched in place.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ndtool.h"
#include <ndfs/sintran_identity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static void print_value(const char *label, int value)
{
    if (value < 0)
        printf("  %-14s -\n", label);
    else
        printf("  %-14s %6d  (%06o octal)\n", label, value, (unsigned)value);
}

static void print_copy(const ndfs_dpit_copy_t *c)
{
    if (c->segment_number < 0) {
        printf("Segment %s (%s copy): no name record on the pack\n", c->name, c->role);
        return;
    }
    printf("Segment %02o %s (%s copy)\n", (unsigned)c->segment_number, c->name, c->role);
    if (!c->present) {
        printf("  no usable segment-table entry (LOGAD %06o SEGLE %d MADR %d FLAG %06o)\n",
               (unsigned)c->logad, c->segle, c->madr, (unsigned)c->flag);
        return;
    }
    printf("  segment file %d, %s, page %d, %d pages\n", c->segment_file,
           c->file_path[0] ? c->file_path : "(file for this segment file number not known)",
           c->madr, c->segle);
    if (!c->file_found) {
        printf("  the file is missing or does not reach page %d\n", c->madr);
        return;
    }
    print_value("CPU number", c->sysno);
    print_value("FCPUN copy", c->fcpun);
    print_value("CPU type", c->cpu_type);
    print_value("patch level", c->revle);
    if (c->sysno_offset < 0 || c->fcpun_offset < 0 || c->type_offset < 0)
        printf("  a word shown as - lies outside this segment or past the end of its file\n");
}

static void print_identity(const ndfs_cpu_identity_t *id)
{
    printf("SINTRAN version: %c (from INIBU %06o)\n", id->version,
           id->sym.inibu < 0 ? 0u : (unsigned)id->sym.inibu);
    printf("Symbols from %s and %s:\n", id->symbol1_path, id->symbol2_path);
    printf("  SYSNO=%06o HWINF=%06o REVLE=%06o FCPUN=%06o\n",
           (unsigned)id->sym.sysno, (unsigned)id->sym.hwinf, (unsigned)id->sym.revle,
           (unsigned)id->sym.fcpun);
    printf("Segment names from %s\n", id->names_path[0] ? id->names_path : "(not found)");
    print_value("33CPN", id->sym.cpn33);
    print_value("33CPU", id->sym.cpu33);
    print_copy(&id->copies[0]);
    print_copy(&id->copies[1]);
    printf("Directory stamp (page 0): ");
    if (!id->dir_ext_valid)
        printf("no valid Extended Info Block\n");
    else
        printf("%s, system number %u (%o octal)\n",
               id->dir_entered ? "entered" : "not entered",
               (unsigned)id->dir_system_number, (unsigned)id->dir_system_number);

    /* Say what does not agree. */
    if (id->copies[0].file_found && id->copies[1].file_found) {
        if (id->copies[0].sysno != id->copies[1].sysno || id->copies[0].cpu_type != id->copies[1].cpu_type)
            printf("NOTE: the image copy and the save copy differ; a cold start uses the save copy\n");
    }
    if (id->copies[0].file_found && id->copies[0].fcpun >= 0 && id->copies[0].sysno != id->copies[0].fcpun)
        printf("NOTE: SYSNO and FCPUN differ in the image copy\n");
    if (id->dir_ext_valid && id->dir_entered && id->dir_system_number != 0 &&
        id->copies[0].file_found && id->copies[0].sysno >= 0 &&
        (int)id->dir_system_number != id->copies[0].sysno)
        printf("NOTE: the directory was entered by system %u, not by this pack's CPU number %d:\n"
               "      SINTRAN refuses the enter with 3203 DIRECTORY ENTERED BY ANOTHER SYSTEM\n",
               (unsigned)id->dir_system_number, id->copies[0].sysno);
}

int cmd_cpu_identity(ndtool_ctx_t *ctx)
{
    ndfs_cpu_identity_t id;
    ndfs_error_t err = ndfs_read_cpu_identity(ctx->fs, &id);
    if (err == NDFS_ERR_NOT_FOUND) {
        if (id.symbol1_path[0] == '\0' || id.symbol2_path[0] == '\0')
            fprintf(stderr, "Not a SINTRAN system pack: no (SYSTEM)SYMBOL-1-LIST:SYMB / SYMBOL-2-LIST:SYMB\n");
        else if (id.sym.sysno < 0 || id.sym.fcpun < 0)
            fprintf(stderr, "The symbol lists do not define SYSNO and FCPUN; cannot address the words\n");
        else if (id.segment_table_page < 0)
            fprintf(stderr, "No SEGFIL0:DATA with a segment table found\n");
        else
            fprintf(stderr, "No RT-loader name records for S3IDPIT / S3SDPIT found (RTFIL:DATA or SEGFIL0)\n");
        return -1;
    }
    if (err != NDFS_OK) {
        fprintf(stderr, "Error reading the CPU identity: %s\n", ndfs_strerror(err));
        return -1;
    }
    print_identity(&id);
    return 0;
}

/* Parse "NUMBER[:TYPE]"; numbers decimal, or octal with a trailing B (as ND writes them). */
static bool parse_nd_number(const char *s, size_t len, int *out)
{
    long v = 0;
    size_t i;
    int base = 10;
    if (len == 0) return false;
    if (s[len - 1] == 'B' || s[len - 1] == 'b') { base = 8; len--; }
    if (len == 0) return false;
    for (i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > (base == 8 ? '7' : '9')) return false;
        v = v * base + (s[i] - '0');
        if (v > 0xFFFF) return false;
    }
    *out = (int)v;
    return true;
}

int cmd_set_cpu(ndtool_ctx_t *ctx, const char *spec, bool keep_stamp)
{
    int number = -1, type = -1;
    const char *colon;
    ndfs_cpu_identity_t before, after;
    ndfs_error_t err;

    if (!spec || !*spec) {
        fprintf(stderr, "Usage: --set-cpu NUMBER[:TYPE]   (decimal, or octal with a trailing B)\n");
        return -1;
    }
    colon = strchr(spec, ':');
    if (!parse_nd_number(spec, colon ? (size_t)(colon - spec) : strlen(spec), &number) ||
        (colon && !parse_nd_number(colon + 1, strlen(colon + 1), &type))) {
        fprintf(stderr, "Bad --set-cpu value '%s': want NUMBER[:TYPE], 0..65535 each\n", spec);
        return -1;
    }

    err = ndfs_read_cpu_identity(ctx->fs, &before);
    if (err != NDFS_OK) {
        fprintf(stderr, "Cannot read the CPU identity first: %s\n", ndfs_strerror(err));
        return -1;
    }
    if (!before.copies[0].present || !before.copies[1].present ||
        !before.copies[0].file_found || !before.copies[1].file_found) {
        print_identity(&before);
        fprintf(stderr, "Refusing: both disk copies of the resident data page must be reachable\n");
        return -1;
    }

    printf("CPU number %d -> %d", before.copies[0].sysno, number);
    if (type >= 0) printf(", CPU type %d -> %d", before.copies[0].cpu_type, type);
    printf(", in %s page %d and %s page %d", before.copies[0].file_path, before.copies[0].madr,
           before.copies[1].file_path, before.copies[1].madr);
    if (before.cpn33_value_offset >= 0) printf(", 33CPN in %s", before.symbol2_path);
    if (!keep_stamp && (before.dir_entered || before.dir_system_number != 0))
        printf(", directory stamp (system %u) cleared", (unsigned)before.dir_system_number);
    if (ctx->dry_run) {
        printf(" [dry run]\n");
        return 0;
    }
    printf("\n");

    err = ndfs_write_cpu_identity(ctx->fs, number, type, keep_stamp, &after);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error changing the CPU identity: %s\n", ndfs_strerror(err));
        return -1;
    }
    if (ndtool_save_image(ctx) != 0) return -1;
    ctx->modified = false;

    print_identity(&after);
    printf("Done. The pack now reports CPU NUMBER %d%s at the next boot.\n", number,
           type >= 0 ? " and the new CPU TYPE" : "");
    printf("On an ND-110 or ND-120 with a programmed identification PROM, SINTRAN L and later take\n"
           "the number and type from the PROM instead; the disk values still decide directory ownership.\n");
    return 0;
}
