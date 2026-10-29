/**
 * SINTRAN III segments on a system pack: list, export, import.
 *
 * See include/ndfs/sintran_segments.h for the model and the sources.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/sintran_segments.h>
#include <ndfs/sintran.h>
#include <ndfs/parity.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define PAGE_BYTES        2048
#define SEG_ENTRY_BYTES   16
#define NAME_RECORD_BYTES 16

#define INIBU_K 0x6E7A
#define INIBU_L 0x7853
#define INIBU_M 0x84D7

static int word_be(const uint8_t *b, size_t o) { return (b[o] << 8) | b[o + 1]; }

/* ── Names ────────────────────────────────────────────────────────────── */

int ndfs_sintran_collect_segment_names(const uint8_t *buf, size_t size,
                                       char names[256][NDFS_SEGNAME_MAX + 1])
{
    size_t o;
    int found = 0;
    char decoded[NDFS_SEGNAME_MAX + 1];

    if (!buf || !names || size < NAME_RECORD_BYTES) return 0;
    for (o = 0; o + NAME_RECORD_BYTES <= size; o += 2) {
        int segno;
        if (word_be(buf, o + 10) != NDFS_RTFIL_TYPE_SEGMENT_NAME) continue;
        if (word_be(buf, o + 8) != 0 || word_be(buf, o + 14) != 0) continue;
        segno = word_be(buf, o + 6);
        if (segno == 0 || segno > 0377 || segno != word_be(buf, o + 12)) continue;
        if (!ndfs_sintran_decode_packed_name((uint16_t)word_be(buf, o), (uint16_t)word_be(buf, o + 2),
                                             (uint16_t)word_be(buf, o + 4), decoded, sizeof(decoded)))
            continue;
        if (names[segno][0] == '\0') {
            strcpy(names[segno], decoded);
            found++;
        }
    }
    return found;
}

/* ── Symbols ──────────────────────────────────────────────────────────── */

static bool read_symbol(const ndfs_filesystem_t *fs, const char *path, const char *name, int *out)
{
    uint8_t *raw; size_t len;
    bool ok;
    if (ndfs_read_file(fs, path, &raw, &len) != NDFS_OK) return false;
    ndfs_strip_parity(raw, len);
    ok = ndfs_sintran_find_symbol(raw, len, name, out, NULL, NULL);
    ndfs_free_data(raw);
    return ok;
}

static char version_from_inibu(int inibu)
{
    switch (inibu) {
        case INIBU_K: return 'K';
        case INIBU_L: return 'L';
        case INIBU_M: return 'M';
        default:      return '?';
    }
}

/* ── Segment table ────────────────────────────────────────────────────── */

static void read_entry(const uint8_t *seg, int page, int n, ndfs_segment_t *s)
{
    size_t o = (size_t)page * PAGE_BYTES + (size_t)n * SEG_ENTRY_BYTES;
    s->number = n;
    s->segli = word_be(seg, o + 0);
    s->prese = word_be(seg, o + 2);
    s->logad = word_be(seg, o + 4);
    s->segle = word_be(seg, o + 6) & 0x3FF;
    s->madr  = word_be(seg, o + 8);
    s->flag  = word_be(seg, o + 10);
    s->sgsta = word_be(seg, o + 12);
    s->bpagl = word_be(seg, o + 14);
    s->segfile = (s->flag >> 13) & 7;
    s->ok5 = (s->flag & 1) != 0;
    s->memory_only = (s->segle == 0);   /* no pages: S3DPIT, S3RPIT ... MADR 0 is a real offset (S3IMAGE, S3SAVE) */
    s->disk_page = -1;
}

/* The NDFS file whose first disk page is `page`, for labelling only. */
static void label_file_at_page(const ndfs_filesystem_t *fs, long page, char *path, size_t path_sz)
{
    ndfs_object_entry_t *objs;
    size_t count, i;
    path[0] = '\0';
    if (page < 0) return;
    if (ndfs_get_object_entries(fs, &objs, &count) != NDFS_OK) return;
    for (i = 0; i < count; i++) {
        uint32_t *blocks; size_t nb;
        char type[NDFS_TYPE_MAX + 1];
        size_t t;
        char candidate[NDFS_IDENTITY_PATH_MAX];
        if (objs[i].pages_in_file == 0) continue;
        strncpy(type, objs[i].type, sizeof(type) - 1);
        type[sizeof(type) - 1] = '\0';
        t = strlen(type);
        while (t > 0 && (type[t - 1] == 0x27 || type[t - 1] == ' ')) type[--t] = '\0';
        snprintf(candidate, sizeof(candidate), "%s/%s:%s", objs[i].user_name, objs[i].object_name, type);
        if (ndfs_get_file_blocks(fs, candidate, &blocks, &nb) != NDFS_OK) continue;
        if (nb > 0 && blocks[0] == (uint32_t)page) {
            strncpy(path, candidate, path_sz - 1);
            path[path_sz - 1] = '\0';
            free(blocks);
            break;
        }
        free(blocks);
    }
    ndfs_free_object_entries(objs);
}

