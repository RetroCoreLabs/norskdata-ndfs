/**
 * SINTRAN III initial-command buffer: locate / read / write / repair / diagnose.
 *
 * Ported behaviour-for-behaviour from the RetroFS.NDFS C# implementation and the
 * nd100x glass reference, both live-verified on real K03/L07/M06 packs. See
 * docs/SINTRAN-INITIAL-COMMANDS-SPEC.md for the full derivation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/sintran.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* Kernel symbol INIBU (start of the buffer) per SINTRAN version (octal in the
 * symbol lists): K03 = 067172, L07 = 074123, M06 = 102327. */
#define INIBU_K 0x6E7A
#define INIBU_L 0x7853
#define INIBU_M 0x84D7

#define SEG_ENTRY_BYTES 16
#define PAGE_BYTES      2048
#define CMD_END         0x27   /* "'" terminates a command; alone = end of buffer */

/* Version try-order (L, M, K) shared by the locator and the diagnostic. */
static const char VERSION_ORDER[3] = { 'L', 'M', 'K' };

static int inibu_for(char version)
{
    switch (version) {
        case 'K': return INIBU_K;
        case 'L': return INIBU_L;
        case 'M': return INIBU_M;
        default:  return 0;
    }
}

/* Big-endian 16-bit word read. */
static int word_be(const uint8_t *b, size_t o) { return (b[o] << 8) | b[o + 1]; }

typedef struct {
    int logad, segle, madr, flag, sgsta;
} seg_entry_t;

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

int ndfs_sintran_find_segment_table_page(const uint8_t *seg, size_t size)
{
    int pages = (int)(size / PAGE_BYTES);
    int best = -1, best_good = -1;
    int p;

    if (!seg) return -1;

    for (p = 0; p < pages; p++) {
        int good = 0, n, i;
        bool zero = true, self_ref = false;

        /* Entry 0 must be all-zero. */
        for (i = 0; i < SEG_ENTRY_BYTES; i++) {
            if (seg[(size_t)p * PAGE_BYTES + i] != 0) { zero = false; break; }
        }
        if (!zero) continue;

        for (n = 1; n < 128; n++) {
            seg_entry_t e = seg_entry(seg, p, n);
            if (e.segle == 0 && e.madr == 0 && e.logad == 0) continue;
            if (e.segle > 0 && e.segle < 1024 && e.madr > 0 &&
                (e.flag & 1) != 0 && (e.sgsta & 0xE000) != 0) {
                good++;
                if (e.madr == p && e.segle <= 32) self_ref = true;
            }
        }

        if (good >= 40 && self_ref && good > best_good) {
            best = p;
            best_good = good;
        }
    }
    return best;
}

/* ── Command-string array helpers ─────────────────────────────────────── */

void ndfs_sintran_free_commands(char **commands, size_t count)
{
    size_t i;
    if (!commands) return;
    for (i = 0; i < count; i++) free(commands[i]);
    free(commands);
}

