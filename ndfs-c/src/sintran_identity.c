/**
 * SINTRAN III CPU number and CPU type on a system pack: read / change / verify.
 *
 * See include/ndfs/sintran_identity.h for what is stored where, and NDInsight
 * chapter SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md for the measurements.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/sintran_identity.h>
#include <ndfs/sintran.h>
#include <ndfs/parity.h>
#include <ndfs/master_block.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

#define PAGE_BYTES      2048
#define SEG_ENTRY_BYTES 16
#define NAME_RECORD_BYTES 16

/* INIBU per version, as sintran.c (octal 067172 / 074123 / 102327). Only used to
 * print the version letter; nothing is addressed through it. */
#define INIBU_K 0x6E7A
#define INIBU_L 0x7853
#define INIBU_M 0x84D7

static int word_be(const uint8_t *b, size_t o) { return (b[o] << 8) | b[o + 1]; }

/* ── Pure helpers ─────────────────────────────────────────────────────── */

long ndfs_sintran_segment_word_offset(int logad, int segle, int madr, int addr)
{
    int base = (logad & 077) * 1024;
    if (addr < base || addr >= base + segle * 1024) return -1;
    return (long)madr * PAGE_BYTES + (long)(addr - base) * 2;
}

bool ndfs_sintran_find_symbol(const uint8_t *text, size_t len, const char *name,
                              int *out_value, size_t *out_value_offset, int *out_value_width)
{
    size_t nlen = strlen(name);
    size_t i = 0;

    if (!text || !name || nlen == 0) return false;

    while (i < len) {
        size_t p = i;
        int value = 0, width = 0;

        /* Leading blanks on the line are allowed (the N500 lists indent). */
        while (p < len && (text[p] == ' ' || text[p] == '\t')) p++;

        if (p + nlen < len && memcmp(text + p, name, nlen) == 0 && text[p + nlen] == '=') {
            size_t d = p + nlen + 1;
            while (d + (size_t)width < len &&
                   text[d + (size_t)width] >= '0' && text[d + (size_t)width] <= '7') {
                value = value * 8 + (text[d + (size_t)width] - '0');
                width++;
            }
            if (width > 0) {
                if (out_value) *out_value = value;
                if (out_value_offset) *out_value_offset = d;
                if (out_value_width) *out_value_width = width;
                return true;
            }
        }

        /* Next line. */
        while (i < len && text[i] != '\n' && text[i] != '\r') i++;
        while (i < len && (text[i] == '\n' || text[i] == '\r')) i++;
    }
    return false;
}

bool ndfs_sintran_decode_packed_name(uint16_t w0, uint16_t w1, uint16_t w2,
                                     char *out, size_t out_size)
{
    uint64_t n = ((uint64_t)w0 << 32) | ((uint64_t)w1 << 16) | (uint64_t)w2;
    char rev[NDFS_SEGNAME_MAX + 1];
    int count = 0, i;

    if (!out || out_size < NDFS_SEGNAME_MAX + 1) return false;
    out[0] = '\0';
    if (n == 0) return false;

    while (n != 0) {
        unsigned c6 = (unsigned)(n & 0x3F);
        char ch;
        n >>= 6;
        if (count >= NDFS_SEGNAME_MAX) return false;
        /* SIXBIT: 1..26 are A..Z (ASCII & 0x3F), 0x30..0x39 are the digits. */
        if (c6 >= 1 && c6 <= 26)        ch = (char)('A' + (c6 - 1));
        else if (c6 >= 0x30 && c6 <= 0x39) ch = (char)c6;
        else return false;
        rev[count++] = ch;
    }
    for (i = 0; i < count; i++) out[i] = rev[count - 1 - i];
    out[count] = '\0';
    return true;
}