/* Segment-file bases from the kernel tables inside the S3IDPIT copy. */
static void read_segfile_tables(const ndfs_filesystem_t *fs, const uint8_t *seg, size_t seg_size,
                                ndfs_segment_list_t *out, int idpit_index)
{
    int n;
    const ndfs_segment_t *dp = idpit_index >= 0 ? &out->segments[idpit_index] : NULL;
    long dpit_off = -1;
    int ablpage0 = 0, massno0 = 0;

    for (n = 0; n < NDFS_SEGFILE_COUNT; n++) {
        out->segfiles[n].defined = false;
        out->segfiles[n].base_page = -1;
        out->segfiles[n].source = "unknown";
        out->segfiles[n].file_path[0] = '\0';
        out->segfiles[n].on_this_disk = true;
    }

    /* Where the DPIT image copy is inside SEGFIL0 (segment file 0 by construction:
     * the table and the DPIT image are both in SEGFIL0:DATA). */
    if (dp && !dp->memory_only && out->have_symbols &&
        (size_t)(dp->madr + dp->segle) * PAGE_BYTES <= seg_size) {
        dpit_off = (long)dp->madr * PAGE_BYTES;
    }

    if (dpit_off >= 0) {
        int base = (dp->logad & 077) * 1024;
        for (n = 0; n < NDFS_SEGFILE_COUNT; n++) {
            ndfs_segfile_t *sf = &out->segfiles[n];
            long bl = ndfs_sintran_segment_word_offset(dp->logad, dp->segle, dp->madr, out->blst_sym + 2 * n);
            long ab = ndfs_sintran_segment_word_offset(dp->logad, dp->segle, dp->madr, out->ablpa_sym + n);
            long mn = ndfs_sintran_segment_word_offset(dp->logad, dp->segle, dp->madr, out->massn_sym + n);
            (void)base;
            if (bl < 0 || ab < 0 || mn < 0) continue;
            sf->dblst = ((uint32_t)word_be(seg, (size_t)bl) << 16) | (uint32_t)word_be(seg, (size_t)bl + 2);
            sf->blocks_per_page = word_be(seg, (size_t)ab);
            sf->massno = word_be(seg, (size_t)mn);
            if (n == 0) { ablpage0 = sf->blocks_per_page; massno0 = sf->massno; }
            if (sf->blocks_per_page == 0) sf->blocks_per_page = ablpage0;
            sf->on_this_disk = (sf->massno == 0 || sf->massno == massno0);
            if (sf->dblst != 0 && sf->blocks_per_page > 0) {
                sf->defined = true;
                sf->base_page = (long)(sf->dblst / (uint32_t)sf->blocks_per_page);
                sf->source = "DBLST / ABLPAGE in S3IDPIT";
            } else if (n == 4 && ablpage0 > 0) {
                /* Set at every start-up, not stored: BLST(11B) := ABLPAGE(0) + XXSWTYPE
                 * (PH-P2-OPPSTART.NPL:751), i.e. one page when XXSWTYPE is 0. Measured on
                 * the K packs: the save copy at MADR 27 is disk page 28. */
                sf->defined = true;
                sf->blocks_per_page = ablpage0;
                sf->base_page = 1;
                sf->source = "start-up rule, ABLPAGE(0) blocks (OPPSTART:751)";
            }
        }
    }

    /* Without the tables, segment file 0 is where SEGFIL0:DATA begins. */
    if (!out->segfiles[0].defined) {
        uint32_t *blocks; size_t nb;
        if (ndfs_get_file_blocks(fs, out->segfil_path, &blocks, &nb) == NDFS_OK) {
            if (nb > 0) {
                out->segfiles[0].defined = true;
                out->segfiles[0].base_page = (long)blocks[0];
                out->segfiles[0].blocks_per_page = 0;
                out->segfiles[0].source = "first page of SEGFIL0:DATA (no symbol list)";
            }
            free(blocks);
        }
    }

    for (n = 0; n < NDFS_SEGFILE_COUNT; n++) {
        if (out->segfiles[n].defined && out->segfiles[n].on_this_disk)
            label_file_at_page(fs, out->segfiles[n].base_page, out->segfiles[n].file_path,
                               sizeof(out->segfiles[n].file_path));
    }
}

