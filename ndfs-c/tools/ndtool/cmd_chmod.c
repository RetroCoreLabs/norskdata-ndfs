/**
 * ndtool: view/modify file access permissions.
 *
 * Symbolic spec syntax (comma-separated clauses):
 *
 *   TIER OP RIGHTS [, TIER OP RIGHTS ...]
 *
 *   TIER   = OWN | FRIEND | PUBLIC   (also O | F | P, case-insensitive)
 *            ALL applies the clause to all three tiers.
 *   OP     = '='  set the tier to exactly these rights
 *            '+'  add these rights
 *            '-'  remove these rights
 *   RIGHTS = any combination of the letters:
 *            R read   W write   A append   C common/execute   D directory/delete
 *            (an empty right list is allowed with '=', meaning "no access")
 *
 * Examples:
 *   OWN+WD                 add write+directory to the owner
 *   OWN=RWACD,FRIEND=RW    owner full, friends read/write, public unchanged
 *   PUBLIC-A               remove append from public
 *   ALL=R                  read-only for everyone
 *
 * Raw fallback: pass a value beginning with 0x (or a decimal number) to set
 * the whole 15-bit access word directly, e.g. 0x03FF.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ndtool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Map a rights letter to its bit; returns 0 on an unknown letter. */
static unsigned right_bit(char c)
{
    switch (toupper((unsigned char)c)) {
    case 'R': return NDFS_ACC_READ;
    case 'W': return NDFS_ACC_WRITE;
    case 'A': return NDFS_ACC_APPEND;
    case 'C': return NDFS_ACC_COMMON;
    case 'D': return NDFS_ACC_DIRECTORY;
    default:  return 0;
    }
}

/* Case-insensitive prefix test; returns the keyword length on match, else 0. */
static size_t ci_prefix(const char *p, const char *kw)
{
    size_t i;
    for (i = 0; kw[i]; i++) {
        if (toupper((unsigned char)p[i]) != toupper((unsigned char)kw[i])) return 0;
    }
    return i;
}

/* Parse a tier token at *s; advances *s past it. Returns the tier shift, or
 * -1 for ALL, or -2 on error. */
static int parse_tier(const char **s)
{
    const char *p = *s;
    size_t n;
    if ((n = ci_prefix(p, "OWN")))    { *s = p + n; return NDFS_ACC_OWN_SHIFT; }
    if ((n = ci_prefix(p, "FRIEND"))) { *s = p + n; return NDFS_ACC_FRIEND_SHIFT; }
    if ((n = ci_prefix(p, "PUBLIC"))) { *s = p + n; return NDFS_ACC_PUBLIC_SHIFT; }
    if ((n = ci_prefix(p, "ALL")))    { *s = p + n; return -1; }
    /* Single-letter aliases. */
    if (toupper((unsigned char)*p) == 'O') { *s = p + 1; return NDFS_ACC_OWN_SHIFT; }
    if (toupper((unsigned char)*p) == 'F') { *s = p + 1; return NDFS_ACC_FRIEND_SHIFT; }
    if (toupper((unsigned char)*p) == 'P') { *s = p + 1; return NDFS_ACC_PUBLIC_SHIFT; }
    return -2;
}

/* Apply one tier's 5-bit value into the access word. shift=-1 means all. */
static void apply_tier(uint16_t *bits, int shift, char op, unsigned rights)
{
    int shifts[3] = { NDFS_ACC_OWN_SHIFT, NDFS_ACC_FRIEND_SHIFT, NDFS_ACC_PUBLIC_SHIFT };
    int n = (shift < 0) ? 3 : 1;
    int i;
    for (i = 0; i < n; i++) {
        int sh = (shift < 0) ? shifts[i] : shift;
        unsigned cur = (*bits >> sh) & NDFS_ACC_TIER_MASK;
        unsigned nv;
        if (op == '=')      nv = rights;
        else if (op == '+') nv = cur | rights;
        else                nv = cur & ~rights;
        *bits = (uint16_t)((*bits & ~(NDFS_ACC_TIER_MASK << sh)) | ((nv & NDFS_ACC_TIER_MASK) << sh));
    }
}

/* Parse the full symbolic spec, mutating *bits from its current value.
 * Returns 0 on success, -1 on a syntax error. */
static int parse_clause(const char *clause, uint16_t *bits)
{
    const char *p = clause;
    int shift;
    char op;
    unsigned rights = 0;

    while (*p == ' ') p++;
    if (*p == '\0') return 0; /* empty clause (e.g. trailing comma) */
    shift = parse_tier(&p);
    if (shift == -2) { fprintf(stderr, "chmod: bad tier in '%s'\n", clause); return -1; }
    if (*p != '=' && *p != '+' && *p != '-') {
        fprintf(stderr, "chmod: expected = + or - in '%s'\n", clause);
        return -1;
    }
    op = *p++;
    for (; *p && *p != ' '; p++) {
        unsigned b = right_bit(*p);
        if (b == 0) { fprintf(stderr, "chmod: bad right '%c'\n", *p); return -1; }
        rights |= b;
    }
    apply_tier(bits, shift, op, rights);
    return 0;
}

