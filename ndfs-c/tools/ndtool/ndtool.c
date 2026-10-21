/**
 * ndtool: CLI tool for working with NDFS disk images.
 * Main entry point and helper functions.
 *
 * SPDX-License-Identifier: MIT
 */

/* Expose POSIX fileno()/isatty() under strict -std=c99 (must precede any
 * system header include). */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "ndtool.h"
#include "parity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

/* ── Forward declarations ─────────────────────────────────────────── */

static void print_usage(const char *prog);

/* ── Long options ─────────────────────────────────────────────────── */

static struct option long_options[] = {
    {"put",       required_argument, 0, 'P'},
    {"rm",        required_argument, 0, 'R'},
    {"useradd",  required_argument, 0, 'A'},
    {"userdel",  required_argument, 0, 'D'},
    {"quotaadd", required_argument, 0, 1003},
    {"quotadel", required_argument, 0, 1004},
    {"objblocks",     required_argument, 0, 1016},
    {"giveobjblocks", required_argument, 0, 1017},
    {"takeobjblocks", required_argument, 0, 1018},
    {"friendadd", required_argument, 0, 1013},
    {"frienddel", required_argument, 0, 1014},
    {"friends",    required_argument, 0, 1015},
    {"passwd",    required_argument, 0, 'W'},
    {"create",    required_argument, 0, 'C'},
    {"shell",     no_argument,       0, 'S'},
    {"fsck",      no_argument,       0, 1005},
    {"fix",       no_argument,       0, 1030},
    {"recover-orphans", required_argument, 0, 1031},
    {"salvage",   required_argument, 0, 1032},
    {"fsck-undo", no_argument,       0, 1033},
    {"editor",    required_argument, 0, 1006},
    {"xat",       no_argument,       0, 1007},
    {"dest",      required_argument, 0, 1008},
    {"overwrite", no_argument,       0, 1009},
    {"interactive", no_argument,     0, 1010},
    {"stat",      required_argument, 0, 1011},
    {"chmod",     required_argument, 0, 1012},
    {"pages",     required_argument, 0, 1001},
    {"bit-file",  required_argument, 0, 1020},
    {"boot",      required_argument, 0, 1021},
    {"boot-from", required_argument, 0, 1022},
    {"boot-file", required_argument, 0, 1023},
    {"name",      required_argument, 0, 1002},
    {"version",   no_argument,       0, 'V'},
    {"ver",       no_argument,       0, 'V'},
    {0, 0, 0, 0}
};

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    ndtool_ctx_t ctx;
    int opt;
    int ret = 0;

    /* Mode flags */
    int mode_list = 0;
    int mode_extract = 0;
    int mode_info = 0;
    int mode_users = 0;
    int mode_shell = 0;
    int mode_fsck = 0;
    int fsck_fix = 0;
    int fsck_undo = 0;
    const char *fsck_recover_user = NULL;
    const char *fsck_salvage_dir = NULL;

    /* Write operation args */
    const char *put_local = NULL;
    const char *rm_path = NULL;
    const char *useradd_name = NULL;
    const char *userdel_name = NULL;
    const char *addquota_name = NULL;
    /* Object blocks: files per user. See docs/NDFS-OBJECT-BLOCKS-SPEC.md. */
    const char *objblocks_name = NULL;
    const char *giveobjblocks_name = NULL;
    const char *takeobjblocks_name = NULL;
    uint32_t objblock_count = 0;
    const char *remquota_name = NULL;
    uint32_t quota_pages = 0;
    const char *passwd_name = NULL;
    const char *friend_add_arg = NULL;
    const char *friend_del_arg = NULL;
    const char *friends_arg = NULL;
    const char *create_template = NULL;
    uint32_t custom_pages = 0;
    const char *dir_name = NULL;
    const char *stat_path = NULL;
    const char *chmod_spec = NULL;

    int needs_write = 0;

    memset(&ctx, 0, sizeof(ctx));

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    while ((opt = getopt_long(argc, argv, "tuxidplvfho:VnF:", long_options, NULL)) != -1) {
        switch (opt) {
        case 't': mode_list = 1; break;
        case 'u': mode_users = 1; break;
        case 'x': mode_extract = 1; break;
        case 'i': mode_info = 1; break;
        case 'd': ctx.with_dirs = true; break;
        case 'p': ctx.do_parity = true; break;
        case 'l': ctx.lowercase = true; break;
        case 'v': ctx.verbose = true; break;
        case 'f': /* force == overwrite existing targets */
            ctx.force = true;
            ctx.on_exist = NDTOOL_ON_EXIST_OVERWRITE;
            break;
        case 'F': ctx.filter_file = optarg; break;
        case 'n': ctx.dry_run = true; break;
        case 'o': ctx.output_dir = optarg; break;
        case 'h': print_usage(argv[0]); return 0;
        case 'V': ndtool_print_version(); return 0;

        case 'P': put_local = optarg; needs_write = 1; break;
        case 'R': rm_path = optarg; needs_write = 1; break;
        case 'A': useradd_name = optarg; needs_write = 1; break;
        case 'D': userdel_name = optarg; needs_write = 1; break;
        case 1003: addquota_name = optarg; needs_write = 1; break;
        case 1016: objblocks_name = optarg; break;
        case 1017: giveobjblocks_name = optarg; needs_write = 1; break;
        case 1018: takeobjblocks_name = optarg; needs_write = 1; break;
        case 1004: remquota_name = optarg; needs_write = 1; break;
        case 'W': passwd_name = optarg; needs_write = 1; break;
        case 1013: friend_add_arg = optarg; needs_write = 1; break;
        case 1014: friend_del_arg = optarg; needs_write = 1; break;
        case 1015: friends_arg = optarg; break;
        case 'C': create_template = optarg; needs_write = 1; break;
        case 'S': mode_shell = 1; needs_write = 1; break;
        case 1005: mode_fsck = 1; break;
        case 1030: fsck_fix = 1; mode_fsck = 1; break;
        case 1031: fsck_recover_user = optarg; fsck_fix = 1; mode_fsck = 1; break;
        case 1032: fsck_salvage_dir = optarg; mode_fsck = 1; break;
        case 1033: fsck_undo = 1; needs_write = 1; break;
        case 1006: ctx.editor = optarg; break;
        case 1007: ctx.use_xat = true; break;
        case 1008: ctx.dest_user = optarg; break;
        case 1009: ctx.on_exist = NDTOOL_ON_EXIST_OVERWRITE; break;
        case 1010: ctx.on_exist = NDTOOL_ON_EXIST_PROMPT; break;
        case 1011: stat_path = optarg; break;
        case 1012: chmod_spec = optarg; needs_write = 1; break;

        case 1001: custom_pages = (uint32_t)atol(optarg); break;
        case 1020: ctx.create_bit_file = optarg; break;
        case 1021: ctx.create_boot = optarg; break;
        case 1022: ctx.create_boot_from = optarg; break;
        case 1023: ctx.create_boot_file = optarg; break;
        case 1002: dir_name = optarg; break;

        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* -f short option: re-parse to get filter_user */
    /* We use -u with an argument: if -u is set AND next positional before image looks like a user */
    /* Actually: -u alone = list users. -u USER -t = list files for USER. */
    /* Let's handle the -u USER filter via optind scanning */

    /* The image file is ALWAYS the last argument */
    if (create_template) {
        /* For --create, the image path is the last arg */
        if (strcmp(create_template, "list") == 0) {
            /* "--create list" only prints the disk type table; no image file is involved */
            return cmd_create(&ctx, create_template, 0, NULL);
        }
        if (optind >= argc) {
            fprintf(stderr, "Error: no output image path specified\n");
            return 1;
        }
        ctx.image_path = argv[argc - 1];

        /* If --addquota/--remquota, grab the pages arg */
        if ((addquota_name || remquota_name) && optind < argc - 1) {
            quota_pages = (uint32_t)atol(argv[optind]);
        }
    } else {
        if (optind >= argc) {
            fprintf(stderr, "Error: no image file specified\n");
            return 1;
        }
        ctx.image_path = argv[argc - 1];

        /* Parse positional args between optind and last arg */
        /* For --put LOCAL [NDFS_PATH] IMAGE */
        /* For --quota INDEX PAGES IMAGE */
        /* For -u USER (filter) */
        if (mode_users && mode_list && optind < argc - 1) {
            ctx.filter_user = argv[optind];
        } else if (mode_users && !mode_list && optind < argc - 1) {
            ctx.filter_user = argv[optind];
        } else if (mode_list && optind < argc - 1) {
            ctx.filter_user = argv[optind];
        }

        if ((addquota_name || remquota_name) && optind < argc - 1) {
            quota_pages = (uint32_t)atol(argv[optind]);
        }

        /* --giveobjblocks/--takeobjblocks NAME COUNT IMAGE - the count is the
         * positional argument between the option's NAME and the image path. */
        if ((giveobjblocks_name || takeobjblocks_name) && optind < argc - 1) {
            objblock_count = (uint32_t)atol(argv[optind]);
        }
    }

    /* Handle --create separately (no existing image to open) */
    if (create_template) {
        ret = cmd_create(&ctx, create_template, custom_pages, dir_name);
        return ret;
    }

    /* --fix writes the image unless -n: open it for writing. */
    if (mode_fsck && fsck_fix && !ctx.dry_run) needs_write = 1;

    /* Read the image file */
    {
        size_t img_size = 0;
        uint8_t *img_data = ndtool_read_local_file(ctx.image_path, &img_size);
        if (!img_data) {
            fprintf(stderr, "Error: cannot read '%s'\n", ctx.image_path);
            return 1;
        }

        size_t rem = img_size % 2048;
        if (rem != 0) {
            if (needs_write) {
                fprintf(stderr,
                        "Error: cannot write to '%s': image size %zu bytes is not a "
                        "multiple of the 2048-byte page size; pad it up to the next "
                        "boundary first.\n",
                        ctx.image_path, img_size);
                free(img_data);
                return 1;
            }
            fprintf(stderr,
                    "Warning: image size %zu bytes is not a multiple of the 2048-byte "
                    "page size (%zu trailing bytes / partial last page); proceeding "
                    "anyway. Zero-pad to the next 2048-byte boundary to silence this.\n",
                    img_size, rem);
        }

        bool read_only = !needs_write;
        ndfs_error_t err = ndfs_open_buffer_copy(img_data, img_size, read_only, &ctx.fs);
        free(img_data);

        if (err != NDFS_OK) {
            fprintf(stderr, "Error: cannot open NDFS image: %s\n", ndfs_strerror(err));
            return 1;
        }
    }

    /* Dispatch command */
    if (mode_shell) {
        ret = cmd_shell(&ctx);
    } else if (fsck_undo) {
        ret = cmd_fsck_undo(&ctx);
    } else if (mode_fsck) {
        /* Exit code: 0 CLEAN, 1 DAMAGED, 2 FAILED, 3 ndtool could not do what was asked. */
        ret = cmd_fsck(&ctx, fsck_fix != 0, fsck_recover_user, fsck_salvage_dir);
    } else if (mode_info) {
        ret = cmd_info(&ctx);
    } else if (mode_list) {
        ret = cmd_list(&ctx);
    } else if (mode_users && !mode_list) {
        ret = cmd_users(&ctx);
    } else if (stat_path) {
        ret = cmd_stat(&ctx, stat_path, ctx.verbose);
    } else if (chmod_spec) {
        /* --chmod SPEC PATH <image>: PATH is the positional before the image. */
        const char *path = (optind < argc - 1) ? argv[optind] : NULL;
        ret = cmd_chmod(&ctx, chmod_spec, path);
    } else if (mode_extract) {
        ret = cmd_extract(&ctx);
    } else if (put_local) {
        const char *ndfs_path = NULL;
        /* If there's an arg between put_local and the image path */
        if (optind < argc - 1) {
            ndfs_path = argv[optind];
        }
        ret = cmd_put(&ctx, put_local, ndfs_path);
    } else if (rm_path) {
        ret = cmd_delete(&ctx, rm_path);
    } else if (useradd_name) {
        uint32_t quota = 100;
        if (optind < argc - 1) {
            quota = (uint32_t)atol(argv[optind]);
        }
        ret = cmd_useradd(&ctx, useradd_name, quota);
    } else if (userdel_name) {
        ret = cmd_userdel(&ctx, userdel_name);
    } else if (addquota_name && quota_pages > 0) {
        ret = cmd_addquota(&ctx, addquota_name, quota_pages);
    } else if (remquota_name && quota_pages > 0) {
        ret = cmd_remquota(&ctx, remquota_name, quota_pages);
    } else if (objblocks_name) {
        ret = cmd_objblocks(&ctx, objblocks_name);
    } else if (giveobjblocks_name) {
        /* Default to a single block, matching @GIVE-OBJECT-BLOCKS with no count. */
        ret = cmd_giveobjblocks(&ctx, giveobjblocks_name,
                                objblock_count > 0 ? objblock_count : 1);
    } else if (takeobjblocks_name) {
        ret = cmd_takeobjblocks(&ctx, takeobjblocks_name,
                                objblock_count > 0 ? objblock_count : 1);
    } else if (passwd_name) {
        ret = cmd_passwd(&ctx, passwd_name);
    } else if (friend_add_arg) {
        ret = cmd_friend_add(&ctx, friend_add_arg);
    } else if (friend_del_arg) {
        ret = cmd_friend_del(&ctx, friend_del_arg);
    } else if (friends_arg) {
        ret = cmd_friend_list(&ctx, friends_arg);
    } else {
        /* Default: show info */
        ret = cmd_info(&ctx);
    }

    ndfs_close(ctx.fs);
    return ret;
}

/* ── Usage / Version ──────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
    printf("ndtool %s - NDFS disk image tool\n\n", NDTOOL_VERSION);
    printf("Usage: %s [options] [args] <image>\n\n", prog);
    printf("Paths: NDFS_PATH = a file INSIDE the image (USER/NAME:TYPE, a SINTRAN\n");
    printf("       file); HOSTFILE = a file on your computer. <image> is the .ndfs\n");
    printf("       image file on your computer.\n\n");
    printf("Modes:\n");
    printf("  -t              List files\n");
    printf("  -u              List users (combine with -t to filter by user)\n");
    printf("  -i              Show filesystem info\n");
    printf("  -x              Extract files OUT of the image onto your computer\n");
    printf("  --put HOSTFILE [NDFS_PATH]  Copy a file from your computer INTO the image\n");
    printf("                  HOSTFILE may be a wildcard (quote it); use --dest USER\n");
    printf("  --rm NDFS_PATH  Delete a file from inside the image\n");
    printf("  --useradd NAME [QUOTA]  Add user (default quota: 100 pages)\n");
    printf("  --userdel NAME          Remove user (must have no files)\n");
    printf("  --quotaadd NAME PAGES   Add pages to user quota (checks disk space)\n");
    printf("  --quotadel NAME PAGES   Remove pages from user quota (checks usage)\n");
    printf("  --passwd NAME            Clear user password\n");
    printf("  --objblocks NAME         Show object blocks and file count for a user\n");
    printf("  --giveobjblocks NAME N   Give N more object blocks (256 files each, max 16)\n");
    printf("                           The @GIVE-OBJECT-BLOCKS equivalent. Raises the\n");
    printf("                           file limit only; blocks are used on demand and\n");
    printf("                           no disk pages are reserved (see --quotaadd).\n");
    printf("  --takeobjblocks NAME N   Lower the object block maximum by N\n");
    printf("  --friends USER           List USER's friends and their rights\n");
    printf("  --friendadd OWNER:FRIEND[:RWACD]  Add a friend (default rights RWA)\n");
    printf("  --frienddel OWNER:FRIEND          Remove a friend\n");
    printf("                  OWNER/FRIEND may be a user name or index (0-255).\n");
    printf("                  Rights: R read W write A append C common D directory\n");
    printf("  --fsck          Full filesystem check. The last lines are IMAGE VERDICT: CLEAN, DAMAGED or\n");
    printf("                  FAILED, with every failed file and why. Exit code 0 CLEAN, 1 DAMAGED, 2 FAILED,\n");
    printf("                  3 ndtool could not do what was asked. -v adds orphan runs, both owners of every\n");
    printf("                  cross-link, the quota table and one line per finding (FAILED|file|page|block|reason).\n");
    printf("  --fix           With --fsck: do the safe repairs and check again. Set a bit for a page that a\n");
    printf("                  file uses but the bit file calls free (R1); clear the bit of an orphan page that\n");
    printf("                  is all zero (R4); recompute a wrong Extended Info Block checksum (R3). Orphan\n");
    printf("                  pages with data are never freed. Stop the emulator first and add -f. -n lists\n");
    printf("                  the repairs and writes nothing. IMAGE.fsck-undo is written before the image.\n");
    printf("  --recover-orphans USER  With --fsck --fix: runs of orphan pages that hold data become files\n");
    printf("                  USER/ORPHAN-n:DATA (n = first block). The pages stay where they are.\n");
    printf("  --salvage DIR   With --fsck: copy every failed file to DIR and write SALVAGE-REPORT.txt\n");
    printf("                  naming the shared pages. The image is not changed.\n");
    printf("  --fsck-undo     Put back the pages saved in IMAGE.fsck-undo by the last --fix.\n");
    printf("  --stat PATH     Show detailed file metadata (add -v for block list)\n");
    printf("  --chmod SPEC PATH   Change a file's access permissions\n");
    printf("  --create TEMPLATE  Create new image\n");
    printf("  --shell         Interactive shell\n");
    printf("\nOptions:\n");
    printf("  -v              Verbose output\n");
    printf("  -l              Lowercase filenames on extract\n");
    printf("  -p              Even parity: strip on extract, set on copy-in\n");
    printf("                  (for text files: :MODE, :SYMB, :TEXT, :C, etc.)\n");
    printf("  -d              Create user subdirectories on extract\n");
    printf("  -o DIR          Output directory for extract\n");
    printf("  -F PATTERN      Filter/extract by filename glob (* and ?),\n");
    printf("                  e.g. 'SYSTEM/*:MODE' or '*/STARTUP:MODE'\n");
    printf("  --dest USER     Destination user for --put (required for wildcards)\n");
    printf("  -n              Dry run (preview create/overwrite/skip)\n");
    printf("  --overwrite, -f Overwrite existing targets (default: skip them)\n");
    printf("  --interactive   Prompt before overwriting each existing target\n");
    printf("  --xat           Write/read .xat sidecar files for metadata preservation\n");
    printf("  --editor CMD    Editor for 'edit' command (default: $EDITOR or code --wait)\n");
    printf("  -h              Show this help\n");
    printf("  -V, --version, --ver  Show version and build date/time\n");
    printf("\nAccess permissions (--chmod / shell 'chmod'):\n");
    printf("  SPEC is comma-separated clauses: TIER OP RIGHTS\n");
    printf("    TIER   OWN | FRIEND | PUBLIC | ALL   (or O | F | P)\n");
    printf("    OP     =  set exactly    +  add    -  remove\n");
    printf("    RIGHTS letters: R read  W write  A append  C common  D directory\n");
    printf("  Examples:  OWN+WD   OWN=RWACD,FRIEND=RW   PUBLIC-A   ALL=R\n");
    printf("  Raw form:  a 0x.. or decimal value sets the whole 15-bit word (0x03FF)\n");
    printf("  Use -n to preview the before -> after change without writing.\n");
    printf("\nCreate templates:\n");
    printf("  floppy360       360 KB floppy (154 pages)\n");
    printf("  floppy12        1.2 MB floppy (512 pages)\n");
    printf("  smd75           75 MB SMD disk\n");
    printf("  winchester74    74 MB Winchester disk (36864 pages)\n");
    printf("  scsi126         126 MB SCSI disk (64656 pages)\n");
    printf("  custom          Custom size (use --pages N)\n");
    printf("  DISC-xx ...     A SINTRAN disk type by name (DISC-14MB, DISC-21MB, DISC-23MB, DISC-28MB,\n");
    printf("                  DISC-45MB, DISC-74MB: Winchester; DISC-38MB, DISC-70MB, DISC-75MB,\n");
    printf("                  DISC-288MB-R/F/E/N, DISC-450MB-F/N and more: SMD). The image has the\n");
    printf("                  directory size SINTRAN gives the type, and the full disk layout length\n");
    printf("                  where that is known. Run --create list for every name with its size in\n");
    printf("                  pages and MB, controller family, drive model and test status.\n");
    printf("  -R -F -E -N     End of a SMD name: formats of one size (R removable, F fixed, E EMD drive,\n");
    printf("                  N NEC drive) with different heads/sectors, so different image lengths.\n");
    printf("  SCSI            Only scsi126 (64656 pages) is made. Its last page holds the SINTRAN disk\n");
    printf("                  area table; other SCSI sizes are not tested. Floppies: floppy360 and\n");
    printf("                  floppy12 only (or custom).\n");
    printf("\n  --pages N       Page count for custom template\n");
    printf("  --bit-file A    SINTRAN's bit file address prompt: 0 = default (9*floor(floor(size/2)/9)),\n");
    printf("                  -1 = end of disc, anything else = an OCTAL page address\n");
    printf("  --boot WHAT     Boot area in page 0: auto (default), none, smd, winchester, scsi,\n");
    printf("                  floppy360, floppy616. Not verified: no created image has been booted.\n");
    printf("  --boot-from IMG Copy bytes 0-1999 of page 0 of the image IMG as the boot area\n");
    printf("  --boot-file F   Use the file F (at most 2000 bytes) as the boot area\n");
    printf("  --name NAME     Directory/volume name for new image\n");
    printf("\nExamples:\n");
    printf("  %s -t disk.ndfs\n", prog);
    printf("  %s -t -u SYSTEM disk.ndfs\n", prog);
    printf("  %s -i -v disk.ndfs\n", prog);
    printf("  %s -x -o output/ -d -l disk.ndfs\n", prog);
    printf("  %s --put myfile.txt SYSTEM/MYFILE:TEXT disk.ndfs\n", prog);
    printf("  %s -x -p -F 'SYSTEM/*:MODE' -o out/ disk.ndfs\n", prog);
    printf("  %s --put '*.NPL' --dest TEST -p disk.ndfs\n", prog);
    printf("  %s --stat -v SYSTEM/LOAD:MODE disk.ndfs\n", prog);
    printf("  %s --chmod 'OWN+WD,PUBLIC-A' SYSTEM/LOAD:MODE disk.ndfs\n", prog);
    printf("  %s --create floppy360 --name MYDISK newdisk.ndfs\n", prog);
    printf("  %s --shell disk.ndfs\n", prog);
}