/* ── List ─────────────────────────────────────────────────────────────── */

ndfs_error_t ndfs_list_segments(const ndfs_filesystem_t *fs, ndfs_segment_list_t *out)
{
    uint8_t *seg = NULL; size_t seg_size = 0;
    uint8_t *rt = NULL; size_t rt_size = 0;
    char path[NDFS_IDENTITY_PATH_MAX];
    char names[256][NDFS_SEGNAME_MAX + 1];
    ndfs_error_t err;
    int n, inibu = -1, idpit_index = -1;

    if (!fs || !out) return NDFS_ERR_NULL_PTR;
    memset(out, 0, sizeof(*out));
    memset(names, 0, sizeof(names));
    out->version = '?';
    out->segment_table_page = -1;

    /* Symbols: INIBU for the version letter, BLST / ABLPA / MASSN for the segment files. */
    if (ndfs_sintran_find_system_file(fs, "SYMBOL-1-LIST", "SYMB", path, sizeof(path)) == NDFS_OK) {
        if (read_symbol(fs, path, "INIBU", &inibu)) out->version = version_from_inibu(inibu);
        out->have_symbols = read_symbol(fs, path, "BLST", &out->blst_sym) &&
                            read_symbol(fs, path, "ABLPA", &out->ablpa_sym) &&
                            read_symbol(fs, path, "MASSN", &out->massn_sym);
    }

    /* SEGFIL0 and its segment table. */
    err = ndfs_sintran_find_system_file(fs, "SEGFIL0", "DATA", out->segfil_path, sizeof(out->segfil_path));
    if (err == NDFS_ERR_NOT_FOUND)
        err = ndfs_sintran_find_system_file(fs, "SEGFIL", "DATA", out->segfil_path, sizeof(out->segfil_path));
    if (err != NDFS_OK) return NDFS_ERR_NOT_FOUND;
    err = ndfs_read_file(fs, out->segfil_path, &seg, &seg_size);
    if (err != NDFS_OK) return err;
    out->segment_table_page = ndfs_sintran_find_segment_table_page(seg, seg_size);
    if (out->segment_table_page < 0) { ndfs_free_data(seg); return NDFS_ERR_NOT_FOUND; }

    /* Names: RTFIL:DATA first, then the loader table copies inside SEGFIL0. */
    if (ndfs_sintran_find_system_file(fs, "RTFIL", "DATA", path, sizeof(path)) == NDFS_OK &&
        ndfs_read_file(fs, path, &rt, &rt_size) == NDFS_OK) {
        if (ndfs_sintran_collect_segment_names(rt, rt_size, names) > 0)
            strncpy(out->names_path, path, sizeof(out->names_path) - 1);
        ndfs_free_data(rt);
    }
    if (out->names_path[0] == '\0' && ndfs_sintran_collect_segment_names(seg, seg_size, names) > 0)
        strncpy(out->names_path, "SEGFIL0 (loader table copy)", sizeof(out->names_path) - 1);

    /* Every entry that is used or named. */
    out->count = 0;
    for (n = 1; n < 256; n++) {
        ndfs_segment_t s;
        read_entry(seg, out->segment_table_page, n, &s);
        if (s.segle == 0 && s.madr == 0 && s.logad == 0 && names[n][0] == '\0') continue;
        strcpy(s.name, names[n]);
        if (strcmp(s.name, NDFS_SEGNAME_IMAGE_DPIT) == 0) idpit_index = out->count;
        out->segments[out->count++] = s;
    }

    read_segfile_tables(fs, seg, seg_size, out, idpit_index);

    /* Disk page of every segment with pages on a known, local segment file. The 5OK bit
     * is NOT a condition: it means "loaded in memory" (set by SEGIN at run time, see
     * NDInsight SEGMENTS-INTRO-AND-DEEP-DIVE), and a segment the RT-loader has just
     * written has it clear while its pages are on disk. Measured 2026-10-29: segment
     * 101B loaded onto a fresh segment file 1 had FLAG 020000 and its code at
     * DBLST(1)/ABLPAGE + MADR. System segments show 5OK only because the table image
     * was written while they were loaded. */
    for (n = 0; n < out->count; n++) {
        ndfs_segment_t *s = &out->segments[n];
        const ndfs_segfile_t *sf = &out->segfiles[s->segfile];
        if (s->memory_only || s->segle == 0) continue;
        if (!sf->defined || !sf->on_this_disk) continue;
        s->disk_page = sf->base_page + s->madr;
    }

    ndfs_free_data(seg);
    return NDFS_OK;
}