static bool is_printable(const char *s)
{
    size_t i, len = strlen(s);
    if (len == 0) return false;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

ndfs_error_t ndfs_sintran_parse_buffer(const uint8_t *seg, size_t size,
                                        size_t off, size_t cap_bytes,
                                        char ***out_commands, size_t *out_count)
{
    char **cmds = NULL;
    size_t count = 0, cap = 0;
    size_t i = off, end;

    if (!seg || !out_commands || !out_count) return NDFS_ERR_NULL_PTR;
    *out_commands = NULL;
    *out_count = 0;

    end = off + cap_bytes;
    if (end > size) end = size;

    while (i < end) {
        char *s;
        size_t start, slen, k;

        if (seg[i] == 0x00) { i++; continue; }   /* word-alignment padding */
        if (seg[i] == CMD_END) break;            /* empty command = terminator */

        start = i;
        while (i < end && seg[i] != CMD_END) i++;
        slen = i - start;
        i++;                                     /* consume the "'" */
        if (i < end && seg[i] == 0x00 && (((i - off) & 1) != 0)) i++;

        s = (char *)malloc(slen + 1);
        if (!s) { ndfs_sintran_free_commands(cmds, count); return NDFS_ERR_ALLOC; }
        for (k = 0; k < slen; k++) s[k] = (char)(seg[start + k] & 0x7F);
        s[slen] = '\0';

        if (!is_printable(s)) {                  /* not a real buffer */
            free(s);
            ndfs_sintran_free_commands(cmds, count);
            return NDFS_ERR_CORRUPT;
        }

        if (count == cap) {
            size_t ncap = cap ? cap * 2 : 8;
            char **tmp = (char **)realloc(cmds, ncap * sizeof(char *));
            if (!tmp) { free(s); ndfs_sintran_free_commands(cmds, count); return NDFS_ERR_ALLOC; }
            cmds = tmp; cap = ncap;
        }
        cmds[count++] = s;
    }

    *out_commands = cmds;
    *out_count = count;
    return NDFS_OK;
}

/* ── Byte-vector growth for the encoder ───────────────────────────────── */

typedef struct { uint8_t *b; size_t len, cap; } bytevec_t;

static bool bv_push(bytevec_t *v, uint8_t x)
{
    if (v->len == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 64;
        uint8_t *tmp = (uint8_t *)realloc(v->b, ncap);
        if (!tmp) return false;
        v->b = tmp; v->cap = ncap;
    }
    v->b[v->len++] = x;
    return true;
}

ndfs_error_t ndfs_sintran_encode_buffer(const char *const *commands, size_t count,
                                        int max_text_bytes,
                                        uint8_t **out_bytes, size_t *out_len,
                                        int *out_text_len)
{
    bytevec_t v = { NULL, 0, 0 };
    size_t idx, j;
    int limit = max_text_bytes > 0 ? max_text_bytes : NDFS_INITCMD_MAX_TEXT_BYTES;
    int text_len;

    if (!commands && count > 0) return NDFS_ERR_NULL_PTR;
    if (!out_bytes || !out_len || !out_text_len) return NDFS_ERR_NULL_PTR;

    for (idx = 0; idx < count; idx++) {
        const char *c = commands[idx] ? commands[idx] : "";
        size_t clen = strlen(c);

        /* Validate: printable ASCII, no embedded quote. */
        if (clen == 0) { free(v.b); return NDFS_ERR_INVALID_ARG; }
        for (j = 0; j < clen; j++) {
            unsigned char ch = (unsigned char)c[j];
            if (ch < 0x20 || ch > 0x7E || ch == CMD_END) { free(v.b); return NDFS_ERR_INVALID_ARG; }
        }

        for (j = 0; j < clen; j++) {
            unsigned char up = (unsigned char)toupper((unsigned char)c[j]);
            if (!bv_push(&v, (uint8_t)(up & 0x7F))) { free(v.b); return NDFS_ERR_ALLOC; }
        }
        if (!bv_push(&v, CMD_END)) { free(v.b); return NDFS_ERR_ALLOC; }
        if ((v.len & 1) != 0) { if (!bv_push(&v, 0x00)) { free(v.b); return NDFS_ERR_ALLOC; } }
    }

    text_len = (int)v.len;                 /* the length-cell value */
    if (text_len > limit) { free(v.b); return NDFS_ERR_OUT_OF_RANGE; }

    if (!bv_push(&v, CMD_END)) { free(v.b); return NDFS_ERR_ALLOC; }   /* buffer terminator */
    if ((v.len & 1) != 0) { if (!bv_push(&v, 0x00)) { free(v.b); return NDFS_ERR_ALLOC; } }

    *out_bytes = v.b;
    *out_len = v.len;
    *out_text_len = text_len;
    return NDFS_OK;
}

bool ndfs_sintran_reencode_matches(const char *const *commands, size_t count,
                                   int stored_text_len)
{
    uint8_t *bytes = NULL;
    size_t len = 0;
    int text_len = 0;
    ndfs_error_t err = ndfs_sintran_encode_buffer(commands, count,
                                                  NDFS_INITCMD_MAX_TEXT_BYTES,
                                                  &bytes, &len, &text_len);
    if (err != NDFS_OK) return false;
    free(bytes);
    return text_len == stored_text_len;
}

/* Build the 262-byte region image (text area + length cell). */
static ndfs_error_t build_region_image(const uint8_t *enc, size_t enc_len, int text_len,
                                       uint8_t region[NDFS_INITCMD_CAP_BYTES])
{
    if (enc_len > NDFS_INITCMD_LEN_CELL_BYTES) return NDFS_ERR_OUT_OF_RANGE;
    memset(region, 0, NDFS_INITCMD_CAP_BYTES);
    memcpy(region, enc, enc_len);
    region[NDFS_INITCMD_LEN_CELL_BYTES]     = (uint8_t)(text_len >> 8);
    region[NDFS_INITCMD_LEN_CELL_BYTES + 1] = (uint8_t)(text_len & 0xFF);
    return NDFS_OK;
}

/* ── Locate (self-consistent) ─────────────────────────────────────────── */

/* Returns NDFS_OK and fills *out (commands allocated) on success, else
 * NDFS_ERR_NOT_FOUND. If out is NULL only the offset/version are reported. */
static ndfs_error_t locate(const uint8_t *seg, size_t size,
                           ndfs_initial_commands_t *out,
                           long *out_offset, char *out_version, int *out_segnum)
{
    int st_page = ndfs_sintran_find_segment_table_page(seg, size);
    int k;
    if (st_page < 0) return NDFS_ERR_NOT_FOUND;

    for (k = 0; k < 3; k++) {
        char letter = VERSION_ORDER[k];
        int inibu = inibu_for(letter);
        int n;
        if (inibu == 0) continue;

        for (n = 1; n < 128; n++) {
            seg_entry_t e = seg_entry(seg, st_page, n);
            int seg_base, off, text_len;
            long byte_offset;
            char **cmds; size_t count;
            ndfs_error_t perr;

            if (e.madr == 0 || e.segle == 0) continue;
            seg_base = (e.logad & 0x3F) * 1024;
            if (inibu < seg_base || inibu >= seg_base + e.segle * 1024) continue;

            off = inibu - seg_base;
            byte_offset = (long)e.madr * PAGE_BYTES + (long)off * 2;
            if ((size_t)(byte_offset + NDFS_INITCMD_CAP_BYTES) > size) continue;

            text_len = word_be(seg, (size_t)(byte_offset + NDFS_INITCMD_LEN_CELL_BYTES));
            if (text_len == 0 || text_len > NDFS_INITCMD_LEN_CELL_BYTES) continue;

            perr = ndfs_sintran_parse_buffer(seg, size, (size_t)byte_offset,
                                             (size_t)text_len, &cmds, &count);
            if (perr != NDFS_OK) continue;

            if (count > 0 &&
                ndfs_sintran_reencode_matches((const char *const *)cmds, count, text_len)) {
                if (out) {
                    out->version = letter;
                    out->segment_number = n;
                    out->madr = (uint32_t)e.madr;
                    out->byte_offset = byte_offset;
                    out->segment_table_page = st_page;
                    out->text_length = text_len;
                    out->length_cell_offset = byte_offset + NDFS_INITCMD_LEN_CELL_BYTES;
                    out->commands = cmds;
                    out->command_count = count;
                } else {
                    ndfs_sintran_free_commands(cmds, count);
                }
                if (out_offset) *out_offset = byte_offset;
                if (out_version) *out_version = letter;
                if (out_segnum) *out_segnum = n;
                return NDFS_OK;
            }
            ndfs_sintran_free_commands(cmds, count);
        }
    }
    return NDFS_ERR_NOT_FOUND;
}

/* ── SEGFIL0 discovery + buffer read ──────────────────────────────────── */

static bool eq_ci(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

/* Build "USER/NAME:TYPE" for the SEGFIL0 object, or NDFS_ERR_NOT_FOUND. */
static ndfs_error_t find_segfil_path(const ndfs_filesystem_t *fs, char *path, size_t path_sz)
{
    ndfs_object_entry_t *objs;
    size_t count, i;
    ndfs_error_t err = ndfs_get_object_entries(fs, &objs, &count);
    if (err != NDFS_OK) return err;

    for (i = 0; i < count; i++) {
        if (eq_ci(objs[i].object_name, "SEGFIL0") || eq_ci(objs[i].object_name, "SEGFIL")) {
            char type[NDFS_TYPE_MAX + 1];
            size_t t = 0;
            /* Trim trailing 0x27/space from type. */
            strncpy(type, objs[i].type, sizeof(type) - 1);
            type[sizeof(type) - 1] = '\0';
            t = strlen(type);
            while (t > 0 && (type[t - 1] == 0x27 || type[t - 1] == ' ')) type[--t] = '\0';

            if (t > 0)
                snprintf(path, path_sz, "%s/%s:%s", objs[i].user_name, objs[i].object_name, type);
            else
                snprintf(path, path_sz, "%s/%s", objs[i].user_name, objs[i].object_name);
            ndfs_free_object_entries(objs);
            return NDFS_OK;
        }
    }
    ndfs_free_object_entries(objs);
    return NDFS_ERR_NOT_FOUND;
}

/* Read SEGFIL0's bytes; caller frees *out_seg with ndfs_free_data. */
static ndfs_error_t read_segfil(const ndfs_filesystem_t *fs, char *path, size_t path_sz,
                                uint8_t **out_seg, size_t *out_size)
{
    ndfs_error_t err = find_segfil_path(fs, path, path_sz);
    if (err != NDFS_OK) return err;
    return ndfs_read_file(fs, path, out_seg, out_size);
}

void ndfs_initial_commands_free(ndfs_initial_commands_t *ic)
{
    if (!ic) return;
    ndfs_sintran_free_commands(ic->commands, ic->command_count);
    ic->commands = NULL;
    ic->command_count = 0;
}

ndfs_error_t ndfs_read_initial_commands(const ndfs_filesystem_t *fs,
                                        ndfs_initial_commands_t *out)
{
    char path[NDFS_NAME_MAX * 2 + 8];
    uint8_t *seg; size_t size;
    ndfs_error_t err;

    if (!fs || !out) return NDFS_ERR_NULL_PTR;
    memset(out, 0, sizeof(*out));

    err = read_segfil(fs, path, sizeof(path), &seg, &size);
    if (err != NDFS_OK) return (err == NDFS_ERR_NOT_FOUND) ? NDFS_ERR_NOT_FOUND : err;

    err = locate(seg, size, out, NULL, NULL, NULL);
    ndfs_free_data(seg);
    return err;
}

/* Shared write-at-offset: encode + build region + patch SEGFIL0 in place. */
static ndfs_error_t write_region_at(ndfs_filesystem_t *fs, const char *segfil_path,
                                    long byte_offset,
                                    const char *const *commands, size_t count)
{
    uint8_t *enc = NULL; size_t enc_len = 0; int text_len = 0;
    uint8_t region[NDFS_INITCMD_CAP_BYTES];
    ndfs_error_t err;

    err = ndfs_sintran_encode_buffer(commands, count, NDFS_INITCMD_MAX_TEXT_BYTES,
                                     &enc, &enc_len, &text_len);
    if (err != NDFS_OK) return err;

    err = build_region_image(enc, enc_len, text_len, region);
    free(enc);
    if (err != NDFS_OK) return err;

    return ndfs_patch_file_region(fs, segfil_path, (size_t)byte_offset,
                                  region, NDFS_INITCMD_CAP_BYTES);
}

ndfs_error_t ndfs_write_initial_commands(ndfs_filesystem_t *fs,
                                         const char *const *commands,
                                         size_t count)
{
    char path[NDFS_NAME_MAX * 2 + 8];
    uint8_t *seg; size_t size;
    long byte_offset = 0;
    ndfs_error_t err;

    if (!fs || (!commands && count > 0)) return NDFS_ERR_NULL_PTR;
    /* Read-only is enforced by ndfs_patch_file_region (fs is opaque here). */

    err = read_segfil(fs, path, sizeof(path), &seg, &size);
    if (err != NDFS_OK) return err;

    err = locate(seg, size, NULL, &byte_offset, NULL, NULL);
    ndfs_free_data(seg);
    if (err != NDFS_OK) return err;

    return write_region_at(fs, path, byte_offset, commands, count);
}

/* ── Repair (header-corrupted buffer) ─────────────────────────────────── */

static const char *const REPAIR_KEYWORDS[] = {
    "DIRECTORY", "DISC-", "SET-AVAIL", "APPEND-BATCH", "LOAD-MODE",
    "HENT-MODE", "CONNECT", "ENTER-DIR", "SYSTEM-OUTPUT"
};
#define REPAIR_KEYWORD_COUNT (sizeof(REPAIR_KEYWORDS) / sizeof(REPAIR_KEYWORDS[0]))

/* Score a CAP_BYTES region by surviving command keywords. */
static int score_region(const uint8_t *seg, size_t size, long byte_offset)
{
    char text[NDFS_INITCMD_CAP_BYTES + 1];
    size_t n = NDFS_INITCMD_CAP_BYTES, i;
    int score = 0;
    if ((size_t)(byte_offset + NDFS_INITCMD_CAP_BYTES) > size)
        n = size - (size_t)byte_offset;
    for (i = 0; i < n; i++) {
        int c = seg[(size_t)byte_offset + i] & 0x7F;
        text[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    text[n] = '\0';
    for (i = 0; i < REPAIR_KEYWORD_COUNT; i++)
        if (strstr(text, REPAIR_KEYWORDS[i])) score++;
    return score;
}

/* Find the unambiguous repair target offset. Returns NDFS_ERR_NOT_FOUND if none. */
static ndfs_error_t locate_repair_target(const uint8_t *seg, size_t size,
                                         long *out_offset, int *out_score)
{
    int st_page = ndfs_sintran_find_segment_table_page(seg, size);
    int k, best_score = 0, second_score = 0;
    long best_offset = -1;
    if (st_page < 0) return NDFS_ERR_NOT_FOUND;

    for (k = 0; k < 3; k++) {
        int inibu = inibu_for(VERSION_ORDER[k]);
        int n;
        if (inibu == 0) continue;
        for (n = 1; n < 128; n++) {
            seg_entry_t e = seg_entry(seg, st_page, n);
            int seg_base, off, score;
            long byte_offset;
            if (e.madr == 0 || e.segle == 0) continue;
            seg_base = (e.logad & 0x3F) * 1024;
            if (inibu < seg_base || inibu >= seg_base + e.segle * 1024) continue;
            off = inibu - seg_base;
            byte_offset = (long)e.madr * PAGE_BYTES + (long)off * 2;
            if ((size_t)(byte_offset + NDFS_INITCMD_CAP_BYTES) > size) continue;

            score = score_region(seg, size, byte_offset);
            if (score > best_score) { second_score = best_score; best_score = score; best_offset = byte_offset; }
            else if (score > second_score) { second_score = score; }
        }
    }

    if (best_offset < 0 || best_score < 2 || best_score == second_score)
        return NDFS_ERR_NOT_FOUND;
    if (out_offset) *out_offset = best_offset;
    if (out_score) *out_score = best_score;
    return NDFS_OK;
}

ndfs_error_t ndfs_repair_initial_commands(ndfs_filesystem_t *fs,
                                          const char *const *commands,
                                          size_t count,
                                          long *out_byte_offset,
                                          int *out_score)
{
    char path[NDFS_NAME_MAX * 2 + 8];
    uint8_t *seg; size_t size;
    long byte_offset = 0;
    int score = 0;
    ndfs_error_t err;

    if (!fs || (!commands && count > 0)) return NDFS_ERR_NULL_PTR;
    /* Read-only is enforced by ndfs_patch_file_region (fs is opaque here). */

    err = read_segfil(fs, path, sizeof(path), &seg, &size);
    if (err != NDFS_OK) return err;

    err = locate_repair_target(seg, size, &byte_offset, &score);
    ndfs_free_data(seg);
    if (err != NDFS_OK) return err;

    err = write_region_at(fs, path, byte_offset, commands, count);
    if (err != NDFS_OK) return err;

    if (out_byte_offset) *out_byte_offset = byte_offset;
    if (out_score) *out_score = score;
    return NDFS_OK;
}

/* ── Diagnostic: enumerate every candidate ────────────────────────────── */

void ndfs_free_initcmd_candidates(ndfs_initcmd_candidate_t *candidates, size_t count)
{
    size_t i;
    if (!candidates) return;
    for (i = 0; i < count; i++)
        ndfs_sintran_free_commands(candidates[i].commands, candidates[i].command_count);
    free(candidates);
}

static ndfs_error_t diagnose_seg(const uint8_t *seg, size_t size,
                                 ndfs_initcmd_candidate_t **out, size_t *out_count)
{
    int st_page = ndfs_sintran_find_segment_table_page(seg, size);
    ndfs_initcmd_candidate_t *arr = NULL;
    size_t count = 0, cap = 0;
    int k;

    *out = NULL; *out_count = 0;
    if (st_page < 0) return NDFS_OK;   /* zero candidates */

    for (k = 0; k < 3; k++) {
        int inibu = inibu_for(VERSION_ORDER[k]);
        int n;
        if (inibu == 0) continue;
        for (n = 1; n < 128; n++) {
            seg_entry_t e = seg_entry(seg, st_page, n);
            int seg_base, off, text_len;
            long byte_offset;
            bool plausible;
            char **cmds = NULL; size_t ccount = 0;
            bool consistent = false;
            ndfs_initcmd_candidate_t c;

            if (e.madr == 0 || e.segle == 0) continue;
            seg_base = (e.logad & 0x3F) * 1024;
            if (inibu < seg_base || inibu >= seg_base + e.segle * 1024) continue;
            off = inibu - seg_base;
            byte_offset = (long)e.madr * PAGE_BYTES + (long)off * 2;
            if ((size_t)(byte_offset + NDFS_INITCMD_CAP_BYTES) > size) continue;

            text_len = word_be(seg, (size_t)(byte_offset + NDFS_INITCMD_LEN_CELL_BYTES));
            plausible = text_len > 0 && text_len <= NDFS_INITCMD_LEN_CELL_BYTES;
            if (plausible) {
                if (ndfs_sintran_parse_buffer(seg, size, (size_t)byte_offset,
                                              (size_t)text_len, &cmds, &ccount) != NDFS_OK) {
                    cmds = NULL; ccount = 0;
                }
            }
            if (cmds && ccount > 0)
                consistent = ndfs_sintran_reencode_matches((const char *const *)cmds, ccount, text_len);

            c.version = VERSION_ORDER[k];
            c.segment_number = n;
            c.madr = (uint32_t)e.madr;
            c.byte_offset = byte_offset;
            c.length_cell = text_len;
            c.length_plausible = plausible;
            c.consistent = consistent;
            c.commands = cmds;
            c.command_count = ccount;

            if (count == cap) {
                size_t ncap = cap ? cap * 2 : 16;
                ndfs_initcmd_candidate_t *tmp =
                    (ndfs_initcmd_candidate_t *)realloc(arr, ncap * sizeof(*arr));
                if (!tmp) {
                    ndfs_sintran_free_commands(cmds, ccount);
                    ndfs_free_initcmd_candidates(arr, count);
                    return NDFS_ERR_ALLOC;
                }
                arr = tmp; cap = ncap;
            }
            arr[count++] = c;
        }
    }

    *out = arr;
    *out_count = count;
    return NDFS_OK;
}

ndfs_error_t ndfs_diagnose_initial_commands(const ndfs_filesystem_t *fs,
                                            ndfs_initcmd_candidate_t **out,
                                            size_t *out_count)
{
    char path[NDFS_NAME_MAX * 2 + 8];
    uint8_t *seg; size_t size;
    ndfs_error_t err;

    if (!fs || !out || !out_count) return NDFS_ERR_NULL_PTR;
    *out = NULL; *out_count = 0;

    err = read_segfil(fs, path, sizeof(path), &seg, &size);
    if (err != NDFS_OK) return err;

    err = diagnose_seg(seg, size, out, out_count);
    ndfs_free_data(seg);
    return err;
}