void ndtool_print_version(void)
{
    printf("ndtool %s (built %s %s)\n", NDTOOL_VERSION, __DATE__, __TIME__);
}

/* ── Helper: convert NDFS name to host filename ───────────────────── */

void ndtool_to_host_name(const char *ndfs_name, char *out, size_t out_len, bool lowercase)
{
    size_t i;
    size_t len = strlen(ndfs_name);
    if (len >= out_len) len = out_len - 1;

    for (i = 0; i < len; i++) {
        if (ndfs_name[i] == ':') {
            out[i] = '.';
        } else if (lowercase) {
            out[i] = (char)tolower((unsigned char)ndfs_name[i]);
        } else {
            out[i] = ndfs_name[i];
        }
    }
    out[i] = '\0';
}

/* ── Helper: convert host filename to NDFS name ───────────────────── */

void ndtool_to_ndfs_name(const char *host_name, char *out, size_t out_len)
{
    size_t i;
    size_t len = strlen(host_name);
    size_t last_dot = (size_t)-1;

    if (len >= out_len) len = out_len - 1;

    /* Find last dot */
    for (i = 0; i < len; i++) {
        if (host_name[i] == '.') last_dot = i;
    }

    for (i = 0; i < len; i++) {
        if (i == last_dot) {
            out[i] = ':';
        } else {
            out[i] = (char)toupper((unsigned char)host_name[i]);
        }
    }
    out[i] = '\0';
}