static int parse_spec(const char *spec, uint16_t *bits)
{
    char work[256];
    size_t start = 0, i;

    strncpy(work, spec, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    /* Split on commas in place, parsing each clause. */
    for (i = 0; ; i++) {
        if (work[i] == ',' || work[i] == '\0') {
            char saved = work[i];
            work[i] = '\0';
            if (parse_clause(work + start, bits) != 0) return -1;
            start = i + 1;
            if (saved == '\0') break;
        }
    }
    return 0;
}

/* Print the before -> after view, then write unless -n or unchanged. */
static int show_and_write(ndtool_ctx_t *ctx, const ndfs_file_entry_t *meta,
                          const char *path, uint16_t before, uint16_t bits);

int cmd_chmod(ndtool_ctx_t *ctx, const char *spec, const char *path)
{
    ndfs_file_entry_t meta;
    ndfs_object_entry_t obj;
    uint16_t bits, before;

    if (!spec || !path) {
        fprintf(stderr, "Usage: chmod SPEC USER/FILE:TYPE   (e.g. 'OWN+WD,PUBLIC-A')\n");
        return -1;
    }
    if (ndfs_get_metadata(ctx->fs, path, &meta) != NDFS_OK ||
        ndfs_get_object_entry(ctx->fs, meta.full_name, meta.user_name, &obj) != NDFS_OK) {
        fprintf(stderr, "File not found: %s\n", path);
        return -1;
    }

    before = obj.access_bits;
    bits = before;

    /* Raw numeric form: 0x.. or decimal. */
    if (spec[0] == '0' && (spec[1] == 'x' || spec[1] == 'X')) {
        bits = (uint16_t)strtoul(spec, NULL, 16);
    } else if (isdigit((unsigned char)spec[0])) {
        bits = (uint16_t)strtoul(spec, NULL, 10);
    } else if (parse_spec(spec, &bits) != 0) {
        return -1;
    }
    return show_and_write(ctx, &meta, path, before, bits);
}

/*
 * --set-file-access ACCESS PATH / shell set-file-access PATH ACCESS:
 * the @SET-FILE-ACCESS equivalent. ACCESS is SINTRAN's own form,
 * "<public>,<friend>,<own>", each field N or letters RWACD, an empty field
 * meaning no change: 'N,N,RWACD', ',RW,'.
 */
int cmd_set_file_access(ndtool_ctx_t *ctx, const char *access, const char *path)
{
    ndfs_file_entry_t meta;
    ndfs_object_entry_t obj;
    uint16_t bits;

    if (!access || !path) {
        fprintf(stderr, "Usage: set-file-access USER/FILE:TYPE PUBLIC,FRIEND,OWN   (e.g. N,N,RWACD)\n");
        return -1;
    }
    if (ndfs_get_metadata(ctx->fs, path, &meta) != NDFS_OK ||
        ndfs_get_object_entry(ctx->fs, meta.full_name, meta.user_name, &obj) != NDFS_OK) {
        fprintf(stderr, "File not found: %s\n", path);
        return -1;
    }
    if (ndfs_apply_access_triple(obj.access_bits, access, &bits) != NDFS_OK) {
        fprintf(stderr, "set-file-access: bad access '%s': give PUBLIC,FRIEND,OWN, "
                        "each N or letters RWACD, empty = no change\n", access);
        return -1;
    }
    return show_and_write(ctx, &meta, path, obj.access_bits, bits);
}

static int show_and_write(ndtool_ctx_t *ctx, const ndfs_file_entry_t *meta_in,
                          const char *path, uint16_t before, uint16_t bits)
{
    const ndfs_file_entry_t meta = *meta_in;
    char o1[64], f1[64], p1[64], o2[64], f2[64], p2[64];

    ndtool_access_tier_str(before, NDFS_ACC_OWN_SHIFT, o1, sizeof(o1));
    ndtool_access_tier_str(before, NDFS_ACC_FRIEND_SHIFT, f1, sizeof(f1));
    ndtool_access_tier_str(before, NDFS_ACC_PUBLIC_SHIFT, p1, sizeof(p1));
    ndtool_access_tier_str(bits, NDFS_ACC_OWN_SHIFT, o2, sizeof(o2));
    ndtool_access_tier_str(bits, NDFS_ACC_FRIEND_SHIFT, f2, sizeof(f2));
    ndtool_access_tier_str(bits, NDFS_ACC_PUBLIC_SHIFT, p2, sizeof(p2));

    printf("%s  (0x%04X -> 0x%04X)\n", meta.full_name, before, bits);
    printf("  OWN    : %-30s -> %s\n", o1, o2);
    printf("  FRIEND : %-30s -> %s\n", f1, f2);
    printf("  PUBLIC : %-30s -> %s\n", p1, p2);

    if (ctx->dry_run) {
        printf("(dry run: not written)\n");
        return 0;
    }
    if (bits == before) {
        printf("(no change)\n");
        return 0;
    }

    if (ndfs_set_file_access(ctx->fs, meta.full_name, bits) != NDFS_OK) {
        fprintf(stderr, "chmod: failed to set access on %s\n", path);
        return -1;
    }
    ctx->modified = false; /* set_file_access persisted in-memory; save below */
    if (ndtool_save_image(ctx) != 0) return -1;
    return 0;
}