int ndfs_sintran_find_segment_by_name(const uint8_t *buf, size_t size, const char *name)
{
    size_t o;
    char decoded[NDFS_SEGNAME_MAX + 1];

    if (!buf || !name || size < NAME_RECORD_BYTES) return -1;

    for (o = 0; o + NAME_RECORD_BYTES <= size; o += 2) {
        int segno;
        if (word_be(buf, o + 10) != NDFS_RTFIL_TYPE_SEGMENT_NAME) continue;
        if (word_be(buf, o + 8) != 0 || word_be(buf, o + 14) != 0) continue;
        segno = word_be(buf, o + 6);
        if (segno == 0 || segno > 0377 || segno != word_be(buf, o + 12)) continue;
        if (!ndfs_sintran_decode_packed_name((uint16_t)word_be(buf, o), (uint16_t)word_be(buf, o + 2),
                                             (uint16_t)word_be(buf, o + 4), decoded, sizeof(decoded)))
            continue;
        if (strcmp(decoded, name) == 0) return segno;
    }
    return -1;
}

/* ── File lookup ──────────────────────────────────────────────────────── */

static bool eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

static void trim_type(char *type)
{
    size_t t = strlen(type);
    while (t > 0 && (type[t - 1] == 0x27 || type[t - 1] == ' ')) type[--t] = '\0';
}

/* Build "USER/NAME:TYPE" for the first object owned by SYSTEM with this name and
 * type. Returns NDFS_ERR_NOT_FOUND when there is none. */
ndfs_error_t ndfs_sintran_find_system_file(const ndfs_filesystem_t *fs, const char *name,
                                           const char *want_type, char *path, size_t path_sz)
{
    ndfs_object_entry_t *objs;
    size_t count, i;
    ndfs_error_t err = ndfs_get_object_entries(fs, &objs, &count);
    if (err != NDFS_OK) return err;

    for (i = 0; i < count; i++) {
        char type[NDFS_TYPE_MAX + 1];
        if (!eq_ci(objs[i].user_name, "SYSTEM")) continue;
        if (!eq_ci(objs[i].object_name, name)) continue;
        strncpy(type, objs[i].type, sizeof(type) - 1);
        type[sizeof(type) - 1] = '\0';
        trim_type(type);
        if (!eq_ci(type, want_type)) continue;
        snprintf(path, path_sz, "%s/%s:%s", objs[i].user_name, objs[i].object_name, type);
        ndfs_free_object_entries(objs);
        return NDFS_OK;
    }
    ndfs_free_object_entries(objs);
    return NDFS_ERR_NOT_FOUND;
}

/* SEGFIL0:DATA, or the older name SEGFIL:DATA. */
static ndfs_error_t find_segfil(const ndfs_filesystem_t *fs, char *path, size_t path_sz)
{
    ndfs_error_t err = ndfs_sintran_find_system_file(fs, "SEGFIL0", "DATA", path, path_sz);
    if (err == NDFS_ERR_NOT_FOUND)
        err = ndfs_sintran_find_system_file(fs, "SEGFIL", "DATA", path, path_sz);
    return err;
}

/* ── Symbol lists ─────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *text;      /* parity stripped */
    size_t   len;
    bool     has_parity;
} symlist_t;

static ndfs_error_t read_symlist(const ndfs_filesystem_t *fs, const char *path, symlist_t *out)
{
    uint8_t *raw; size_t len, i;
    ndfs_error_t err = ndfs_read_file(fs, path, &raw, &len);
    if (err != NDFS_OK) return err;

    out->has_parity = false;
    for (i = 0; i < len; i++) {
        if (raw[i] & 0x80) { out->has_parity = true; break; }
    }
    ndfs_strip_parity(raw, len);
    out->text = raw;
    out->len = len;
    return NDFS_OK;
}

static void free_symlist(symlist_t *s)
{
    if (s->text) ndfs_free_data(s->text);
    s->text = NULL;
    s->len = 0;
}