/* ── Helper: read local file ──────────────────────────────────────── */

uint8_t *ndtool_read_local_file(const char *path, size_t *out_size)
{
    FILE *f;
    uint8_t *data;
    long fsize;

    f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize < 0) {
        fclose(f);
        return NULL;
    }

    data = (uint8_t *)malloc((size_t)fsize);
    if (!data) {
        fclose(f);
        return NULL;
    }

    if (fread(data, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(data);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *out_size = (size_t)fsize;
    return data;
}

/* ── Helper: write local file ─────────────────────────────────────── */

int ndtool_write_local_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", path);
        return -1;
    }

    if (fwrite(data, 1, size, f) != size) {
        fprintf(stderr, "Error: write failed for '%s'\n", path);
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

/* ── Helper: wildcard detection ───────────────────────────────────── */

bool ndtool_has_wildcard(const char *s)
{
    if (!s) return false;
    for (; *s; s++) {
        if (*s == '*' || *s == '?') return true;
    }
    return false;
}

/* ── Helper: overwrite decision ───────────────────────────────────── */

bool ndtool_confirm_overwrite(ndtool_ctx_t *ctx, const char *what, bool exists)
{
    if (!exists) return true;

    switch (ctx->on_exist) {
    case NDTOOL_ON_EXIST_OVERWRITE:
        if (ctx->verbose) printf("  overwriting: %s\n", what);
        return true;

    case NDTOOL_ON_EXIST_PROMPT: {
        char resp[16];
        /* Non-interactive stdin: fall back to the safe default (skip). */
        if (!isatty(fileno(stdin))) {
            printf("  skipped (exists, no tty): %s\n", what);
            return false;
        }
        printf("Overwrite %s? [y/N] ", what);
        fflush(stdout);
        if (fgets(resp, sizeof(resp), stdin) &&
            (resp[0] == 'y' || resp[0] == 'Y')) {
            return true;
        }
        printf("  skipped: %s\n", what);
        return false;
    }

    case NDTOOL_ON_EXIST_DENY:
    default:
        printf("  skipped (exists): %s\n", what);
        return false;
    }
}

/* ── Helper: save image ───────────────────────────────────────────── */

int ndtool_save_image(ndtool_ctx_t *ctx)
{
    uint8_t *buf = NULL;
    size_t buf_size = 0;
    ndfs_error_t err;
    int ret;

    if (ctx->dry_run) {
        printf("(dry run: not saving)\n");
        return 0;
    }

    err = ndfs_to_buffer(ctx->fs, &buf, &buf_size);
    if (err != NDFS_OK) {
        fprintf(stderr, "Error: cannot export image: %s\n", ndfs_strerror(err));
        return -1;
    }

    ret = ndtool_write_local_file(ctx->image_path, buf, buf_size);
    ndfs_free_data(buf);
    return ret;
}