/* ── Lookup ───────────────────────────────────────────────────────────── */

int ndfs_find_segment(const ndfs_segment_list_t *list, const char *name_or_number)
{
    int i;
    size_t len;
    if (!list || !name_or_number || !*name_or_number) return -1;
    len = strlen(name_or_number);

    /* A number: all digits, octal with a trailing B, decimal without. */
    {
        bool digits = true;
        size_t k, end = len;
        int base = 10;
        if (name_or_number[len - 1] == 'B' || name_or_number[len - 1] == 'b') { base = 8; end--; }
        if (end == 0) digits = false;
        for (k = 0; k < end && digits; k++)
            if (name_or_number[k] < '0' || name_or_number[k] > (base == 8 ? '7' : '9')) digits = false;
        if (digits) {
            long v = strtol(name_or_number, NULL, base);
            for (i = 0; i < list->count; i++)
                if (list->segments[i].number == v) return i;
            return -1;
        }
    }

    for (i = 0; i < list->count; i++) {
        const char *a = list->segments[i].name, *b = name_or_number;
        bool eq = true;
        while (*a && *b) {
            if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) { eq = false; break; }
            a++; b++;
        }
        if (eq && *a == '\0' && *b == '\0') return i;
    }
    return -1;
}

/* ── Export / import ──────────────────────────────────────────────────── */

static ndfs_error_t check_place(const ndfs_filesystem_t *fs, const ndfs_segment_list_t *list, int index)
{
    const ndfs_segment_t *s;
    if (!fs || !list || index < 0 || index >= list->count) return NDFS_ERR_INVALID_ARG;
    s = &list->segments[index];
    if (s->memory_only || s->segle == 0 || s->disk_page < 0) return NDFS_ERR_NOT_FOUND;
    if ((uint32_t)(s->disk_page + s->segle) > ndfs_get_total_pages(fs)) return NDFS_ERR_OUT_OF_RANGE;
    return NDFS_OK;
}

ndfs_error_t ndfs_export_segment(const ndfs_filesystem_t *fs, const ndfs_segment_list_t *list,
                                 int index, uint8_t **out_data, size_t *out_size)
{
    const ndfs_segment_t *s;
    uint8_t *buf;
    int p;
    ndfs_error_t err = check_place(fs, list, index);
    if (err != NDFS_OK) return err;
    if (!out_data || !out_size) return NDFS_ERR_NULL_PTR;
    s = &list->segments[index];
    buf = (uint8_t *)malloc((size_t)s->segle * PAGE_BYTES);
    if (!buf) return NDFS_ERR_ALLOC;
    for (p = 0; p < s->segle; p++) {
        err = ndfs_read_disk_page(fs, (uint32_t)(s->disk_page + p), buf + (size_t)p * PAGE_BYTES);
        if (err != NDFS_OK) { free(buf); return err; }
    }
    *out_data = buf;
    *out_size = (size_t)s->segle * PAGE_BYTES;
    return NDFS_OK;
}

ndfs_error_t ndfs_import_segment(ndfs_filesystem_t *fs, const ndfs_segment_list_t *list,
                                 int index, const uint8_t *data, size_t size)
{
    const ndfs_segment_t *s;
    uint8_t page[PAGE_BYTES];
    size_t done = 0;
    ndfs_error_t err = check_place(fs, list, index);
    if (err != NDFS_OK) return err;
    if (!data) return NDFS_ERR_NULL_PTR;
    s = &list->segments[index];
    if (size > (size_t)s->segle * PAGE_BYTES) return NDFS_ERR_OUT_OF_RANGE;

    /* A whole page at a time; the last partial page keeps its remaining bytes. */
    while (done < size) {
        uint32_t pg = (uint32_t)(s->disk_page + (long)(done / PAGE_BYTES));
        size_t chunk = size - done < PAGE_BYTES ? size - done : PAGE_BYTES;
        if (chunk < PAGE_BYTES) {
            err = ndfs_read_disk_page(fs, pg, page);
            if (err != NDFS_OK) return err;
        }
        memcpy(page, data + done, chunk);
        err = ndfs_write_disk_page(fs, pg, page);
        if (err != NDFS_OK) return err;
        done += chunk;
    }
    return NDFS_OK;
}