static int sym_or_minus_one(const symlist_t *a, const symlist_t *b, const char *name)
{
    int v;
    if (a->text && ndfs_sintran_find_symbol(a->text, a->len, name, &v, NULL, NULL)) return v;
    if (b->text && ndfs_sintran_find_symbol(b->text, b->len, name, &v, NULL, NULL)) return v;
    return -1;
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

/* ── Segment names and table ──────────────────────────────────────────── */

typedef struct { int logad, segle, madr, flag, sgsta; } seg_entry_t;

static seg_entry_t seg_entry(const uint8_t *seg, int page, int n)
{
    seg_entry_t e;
    size_t o = (size_t)page * PAGE_BYTES + (size_t)n * SEG_ENTRY_BYTES;
    e.logad = word_be(seg, o + 4);
    e.segle = word_be(seg, o + 6) & 0x3FF;
    e.madr  = word_be(seg, o + 8);
    e.flag  = word_be(seg, o + 10);
    e.sgsta = word_be(seg, o + 12);
    return e;
}

/* Resolve both DPIT names to segment numbers: RTFIL:DATA first (small), then the
 * copies of the loader table inside SEGFIL0. Fills out->names_path. */
static void resolve_segment_names(const ndfs_filesystem_t *fs, const uint8_t *seg, size_t seg_size,
                                  ndfs_cpu_identity_t *out)
{
    char path[NDFS_IDENTITY_PATH_MAX];
    uint8_t *rt = NULL; size_t rt_size = 0;
    int i;

    for (i = 0; i < 2; i++) out->copies[i].segment_number = -1;
    out->names_path[0] = '\0';

    if (ndfs_sintran_find_system_file(fs, "RTFIL", "DATA", path, sizeof(path)) == NDFS_OK &&
        ndfs_read_file(fs, path, &rt, &rt_size) == NDFS_OK) {
        for (i = 0; i < 2; i++)
            out->copies[i].segment_number = ndfs_sintran_find_segment_by_name(rt, rt_size, out->copies[i].name);
        ndfs_free_data(rt);
        if (out->copies[0].segment_number > 0 && out->copies[1].segment_number > 0) {
            strncpy(out->names_path, path, sizeof(out->names_path) - 1);
            return;
        }
    }

    /* Fall back to the S3RTFIL segment pages inside SEGFIL0. */
    for (i = 0; i < 2; i++)
        if (out->copies[i].segment_number <= 0)
            out->copies[i].segment_number = ndfs_sintran_find_segment_by_name(seg, seg_size, out->copies[i].name);
    if (out->copies[0].segment_number > 0 && out->copies[1].segment_number > 0)
        strncpy(out->names_path, "SEGFIL0 (loader table copy)", sizeof(out->names_path) - 1);
}

/* The file a segment file number names. 0 is SEGFIL0; on K the save copy sits on
 * segment file 4, which the K release information places in SINTRAN:DATA. Other
 * numbers are reported and never written. */
static ndfs_error_t segment_file_path(const ndfs_filesystem_t *fs, int segment_file,
                                      const char *segfil_path, char *path, size_t path_sz)
{
    switch (segment_file) {
        case 0:
            strncpy(path, segfil_path, path_sz - 1);
            path[path_sz - 1] = '\0';
            return NDFS_OK;
        case 4:
            return ndfs_sintran_find_system_file(fs, "SINTRAN", "DATA", path, path_sz);
        default:
            path[0] = '\0';
            return NDFS_ERR_NOT_FOUND;
    }
}

/* A word offset is usable only when the file really holds it; otherwise -1. The K
 * pack's SINTRAN:DATA is 60 pages while S3SDPIT is listed as 35 pages from page 27,
 * so the segment's tail is past the file end but every identity word is inside it.
 * The check is therefore per word, not per segment. */
static long offset_in_file(long offset, size_t file_size)
{
    if (offset < 0) return -1;
    if ((size_t)offset + 2 > file_size) return -1;
    return offset;
}

/* Read the four identity words of one copy out of its file. */
static void read_copy_words(const ndfs_filesystem_t *fs, const ndfs_sintran_symbols_t *sym,
                            ndfs_dpit_copy_t *c)
{
    uint8_t *data = NULL; size_t size = 0;

    c->sysno = c->fcpun = c->cpu_type = c->revle = -1;
    c->sysno_offset = ndfs_sintran_segment_word_offset(c->logad, c->segle, c->madr, sym->sysno);
    c->fcpun_offset = ndfs_sintran_segment_word_offset(c->logad, c->segle, c->madr, sym->fcpun);
    c->type_offset  = (sym->hwinf >= 0)
                    ? ndfs_sintran_segment_word_offset(c->logad, c->segle, c->madr, sym->hwinf + 2) : -1;
    c->revle_offset = (sym->revle >= 0)
                    ? ndfs_sintran_segment_word_offset(c->logad, c->segle, c->madr, sym->revle) : -1;
    c->file_found = false;

    if (c->file_path[0] == '\0') return;
    if (ndfs_read_file(fs, c->file_path, &data, &size) != NDFS_OK) return;

    /* The file must at least reach the copy's first page; each word is then checked
     * against the file length on its own. */
    if ((long)size > (long)c->madr * PAGE_BYTES) {
        c->file_found = true;
        c->sysno_offset = offset_in_file(c->sysno_offset, size);
        c->fcpun_offset = offset_in_file(c->fcpun_offset, size);
        c->type_offset  = offset_in_file(c->type_offset, size);
        c->revle_offset = offset_in_file(c->revle_offset, size);
        if (c->sysno_offset >= 0) c->sysno    = word_be(data, (size_t)c->sysno_offset);
        if (c->fcpun_offset >= 0) c->fcpun    = word_be(data, (size_t)c->fcpun_offset);
        if (c->type_offset  >= 0) c->cpu_type = word_be(data, (size_t)c->type_offset);
        if (c->revle_offset >= 0) c->revle    = word_be(data, (size_t)c->revle_offset);
    }
    ndfs_free_data(data);
}

/* ── Read ─────────────────────────────────────────────────────────────── */

ndfs_error_t ndfs_read_cpu_identity(const ndfs_filesystem_t *fs, ndfs_cpu_identity_t *out)
{
    symlist_t s1 = { NULL, 0, false }, s2 = { NULL, 0, false };
    char segfil_path[NDFS_IDENTITY_PATH_MAX];
    uint8_t *seg = NULL; size_t seg_size = 0;
    const ndfs_master_block_t *mb = NULL;
    ndfs_error_t err;
    int i, v;
    size_t off; int width;

    if (!fs || !out) return NDFS_ERR_NULL_PTR;
    memset(out, 0, sizeof(*out));
    out->version = '?';
    out->cpn33_value_offset = out->cpu33_value_offset = -1;
    out->segment_table_page = -1;
    out->sym.sysno = out->sym.hwinf = out->sym.revle = out->sym.fcpun =
    out->sym.cpn33 = out->sym.cpu33 = out->sym.inibu = -1;
    strcpy(out->copies[0].name, NDFS_SEGNAME_IMAGE_DPIT); out->copies[0].role = "image";
    strcpy(out->copies[1].name, NDFS_SEGNAME_SAVE_DPIT);  out->copies[1].role = "save";
    for (i = 0; i < 2; i++) {
        out->copies[i].segment_number = -1;
        out->copies[i].sysno = out->copies[i].fcpun = out->copies[i].cpu_type = out->copies[i].revle = -1;
        out->copies[i].sysno_offset = out->copies[i].fcpun_offset =
        out->copies[i].type_offset = out->copies[i].revle_offset = -1;
    }

    /* Directory stamp first: it needs no SINTRAN at all. */
    if (ndfs_get_master_block(fs, &mb) == NDFS_OK && mb) {
        out->dir_ext_valid = mb->ext_valid;
        out->dir_entered = (mb->ext_flag_word & NDFS_EXT_FLAG_ENTERED) != 0;
        out->dir_system_number = mb->ext_last_system_number;
    }

    /* The pack's own symbol lists. */
    err = ndfs_sintran_find_system_file(fs, "SYMBOL-1-LIST", "SYMB", out->symbol1_path, sizeof(out->symbol1_path));
    if (err != NDFS_OK) return NDFS_ERR_NOT_FOUND;
    err = ndfs_sintran_find_system_file(fs, "SYMBOL-2-LIST", "SYMB", out->symbol2_path, sizeof(out->symbol2_path));
    if (err != NDFS_OK) return NDFS_ERR_NOT_FOUND;
    err = read_symlist(fs, out->symbol1_path, &s1);
    if (err != NDFS_OK) return err;
    err = read_symlist(fs, out->symbol2_path, &s2);
    if (err != NDFS_OK) { free_symlist(&s1); return err; }
    out->symbol2_has_parity = s2.has_parity;

    out->sym.sysno = sym_or_minus_one(&s1, &s2, "SYSNO");
    out->sym.hwinf = sym_or_minus_one(&s1, &s2, "HWINF");
    out->sym.revle = sym_or_minus_one(&s1, &s2, "REVLE");
    out->sym.fcpun = sym_or_minus_one(&s2, &s1, "FCPUN");   /* K keeps it in list 2 */
    out->sym.inibu = sym_or_minus_one(&s1, &s2, "INIBU");
    if (ndfs_sintran_find_symbol(s2.text, s2.len, "33CPN", &v, &off, &width)) {
        out->sym.cpn33 = v; out->cpn33_value_offset = (long)off; out->cpn33_value_width = width;
    }
    if (ndfs_sintran_find_symbol(s2.text, s2.len, "33CPU", &v, &off, &width)) {
        out->sym.cpu33 = v; out->cpu33_value_offset = (long)off; out->cpu33_value_width = width;
    }
    free_symlist(&s1);
    free_symlist(&s2);

    out->version = version_from_inibu(out->sym.inibu);

    /* Without SYSNO and FCPUN nothing can be addressed safely. */
    if (out->sym.sysno < 0 || out->sym.fcpun < 0) return NDFS_ERR_NOT_FOUND;

    /* The segment table inside SEGFIL0. */
    err = find_segfil(fs, segfil_path, sizeof(segfil_path));
    if (err != NDFS_OK) return NDFS_ERR_NOT_FOUND;
    err = ndfs_read_file(fs, segfil_path, &seg, &seg_size);
    if (err != NDFS_OK) return err;
    out->segment_table_page = ndfs_sintran_find_segment_table_page(seg, seg_size);
    if (out->segment_table_page < 0) { ndfs_free_data(seg); return NDFS_ERR_NOT_FOUND; }

    /* The segment numbers, by name, from the pack's RT-loader records. */
    resolve_segment_names(fs, seg, seg_size, out);
    if (out->copies[0].segment_number <= 0 || out->copies[1].segment_number <= 0) {
        ndfs_free_data(seg);
        return NDFS_ERR_NOT_FOUND;
    }

    for (i = 0; i < 2; i++) {
        ndfs_dpit_copy_t *c = &out->copies[i];
        seg_entry_t e = seg_entry(seg, out->segment_table_page, c->segment_number);
        c->logad = e.logad; c->segle = e.segle; c->madr = e.madr; c->flag = e.flag;
        c->segment_file = (e.flag >> 13) & 7;
        c->file_path[0] = '\0';
        /* Usable: a length, a disk address (the 5OK bit) and a range that covers SYSNO.
         * The logical base comes from the entry itself, not from a constant. */
        c->present = (e.segle > 0) && (e.flag & 1) != 0 &&
                     ndfs_sintran_segment_word_offset(e.logad, e.segle, e.madr, out->sym.sysno) >= 0;
        if (!c->present) continue;
        if (segment_file_path(fs, c->segment_file, segfil_path, c->file_path, sizeof(c->file_path)) != NDFS_OK) {
            c->file_path[0] = '\0';
            continue;
        }
        read_copy_words(fs, &out->sym, c);
    }
    ndfs_free_data(seg);
    return NDFS_OK;
}

/* ── Write ────────────────────────────────────────────────────────────── */

static ndfs_error_t patch_word(ndfs_filesystem_t *fs, const char *path, long offset, int value)
{
    uint8_t b[2];
    if (offset < 0) return NDFS_ERR_NOT_FOUND;
    b[0] = (uint8_t)((value >> 8) & 0xFF);
    b[1] = (uint8_t)(value & 0xFF);
    return ndfs_patch_file_region(fs, path, (size_t)offset, b, 2);
}

/* Rewrite the octal digits of a `NAME=OOOOOO` symbol in place, same width, with the
 * list's own parity convention. */
static ndfs_error_t patch_symbol_value(ndfs_filesystem_t *fs, const char *path, long offset,
                                       int width, bool parity, int value)
{
    uint8_t digits[16];
    int i;
    if (offset < 0 || width <= 0 || width > 15) return NDFS_ERR_NOT_FOUND;
    /* The value must fit the digits that are there; 6 digits hold any 16-bit word. */
    if (width < 6 && value >= (1 << (3 * width))) return NDFS_ERR_OUT_OF_RANGE;
    for (i = width - 1; i >= 0; i--) {
        digits[i] = (uint8_t)('0' + (value & 7));
        value >>= 3;
    }
    if (parity) ndfs_set_parity(digits, (size_t)width);
    return ndfs_patch_file_region(fs, path, (size_t)offset, digits, (size_t)width);
}

ndfs_error_t ndfs_write_cpu_identity(ndfs_filesystem_t *fs, int cpu_number, int cpu_type,
                                     bool keep_stamp, ndfs_cpu_identity_t *out_after)
{
    ndfs_cpu_identity_t id;
    ndfs_error_t err;
    int i;

    if (!fs) return NDFS_ERR_NULL_PTR;
    if (cpu_number < 0 || cpu_number > 0xFFFF) return NDFS_ERR_INVALID_ARG;
    if (cpu_type > 0xFFFF) return NDFS_ERR_INVALID_ARG;

    err = ndfs_read_cpu_identity(fs, &id);
    if (err != NDFS_OK) return err;

    /* Everything must be addressable before the first byte is written. */
    for (i = 0; i < 2; i++) {
        const ndfs_dpit_copy_t *c = &id.copies[i];
        if (!c->present || !c->file_found) return NDFS_ERR_NOT_FOUND;
        if (c->sysno_offset < 0 || c->fcpun_offset < 0) return NDFS_ERR_NOT_FOUND;
        if (cpu_type >= 0 && c->type_offset < 0) return NDFS_ERR_NOT_FOUND;
    }

    for (i = 0; i < 2; i++) {
        const ndfs_dpit_copy_t *c = &id.copies[i];
        err = patch_word(fs, c->file_path, c->sysno_offset, cpu_number);
        if (err != NDFS_OK) return err;
        err = patch_word(fs, c->file_path, c->fcpun_offset, cpu_number);
        if (err != NDFS_OK) return err;
        if (cpu_type >= 0) {
            err = patch_word(fs, c->file_path, c->type_offset, cpu_type);
            if (err != NDFS_OK) return err;
        }
    }

    /* The generation symbols, so a later ND patch run keeps the new values. */
    if (id.cpn33_value_offset >= 0) {
        err = patch_symbol_value(fs, id.symbol2_path, id.cpn33_value_offset, id.cpn33_value_width,
                                 id.symbol2_has_parity, cpu_number);
        if (err != NDFS_OK) return err;
    }
    if (cpu_type >= 0 && id.cpu33_value_offset >= 0) {
        err = patch_symbol_value(fs, id.symbol2_path, id.cpu33_value_offset, id.cpu33_value_width,
                                 id.symbol2_has_parity, cpu_type);
        if (err != NDFS_OK) return err;
    }

    if (!keep_stamp && (id.dir_entered || id.dir_system_number != 0)) {
        err = ndfs_release_directory(fs, NULL, NULL);
        if (err != NDFS_OK) return err;
    }

    /* Read back and compare. */
    err = ndfs_read_cpu_identity(fs, &id);
    if (err != NDFS_OK) return err;
    for (i = 0; i < 2; i++) {
        const ndfs_dpit_copy_t *c = &id.copies[i];
        if (c->sysno != cpu_number || c->fcpun != cpu_number) return NDFS_ERR_CORRUPT;
        if (cpu_type >= 0 && c->cpu_type != cpu_type) return NDFS_ERR_CORRUPT;
    }
    if (id.cpn33_value_offset >= 0 && id.sym.cpn33 != cpu_number) return NDFS_ERR_CORRUPT;
    if (cpu_type >= 0 && id.cpu33_value_offset >= 0 && id.sym.cpu33 != cpu_type) return NDFS_ERR_CORRUPT;
    if (!keep_stamp && (id.dir_entered || id.dir_system_number != 0)) return NDFS_ERR_CORRUPT;

    if (out_after) *out_after = id;
    return NDFS_OK;
}
