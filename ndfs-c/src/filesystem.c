/**
 * NDFS filesystem implementation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

#include <ndfs/filesystem.h>
#include <ndfs/parity.h>
#include "endian_util.h"
#include "ndfs_name.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

/* ── Internal filesystem struct ──────────────────────────────────── */

#define MAX_INTERNAL_USERS   NDFS_MAX_USERS
#define MAX_INTERNAL_OBJECTS 16384

/* SubIndexed ceiling: 512 group index blocks x 512 data-page pointers each
 * (NDFS-FORMAT.md "Sub-Indexed (Type 10)"). A sub-index block itself only
 * has 512 pointer slots, so a file needing more than this many data pages
 * genuinely cannot be represented and must be rejected. */
#define NDFS_MAX_OBJECT_FILE_PAGES \
    ((uint32_t)NDFS_MAX_OBJECT_FILE_PTRS * (uint32_t)NDFS_MAX_OBJECT_FILE_PTRS)

/* ── Page cache ──────────────────────────────────────────────────────
 *
 * The whole image is no longer resident: pages are pulled/pushed through
 * fs->io (the ndfs_block_io seam) and only a handful are held in RAM at a
 * time.  read_page()/write_page_ptr() hand out a pointer INTO a cache slot
 * and callers keep using it, so a slot must not be reused while a live
 * pointer references it.
 *
 * Two safety mechanisms, together:
 *   1. LRU eviction only ever picks an UNPINNED slot.  A raw pointer deref
 *      (e.g. ndfs_bp_from_bytes(page, ...)) does NOT refresh a slot's LRU
 *      stamp, so a pointer held across N distinct read_page() calls survives
 *      only while fewer than NDFS_CACHE_SLOTS-1 other pages are touched
 *      in between.
 *   2. Any pointer held across an UNBOUNDED loop of page loads (an outer
 *      index page walked while reading its data pages) is explicitly pinned
 *      with cache_pin()/cache_unpin() so LRU can never evict it.  The worst
 *      real chain holds 3 pages live at once (sub-index + group-index +
 *      data); at most 2 are ever pinned simultaneously.  8 slots gives
 *      generous headroom and keeps short-lived (bounded) holders safe under
 *      rule 1 without needing a pin.
 *
 * Writes are write-back: write_page_ptr() marks a slot dirty; the dirty page
 * is pushed to the backend via write_block on eviction, on unpin, and on
 * close.  Because the cache is keyed by page_id (a page occupies at most one
 * slot), a read and a write of the same page alias the SAME slot, preserving
 * the in-place aliasing the write path relies on. */

#define NDFS_CACHE_SLOTS 8   /* 8 x 2048 = 16 KiB resident page window */

typedef struct {
    uint32_t page_id;   /* which page this slot holds (valid only if `valid`) */
    bool     valid;     /* slot currently holds a loaded page */
    bool     dirty;     /* holds unflushed writes; push to backend before reuse */
    uint32_t pin;       /* >0: pointer still in use, never evict this slot */
    uint32_t lru;       /* last-touch tick; lowest among unpinned slots is victim */
    uint8_t  buf[NDFS_PAGE_SIZE];
} ndfs_page_slot;

struct ndfs_filesystem {
    ndfs_block_io       io;          /* the one storage path (backend vtable) */
    uint32_t            total_pages; /* image size in whole pages */
    size_t              size;        /* total_pages * NDFS_PAGE_SIZE (working size) */
    bool                read_only;
    bool                unaligned;   /* image size was not a whole multiple of NDFS_PAGE_SIZE */

    /* Page cache (see block above). */
    ndfs_page_slot      cache[NDFS_CACHE_SLOTS];
    uint32_t            lru_clock;

    ndfs_master_block_t master_block;
    ndfs_bit_file_t     bit_file;

    /* User entries */
    ndfs_user_entry_t   users[MAX_INTERNAL_USERS];
    bool                user_valid[MAX_INTERNAL_USERS];
    size_t              user_count;

    /* Object entries */
    ndfs_object_entry_t *objects;
    size_t              object_count;
    size_t              object_capacity;

    /* Pages that could not be read on load - see ndfs_get_damage_report().
     * All zero on an intact image; above zero means the listing is what
     * survived a damaged one. */
    ndfs_damage_report_t damage;

    /* fsck repair: while `active`, the first write to each page copies the old page here
     * (4 byte block number, little endian, then 2048 bytes). See ndfs_fsck_ex(). */
    struct {
        bool     active;
        bool     failed;
        uint8_t *data;
        size_t   len, cap;
        uint8_t *seen;      /* one byte per page: already copied */
    } undo;
};

/* ── Forward declarations ────────────────────────────────────────── */

static const uint8_t *read_page(const struct ndfs_filesystem *fs, uint32_t block_id);
static uint8_t *write_page_ptr(struct ndfs_filesystem *fs, uint32_t block_id);
static void undo_capture(struct ndfs_filesystem *fs, uint32_t block, const uint8_t *old);
static ndfs_error_t load_structures(struct ndfs_filesystem *fs);
static ndfs_error_t write_bit_file(struct ndfs_filesystem *fs);
static void write_bit_file_pages(struct ndfs_filesystem *fs);
static ndfs_error_t write_user_page(struct ndfs_filesystem *fs, uint32_t user_index);
static ndfs_error_t write_object_page(struct ndfs_filesystem *fs, uint32_t object_index);
static void parse_path(const char *path,
                       char *user_name, size_t user_sz,
                       char *obj_name, size_t obj_sz,
                       char *file_type, size_t type_sz);
static int find_user_by_name(const struct ndfs_filesystem *fs, const char *name);
static int find_user_by_index(const struct ndfs_filesystem *fs, uint8_t idx);
static int find_object(const struct ndfs_filesystem *fs, const char *path);
static void str_toupper(char *s);
static ndfs_error_t read_object_data(const struct ndfs_filesystem *fs,
                                     const ndfs_object_entry_t *obj,
                                     uint8_t **out_data, size_t *out_size);
static void free_file_blocks(struct ndfs_filesystem *fs,
                             const ndfs_object_entry_t *obj);
static uint32_t count_real_data_pages_in_buffer(const uint8_t *file_data,
                                                size_t file_size,
                                                uint32_t data_pages,
                                                const ndfs_hole_list_t *holes);
static uint32_t count_real_data_pages_in_object(const struct ndfs_filesystem *fs,
                                                const ndfs_object_entry_t *obj);
static ndfs_error_t create_new_file(struct ndfs_filesystem *fs,
                                    const char *obj_name, const char *file_type,
                                    int user_idx,
                                    const uint8_t *file_data, size_t file_size,
                                    const ndfs_hole_list_t *holes,
                                    const uint16_t *access_bits);
static ndfs_error_t update_existing_file(struct ndfs_filesystem *fs,
                                         int obj_idx, int user_idx,
                                         const uint8_t *file_data, size_t file_size,
                                         const ndfs_hole_list_t *holes);
static ndfs_error_t write_file_impl(ndfs_filesystem_t *fs,
                                    const char *path,
                                    const uint8_t *file_data,
                                    size_t file_size,
                                    const ndfs_hole_list_t *holes,
                                    const uint16_t *access_bits);
static ndfs_error_t allocate_and_write_data(struct ndfs_filesystem *fs,
                                            const uint8_t *file_data, size_t file_size,
                                            uint32_t data_pages,
                                            uint32_t *out_top_block_id,
                                            ndfs_pointer_type_t *out_pointer_type,
                                            uint32_t *out_struct_pages,
                                            const ndfs_hole_list_t *holes);
static void add_object(struct ndfs_filesystem *fs, const ndfs_object_entry_t *obj);
static size_t objects_for_user_by_index(const struct ndfs_filesystem *fs, uint8_t idx);
static void persist_master_block(struct ndfs_filesystem *fs);

/* ── Helpers ─────────────────────────────────────────────────────── */

/* Push a dirty slot back to the backend, then mark it clean. Returns false
 * only if the backend's write_block fails (I/O error). A clean or invalid
 * slot is a no-op success. */
static bool cache_flush_slot(struct ndfs_filesystem *fs, ndfs_page_slot *slot)
{
    if (!slot->valid || !slot->dirty) return true;
    if (!fs->io.write_block) return false;   /* read-only backend, can't flush */
    if (!fs->io.write_block(fs->io.ctx, slot->page_id, slot->buf)) return false;
    slot->dirty = false;
    return true;
}

/* Flush every dirty slot (used by close and by ndfs_to_buffer). Returns the
 * first error encountered, or NDFS_OK. */
static ndfs_error_t cache_flush_all(struct ndfs_filesystem *fs)
{
    size_t i;
    for (i = 0; i < NDFS_CACHE_SLOTS; i++) {
        if (!cache_flush_slot(fs, &fs->cache[i])) return NDFS_ERR_IO;
    }
    return NDFS_OK;
}

/* Locate the slot already holding `page_id`, or NULL if not cached. */
static ndfs_page_slot *cache_find(struct ndfs_filesystem *fs, uint32_t page_id)
{
    size_t i;
    for (i = 0; i < NDFS_CACHE_SLOTS; i++) {
        if (fs->cache[i].valid && fs->cache[i].page_id == page_id) {
            return &fs->cache[i];
        }
    }
    return NULL;
}

/* Choose an eviction victim: prefer an invalid slot, else the least-recently
 * used UNPINNED slot. Returns NULL only if every slot is pinned (a caller
 * release-discipline bug -- can't happen while max simultaneous pins << slots). */
static ndfs_page_slot *cache_victim(struct ndfs_filesystem *fs)
{
    ndfs_page_slot *victim = NULL;
    size_t i;
    for (i = 0; i < NDFS_CACHE_SLOTS; i++) {
        ndfs_page_slot *s = &fs->cache[i];
        if (!s->valid) return s;                 /* free slot: best choice */
        if (s->pin > 0) continue;                /* pinned: never evict */
        if (!victim || s->lru < victim->lru) victim = s;
    }
    return victim;
}

/* Core cache access shared by read_page/write_page_ptr. Returns the slot
 * holding `page_id` (loading it via the backend if needed), or NULL on I/O
 * failure / exhausted cache. `for_write` marks the slot dirty. */
static ndfs_page_slot *cache_get(struct ndfs_filesystem *fs, uint32_t page_id,
                                 bool for_write)
{
    ndfs_page_slot *slot;

    if (page_id >= fs->total_pages) return NULL;   /* out of range, as before */

    slot = cache_find(fs, page_id);
    if (!slot) {
        slot = cache_victim(fs);
        if (!slot) return NULL;                    /* all slots pinned: bug */
        if (!cache_flush_slot(fs, slot)) return NULL;  /* evict old dirty page */
        if (!fs->io.read_block(fs->io.ctx, page_id, slot->buf)) return NULL;
        slot->page_id = page_id;
        slot->valid   = true;
        slot->dirty   = false;
        slot->pin     = 0;
    }
    slot->lru = ++fs->lru_clock;                   /* touch: most-recently used */
    if (for_write) slot->dirty = true;
    return slot;
}

/* Fetch page `block_id` for reading. The returned pointer is valid until the
 * slot is evicted; hold it across other page accesses only under a cache_pin
 * (see the page-cache note above).
 *
 * Takes `const fs` because reading logically doesn't change the filesystem,
 * but the page cache IS mutable state hung off the handle -- so we cast the
 * const away to touch it, the way a C++ `mutable` member would. The handle is
 * always a live heap object, never truly const, so this is safe. */
static const uint8_t *read_page(const struct ndfs_filesystem *fs, uint32_t block_id)
{
    struct ndfs_filesystem *m = (struct ndfs_filesystem *)(uintptr_t)fs;
    ndfs_page_slot *slot = cache_get(m, block_id, false);
    return slot ? slot->buf : NULL;
}

/* Fetch page `block_id` for writing (marks it dirty; flushed to the backend
 * on eviction/unpin/close). Same page_id as a concurrent read_page returns the
 * SAME slot, so in-place mutation followed by a re-read is coherent. */
static uint8_t *write_page_ptr(struct ndfs_filesystem *fs, uint32_t block_id)
{
    ndfs_page_slot *slot = cache_get(fs, block_id, true);
    if (slot && fs->undo.active) undo_capture(fs, block_id, slot->buf);
    return slot ? slot->buf : NULL;
}

/* Pin the slot holding `page_id` so LRU cannot evict it while a caller keeps
 * a live pointer into it across an unbounded loop of other page accesses.
 * The page must already be resident (call right after read_page/write_page_ptr
 * of the same id). No-op if not found (defensive). */
static void cache_pin(const struct ndfs_filesystem *fs, uint32_t page_id)
{
    struct ndfs_filesystem *m = (struct ndfs_filesystem *)(uintptr_t)fs;
    ndfs_page_slot *slot = cache_find(m, page_id);
    if (slot) slot->pin++;
}

/* Release a pin taken by cache_pin. Dirty pages are NOT flushed here -- they
 * stay dirty until eviction/close, which keeps surgical writes cheap. */
static void cache_unpin(const struct ndfs_filesystem *fs, uint32_t page_id)
{
    struct ndfs_filesystem *m = (struct ndfs_filesystem *)(uintptr_t)fs;
    ndfs_page_slot *slot = cache_find(m, page_id);
    if (slot && slot->pin > 0) slot->pin--;
}

static void str_toupper(char *s)
{
    while (*s) {
        *s = (char)toupper((unsigned char)*s);
        s++;
    }
}

static int strcasecmp_port(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = toupper((unsigned char)*a);
        int cb = toupper((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return toupper((unsigned char)*a) - toupper((unsigned char)*b);
}

static void parse_path(const char *path,
                       char *user_name, size_t user_sz,
                       char *obj_name, size_t obj_sz,
                       char *file_type, size_t type_sz)
{
    const char *p = path;
    const char *slash;
    const char *colon;
    const char *dot;
    const char *file_part;

    /* Skip leading slashes */
    while (*p == '/') p++;

    user_name[0] = '\0';
    obj_name[0]  = '\0';
    file_type[0] = '\0';

    /* Find slash separator */
    slash = strchr(p, '/');
    if (slash) {
        size_t ulen = (size_t)(slash - p);
        if (ulen >= user_sz) ulen = user_sz - 1;
        memcpy(user_name, p, ulen);
        user_name[ulen] = '\0';
        file_part = slash + 1;
        /* Skip trailing slashes */
        while (*file_part == '/') file_part++;
    } else {
        file_part = p;
    }

    /* Split filename into name and type */
    colon = strchr(file_part, ':');
    if (colon) {
        size_t nlen = (size_t)(colon - file_part);
        size_t tlen = strlen(colon + 1);
        if (nlen >= obj_sz) nlen = obj_sz - 1;
        memcpy(obj_name, file_part, nlen);
        obj_name[nlen] = '\0';
        if (tlen >= type_sz) tlen = type_sz - 1;
        memcpy(file_type, colon + 1, tlen);
        file_type[tlen] = '\0';
    } else {
        dot = strrchr(file_part, '.');
        if (dot) {
            size_t nlen = (size_t)(dot - file_part);
            size_t tlen = strlen(dot + 1);
            if (nlen >= obj_sz) nlen = obj_sz - 1;
            memcpy(obj_name, file_part, nlen);
            obj_name[nlen] = '\0';
            if (tlen >= type_sz) tlen = type_sz - 1;
            memcpy(file_type, dot + 1, tlen);
            file_type[tlen] = '\0';
        } else {
            size_t nlen = strlen(file_part);
            if (nlen >= obj_sz) nlen = obj_sz - 1;
            memcpy(obj_name, file_part, nlen);
            obj_name[nlen] = '\0';
        }
    }
}

static int find_user_by_name(const struct ndfs_filesystem *fs, const char *name)
{
    size_t i;
    for (i = 0; i < MAX_INTERNAL_USERS; i++) {
        if (fs->user_valid[i] &&
            strcasecmp_port(fs->users[i].user_name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int find_user_by_index(const struct ndfs_filesystem *fs, uint8_t idx)
{
    size_t i;
    for (i = 0; i < MAX_INTERNAL_USERS; i++) {
        if (fs->user_valid[i] && fs->users[i].user_index == idx) {
            return (int)i;
        }
    }
    return -1;
}

static int find_object(const struct ndfs_filesystem *fs, const char *path)
{
    char user_name[NDFS_NAME_MAX + 1];
    char obj_name[NDFS_NAME_MAX + 1];
    char file_type[NDFS_TYPE_MAX + 1];
    char full_name[NDFS_NAME_MAX + NDFS_TYPE_MAX + 2];
    char entry_full[NDFS_NAME_MAX + NDFS_TYPE_MAX + 2];
    size_t i;

    parse_path(path, user_name, sizeof(user_name),
               obj_name, sizeof(obj_name),
               file_type, sizeof(file_type));

    str_toupper(user_name);
    str_toupper(obj_name);
    str_toupper(file_type);

    if (file_type[0] != '\0') {
        snprintf(full_name, sizeof(full_name), "%s:%s", obj_name, file_type);
    } else {
        snprintf(full_name, sizeof(full_name), "%s", obj_name);
    }

    for (i = 0; i < fs->object_count; i++) {
        const ndfs_object_entry_t *o = &fs->objects[i];

        /* Filter by user if specified */
        if (user_name[0] != '\0' &&
            strcasecmp_port(o->user_name, user_name) != 0) {
            continue;
        }

        /* Build entry full name */
        if (o->type[0] != '\0') {
            snprintf(entry_full, sizeof(entry_full), "%s:%s",
                     o->object_name, o->type);
        } else {
            snprintf(entry_full, sizeof(entry_full), "%s", o->object_name);
        }

        if (strcasecmp_port(entry_full, full_name) == 0) return (int)i;

        /* Also try matching object name only (without type) */
        if (file_type[0] == '\0' &&
            strcasecmp_port(o->object_name, obj_name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static size_t objects_for_user_by_index(const struct ndfs_filesystem *fs, uint8_t idx)
{
    size_t count = 0;
    size_t i;
    for (i = 0; i < fs->object_count; i++) {
        if (fs->objects[i].user_index == idx) count++;
    }
    return count;
}

static void add_object(struct ndfs_filesystem *fs, const ndfs_object_entry_t *obj)
{
    if (fs->object_count >= fs->object_capacity) {
        size_t new_cap = fs->object_capacity == 0 ? 64 : fs->object_capacity * 2;
        ndfs_object_entry_t *new_arr = (ndfs_object_entry_t *)realloc(
            fs->objects, new_cap * sizeof(ndfs_object_entry_t));
        if (!new_arr) return;
        fs->objects = new_arr;
        fs->object_capacity = new_cap;
    }
    fs->objects[fs->object_count] = *obj;
    fs->object_count++;
}

/* Assign every entry the number SINTRAN reports - the 307 in "FILE 307".
 *
 * The number is logical_block*256 + slot_in_block, where **logical_block is the ORDINAL
 * RANK of the entry's index-block group among the groups that user occupies**, ascending
 * - NOT the physical group number.
 *
 * That distinction only shows up once an overflow block has been relocated, and SINTRAN
 * relocates them freely. Measured live on 2026-08-02: user 8 with 300 files held block 0
 * in group 0 and its overflow in group 2; creating files for user 136 (whose HOME is
 * group 2, slot 8) moved user 8's overflow to group 3, and then user 200 moved it to
 * group 4. All 300 files survived, reported by SINTRAN throughout as FILE 0..299.
 *
 * The old "block = page / 512" formula gave 0..255 then 1024..1067 for that user, where
 * SINTRAN says 256..299. It looked right on the earlier BIGMAN pack only because nothing
 * had displaced that pack's overflow block, so rank happened to equal group number.
 */
static void assign_file_numbers(struct ndfs_filesystem *fs)
{
    size_t i, j;

    for (i = 0; i < fs->object_count; i++) {
        uint8_t  user = fs->objects[i].user_index;
        uint32_t page = fs->objects[i].object_index / NDFS_ENTRIES_PER_PAGE;
        uint32_t group = page / NDFS_PAGES_PER_INDEX_BLOCK;
        uint32_t slot_base, offset_pages, slot;
        uint32_t rank = 0;

        /* Rank of this group among the groups this user occupies. */
        for (j = 0; j < fs->object_count; j++) {
            uint32_t other_page, other_group;
            if (fs->objects[j].user_index != user) continue;
            other_page  = fs->objects[j].object_index / NDFS_ENTRIES_PER_PAGE;
            other_group = other_page / NDFS_PAGES_PER_INDEX_BLOCK;
            if (other_group < group) {
                size_t k;
                int already = 0;
                for (k = 0; k < j; k++) {
                    uint32_t p2, g2;
                    if (fs->objects[k].user_index != user) continue;
                    p2 = fs->objects[k].object_index / NDFS_ENTRIES_PER_PAGE;
                    g2 = p2 / NDFS_PAGES_PER_INDEX_BLOCK;
                    if (g2 == other_group) { already = 1; break; }
                }
                if (!already) rank++;
            }
        }

        /* The slot WITHIN a group is (user % 64) - 64 users fill one 512-page group. */
        slot_base = (uint32_t)(user % NDFS_USERS_PER_INDEX_BLOCK) * NDFS_PAGES_PER_OBJECT_BLOCK;
        offset_pages = (page % NDFS_PAGES_PER_INDEX_BLOCK) - slot_base;

        if ((page % NDFS_PAGES_PER_INDEX_BLOCK) < slot_base
            || offset_pages >= NDFS_PAGES_PER_OBJECT_BLOCK) {
            fs->objects[i].file_number = 0u;   /* outside this user's slot */
            continue;
        }

        slot = offset_pages * NDFS_ENTRIES_PER_PAGE
             + (fs->objects[i].object_index % NDFS_ENTRIES_PER_PAGE);
        fs->objects[i].file_number = rank * NDFS_FILES_PER_OBJECT_BLOCK + slot;
    }
}

/* ââ Structure loading ââââââââââââââââââââââââââââââââââ */

/* Number of pages the bit file occupies on disk. SINTRAN sizes it from the directory's
 * declared capacity (Extended Info Block, pages available), not from the length of the
 * device: measured on large SMD formats whose device is longer than the capacity (for
 * example 153576 device pages, 140391 capacity: 9 bit file pages, not 10). Without a valid
 * capacity (floppies) the device length is used. */
static uint32_t bit_file_disk_pages(const ndfs_master_block_t *mb, uint32_t total_pages)
{
    uint32_t basis = total_pages;
    uint32_t bytes;
    if (mb->ext_valid && mb->ext_pages_available > 0 &&
        mb->ext_pages_available <= total_pages)
        basis = mb->ext_pages_available;
    bytes = (((basis + 7) / 8) + 1) & ~1u;
    return (bytes + NDFS_PAGE_SIZE - 1) / NDFS_PAGE_SIZE;
}

static ndfs_error_t load_structures(struct ndfs_filesystem *fs)
{
    const ndfs_master_block_t *mb = &fs->master_block;
    size_t i, j;
    int ui;

    /* Load user file. A user index page that cannot be read is not fatal: the
     * object file does not depend on the user file, and it is what names every
     * file on the disk. A floppy whose user index page is damaged used to load
     * as NDFS_ERR_CORRUPT and report nothing at all; it now loads with no user
     * names, and the objects keep their user index. */
    if (ndfs_bp_is_valid(&mb->user_file_ptr)) {
        const uint8_t *index_page = read_page(fs, mb->user_file_ptr.block_id);
        if (index_page) {
        /* index_page is held across the data-page reads below (up to 8 of
         * them) -- pin it so LRU can't evict its slot mid-loop. */
        cache_pin(fs, mb->user_file_ptr.block_id);

        /* Read up to 8 pointers from the index block */
        for (i = 0; i < NDFS_MAX_USER_FILE_PTRS; i++) {
            ndfs_block_pointer_t ptr = ndfs_bp_from_bytes(index_page, i * 4);
            const uint8_t *data_page;
            if (!ndfs_bp_is_valid(&ptr)) continue;

            data_page = read_page(fs, ptr.block_id);
            /* One unreadable user page costs only the users on it. */
            if (!data_page) { fs->damage.user_pages++; continue; }

            for (j = 0; j < NDFS_ENTRIES_PER_PAGE; j++) {
                ndfs_user_entry_t ue;
                ndfs_error_t err = ndfs_ue_from_bytes(data_page, NDFS_PAGE_SIZE,
                                                       j * NDFS_ENTRY_SIZE, &ue);
                if (err == NDFS_OK) {
                    size_t slot = ue.user_index;
                    if (slot < MAX_INTERNAL_USERS) {
                        fs->users[slot] = ue;
                        fs->user_valid[slot] = true;
                        fs->user_count++;
                    }
                }
            }
        }
        cache_unpin(fs, mb->user_file_ptr.block_id);
        }
    }

    /* Load object file */
    if (ndfs_bp_is_valid(&mb->object_file_ptr)) {
        if (mb->object_file_ptr.type == NDFS_PTR_INDEXED) {
            const uint8_t *idx_page = read_page(fs, mb->object_file_ptr.block_id);
            uint32_t global_idx = 0;
            if (!idx_page) return NDFS_ERR_CORRUPT;
            /* idx_page held across up to 512 data-page reads -- pin it. */
            cache_pin(fs, mb->object_file_ptr.block_id);

            for (i = 0; i < NDFS_MAX_OBJECT_FILE_PTRS; i++) {
                ndfs_block_pointer_t ptr = ndfs_bp_from_bytes(idx_page, i * 4);
                const uint8_t *dp;
                if (!ndfs_bp_is_valid(&ptr)) {
                    global_idx += NDFS_ENTRIES_PER_PAGE;
                    continue;
                }
                dp = read_page(fs, ptr.block_id);
                /* An unreadable data page loses its own 32 entries and
                 * nothing else. The slots are still counted, so every entry
                 * after it keeps the object index it has on the media. */
                if (!dp) { fs->damage.object_pages++;
                           global_idx += NDFS_ENTRIES_PER_PAGE; continue; }

                for (j = 0; j < NDFS_ENTRIES_PER_PAGE; j++) {
                    ndfs_object_entry_t oe;
                    ndfs_error_t err = ndfs_oe_from_bytes(dp, NDFS_PAGE_SIZE,
                                                          j * NDFS_ENTRY_SIZE, &oe);
                    if (err == NDFS_OK) {
                        oe.object_index = global_idx + (uint32_t)j;
                        /* Physical position above. The number SINTRAN shows is
                         * per-user and differs as soon as the user owns more
                         * than one object block, because block n sits a whole
                         * index block further on. */
                        {
                            int32_t fn = ndfs_oe_compute_file_number(oe.object_index,
                                                                     oe.user_index);
                            oe.file_number = (fn < 0) ? 0u : (uint32_t)fn;
                        }
                        /* Resolve user name */
                        ui = find_user_by_index(fs, oe.user_index);
                        if (ui >= 0) {
                            memcpy(oe.user_name, fs->users[ui].user_name,
                                   NDFS_NAME_MAX + 1);
                        }
                        add_object(fs, &oe);
                    }
                }
                global_idx += NDFS_ENTRIES_PER_PAGE;
            }
            cache_unpin(fs, mb->object_file_ptr.block_id);
        } else if (mb->object_file_ptr.type == NDFS_PTR_SUBINDEXED) {
            const uint8_t *sub_idx_page = read_page(fs, mb->object_file_ptr.block_id);
            uint32_t global_idx = 0;
            if (!sub_idx_page) return NDFS_ERR_CORRUPT;
            /* sub_idx_page held across the whole walk -- pin it. */
            cache_pin(fs, mb->object_file_ptr.block_id);

            for (i = 0; i < NDFS_MAX_OBJECT_FILE_PTRS; i++) {
                ndfs_block_pointer_t idx_ptr = ndfs_bp_from_bytes(sub_idx_page, i * 4);
                const uint8_t *idx_page;
                size_t k;
                /* Entry i covers object-file pages i * 512 on, set or not: an empty entry
                 * or a lost index page must not move the entries behind it down. */
                global_idx = (uint32_t)(i * NDFS_MAX_OBJECT_FILE_PTRS * NDFS_ENTRIES_PER_PAGE);
                if (!ndfs_bp_is_valid(&idx_ptr)) continue;

                idx_page = read_page(fs, idx_ptr.block_id);
                /* One lost index page must not cost the entries the other
                 * index pages still name. */
                if (!idx_page) { fs->damage.object_pages++; continue; }
                /* idx_page held across its 512 data-page reads -- pin it too
                 * (sub_idx + idx + data = the deepest 3-page live chain). */
                cache_pin(fs, idx_ptr.block_id);

                for (k = 0; k < NDFS_MAX_OBJECT_FILE_PTRS; k++) {
                    ndfs_block_pointer_t ptr = ndfs_bp_from_bytes(idx_page, k * 4);
                    const uint8_t *dp;
                    if (!ndfs_bp_is_valid(&ptr)) {
                        global_idx += NDFS_ENTRIES_PER_PAGE;
                        continue;
                    }
                    dp = read_page(fs, ptr.block_id);
                    /* Same rule under a sub-index: the lost page costs its own
                     * 32 entries only. */
                    if (!dp) { fs->damage.object_pages++;
                               global_idx += NDFS_ENTRIES_PER_PAGE; continue; }

                    for (j = 0; j < NDFS_ENTRIES_PER_PAGE; j++) {
                        ndfs_object_entry_t oe;
                        ndfs_error_t err = ndfs_oe_from_bytes(dp, NDFS_PAGE_SIZE,
                                                              j * NDFS_ENTRY_SIZE, &oe);
                        if (err == NDFS_OK) {
                            oe.object_index = global_idx + (uint32_t)j;
                            /* See the sibling loop above: physical position is
                             * not the user-visible file number once a user owns
                             * more than one object block. */
                            {
                                int32_t fn = ndfs_oe_compute_file_number(oe.object_index,
                                                                         oe.user_index);
                                oe.file_number = (fn < 0) ? 0u : (uint32_t)fn;
                            }
                            ui = find_user_by_index(fs, oe.user_index);
                            if (ui >= 0) {
                                memcpy(oe.user_name, fs->users[ui].user_name,
                                       NDFS_NAME_MAX + 1);
                            }
                            add_object(fs, &oe);
                        }
                    }
                    global_idx += NDFS_ENTRIES_PER_PAGE;
                }
                cache_unpin(fs, idx_ptr.block_id);
            }
            cache_unpin(fs, mb->object_file_ptr.block_id);
        }
    }

    /* Load bit file */
    if (ndfs_bp_is_valid(&mb->bit_file_ptr)) {
        uint32_t total_pages = (uint32_t)(fs->size / NDFS_PAGE_SIZE);
        /* Whole 16-bit words - the bit file is word addressed, so ceil(pages/8) cuts
         * the last word in half on any device whose page count is not a multiple of 16
         * and loses the pages living in its high byte. See bf_byte_index(). */
        uint32_t bitmap_bytes = (((total_pages + 7) / 8) + 1) & ~1u;
        uint32_t bitmap_pages = bit_file_disk_pages(mb, total_pages);
        ndfs_error_t err;

        err = ndfs_bf_init(&fs->bit_file, total_pages);
        if (err != NDFS_OK) return err;
        fs->bit_file.bit_address = mb->bit_file_ptr.block_id;

        /* Bound the ALLOCATION window by the directory's declared capacity
         * (ext_pages_available, words 1756B-1757B) when the extended-info block is valid.
         * SINTRAN's downward allocator never hands out a page above capacity-1; the gap
         * between capacity and the physical device is the drive's bad-sector spare region,
         * which stays free-but-unreachable.
         *
         * The `<= total_pages` clamp is load-bearing, not defensive noise: the real
         * Winchester WD0.img declares a capacity of 36396 pages in a file only 36360 pages
         * long. Without the clamp the allocator would start at page 36395, which does not
         * exist in the file.
         *
         * Falls back to the whole device when there is no valid extended info (FLOMON
         * floppies never have one). */
        if (mb->ext_valid &&
            mb->ext_pages_available > 0 &&
            mb->ext_pages_available <= total_pages) {
            fs->bit_file.alloc_ceiling = mb->ext_pages_available;
        }

        /* Read contiguous bitmap pages */
        {
            /* The in-memory bitmap covers the whole device; pages past the on-disk bit file
             * stay zero (free). */
            size_t tmp_len = (size_t)bitmap_pages * NDFS_PAGE_SIZE;
            uint8_t *tmp;
            if (tmp_len < bitmap_bytes) tmp_len = bitmap_bytes;
            tmp = (uint8_t *)calloc(tmp_len, 1);
            if (!tmp) return NDFS_ERR_ALLOC;

            for (i = 0; i < bitmap_pages; i++) {
                const uint8_t *page = read_page(fs, mb->bit_file_ptr.block_id + (uint32_t)i);
                /* A bitmap page that cannot be read leaves its own pages
                 * reading as free; it must not cost the file listing. */
                if (page) {
                    memcpy(tmp + i * NDFS_PAGE_SIZE, page, NDFS_PAGE_SIZE);
                } else {
                    fs->damage.bit_file_pages++;
                }
            }
            ndfs_bf_load(&fs->bit_file, tmp, bitmap_bytes);
            free(tmp);
        }
    }

    /* Only possible once every entry is loaded - see assign_file_numbers. */
    assign_file_numbers(fs);

    return NDFS_OK;
}

/* ── File data reading ───────────────────────────────────────────── */

static ndfs_error_t read_indexed_data(const struct ndfs_filesystem *fs,
                                      uint32_t index_block_id,
                                      uint32_t bytes_in_file,
                                      uint8_t *result)
{
    const uint8_t *index_page = read_page(fs, index_block_id);
    size_t bytes_read = 0;
    size_t i;

    if (!index_page) return NDFS_ERR_CORRUPT;
    /* index_page held across up to 512 data-page reads -- pin it. */
    cache_pin(fs, index_block_id);

    for (i = 0; i < NDFS_MAX_OBJECT_FILE_PTRS && bytes_read < bytes_in_file; i++) {
        ndfs_block_pointer_t ptr = ndfs_bp_from_bytes(index_page, i * 4);
        size_t to_copy = NDFS_PAGE_SIZE;
        if (to_copy > bytes_in_file - bytes_read) to_copy = bytes_in_file - bytes_read;

        if (ptr.block_id == 0) {
            /* Sparse hole: result is already zeroed */
            bytes_read += to_copy;
        } else {
            const uint8_t *page = read_page(fs, ptr.block_id);
            if (!page) { cache_unpin(fs, index_block_id); return NDFS_ERR_CORRUPT; }
            memcpy(result + bytes_read, page, to_copy);
            bytes_read += to_copy;
        }
    }
    cache_unpin(fs, index_block_id);
    return NDFS_OK;
}

static ndfs_error_t read_subindexed_data(const struct ndfs_filesystem *fs,
                                         uint32_t sub_index_block_id,
                                         uint32_t bytes_in_file,
                                         uint8_t *result)
{
    const uint8_t *sub_page = read_page(fs, sub_index_block_id);
    size_t bytes_read = 0;
    size_t si, i;

    if (!sub_page) return NDFS_ERR_CORRUPT;
    /* sub_page held across the whole walk; idx_page across each inner loop.
     * These two + the transient data page are the deepest 3-page live chain. */
    cache_pin(fs, sub_index_block_id);

    for (si = 0; si < NDFS_MAX_OBJECT_FILE_PTRS && bytes_read < bytes_in_file; si++) {
        ndfs_block_pointer_t idx_ptr = ndfs_bp_from_bytes(sub_page, si * 4);
        const uint8_t *idx_page;
        /* Entry si of the sub-index block covers file bytes from si * 512 pages on, whether
         * it is set or not. An empty entry is a hole of 512 pages (1 MB): the position must
         * still move past it, or every page behind it lands 1 MB too early. Measured
         * 2026-10-14 on a domain the ND Linker B01 wrote under SINTRAN III: entries 0, 2, 4
         * and 6 set, 1, 3 and 5 empty; the segments the domain header puts at 0x202000,
         * 0x402000 and 0x602000 came out at 0x102000, 0x202000 and 0x302000. */
        bytes_read = si * (size_t)NDFS_MAX_OBJECT_FILE_PTRS * NDFS_PAGE_SIZE;
        if (bytes_read >= bytes_in_file) break;
        if (!ndfs_bp_is_valid(&idx_ptr)) continue;

        idx_page = read_page(fs, idx_ptr.block_id);
        if (!idx_page) continue;
        cache_pin(fs, idx_ptr.block_id);

        for (i = 0; i < NDFS_MAX_OBJECT_FILE_PTRS && bytes_read < bytes_in_file; i++) {
            ndfs_block_pointer_t data_ptr = ndfs_bp_from_bytes(idx_page, i * 4);
            size_t to_copy = NDFS_PAGE_SIZE;
            if (to_copy > bytes_in_file - bytes_read) to_copy = bytes_in_file - bytes_read;

            if (data_ptr.block_id == 0) {
                bytes_read += to_copy;
            } else {
                const uint8_t *page = read_page(fs, data_ptr.block_id);
                if (!page) {
                    cache_unpin(fs, idx_ptr.block_id);
                    cache_unpin(fs, sub_index_block_id);
                    return NDFS_ERR_CORRUPT;
                }
                memcpy(result + bytes_read, page, to_copy);
                bytes_read += to_copy;
            }
        }
        cache_unpin(fs, idx_ptr.block_id);
    }
    cache_unpin(fs, sub_index_block_id);
    return NDFS_OK;
}

static ndfs_error_t read_object_data(const struct ndfs_filesystem *fs,
                                     const ndfs_object_entry_t *obj,
                                     uint8_t **out_data, size_t *out_size)
{
    uint8_t *result;

    if (!obj->file_pointer.block_id) {
        *out_data = NULL;
        *out_size = 0;
        return NDFS_OK;
    }

    result = (uint8_t *)calloc(obj->bytes_in_file, 1);
    if (!result) return NDFS_ERR_ALLOC;

    if (obj->file_pointer.type == NDFS_PTR_CONTIGUOUS) {
        size_t bytes_read = 0;
        uint32_t i;
        for (i = 0; i < obj->pages_in_file && bytes_read < obj->bytes_in_file; i++) {
            const uint8_t *page = read_page(fs, obj->file_pointer.block_id + i);
            size_t to_copy = NDFS_PAGE_SIZE;
            if (to_copy > obj->bytes_in_file - bytes_read) to_copy = obj->bytes_in_file - bytes_read;
            if (!page) { free(result); return NDFS_ERR_CORRUPT; }
            memcpy(result + bytes_read, page, to_copy);
            bytes_read += to_copy;
        }
    } else if (obj->file_pointer.type == NDFS_PTR_INDEXED) {
        ndfs_error_t err = read_indexed_data(fs, obj->file_pointer.block_id,
                                             obj->bytes_in_file, result);
        if (err != NDFS_OK) { free(result); return err; }
    } else if (obj->file_pointer.type == NDFS_PTR_SUBINDEXED) {
        ndfs_error_t err = read_subindexed_data(fs, obj->file_pointer.block_id,
                                                obj->bytes_in_file, result);
        if (err != NDFS_OK) { free(result); return err; }
    }

    *out_data = result;
    *out_size = obj->bytes_in_file;
    return NDFS_OK;
}

/* ── Block freeing ───────────────────────────────────────────────── */

static void free_file_blocks(struct ndfs_filesystem *fs,
                             const ndfs_object_entry_t *obj)
{
    size_t i;

    if (!obj->file_pointer.block_id) return;

    if (obj->file_pointer.type == NDFS_PTR_INDEXED) {
        const uint8_t *index_page = read_page(fs, obj->file_pointer.block_id);
        if (index_page) {
            for (i = 0; i < NDFS_MAX_OBJECT_FILE_PTRS; i++) {
                ndfs_block_pointer_t ptr = ndfs_bp_from_bytes(index_page, i * 4);
                if (ptr.block_id > 0) {
                    ndfs_bf_mark_free(&fs->bit_file, ptr.block_id);
                }
            }
        }
        ndfs_bf_mark_free(&fs->bit_file, obj->file_pointer.block_id);
    } else if (obj->file_pointer.type == NDFS_PTR_CONTIGUOUS) {
        ndfs_bf_free_range(&fs->bit_file, obj->file_pointer.block_id,
                           obj->pages_in_file);
    } else if (obj->file_pointer.type == NDFS_PTR_SUBINDEXED) {
        const uint8_t *sub_page = read_page(fs, obj->file_pointer.block_id);
        size_t si;
        if (sub_page) {
            /* sub_page held across each group-index read -- pin it. */
            cache_pin(fs, obj->file_pointer.block_id);
            for (si = 0; si < NDFS_MAX_OBJECT_FILE_PTRS; si++) {
                ndfs_block_pointer_t idx_ptr = ndfs_bp_from_bytes(sub_page, si * 4);
                const uint8_t *idx_page;
                if (!ndfs_bp_is_valid(&idx_ptr)) continue;
                idx_page = read_page(fs, idx_ptr.block_id);
                if (idx_page) {
                    for (i = 0; i < NDFS_MAX_OBJECT_FILE_PTRS; i++) {
                        ndfs_block_pointer_t dp = ndfs_bp_from_bytes(idx_page, i * 4);
                        if (dp.block_id > 0) ndfs_bf_mark_free(&fs->bit_file, dp.block_id);
                    }
                }
                ndfs_bf_mark_free(&fs->bit_file, idx_ptr.block_id);
            }
            cache_unpin(fs, obj->file_pointer.block_id);
        }
        ndfs_bf_mark_free(&fs->bit_file, obj->file_pointer.block_id);
    }
}

/* Count real (non-sparse) data-block pages currently belonging to `obj`, by
 * walking its resolved Indexed/SubIndexed/Contiguous block structure exactly
 * the way free_file_blocks() walks it to free them -- except this counts
 * non-zero (real) BlockPointers instead of freeing them, and never counts
 * the index/sub-index structural blocks themselves.
 *
 * This is the disk-truth of how many real pages a file occupies. User quota
 * (pages_used) must be based on this, not on `pages_in_file` (the LOGICAL
 * page count, which counts sparse holes as if they consumed real disk space
 * -- they do NOT, per docs/NDFS-FORMAT.md's "No disk space is allocated for
 * sparse holes") and never plus a flat structural-block estimate (the
 * index/sub-index blocks are filesystem overhead, not user data, and must
 * never count against a user's quota).
 *
 * Callers computing a REFUND/DECREMENT (delete, or update's old-side charge)
 * must call this BEFORE any free_file_blocks()/allocate_and_write_data() call
 * touches the same blocks, since those can overwrite the on-disk bytes this
 * function reads. */
static uint32_t count_real_data_pages_in_object(const struct ndfs_filesystem *fs,
                                                const ndfs_object_entry_t *obj)
{
    uint32_t count = 0;
    size_t i;

    if (!obj->file_pointer.block_id) return 0;

    if (obj->file_pointer.type == NDFS_PTR_CONTIGUOUS) {
        /* Contiguous files have no sparse holes -- every logical page is a
         * real, physically-allocated page. */
        return obj->pages_in_file;
    } else if (obj->file_pointer.type == NDFS_PTR_INDEXED) {
        const uint8_t *index_page = read_page(fs, obj->file_pointer.block_id);
        if (index_page) {
            for (i = 0; i < NDFS_MAX_OBJECT_FILE_PTRS; i++) {
                ndfs_block_pointer_t ptr = ndfs_bp_from_bytes(index_page, i * 4);
                if (ptr.block_id > 0) count++;
            }
        }
    } else if (obj->file_pointer.type == NDFS_PTR_SUBINDEXED) {
        const uint8_t *sub_page = read_page(fs, obj->file_pointer.block_id);
        size_t si;
        if (sub_page) {
            /* sub_page held across each group-index read -- pin it. */
            cache_pin(fs, obj->file_pointer.block_id);
            for (si = 0; si < NDFS_MAX_OBJECT_FILE_PTRS; si++) {
                ndfs_block_pointer_t idx_ptr = ndfs_bp_from_bytes(sub_page, si * 4);
                const uint8_t *idx_page;
                if (!ndfs_bp_is_valid(&idx_ptr)) continue;
                idx_page = read_page(fs, idx_ptr.block_id);
                if (idx_page) {
                    for (i = 0; i < NDFS_MAX_OBJECT_FILE_PTRS; i++) {
                        ndfs_block_pointer_t dp = ndfs_bp_from_bytes(idx_page, i * 4);
                        if (dp.block_id > 0) count++;
                    }
                }
            }
            cache_unpin(fs, obj->file_pointer.block_id);
        }
    }
    return count;
}

/* Index and sub-index blocks that belong to `obj` (the structure around its data pages).
 * Contiguous files have none; an Indexed file has one; a SubIndexed file has the top block
 * plus one block for each valid sub-index slot. SINTRAN charges these blocks to the owner's
 * quota like data pages (measured, docs/NDFS-ALLOCATION-RULES.md section 7). */
static uint32_t count_index_blocks_in_object(const struct ndfs_filesystem *fs,
                                             const ndfs_object_entry_t *obj)
{
    uint32_t count = 0;
    size_t si;

    if (!obj->file_pointer.block_id) return 0;
    if (obj->file_pointer.type == NDFS_PTR_INDEXED) return 1;
    if (obj->file_pointer.type == NDFS_PTR_SUBINDEXED) {
        const uint8_t *sub_page = read_page(fs, obj->file_pointer.block_id);
        if (!sub_page) return 1;
        count = 1;
        for (si = 0; si < NDFS_MAX_OBJECT_FILE_PTRS; si++) {
            ndfs_block_pointer_t idx_ptr = ndfs_bp_from_bytes(sub_page, si * 4);
            if (ndfs_bp_is_valid(&idx_ptr)) count++;
        }
    }
    return count;
}

/* Pages SINTRAN charges to the owner for one file: real data pages plus index blocks. */
static uint32_t count_charged_pages_in_object(const struct ndfs_filesystem *fs,
                                              const ndfs_object_entry_t *obj)
{
    return count_real_data_pages_in_object(fs, obj) + count_index_blocks_in_object(fs, obj);
}

/* Object data pages that belong to one user: the pointers in the user's 8 slots of the object
 * index (slot = user*8 .. user*8+7, in the user's group of 64). Each is charged to the user. */
static uint32_t count_user_object_pages(const struct ndfs_filesystem *fs, uint8_t user_index)
{
    const ndfs_master_block_t *mb = &fs->master_block;
    uint32_t group = (uint32_t)user_index / NDFS_USERS_PER_INDEX_BLOCK;
    uint32_t first = ((uint32_t)user_index % NDFS_USERS_PER_INDEX_BLOCK) * NDFS_PAGES_PER_OBJECT_BLOCK;
    uint32_t count = 0, k;
    const uint8_t *index_page = NULL;

    if (!ndfs_bp_is_valid(&mb->object_file_ptr)) return 0;
    if (mb->object_file_ptr.type == NDFS_PTR_INDEXED) {
        if (group != 0) return 0;
        index_page = read_page(fs, mb->object_file_ptr.block_id);
    } else if (mb->object_file_ptr.type == NDFS_PTR_SUBINDEXED) {
        const uint8_t *sub_page = read_page(fs, mb->object_file_ptr.block_id);
        ndfs_block_pointer_t gp;
        if (!sub_page) return 0;
        gp = ndfs_bp_from_bytes(sub_page, group * 4);
        if (!ndfs_bp_is_valid(&gp)) return 0;
        index_page = read_page(fs, gp.block_id);
    }
    if (!index_page) return 0;
    for (k = 0; k < NDFS_PAGES_PER_OBJECT_BLOCK; k++) {
        ndfs_block_pointer_t p = ndfs_bp_from_bytes(index_page, (first + k) * 4);
        if (ndfs_bp_is_valid(&p)) count++;
    }
    return count;
}

/* ── File creation/update ────────────────────────────────────────── */

/* Physical object-file position of a slot inside one of a user's object blocks.
 *
 * Object block `block` for user `user_index` occupies pages
 * block*512 + user*8 .. +7, and each page holds 32 entries, so:
 *
 *     page     = block*512 + user*8 + slot_in_block/32
 *     physical = page*32 + slot_in_block%32
 *
 * `slot_in_block` is 0..255. The value returned is what goes in
 * object_entry.object_index; the user-VISIBLE number is block*256 + slot_in_block
 * (see ndfs_oe_compute_file_number). Those two coincide only inside a user's
 * FIRST block, which is why the old flat user<<8 model looked right for years.
 */
static uint32_t physical_slot_in_group(uint8_t user_index, uint32_t group,
                                      uint32_t slot_in_block)
{
    /* A user always occupies slot (user % 64) of a group, eight pages wide. The HOME
     * group is user/64, and that pairing reduces to the flat page = user*8 for block 0. */
    uint32_t page = group * NDFS_PAGES_PER_INDEX_BLOCK
                  + (uint32_t)(user_index % NDFS_USERS_PER_INDEX_BLOCK)
                        * NDFS_PAGES_PER_OBJECT_BLOCK
                  + slot_in_block / NDFS_ENTRIES_PER_PAGE;
    return page * NDFS_ENTRIES_PER_PAGE + (slot_in_block % NDFS_ENTRIES_PER_PAGE);
}


/* True if this user has any entry in the given index-block group. */
static bool user_uses_group(const struct ndfs_filesystem *fs, uint8_t user_index,
                            uint32_t group)
{
    size_t i;
    for (i = 0; i < fs->object_count; i++) {
        uint32_t page;
        if (fs->objects[i].user_index != user_index) continue;
        page = fs->objects[i].object_index / NDFS_ENTRIES_PER_PAGE;
        if (page / NDFS_PAGES_PER_INDEX_BLOCK == group) return true;
    }
    return false;
}

/* True if any loaded object entry already sits at this physical position. */
static bool slot_in_use(const struct ndfs_filesystem *fs, uint32_t slot)
{
    size_t i;
    for (i = 0; i < fs->object_count; i++) {
        if (fs->objects[i].object_index == slot) return true;
    }
    return false;
}

/* Find the next free object slot for a user, across ALL their object blocks.
 *
 * A user area holds 256 files per object block, one by default and up to
 * `max_blocks` (16 max) once the operator has run @GIVE-OBJECT-BLOCKS. Blocks
 * are allocated on demand, so this searches every block already allocated and,
 * if they are all full, returns the first slot of the NEXT block while one is
 * still permitted.
 *
 * The caller must raise the user's allocated count when the returned slot falls
 * in a block beyond the current one, and must make the page exist
 * (ensure_object_dir_page). See allocate_user_object_slot below.
 *
 * Returns -1 when every permitted block is full - which now genuinely means the
 * user is at their SINTRAN limit rather than merely past 256 files. */
static int find_free_user_slot(const struct ndfs_filesystem *fs,
                               uint8_t user_index,
                               uint32_t max_blocks,
                               uint32_t allocated_blocks)
{
    uint32_t group, slot_in_block, home;

    if (max_blocks < 1) max_blocks = 1;
    if (max_blocks > NDFS_MAX_OBJECT_BLOCKS) max_blocks = NDFS_MAX_OBJECT_BLOCKS;
    if (allocated_blocks < 1) allocated_blocks = 1;
    if (allocated_blocks > max_blocks) allocated_blocks = max_blocks;

    home = (uint32_t)(user_index / NDFS_USERS_PER_INDEX_BLOCK);

    /* Groups this user already occupies, plus HOME (block 0, which must be searched even
     * when the user has no entries yet). Holes are normal - deleting a file leaves one and
     * SINTRAN never compacts. */
    for (group = home; group < NDFS_MAX_OBJECT_FILE_PTRS; group++) {
        if (group != home && !user_uses_group(fs, user_index, group)) continue;
        for (slot_in_block = 0; slot_in_block < NDFS_FILES_PER_OBJECT_BLOCK; slot_in_block++) {
            uint32_t phys = physical_slot_in_group(user_index, group, slot_in_block);
            if (!slot_in_use(fs, phys)) return (int)phys;
        }
    }

    /* Every occupied block is full. Open the next one if the maximum allows, searching
     * upward from home for a group whose slot is free AND is not an existing user's home.
     *
     * SINTRAN skips such a group (measured live 2026-08-02: user 8 overflowing past 256
     * files did not take group 1, where user 72 lives) and RELOCATES the block if the home
     * owner later needs it. This library instead never places a block in another user's
     * home, so it produces no layout needing relocation. */
    if (allocated_blocks >= max_blocks) return -1;

    for (group = home; group < NDFS_MAX_OBJECT_FILE_PTRS; group++) {
        uint32_t home_owner;
        uint32_t first;

        if (group == home || user_uses_group(fs, user_index, group)) continue;

        home_owner = group * NDFS_USERS_PER_INDEX_BLOCK
                   + (uint32_t)(user_index % NDFS_USERS_PER_INDEX_BLOCK);
        if (home_owner < NDFS_MAX_USERS
            && find_user_by_index(fs, (uint8_t)home_owner) >= 0) {
            continue;   /* that group's slot belongs to a user that exists */
        }

        first = physical_slot_in_group(user_index, group, 0);
        if (!slot_in_use(fs, first)) return (int)first;
    }

    return -1;
}

/* Pick a free object slot for `user`, opening a new object block if needed.
 *
 * Writes the chosen physical slot to *out_slot and returns NDFS_OK, or:
 *   NDFS_ERR_OBJ_BLOCKS_FULL   every permitted block full, more can still be granted
 *   NDFS_ERR_OBJ_BLOCKS_MAX    the user already holds all 16 blocks (4096 files)
 *
 * A user's blocks all sit at slot (user % 64) of an index-block group; block 0 is in the
 * HOME group user/64, and overflow blocks take the same slot in a HIGHER group, skipping
 * any group whose slot belongs to an existing user. find_free_user_slot does the search;
 * this records the resulting block count on the user entry, which byte 47's low nibble
 * carries.
 */
static int allocate_user_object_slot(struct ndfs_filesystem *fs,
                                     ndfs_user_entry_t *user,
                                     uint32_t *out_slot)
{
    int slot;
    uint32_t group, count = 0;

    slot = find_free_user_slot(fs, user->user_index,
                               user->max_object_blocks,
                               user->allocated_object_blocks);
    if (slot < 0) {
        return (user->max_object_blocks >= NDFS_MAX_OBJECT_BLOCKS)
             ? NDFS_ERR_OBJ_BLOCKS_MAX
             : NDFS_ERR_OBJ_BLOCKS_FULL;
    }

    /* The number of distinct groups this user will occupy IS the allocated block count. */
    for (group = 0; group < NDFS_MAX_OBJECT_FILE_PTRS; group++) {
        uint32_t chosen = ((uint32_t)slot / NDFS_ENTRIES_PER_PAGE)
                        / NDFS_PAGES_PER_INDEX_BLOCK;
        if (user_uses_group(fs, user->user_index, group) || group == chosen) count++;
    }
    if (count < 1) count = 1;
    if (count > user->max_object_blocks) count = user->max_object_blocks;
    user->allocated_object_blocks = (uint8_t)count;

    *out_slot = (uint32_t)slot;
    return NDFS_OK;
}

/* Ensure the object-file directory data page that holds object entry
 * `object_index` exists, allocating and linking it (like SINTRAN/RetroCore do
 * on demand) if the index pointer is null. Returns the data block id, or 0 on
 * failure. The page index in the object-file index block is object_index/32;
 * for a user that maps to index-pointer slots user*8 .. user*8+7. */
static uint32_t ensure_object_dir_page(struct ndfs_filesystem *fs,
                                       uint32_t object_index)
{
    ndfs_master_block_t *mb = &fs->master_block;
    uint32_t page_idx = object_index / NDFS_ENTRIES_PER_PAGE;
    uint8_t *index_page;
    ndfs_block_pointer_t ptr;
    uint32_t blk;
    uint8_t *dp;
    ndfs_block_pointer_t newp;
    /* The user who owns this directory page (8 pages per user, 64 users per index block). */
    uint32_t owner_page = object_index / NDFS_ENTRIES_PER_PAGE;
    uint32_t owner_user = (owner_page / NDFS_PAGES_PER_INDEX_BLOCK) * NDFS_USERS_PER_INDEX_BLOCK
                        + (owner_page % NDFS_PAGES_PER_INDEX_BLOCK) / NDFS_PAGES_PER_OBJECT_BLOCK;

    if (!ndfs_bp_is_valid(&mb->object_file_ptr)) return 0;

    if (mb->object_file_ptr.type == NDFS_PTR_INDEXED && page_idx >= NDFS_MAX_OBJECT_FILE_PTRS) {
        /* One-time conversion: a plain Indexed object file caps out at 512
         * directory pages (16,384 files across all users combined -- 64
         * users' worth at 8 reserved index-pointer slots each). Once a page
         * beyond that is needed, wrap the EXISTING Indexed block as group 0
         * of a new SubIndexed structure (sub-index block -> up to 512 group
         * index blocks -> up to 512 directory pages each) -- exactly like a
         * single file's own data grows past 512 pages. No directory data is
         * copied or moved: the old block's directory-page pointers for
         * pageIdx 0-511 stay exactly where they are; only a new level of
         * indirection is added above it. Mirrors EnsureObjectDirPage in the
         * C# reference implementation (RetroFS.NDFS/FileSystems/NdfsFileSystem.cs). */
        uint32_t old_index_block_id = mb->object_file_ptr.block_id;
        uint32_t sub_blk;
        uint8_t *sub_page;
        ndfs_block_pointer_t group0;

        if (ndfs_bf_find_free_hint(&fs->bit_file, &sub_blk) != NDFS_OK) return 0;
        ndfs_bf_mark_used(&fs->bit_file, sub_blk);
        sub_page = write_page_ptr(fs, sub_blk);
        if (!sub_page) return 0;
        memset(sub_page, 0, NDFS_PAGE_SIZE);

        /* Slot 0 of the new sub-index reuses the existing Indexed block
         * verbatim -- its Type stays Indexed, so the existing SubIndexed
         * read/write paths below (which expect each sub-index slot to point
         * at a plain Indexed group block) resolve it exactly as they would
         * any other group. */
        group0.block_id = old_index_block_id;
        group0.type = NDFS_PTR_INDEXED;
        ndfs_bp_to_bytes(&group0, sub_page, 0);

        /* Re-point the master block's object-file pointer at the new
         * sub-index block and persist it immediately, before falling through
         * into the (already-correct) SubIndexed handling below for the page
         * actually being requested. */
        mb->object_file_ptr.block_id = sub_blk;
        mb->object_file_ptr.type = NDFS_PTR_SUBINDEXED;
        persist_master_block(fs);
    }

    if (mb->object_file_ptr.type == NDFS_PTR_INDEXED) {
        if (page_idx >= NDFS_MAX_OBJECT_FILE_PTRS) return 0;
        index_page = write_page_ptr(fs, mb->object_file_ptr.block_id);
        if (!index_page) return 0;
        ptr = ndfs_bp_from_bytes(index_page, page_idx * 4);
        if (ndfs_bp_is_valid(&ptr)) return ptr.block_id;
    } else if (mb->object_file_ptr.type == NDFS_PTR_SUBINDEXED) {
        uint32_t sub_idx = page_idx / NDFS_MAX_OBJECT_FILE_PTRS;
        uint32_t inner = page_idx % NDFS_MAX_OBJECT_FILE_PTRS;
        uint8_t *sub_page = write_page_ptr(fs, mb->object_file_ptr.block_id);
        ndfs_block_pointer_t sub_ptr;
        if (!sub_page) return 0;
        sub_ptr = ndfs_bp_from_bytes(sub_page, sub_idx * 4);
        if (!ndfs_bp_is_valid(&sub_ptr)) {
            /* Allocate a new inner index block and link it in the sub-index. */
            if (ndfs_bf_find_free_hint(&fs->bit_file, &blk) != NDFS_OK) return 0;
            ndfs_bf_mark_used(&fs->bit_file, blk);
            dp = write_page_ptr(fs, blk);
            if (!dp) return 0;
            memset(dp, 0, NDFS_PAGE_SIZE);
            newp.block_id = blk;
            newp.type = NDFS_PTR_CONTIGUOUS;
            sub_page = write_page_ptr(fs, mb->object_file_ptr.block_id);
            ndfs_bp_to_bytes(&newp, sub_page, sub_idx * 4);
            sub_ptr = newp;
        }
        index_page = write_page_ptr(fs, sub_ptr.block_id);
        if (!index_page) return 0;
        ptr = ndfs_bp_from_bytes(index_page, inner * 4);
        if (ndfs_bp_is_valid(&ptr)) return ptr.block_id;
        page_idx = inner; /* offset within the inner index block */
    } else {
        return 0;
    }

    /* Allocate + link a new directory data page. SINTRAN charges it to the owner. */
    if (ndfs_bf_find_free_hint(&fs->bit_file, &blk) != NDFS_OK) return 0;
    ndfs_bf_mark_used(&fs->bit_file, blk);
    {
        int owner_slot = find_user_by_index(fs, (uint8_t)owner_user);
        if (owner_slot >= 0) fs->users[owner_slot].pages_used++;
    }
    dp = write_page_ptr(fs, blk);
    if (!dp) return 0;
    memset(dp, 0, NDFS_PAGE_SIZE);
    newp.block_id = blk;
    newp.type = NDFS_PTR_CONTIGUOUS;
    /* Re-fetch the index page pointer and write the link. */
    if (mb->object_file_ptr.type == NDFS_PTR_INDEXED) {
        index_page = write_page_ptr(fs, mb->object_file_ptr.block_id);
        ndfs_bp_to_bytes(&newp, index_page, (object_index / NDFS_ENTRIES_PER_PAGE) * 4);
    } else {
        ndfs_bp_to_bytes(&newp, index_page, page_idx * 4);
    }
    return blk;
}

/* Ensure the UserFile data page that holds user `user_index` exists,
 * allocating and linking it (mirrors ensure_object_dir_page above) if the
 * index pointer is null. Unlike the object file, the UserFile index block
 * is always plain NDFS_PTR_INDEXED -- 256 users max fits in a single
 * 512-pointer index page (see docs/NDFS-FORMAT.md, "User File | Indexed
 * (01)"), so there is no SubIndexed case to handle here. Returns the
 * on-disk data block id backing the page, or 0 on failure.
 *
 * Without this, ndfs_add_user() for a user whose page (user_index >= 32 on
 * an image only pre-allocated with its first UserFile page) was never
 * allocated would call write_user_page(), which silently no-ops when the
 * index slot is unset -- the new user lives only in fs->users[]/
 * fs->user_valid[] for the rest of this session and vanishes on the next
 * mount. */
static uint32_t ensure_user_dir_page(struct ndfs_filesystem *fs, uint32_t user_index)
{
    ndfs_master_block_t *mb = &fs->master_block;
    uint32_t page_idx = user_index / NDFS_ENTRIES_PER_PAGE;
    uint8_t *index_page;
    ndfs_block_pointer_t ptr;
    uint32_t blk;
    uint8_t *dp;
    ndfs_block_pointer_t newp;

    if (!ndfs_bp_is_valid(&mb->user_file_ptr)) return 0;
    if (page_idx >= NDFS_MAX_USER_FILE_PTRS) return 0;

    index_page = write_page_ptr(fs, mb->user_file_ptr.block_id);
    if (!index_page) return 0;
    ptr = ndfs_bp_from_bytes(index_page, page_idx * 4);
    if (ndfs_bp_is_valid(&ptr)) return ptr.block_id;

    /* Page not yet allocated -- grab a free block from the bitmap, zero it,
     * and link it into the UserFile index block at this page's slot. */
    if (ndfs_bf_find_free_hint(&fs->bit_file, &blk) != NDFS_OK) return 0;
    ndfs_bf_mark_used(&fs->bit_file, blk);
    dp = write_page_ptr(fs, blk);
    if (!dp) return 0;
    memset(dp, 0, NDFS_PAGE_SIZE);
    newp.block_id = blk;
    newp.type = NDFS_PTR_CONTIGUOUS;
    /* Re-fetch the index page after allocating the data block above, rather
     * than reusing the earlier pointer. With the block-IO page cache this
     * re-fetch returns the SAME cache slot (the cache is keyed by page_id), so
     * it is coherent and needs no pin -- and it stays correct no matter how the
     * backend stores pages. (Previously both pointers were fixed offsets into
     * the one resident fs->data image buffer.) */
    index_page = write_page_ptr(fs, mb->user_file_ptr.block_id);
    if (!index_page) return 0;
    ndfs_bp_to_bytes(&newp, index_page, page_idx * 4);
    return blk;
}

/* True when `page` is named in the caller-supplied hole list. A NULL or empty
 * list means the file has no holes -- the normal case. The list is ascending,
 * but this scans linearly because hole lists are short (a handful of entries)
 * and a binary search would be harder to read for no measurable gain. */
static bool page_is_declared_hole(const ndfs_hole_list_t *holes, uint32_t page)
{
    size_t i;

    if (!holes || !holes->pages) return false;
    for (i = 0; i < holes->count; i++) {
        if (holes->pages[i] == page) return true;
        if (holes->pages[i] > page) break; /* ascending: no point looking further */
    }
    return false;
}

/* Write one data page and store its BlockPointer at slot `slot_in_index` of
 * `index_page_buf` (an already-mapped 2048-byte index page).
 *
 * A page becomes a sparse hole ONLY when the caller names it in `holes`. The
 * content of the page never decides this.
 *
 * It used to: any page that was entirely zero and fully within the file was
 * silently turned into a hole. That was wrong, and wrong in a way this library
 * could not see -- reading such a file back through ndfs_read_file() returns
 * the same bytes either way. SINTRAN is not so forgiving. READ-BI and
 * READ-PROGFILE stop at a hole with
 *
 *     NO SUCH PAGE
 *     ERROR IN ACCESSING INPUT FILE
 *
 * so a :BPUN or :PROG copied onto a pack this way is a file the machine refuses
 * to load. Measured on a real SINTRAN III VSX-K pack, 2026-08-18: the four
 * (TCP-IP)TCP-SER-B0..B3 PIOC bank images came in with 0/48/62/53 invented
 * holes; bank 0 loaded and banks 1-3 all failed with NO SUCH PAGE. The same
 * files on genuine ND media have no holes at all -- a 128 KB bank dump is fully
 * allocated even where the bank is empty.
 *
 * Holes are still real and still supported; they just have to be declared,
 * which is what the XAT sidecar's "ndfs.holes" records them for. */
static ndfs_error_t write_data_page_to_index(struct ndfs_filesystem *fs,
                                             const uint8_t *file_data, size_t file_size,
                                             uint32_t data_page_index,
                                             uint8_t *index_page_buf,
                                             uint32_t slot_in_index,
                                             const ndfs_hole_list_t *holes)
{
    size_t page_offset = (size_t)data_page_index * NDFS_PAGE_SIZE;
    size_t page_end = page_offset + NDFS_PAGE_SIZE;
    size_t slice_len;

    if (page_end > file_size) page_end = file_size;
    slice_len = page_end > page_offset ? page_end - page_offset : 0;

    if (page_is_declared_hole(holes, data_page_index)) {
        /* Sparse hole: BlockPointer 0, no disk space consumed. */
        ndfs_write_u32be(index_page_buf, slot_in_index * 4, 0);
    } else {
        uint32_t data_block_id;
        uint8_t *data_page_buf;
        ndfs_block_pointer_t data_ptr;
        ndfs_error_t err;

        err = ndfs_bf_find_free_block(&fs->bit_file, &data_block_id);
        if (err != NDFS_OK) return NDFS_ERR_NO_SPACE;
        ndfs_bf_mark_used(&fs->bit_file, data_block_id);

        data_page_buf = write_page_ptr(fs, data_block_id);
        if (!data_page_buf) return NDFS_ERR_CORRUPT;
        memset(data_page_buf, 0, NDFS_PAGE_SIZE);
        if (slice_len > 0) memcpy(data_page_buf, file_data + page_offset, slice_len);

        data_ptr.block_id = data_block_id;
        data_ptr.type = NDFS_PTR_CONTIGUOUS;
        ndfs_bp_to_bytes(&data_ptr, index_page_buf, slot_in_index * 4);
    }
    return NDFS_OK;
}

/* Count how many of a file's `data_pages` logical pages are NOT sparse holes
 * -- i.e. how many pages write_data_page_to_index() will actually allocate a
 * real disk block for, given the exact same (file_data, file_size,
 * data_page_index) it is called with. Deliberately takes the already-decided
 * `data_pages` (post "if 0 then 1" clamp, as computed by every call site
 * before invoking allocate_and_write_data()) rather than recomputing it from
 * file_size, so this can never silently disagree with what actually gets
 * allocated -- notably for an empty (0-byte) file, which still allocates
 * exactly one real page (see write_data_page_to_index's slice_len != full
 * page case), not zero.
 *
 * User quota (pages_used) must be based on this count, never on the LOGICAL
 * page count (which counts sparse holes as if they consumed real disk space
 * -- they do NOT, per docs/NDFS-FORMAT.md's "No disk space is allocated for
 * sparse holes") and never plus a flat structural index/sub-index block
 * estimate (filesystem overhead, not user data -- never charged to a user's
 * quota). */
static uint32_t count_real_data_pages_in_buffer(const uint8_t *file_data,
                                                size_t file_size,
                                                uint32_t data_pages,
                                                const ndfs_hole_list_t *holes)
{
    uint32_t real_pages = 0;
    uint32_t p;

    (void)file_data;
    (void)file_size;

    for (p = 0; p < data_pages; p++) {
        /* Mirrors write_data_page_to_index() exactly: a page is a hole only
         * when the caller declared it one. The page's contents do not enter
         * into it, which is why file_data/file_size are unused here now. */
        if (!page_is_declared_hole(holes, p)) {
            real_pages++;
        }
    }
    return real_pages;
}

/* Allocate and write the data-block structure for a file of `data_pages`
 * pages, choosing between the plain Indexed layout (<=512 pages: one index
 * block with up to 512 data-page pointers) and the SubIndexed layout (up to
 * 262,144 pages: a top-level sub-index block pointing at up to 512 group
 * index blocks, each holding up to 512 data-page pointers) per
 * NDFS-FORMAT.md "Sub-Indexed (Type 10)". This mirrors the already-audited
 * TS port's allocateAndWriteData()/writeDataPageToIndex() algorithm shape.
 *
 * On success, *out_top_block_id and *out_pointer_type are what the caller must
 * store as the object entry's file_pointer, and *out_struct_pages is the
 * number of structural (non-data) pages consumed -- 1 for Indexed, or
 * 1 + ceil(data_pages/512) for SubIndexed (the sub-index block plus one
 * group index block per 512-page group) -- which the caller must add to
 * data_pages for quota/pages_used accounting. */
static ndfs_error_t allocate_and_write_data(struct ndfs_filesystem *fs,
                                            const uint8_t *file_data, size_t file_size,
                                            uint32_t data_pages,
                                            uint32_t *out_top_block_id,
                                            ndfs_pointer_type_t *out_pointer_type,
                                            uint32_t *out_struct_pages,
                                            const ndfs_hole_list_t *holes)
{
    ndfs_error_t err;
    uint32_t i;

    if (data_pages > NDFS_MAX_OBJECT_FILE_PAGES) return NDFS_ERR_NO_SPACE;

    if (data_pages <= NDFS_MAX_OBJECT_FILE_PTRS) {
        /* Plain Indexed: a single index block holds all data-page pointers. */
        uint32_t index_block_id;
        uint8_t *index_page_buf;

        err = ndfs_bf_find_free_hint(&fs->bit_file, &index_block_id);
        if (err != NDFS_OK) return NDFS_ERR_NO_SPACE;
        ndfs_bf_mark_used(&fs->bit_file, index_block_id);

        index_page_buf = write_page_ptr(fs, index_block_id);
        if (!index_page_buf) return NDFS_ERR_CORRUPT;
        memset(index_page_buf, 0, NDFS_PAGE_SIZE);

        /* index_page_buf is written into after each data page is allocated
         * (write_data_page_to_index does write_page_ptr on the data block, then
         * ndfs_bp_to_bytes into this buffer) -- pin it so its dirty slot can't
         * be evicted mid-loop and its pointer stays live. */
        cache_pin(fs, index_block_id);
        for (i = 0; i < data_pages; i++) {
            err = write_data_page_to_index(fs, file_data, file_size, i,
                                           index_page_buf, i, holes);
            if (err != NDFS_OK) { cache_unpin(fs, index_block_id); return err; }
        }
        cache_unpin(fs, index_block_id);

        *out_top_block_id = index_block_id;
        *out_pointer_type = NDFS_PTR_INDEXED;
        *out_struct_pages = 1;
        return NDFS_OK;
    }

    /* SubIndexed: sub-index block -> ceil(data_pages/512) group index
     * blocks -> up to 512 data pages each. */
    {
        uint32_t sub_index_block_id;
        uint8_t *sub_index_buf;
        uint32_t num_index_blocks =
            (data_pages + NDFS_MAX_OBJECT_FILE_PTRS - 1) / NDFS_MAX_OBJECT_FILE_PTRS;
        uint32_t struct_pages = 1; /* the sub-index block itself */
        uint32_t si;

        err = ndfs_bf_find_free_hint(&fs->bit_file, &sub_index_block_id);
        if (err != NDFS_OK) return NDFS_ERR_NO_SPACE;
        ndfs_bf_mark_used(&fs->bit_file, sub_index_block_id);

        /* sub_index_buf is held and written into across EVERY group-index and
         * data-page allocation below (ndfs_bp_to_bytes at si*4 each loop), so
         * its slot must survive the whole walk -- pin it. (Before the block-IO
         * seam this pointer was a fixed offset into the resident image buffer
         * and needed no protection; now it lives in an evictable cache slot.) */
        sub_index_buf = write_page_ptr(fs, sub_index_block_id);
        if (!sub_index_buf) return NDFS_ERR_CORRUPT;
        memset(sub_index_buf, 0, NDFS_PAGE_SIZE);
        cache_pin(fs, sub_index_block_id);

        for (si = 0; si < num_index_blocks; si++) {
            uint32_t idx_block_id;
            uint8_t *idx_page_buf;
            ndfs_block_pointer_t idx_ptr;
            uint32_t start_page = si * NDFS_MAX_OBJECT_FILE_PTRS;
            uint32_t end_page = start_page + NDFS_MAX_OBJECT_FILE_PTRS;
            uint32_t p;

            if (end_page > data_pages) end_page = data_pages;

            err = ndfs_bf_find_free_hint(&fs->bit_file, &idx_block_id);
            if (err != NDFS_OK) { cache_unpin(fs, sub_index_block_id); return NDFS_ERR_NO_SPACE; }
            ndfs_bf_mark_used(&fs->bit_file, idx_block_id);
            struct_pages++;

            idx_ptr.block_id = idx_block_id;
            idx_ptr.type = NDFS_PTR_CONTIGUOUS;
            ndfs_bp_to_bytes(&idx_ptr, sub_index_buf, si * 4);

            idx_page_buf = write_page_ptr(fs, idx_block_id);
            if (!idx_page_buf) { cache_unpin(fs, sub_index_block_id); return NDFS_ERR_CORRUPT; }
            memset(idx_page_buf, 0, NDFS_PAGE_SIZE);
            /* idx_page_buf held across its data-page allocations -- pin it too
             * (sub_index + group-index + data = the deepest 3-page chain). */
            cache_pin(fs, idx_block_id);

            for (p = start_page; p < end_page; p++) {
                err = write_data_page_to_index(fs, file_data, file_size, p,
                                               idx_page_buf, p - start_page, holes);
                if (err != NDFS_OK) {
                    cache_unpin(fs, idx_block_id);
                    cache_unpin(fs, sub_index_block_id);
                    return err;
                }
            }
            cache_unpin(fs, idx_block_id);
        }
        cache_unpin(fs, sub_index_block_id);

        *out_top_block_id = sub_index_block_id;
        *out_pointer_type = NDFS_PTR_SUBINDEXED;
        *out_struct_pages = struct_pages;
        return NDFS_OK;
    }
}

static ndfs_error_t create_new_file(struct ndfs_filesystem *fs,
                                    const char *obj_name, const char *file_type,
                                    int user_slot,
                                    const uint8_t *file_data, size_t file_size,
                                    const ndfs_hole_list_t *holes,
                                    const uint16_t *access_bits)
{
    uint32_t data_pages = (uint32_t)((file_size + NDFS_PAGE_SIZE - 1) / NDFS_PAGE_SIZE);
    uint32_t top_block_id;
    ndfs_pointer_type_t pointer_type;
    uint32_t struct_pages;
    uint32_t obj_index;
    ndfs_object_entry_t entry;
    ndfs_error_t err;

    if (data_pages == 0) data_pages = 1;
    /* NDFS_MAX_OBJECT_FILE_PAGES (262,144) is the true ceiling: a SubIndexed
     * sub-index block only has 512 pointer slots, so beyond that the file
     * genuinely cannot be represented. allocate_and_write_data() also checks
     * this, but bail out early before touching the object table. */
    if (data_pages > NDFS_MAX_OBJECT_FILE_PAGES) return NDFS_ERR_NO_SPACE;

    /* Choose the object slot inside the OWNING USER's region (SINTRAN
     * partitions the object file: user U owns slots U*256..U*256+255 and the
     * object-index high byte is the owner). A flat global slot would land the
     * file in the wrong user. Make sure that user's directory page exists. */
    {
        int alloc_err = allocate_user_object_slot(fs, &fs->users[user_slot], &obj_index);
        if (alloc_err != NDFS_OK) return alloc_err;
    }
    if (ensure_object_dir_page(fs, obj_index) == 0) return NDFS_ERR_NO_SPACE;

    /* Allocate + write the data-block structure: plain Indexed for
     * <=512 pages, or SubIndexed (sub-index -> group index blocks -> data
     * pages) for larger files. */
    err = allocate_and_write_data(fs, file_data, file_size, data_pages,
                                  &top_block_id, &pointer_type, &struct_pages, holes);
    if (err != NDFS_OK) return err;

    /* Create object entry */
    ndfs_oe_init(&entry);
    entry.object_index = obj_index;
    {
        size_t nlen = strlen(obj_name);
        if (nlen > NDFS_NAME_MAX) nlen = NDFS_NAME_MAX;
        memcpy(entry.object_name, obj_name, nlen);
        entry.object_name[nlen] = '\0';
        str_toupper(entry.object_name);
    }
    {
        size_t tlen = strlen(file_type);
        if (tlen > NDFS_TYPE_MAX) tlen = NDFS_TYPE_MAX;
        memcpy(entry.type, file_type, tlen);
        entry.type[tlen] = '\0';
        str_toupper(entry.type);
    }
    entry.user_index = fs->users[user_slot].user_index;
    /* The number SINTRAN shows is NOT the physical position: a user's files can
     * span several object blocks, each a whole index block apart. Compute it here
     * as well as on load, or a freshly created file reports number 0 until the
     * image is reloaded. */
    entry.file_number = (uint32_t)ndfs_oe_compute_file_number(entry.object_index,
                                                              entry.user_index);
    memcpy(entry.user_name, fs->users[user_slot].user_name, NDFS_NAME_MAX + 1);
    /* pages_in_file counts the pages ACTUALLY ALLOCATED, not the logical extent.
     *
     * Corrected 2026-07-30 to match SINTRAN. On a real pack (SYSTEM)S3-CONFIG-E01:PROG
     * records 89 while its index spans 99 slots with 10 holes, and the machine itself
     * reports "No of pages in file: 89" while transferring page numbers 0..53 then
     * 64..98. Writing the logical extent charged the user quota for holes that occupy
     * no disk, and produced packs disagreeing with SINTRAN-written ones.
     *
     * The logical extent is still ceil(bytes_in_file / NDFS_PAGE_SIZE), which is what
     * ndfs_get_file_blocks uses to walk the index. */
    entry.pages_in_file = count_real_data_pages_in_buffer(file_data, file_size, data_pages, holes);
    entry.bytes_in_file = file_size > 0 ? (uint32_t)file_size : 1;
    entry.file_pointer.block_id = top_block_id;
    entry.file_pointer.type = pointer_type;
    /* A new file inherits the OWNING USER's default file access, which is what
     * SINTRAN does (@SET-DEFAULT-FILE-ACCESS sets the value copied onto every
     * file the user then creates). It used to be a flat NDFS_ACCESS_DEFAULT
     * (0x03FF: owner and friends everything, PUBLIC nothing), which ignored the
     * user entry and left the file unreadable by anyone else -- including
     * SYSTEM, which is neither owner nor friend of an ordinary user. That bites
     * immediately in practice: the RT-LOADER runs as SYSTEM, so a :BPUN copied
     * into a user area could not be read by the very thing meant to load it.
     * A brand-new user gets 0x04FF (see ndfs_ue_init), which grants PUBLIC READ.
     *
     * Falls back to NDFS_ACCESS_DEFAULT only when the user entry carries no
     * default at all, so a pack whose user entries predate this still behaves
     * as before rather than creating files nobody may touch.
     *
     * No separate SubIndexed file-type-flag bit exists (mirrors the TS port),
     * so FT_INDEXED covers both Indexed and SubIndexed layouts.
     *
     * An EXPLICIT access_bits always wins over inheritance: inheriting is
     * right for a file created on a live machine, but wrong for distribution
     * media, where the owning user's default may grant PUBLIC nothing and
     * make every file invisible to SYSTEM. See ndfs_write_file_full() and
     * NDFS_ACCESS_PUBLIC_READ (object_entry.h) for the measurement. */
    if (access_bits) {
        entry.access_bits = (uint16_t)(*access_bits & 0x7FFFu);
    } else {
        entry.access_bits = fs->users[user_slot].default_file_access
                                ? (uint16_t)(fs->users[user_slot].default_file_access & 0x7FFFu)
                                : NDFS_ACCESS_DEFAULT;
    }
    entry.file_type_flags = NDFS_FT_INDEXED;
    /* object_index already encodes [user|fileEntry] (it was chosen inside the
     * user's region). The object-index word and a single-version file's
     * self-referential next/prev version all equal it. */
    entry.disk_object_index = (uint16_t)obj_index;
    entry.next_version = (uint16_t)obj_index;
    entry.prev_version = (uint16_t)obj_index;

    add_object(fs, &entry);

    /* Update user pages used. Quota tracks only REAL disk consumption:
     * sparse (all-zero) pages allocate no block and the index/sub-index
     * structural blocks are filesystem overhead, never charged against the
     * user -- see count_real_data_pages_in_buffer(). struct_pages is still
     * needed as an out-param of allocate_and_write_data() above, but is no
     * longer part of the quota charge. */
    fs->users[user_slot].pages_used +=
        count_real_data_pages_in_buffer(file_data, file_size, data_pages, holes) + struct_pages;

    /* Surgical writes: the new object's directory page, the owner's user page,
     * and the allocation bitmap. Index/data blocks were written above. */
    write_object_page(fs, obj_index);
    write_user_page(fs, fs->users[user_slot].user_index);
    write_bit_file(fs);

    return NDFS_OK;
}

/* Write into an existing contiguous file, in place and within its original
 * reservation.
 *
 * The run is never reallocated, so unlike the indexed path this neither frees
 * nor claims blocks, and the user's page count does not move: the pages were
 * charged at creation and stay charged until the file is deleted.
 *
 * Returns NDFS_ERR_NO_SPACE when the data needs more pages than the file
 * reserved -- a contiguous file cannot grow, because the pages after the run
 * belong to other files. SINTRAN behaves the same way. */
static ndfs_error_t update_existing_contiguous_file(struct ndfs_filesystem *fs,
                                                    ndfs_object_entry_t *existing,
                                                    const uint8_t *file_data,
                                                    size_t file_size)
{
    uint32_t reserved_pages = existing->pages_in_file;
    uint32_t needed_pages =
        (uint32_t)((file_size + NDFS_PAGE_SIZE - 1) / NDFS_PAGE_SIZE);
    uint32_t start_block;
    uint32_t i;

    if (needed_pages > reserved_pages) return NDFS_ERR_NO_SPACE;
    if (existing->file_pointer.block_id == 0) return NDFS_ERR_CORRUPT;

    start_block = existing->file_pointer.block_id;

    /* Write the run straight through. EVERY page of the reservation is
     * rewritten, not just the pages the data covers, so bytes left over from a
     * previous longer write cannot reappear past the new end of file. */
    for (i = 0; i < reserved_pages; i++) {
        uint8_t *page = write_page_ptr(fs, start_block + i);
        size_t offset = (size_t)i * NDFS_PAGE_SIZE;
        size_t count = 0;

        if (!page) return NDFS_ERR_IO;

        memset(page, 0, NDFS_PAGE_SIZE);
        if (offset < file_size) {
            count = file_size - offset;
            if (count > NDFS_PAGE_SIZE) count = NDFS_PAGE_SIZE;
            memcpy(page, file_data + offset, count);
        }
    }

    existing->bytes_in_file = (uint32_t)file_size;

    /* pages_in_file stays at the reservation. For a contiguous file the pages
     * are allocated whether or not the data reaches them, so counting only the
     * written pages would under-report the disk this file actually holds. */
    write_object_page(fs, existing->object_index);

    return NDFS_OK;
}

static ndfs_error_t update_existing_file(struct ndfs_filesystem *fs,
                                         int obj_idx, int user_slot,
                                         const uint8_t *file_data, size_t file_size,
                                         const ndfs_hole_list_t *holes)
{
    ndfs_object_entry_t *existing = &fs->objects[obj_idx];

    /* A contiguous file keeps the run it was created with. It is never
     * converted to an indexed file behind the caller's back, and it never
     * grows. Checked before anything else is computed, and written as a
     * declaration initialiser so the block below stays a pure declaration
     * list -- the file is compiled with declaration-after-statement warnings
     * on. The pages were charged when the run was allocated, so user_slot
     * plays no part on this path. */
    const bool is_contiguous = (existing->file_type_flags & NDFS_FT_CONTIGUOUS) != 0;

    /* Real (non-sparse) pages actually charged against quota for the file's
     * OLD contents -- computed BEFORE anything is freed or reallocated,
     * since free_file_blocks()/allocate_and_write_data() below can overwrite
     * the very on-disk bytes count_real_data_pages_in_object() reads. This
     * replaces the old flat "pages_in_file + structural block(s)" estimate,
     * which overcounted sparse holes as real usage and always charged for
     * index/sub-index structural blocks that are filesystem overhead, not
     * user data -- the exact leak class already found and fixed in the C#
     * port's AllocateFileBlocksSparse. */
    uint32_t old_real_pages = count_charged_pages_in_object(fs, existing);
    uint32_t data_pages;
    uint32_t top_block_id;
    ndfs_pointer_type_t pointer_type;
    uint32_t struct_pages;
    ndfs_error_t err;

    if (is_contiguous) {
        (void)old_real_pages;
        (void)user_slot;
        return update_existing_contiguous_file(fs, existing, file_data, file_size);
    }

    /* Validate the new size BEFORE freeing the old blocks, so a rejected
     * write leaves the existing file (and its accounting) untouched. */
    data_pages = (uint32_t)((file_size + NDFS_PAGE_SIZE - 1) / NDFS_PAGE_SIZE);
    if (data_pages == 0) data_pages = 1;
    if (data_pages > NDFS_MAX_OBJECT_FILE_PAGES) return NDFS_ERR_NO_SPACE;

    /* Free old blocks */
    free_file_blocks(fs, existing);
    fs->users[user_slot].pages_used =
        fs->users[user_slot].pages_used >= old_real_pages
            ? fs->users[user_slot].pages_used - old_real_pages : 0;

    /* Allocate + write the new data-block structure: plain Indexed for
     * <=512 pages, or SubIndexed for larger files. */
    err = allocate_and_write_data(fs, file_data, file_size, data_pages,
                                  &top_block_id, &pointer_type, &struct_pages, holes);
    if (err != NDFS_OK) return err;

    /* Update existing entry */
    /* Allocated count, not the logical extent - see the note at the create site. */
    existing->pages_in_file = count_real_data_pages_in_buffer(file_data, file_size, data_pages, holes);
    existing->bytes_in_file = file_size > 0 ? (uint32_t)file_size : 1;
    existing->file_pointer.block_id = top_block_id;
    existing->file_pointer.type = pointer_type;
    /* No separate SubIndexed file-type-flag bit exists (mirrors the TS port,
     * which also uses FT_INDEXED for any non-contiguous layout); access bits,
     * dates and versioning are intentionally preserved. */
    existing->file_type_flags = NDFS_FT_INDEXED;

    /* Charge only real (non-sparse) pages of the NEW content. struct_pages
     * is still needed as an out-param of allocate_and_write_data() above,
     * but is no longer part of the quota charge -- see
     * count_real_data_pages_in_buffer(). */
    fs->users[user_slot].pages_used +=
        count_real_data_pages_in_buffer(file_data, file_size, data_pages, holes) + struct_pages;

    /* Surgical writes: the file's object page, the owner's user page, and the
     * allocation bitmap (old blocks freed + new blocks allocated above). */
    write_object_page(fs, existing->object_index);
    write_user_page(fs, fs->users[user_slot].user_index);
    write_bit_file(fs);

    return NDFS_OK;
}

/* ── Surgical metadata writes ─────────────────────────────────────────
 *
 * NDFS writes are immediate and surgical (matching RetroCommander/RetroCore):
 * a mutation rewrites ONLY the block(s) it actually touched — never the whole
 * filesystem. Each helper rebuilds a single 2048-byte page from the in-memory
 * model and writes it into the image buffer. Rebuilding a page zero-filled
 * also clears freed slots, so a deleted file does not reappear on reload. */

/* Patch the 32-byte master block (page 0, offset NDFS_MASTER_BLOCK_OFFSET)
 * with the current free-page count, WITHOUT touching anything else on page 0
 * -- in particular the Extended Info Block (offset NDFS_EXTENDED_INFO_OFFSET,
 * a completely separate 16-byte structure) and its checksum, which RetroCore
 * (the golden reference for this project) never rewrites. Real SINTRAN reads
 * master_block.unreserved_pages to report free space without rescanning the
 * bit file, so this cached count must be kept in sync with the live bitmap
 * on every allocation/free -- otherwise it silently goes stale. */
static void persist_master_block(struct ndfs_filesystem *fs)
{
    uint8_t *page0;

    /* Unreserved Pages is the pool for user quotas, not a free page count. SINTRAN keeps it as the
     * pages a directory can hand out (capacity - 1 - bit file pages - 3: page 0, the user file's
     * index block and data page, the object file's index block, and the object data page) minus the
     * sum of what the users have reserved. Measured with SINTRAN M on 9 runs, see
     * docs/NDFS-ALLOCATION-RULES.md section 7a. Setting it to the free page count made DIR show more
     * unreserved pages than the directory has. */
    {
        uint32_t total = (uint32_t)(fs->size / NDFS_PAGE_SIZE);
        uint32_t cap = fs->bit_file.alloc_ceiling ? fs->bit_file.alloc_ceiling : total;
        uint32_t bmp = bit_file_disk_pages(&fs->master_block, total);
        uint64_t sum = 0;
        size_t u;
        uint64_t avail;

        for (u = 0; u < MAX_INTERNAL_USERS; u++) {
            if (fs->user_valid[u]) sum += fs->users[u].pages_reserved;
        }
        avail = (uint64_t)cap > (uint64_t)bmp + 4u ? (uint64_t)cap - bmp - 4u : 0u;
        fs->master_block.unreserved_pages = (uint32_t)(avail > sum ? avail - sum : 0u);
    }

    page0 = write_page_ptr(fs, 0);
    if (!page0) return;

    /* ndfs_mb_write() only clears/rewrites the NDFS_MASTER_BLOCK_SIZE (32)
     * bytes at NDFS_MASTER_BLOCK_OFFSET -- see master_block.c. It never
     * touches the Extended Info Block or its checksum at
     * NDFS_EXTENDED_INFO_OFFSET, so those bytes are left exactly as they
     * were on disk. */
    ndfs_mb_write(&fs->master_block, page0);
}

/* Write the BitFile allocation bitmap (contiguous; a page or two for most
 * disks). The bitmap is small, so it is written whole when allocation
 * changed — that mirrors the C# reference, which writes the affected BitFile
 * blocks on every allocate/free. */
/* Write the in-memory bitmap back to its disk pages (and nothing else). */
static void write_bit_file_pages(struct ndfs_filesystem *fs)
{
    const ndfs_master_block_t *mb = &fs->master_block;
    const uint8_t *bm_data;
    size_t bm_len;
    uint32_t bm_pages, i;

    if (!ndfs_bp_is_valid(&mb->bit_file_ptr)) return;

    ndfs_bf_get_data(&fs->bit_file, &bm_data, &bm_len);
    bm_pages = bit_file_disk_pages(mb, (uint32_t)(fs->size / NDFS_PAGE_SIZE));

    for (i = 0; i < bm_pages; i++) {
        uint8_t *page = write_page_ptr(fs, mb->bit_file_ptr.block_id + i);
        size_t src_off = (size_t)i * NDFS_PAGE_SIZE;
        size_t copy_len = NDFS_PAGE_SIZE;
        if (!page) continue;
        memset(page, 0, NDFS_PAGE_SIZE);
        if (src_off + copy_len > bm_len) copy_len = bm_len - src_off;
        if (copy_len > 0 && src_off < bm_len) memcpy(page, bm_data + src_off, copy_len);
    }
}

static ndfs_error_t write_bit_file(struct ndfs_filesystem *fs)
{
    if (!ndfs_bp_is_valid(&fs->master_block.bit_file_ptr)) return NDFS_OK;

    write_bit_file_pages(fs);

    /* Every call site of write_bit_file() follows an actual allocate/free
     * (create/update/delete file), so this is exactly the choke point where
     * the cached free-page count in the master block goes stale and must be
     * refreshed. Doing it here (rather than at each call site) guarantees no
     * mutation path can forget it. */
    persist_master_block(fs);

    return NDFS_OK;
}

/* Write the single UserFile data page that holds user `user_index`
 * (page = user_index / 32), rebuilt from the in-memory user entries. */
static ndfs_error_t write_user_page(struct ndfs_filesystem *fs, uint32_t user_index)
{
    const ndfs_master_block_t *mb = &fs->master_block;
    uint32_t page_idx = user_index / NDFS_ENTRIES_PER_PAGE;
    const uint8_t *index_block;
    ndfs_block_pointer_t ptr;
    uint8_t *page;
    size_t j;

    if (!ndfs_bp_is_valid(&mb->user_file_ptr)) return NDFS_OK;
    if (page_idx >= NDFS_MAX_USER_FILE_PTRS) return NDFS_OK;

    index_block = read_page(fs, mb->user_file_ptr.block_id);
    if (!index_block) return NDFS_OK;
    ptr = ndfs_bp_from_bytes(index_block, page_idx * 4);
    if (!ndfs_bp_is_valid(&ptr)) return NDFS_OK;

    page = write_page_ptr(fs, ptr.block_id);
    if (!page) return NDFS_ERR_CORRUPT;

    /* Rebuild the page zero-filled so a removed user's slot is cleared. */
    memset(page, 0, NDFS_PAGE_SIZE);
    for (j = 0; j < NDFS_ENTRIES_PER_PAGE; j++) {
        uint32_t uidx = page_idx * NDFS_ENTRIES_PER_PAGE + (uint32_t)j;
        if (uidx < MAX_INTERNAL_USERS && fs->user_valid[uidx]) {
            ndfs_ue_to_bytes(&fs->users[uidx], page + j * NDFS_ENTRY_SIZE);
        }
    }
    return NDFS_OK;
}

/* Resolve the on-disk data block backing ObjectFile page `page_idx`, or 0. */
static uint32_t object_page_block(const struct ndfs_filesystem *fs, uint32_t page_idx)
{
    const ndfs_master_block_t *mb = &fs->master_block;

    if (!ndfs_bp_is_valid(&mb->object_file_ptr)) return 0;

    if (mb->object_file_ptr.type == NDFS_PTR_INDEXED) {
        const uint8_t *idx = read_page(fs, mb->object_file_ptr.block_id);
        ndfs_block_pointer_t ptr;
        if (!idx || page_idx >= NDFS_MAX_OBJECT_FILE_PTRS) return 0;
        ptr = ndfs_bp_from_bytes(idx, page_idx * 4);
        return ndfs_bp_is_valid(&ptr) ? ptr.block_id : 0;
    } else if (mb->object_file_ptr.type == NDFS_PTR_SUBINDEXED) {
        uint32_t sub_idx = page_idx / NDFS_MAX_OBJECT_FILE_PTRS;
        uint32_t inner_idx = page_idx % NDFS_MAX_OBJECT_FILE_PTRS;
        const uint8_t *sub = read_page(fs, mb->object_file_ptr.block_id);
        ndfs_block_pointer_t sub_ptr, data_ptr;
        const uint8_t *inner;
        if (!sub) return 0;
        sub_ptr = ndfs_bp_from_bytes(sub, sub_idx * 4);
        if (!ndfs_bp_is_valid(&sub_ptr)) return 0;
        inner = read_page(fs, sub_ptr.block_id);
        if (!inner) return 0;
        data_ptr = ndfs_bp_from_bytes(inner, inner_idx * 4);
        return ndfs_bp_is_valid(&data_ptr) ? data_ptr.block_id : 0;
    }
    return 0;
}

/* Write the single ObjectFile data page that holds object `object_index`
 * (page = object_index / 32). Rebuilt zero-filled from the in-memory objects,
 * which clears any slot freed by a delete (so it does not reappear on
 * reload — folding in the old clear_object_slot behaviour). */
static ndfs_error_t write_object_page(struct ndfs_filesystem *fs, uint32_t object_index)
{
    uint32_t page_idx = object_index / NDFS_ENTRIES_PER_PAGE;
    uint32_t data_block = object_page_block(fs, page_idx);
    uint8_t *page;
    size_t i;

    if (data_block == 0) return NDFS_OK;

    page = write_page_ptr(fs, data_block);
    if (!page) return NDFS_ERR_CORRUPT;

    memset(page, 0, NDFS_PAGE_SIZE);
    for (i = 0; i < fs->object_count; i++) {
        if (fs->objects[i].object_index / NDFS_ENTRIES_PER_PAGE == page_idx) {
            uint32_t slot = fs->objects[i].object_index % NDFS_ENTRIES_PER_PAGE;
            ndfs_oe_to_bytes(&fs->objects[i], page, NDFS_PAGE_SIZE,
                             slot * NDFS_ENTRY_SIZE);
        }
    }
    return NDFS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════ */

/* ── Built-in buffer backend ─────────────────────────────────────────
 *
 * Wraps a flat in-RAM image; read_block/write_block are plain memcpy in/out.
 * This is the library's historical behaviour, kept as one backend so the whole
 * host test-suite drives the block-IO seam on every run.  ndfs_open_buffer /
 * ndfs_open_buffer_copy install it; the only difference is who owns `data`. */
typedef struct {
    uint8_t *data;
    size_t   size;   /* whole-page working size = total_pages * NDFS_PAGE_SIZE */
    bool     owns;   /* true => free(data) on destroy (the _copy opener) */
} ndfs_buffer_ctx;

static bool buffer_read_block(void *ctx, uint32_t page_id, uint8_t *out)
{
    ndfs_buffer_ctx *b = (ndfs_buffer_ctx *)ctx;
    size_t off = (size_t)page_id * NDFS_PAGE_SIZE;
    if (off + NDFS_PAGE_SIZE > b->size) return false;
    memcpy(out, b->data + off, NDFS_PAGE_SIZE);
    return true;
}

static bool buffer_write_block(void *ctx, uint32_t page_id, const uint8_t *in)
{
    ndfs_buffer_ctx *b = (ndfs_buffer_ctx *)ctx;
    size_t off = (size_t)page_id * NDFS_PAGE_SIZE;
    if (off + NDFS_PAGE_SIZE > b->size) return false;
    memcpy(b->data + off, in, NDFS_PAGE_SIZE);
    return true;
}

static void buffer_destroy(void *ctx)
{
    ndfs_buffer_ctx *b = (ndfs_buffer_ctx *)ctx;
    if (!b) return;
    if (b->owns) free(b->data);
    free(b);
}

/* Common open path: bind a block backend + page count, parse the master block
 * and load the directory structures.  Takes the ndfs_block_io by value and
 * OWNS it from here on: on any failure it invokes io.destroy (if set) so the
 * caller never has to clean up a backend it already handed over. */
static ndfs_error_t open_internal(ndfs_block_io io, uint32_t total_pages,
                                  bool unaligned, bool read_only,
                                  ndfs_filesystem_t **out_fs)
{
    struct ndfs_filesystem *fs;
    const uint8_t *page0;
    ndfs_error_t err;

    if (!io.read_block || !out_fs) {
        if (io.destroy) io.destroy(io.ctx);
        return NDFS_ERR_NULL_PTR;
    }
    if (total_pages == 0) {
        if (io.destroy) io.destroy(io.ctx);
        return NDFS_ERR_TOO_SMALL;
    }

    fs = (struct ndfs_filesystem *)calloc(1, sizeof(*fs));
    if (!fs) {
        if (io.destroy) io.destroy(io.ctx);
        return NDFS_ERR_ALLOC;
    }

    fs->io          = io;                                   /* fs now owns the backend */
    fs->total_pages = total_pages;
    fs->size        = (size_t)total_pages * NDFS_PAGE_SIZE; /* whole pages only */
    fs->unaligned   = unaligned;
    /* Refuse mutation of an unaligned image (writing whole pages would not
       round-trip the dropped tail) or of a backend with no write_block.
       Reads always work.  calloc left the cache all-invalid. */
    fs->read_only   = read_only || unaligned || (io.write_block == NULL);

    /* From here every failure goes through ndfs_close(), which flushes (nothing
       dirty yet), frees the bit file/objects, and calls the backend destroy. */
    page0 = read_page(fs, 0);
    err = page0 ? ndfs_mb_parse(page0, &fs->master_block) : NDFS_ERR_IO;
    if (err != NDFS_OK) { ndfs_close(fs); return err; }

    if (!ndfs_mb_is_valid(&fs->master_block)) {
        ndfs_close(fs);
        return NDFS_ERR_INVALID_IMAGE;
    }
    fs->master_block.image_size = total_pages;

    err = load_structures(fs);
    if (err != NDFS_OK) { ndfs_close(fs); return err; }

    *out_fs = fs;
    return NDFS_OK;
}

ndfs_error_t ndfs_open_block(const ndfs_block_io *io, uint32_t total_pages,
                             bool read_only, ndfs_filesystem_t **out_fs)
{
    /* `io` itself must be non-NULL so we can dereference it (and reach any
       destroy hook).  EVERY other check -- NULL read_block, zero total_pages,
       NULL out_fs -- is delegated to open_internal precisely because it invokes
       io.destroy on failure.  Handling them here with a bare `return` would
       leak a backend that had already opened a resource: e.g. ndfs_open_file on
       a sub-page image (total_pages == 0) would strand its FILE* and ctx. */
    if (!io) return NDFS_ERR_NULL_PTR;
    /* Block backends always deal in whole pages, so never "unaligned".  The
       vtable is copied by value; the caller keeps ownership of io->ctx unless
       it supplied a destroy hook. */
    return open_internal(*io, total_pages, false, read_only, out_fs);
}

ndfs_error_t ndfs_open_buffer(const uint8_t *data, size_t size,
                              bool read_only,
                              ndfs_filesystem_t **out_fs)
{
    ndfs_buffer_ctx *ctx;
    ndfs_block_io io;
    uint32_t total_pages;
    bool unaligned;

    if (!data || !out_fs) return NDFS_ERR_NULL_PTR;
    if (size < NDFS_PAGE_SIZE) return NDFS_ERR_TOO_SMALL;

    /* Tolerate images that aren't a whole multiple of the page size (a common
       dump artefact): work on whole pages only, exactly as before. */
    total_pages = (uint32_t)(size / NDFS_PAGE_SIZE);
    unaligned   = (size % NDFS_PAGE_SIZE != 0);

    ctx = (ndfs_buffer_ctx *)malloc(sizeof(*ctx));
    if (!ctx) return NDFS_ERR_ALLOC;
    /* Cast away const: caller guarantees the buffer stays alive; we never free
       it (owns = false).  read_only, if set, still gates every write. */
    ctx->data = (uint8_t *)(uintptr_t)data;
    ctx->size = (size_t)total_pages * NDFS_PAGE_SIZE;
    ctx->owns = false;

    io.read_block  = buffer_read_block;
    io.write_block = buffer_write_block;
    io.destroy     = buffer_destroy;
    io.ctx         = ctx;

    return open_internal(io, total_pages, unaligned, read_only, out_fs);
}

ndfs_error_t ndfs_open_buffer_copy(const uint8_t *data, size_t size,
                                   bool read_only,
                                   ndfs_filesystem_t **out_fs)
{
    ndfs_buffer_ctx *ctx;
    ndfs_block_io io;
    uint32_t total_pages;
    bool unaligned;
    uint8_t *copy;

    if (!data || !out_fs) return NDFS_ERR_NULL_PTR;
    if (size < NDFS_PAGE_SIZE) return NDFS_ERR_TOO_SMALL;

    total_pages = (uint32_t)(size / NDFS_PAGE_SIZE);
    unaligned   = (size % NDFS_PAGE_SIZE != 0);

    copy = (uint8_t *)malloc(size);
    if (!copy) return NDFS_ERR_ALLOC;
    memcpy(copy, data, size);

    ctx = (ndfs_buffer_ctx *)malloc(sizeof(*ctx));
    if (!ctx) { free(copy); return NDFS_ERR_ALLOC; }
    ctx->data = copy;
    ctx->size = (size_t)total_pages * NDFS_PAGE_SIZE;
    ctx->owns = true;   /* library owns the copy; buffer_destroy frees it */

    io.read_block  = buffer_read_block;
    io.write_block = buffer_write_block;
    io.destroy     = buffer_destroy;
    io.ctx         = ctx;

    /* On failure open_internal calls buffer_destroy, which frees copy + ctx. */
    return open_internal(io, total_pages, unaligned, read_only, out_fs);
}

void ndfs_close(ndfs_filesystem_t *fs)
{
    if (!fs) return;
    /* Write-back cache: push any unflushed pages to the backend before teardown.
       (For read-only handles and the in-RAM buffer this is effectively free.) */
    cache_flush_all(fs);
    ndfs_bf_destroy(&fs->bit_file);
    free(fs->objects);
    if (fs->io.destroy) fs->io.destroy(fs->io.ctx);
    free(fs);
}

/* ── Read operations ─────────────────────────────────────────────── */

ndfs_error_t ndfs_get_master_block(const ndfs_filesystem_t *fs,
                                   const ndfs_master_block_t **out)
{
    if (!fs || !out) return NDFS_ERR_NULL_PTR;
    *out = &fs->master_block;
    return NDFS_OK;
}

ndfs_error_t ndfs_get_directory_name(const ndfs_filesystem_t *fs,
                                     char *buf, size_t buf_len)
{
    size_t len;

    if (!fs || !buf) return NDFS_ERR_NULL_PTR;
    if (buf_len < NDFS_NAME_MAX + 1) return NDFS_ERR_TOO_SMALL;

    len = strlen(fs->master_block.directory_name);
    memcpy(buf, fs->master_block.directory_name, len + 1);
    return NDFS_OK;
}

ndfs_error_t ndfs_list_directory(const ndfs_filesystem_t *fs,
                                 const char *path,
                                 ndfs_file_entry_t **out_entries,
                                 size_t *out_count)
{
    const char *normalized;
    char norm_buf[256];
    ndfs_file_entry_t *entries;
    size_t count = 0;
    size_t i;

    if (!fs || !out_entries || !out_count) return NDFS_ERR_NULL_PTR;

    /* Normalize: skip leading/trailing slashes */
    if (!path) path = "";
    normalized = path;
    while (*normalized == '/') normalized++;
    {
        size_t len = strlen(normalized);
        if (len >= sizeof(norm_buf)) len = sizeof(norm_buf) - 1;
        memcpy(norm_buf, normalized, len);
        norm_buf[len] = '\0';
        while (len > 0 && norm_buf[len - 1] == '/') { norm_buf[--len] = '\0'; }
    }

    if (norm_buf[0] == '\0') {
        /* Root: list users as directories */
        size_t valid_count = 0;
        for (i = 0; i < MAX_INTERNAL_USERS; i++) {
            if (fs->user_valid[i]) valid_count++;
        }
        if (valid_count == 0) {
            *out_entries = NULL;
            *out_count = 0;
            return NDFS_OK;
        }
        entries = (ndfs_file_entry_t *)calloc(valid_count, sizeof(ndfs_file_entry_t));
        if (!entries) return NDFS_ERR_ALLOC;

        for (i = 0; i < MAX_INTERNAL_USERS; i++) {
            if (!fs->user_valid[i]) continue;
            memcpy(entries[count].name, fs->users[i].user_name, NDFS_NAME_MAX + 1);
            entries[count].type[0] = '\0';
            memcpy(entries[count].full_name, fs->users[i].user_name, NDFS_NAME_MAX + 1);
            memcpy(entries[count].user_name, fs->users[i].user_name, NDFS_NAME_MAX + 1);
            entries[count].size = 0;
            entries[count].pages = 0;
            entries[count].is_directory = true;
            count++;
        }
        *out_entries = entries;
        *out_count = count;
    } else {
        /* User directory: list files */
        char user_upper[NDFS_NAME_MAX + 1];
        size_t file_count = 0;
        size_t nlen = strlen(norm_buf);
        if (nlen > NDFS_NAME_MAX) nlen = NDFS_NAME_MAX;
        memcpy(user_upper, norm_buf, nlen);
        user_upper[nlen] = '\0';
        str_toupper(user_upper);

        /* Count matches */
        for (i = 0; i < fs->object_count; i++) {
            if (strcasecmp_port(fs->objects[i].user_name, user_upper) == 0)
                file_count++;
        }

        if (file_count == 0) {
            *out_entries = NULL;
            *out_count = 0;
            return NDFS_OK;
        }

        entries = (ndfs_file_entry_t *)calloc(file_count, sizeof(ndfs_file_entry_t));
        if (!entries) return NDFS_ERR_ALLOC;

        for (i = 0; i < fs->object_count; i++) {
            const ndfs_object_entry_t *obj = &fs->objects[i];
            if (strcasecmp_port(obj->user_name, user_upper) != 0) continue;

            memcpy(entries[count].name, obj->object_name, NDFS_NAME_MAX + 1);
            memcpy(entries[count].type, obj->type, NDFS_TYPE_MAX + 1);
            ndfs_oe_full_name(obj, entries[count].full_name,
                              sizeof(entries[count].full_name));
            memcpy(entries[count].user_name, obj->user_name, NDFS_NAME_MAX + 1);
            entries[count].size = obj->bytes_in_file;
            entries[count].pages = obj->pages_in_file;
            entries[count].is_directory = false;
            count++;
        }
        *out_entries = entries;
        *out_count = count;
    }

    return NDFS_OK;
}

void ndfs_free_entries(ndfs_file_entry_t *entries)
{
    free(entries);
}

ndfs_error_t ndfs_read_file(const ndfs_filesystem_t *fs,
                            const char *path,
                            uint8_t **out_data,
                            size_t *out_size)
{
    int idx;

    if (!fs || !path || !out_data || !out_size) return NDFS_ERR_NULL_PTR;

    idx = find_object(fs, path);
    if (idx < 0) return NDFS_ERR_NOT_FOUND;

    return read_object_data(fs, &fs->objects[idx], out_data, out_size);
}

ndfs_error_t ndfs_read_file_parity(const ndfs_filesystem_t *fs,
                                   const char *path,
                                   ndfs_parity_mode_t parity,
                                   uint8_t **out_data,
                                   size_t *out_size)
{
    ndfs_error_t err = ndfs_read_file(fs, path, out_data, out_size);
    if (err != NDFS_OK) return err;
    if (parity == NDFS_PARITY_STRIP && *out_data && *out_size > 0) {
        ndfs_strip_parity(*out_data, *out_size);
    } else if (parity == NDFS_PARITY_SET && *out_data && *out_size > 0) {
        ndfs_set_parity(*out_data, *out_size);
    }
    return NDFS_OK;
}

void ndfs_free_data(uint8_t *data)
{
    free(data);
}

/* ── Write operations ────────────────────────────────────────────── */

/* Commit all dirty pages to the backend NOW, folding in a prior error.
 *
 * Called at the tail of every public mutation so the on-disk image tracks each
 * change immediately -- restoring RetroCore's immediate surgical-write model
 * rather than deferring every write to ndfs_close(). Two wins over close-only
 * flushing: (1) a crash between operations no longer loses committed writes,
 * and (2) a backend write failure is REPORTED to the caller here instead of
 * being silently swallowed by the void ndfs_close().
 *
 * If `prior` already indicates failure we return it unchanged and do NOT force
 * a commit (a half-done mutation is left for ndfs_close to flush, exactly as a
 * mid-operation failure behaves under the immediate-write reference too). On
 * success the return is the flush result. Cheap when nothing is dirty: a single
 * pass over NDFS_CACHE_SLOTS (8) slots writing only the pages actually touched;
 * pages already flushed by a previous commit this session are clean and skipped. */
static ndfs_error_t commit_writes(struct ndfs_filesystem *fs, ndfs_error_t prior)
{
    if (prior != NDFS_OK) return prior;
    return cache_flush_all(fs);
}

ndfs_error_t ndfs_write_file_parity(ndfs_filesystem_t *fs,
                                    const char *path,
                                    const uint8_t *file_data,
                                    size_t file_size,
                                    ndfs_parity_mode_t parity)
{
    return ndfs_write_file_holes(fs, path, file_data, file_size, parity, NULL);
}

ndfs_error_t ndfs_write_file_holes(ndfs_filesystem_t *fs,
                                   const char *path,
                                   const uint8_t *file_data,
                                   size_t file_size,
                                   ndfs_parity_mode_t parity,
                                   const ndfs_hole_list_t *holes)
{
    return ndfs_write_file_full(fs, path, file_data, file_size, parity, holes, NULL);
}

ndfs_error_t ndfs_write_file(ndfs_filesystem_t *fs,
                             const char *path,
                             const uint8_t *file_data,
                             size_t file_size)
{
    return write_file_impl(fs, path, file_data, file_size, NULL, NULL);
}

ndfs_error_t ndfs_write_file_full(ndfs_filesystem_t *fs,
                                  const char *path,
                                  const uint8_t *file_data,
                                  size_t file_size,
                                  ndfs_parity_mode_t parity,
                                  const ndfs_hole_list_t *holes,
                                  const uint16_t *access_bits)
{
    if (parity == NDFS_PARITY_SET && file_data && file_size > 0) {
        uint8_t *copy = (uint8_t *)malloc(file_size);
        ndfs_error_t err;
        if (!copy) return NDFS_ERR_ALLOC;
        memcpy(copy, file_data, file_size);
        ndfs_set_parity(copy, file_size);
        err = write_file_impl(fs, path, copy, file_size, holes, access_bits);
        free(copy);
        return err;
    }
    return write_file_impl(fs, path, file_data, file_size, holes, access_bits);
}

/* The one real writer. `holes` names the logical pages to leave unallocated and
 * is NULL for the ordinary fully-allocated case; nothing here ever infers a
 * hole from the data (see write_data_page_to_index for why). `access_bits`,
 * when non-NULL, overrides the owning user's inherited default access on a
 * NEWLY CREATED file only -- see create_new_file and NDFS_ACCESS_PUBLIC_READ
 * (object_entry.h). */
static ndfs_error_t write_file_impl(ndfs_filesystem_t *fs,
                                    const char *path,
                                    const uint8_t *file_data,
                                    size_t file_size,
                                    const ndfs_hole_list_t *holes,
                                    const uint16_t *access_bits)
{
    char user_name[NDFS_NAME_MAX + 1];
    char obj_name[NDFS_NAME_MAX + 1];
    char file_type[NDFS_TYPE_MAX + 1];
    int user_slot;
    int existing_idx;
    uint32_t data_pages, additional_needed;
    ndfs_user_entry_t *user;
    ndfs_error_t err;

    if (!fs || !path) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    parse_path(path, user_name, sizeof(user_name),
               obj_name, sizeof(obj_name),
               file_type, sizeof(file_type));

    if (obj_name[0] == '\0') return NDFS_ERR_INVALID_ARG;

    /* Find user */
    if (user_name[0] != '\0') {
        user_slot = find_user_by_name(fs, user_name);
    } else {
        /* Default to first user */
        user_slot = -1;
        {
            size_t i;
            for (i = 0; i < MAX_INTERNAL_USERS; i++) {
                if (fs->user_valid[i]) { user_slot = (int)i; break; }
            }
        }
    }
    if (user_slot < 0) return NDFS_ERR_NOT_FOUND;
    user = &fs->users[user_slot];

    /* Calculate real (non-sparse) pages the new content needs. Quota is
     * charged only for real disk consumption -- sparse (all-zero) pages
     * allocate no block, and the index/sub-index structural pages a large
     * file needs are filesystem overhead, never charged to the user. See
     * count_real_data_pages_in_buffer(). */
    data_pages = (uint32_t)((file_size + NDFS_PAGE_SIZE - 1) / NDFS_PAGE_SIZE);
    if (data_pages == 0) data_pages = 1;

    /* Check for existing file */
    existing_idx = find_object(fs, path);

    {
        uint32_t new_real_pages =
            count_real_data_pages_in_buffer(file_data, file_size, data_pages, holes);
        uint32_t existing_real_pages = 0;
        /* Index and sub-index blocks are charged like data pages (SINTRAN, measured), so the
         * reservation must cover them: one block, or a top block plus one per 512 pages. */
        uint32_t new_struct_pages = data_pages <= NDFS_MAX_OBJECT_FILE_PTRS
            ? 1u : 1u + (data_pages + NDFS_MAX_OBJECT_FILE_PTRS - 1) / NDFS_MAX_OBJECT_FILE_PTRS;
        uint32_t existing_struct_pages = 0;

        if (existing_idx >= 0) {
            existing_real_pages =
                count_real_data_pages_in_object(fs, &fs->objects[existing_idx]);
            existing_struct_pages =
                count_index_blocks_in_object(fs, &fs->objects[existing_idx]);
        }

        new_real_pages += new_struct_pages;
        existing_real_pages += existing_struct_pages;
        /* The first file of a user also needs the user's first object data page. */
        if (existing_idx < 0 && count_user_object_pages(fs, user->user_index) == 0)
            new_real_pages++;

        additional_needed = new_real_pages > existing_real_pages
            ? new_real_pages - existing_real_pages : 0;

        /* A contiguous file already owns every page it will ever have, so a
         * write can never need more. Skipping the quota step matters: the
         * check below would otherwise expand the user's reservation for pages
         * that are never allocated, and it would do so before
         * update_existing_contiguous_file() rejects an oversized write. */
        if (existing_idx >= 0 &&
            (fs->objects[existing_idx].file_type_flags & NDFS_FT_CONTIGUOUS)) {
            additional_needed = 0;
        }
    }

    /* Check and expand quota if needed */
    if (additional_needed > 0) {
        int32_t avail = (int32_t)user->pages_reserved - (int32_t)user->pages_used;
        if (avail < (int32_t)additional_needed) {
            uint32_t expansion = additional_needed - (avail > 0 ? (uint32_t)avail : 0);
            uint32_t free_on_disk = ndfs_bf_count_free(&fs->bit_file);
            if (free_on_disk < expansion) return NDFS_ERR_NO_SPACE;
            user->pages_reserved += expansion;
        }
    }

    if (existing_idx >= 0) {
        err = update_existing_file(fs, existing_idx, user_slot, file_data, file_size, holes);
    } else {
        err = create_new_file(fs, obj_name, file_type, user_slot, file_data, file_size,
                              holes, access_bits);
    }

    /* create_new_file / update_existing_file performed their own surgical
     * metadata writes (object page, owner user page, bitmap); commit them (and
     * the index/data pages) to the backend now. */
    return commit_writes(fs, err);
}

/* Put a contiguous file in the object file: `pages` pages from `start_block`, which must already
 * be allocated in the bit file. Shared by ndfs_create_contiguous_file() and by the orphan recovery
 * of fsck. Writes the object page, the user page and the bit file; the caller flushes. */
static void register_contiguous_run(struct ndfs_filesystem *fs, ndfs_user_entry_t *user,
                                    uint32_t obj_index, const char *obj_name, const char *file_type,
                                    uint32_t start_block, uint32_t pages, uint32_t bytes_in_file)
{
    ndfs_object_entry_t entry;

    ndfs_oe_init(&entry);
    entry.object_index = obj_index;
    {
        size_t nlen = strlen(obj_name);
        if (nlen > NDFS_NAME_MAX) nlen = NDFS_NAME_MAX;
        memcpy(entry.object_name, obj_name, nlen);
        entry.object_name[nlen] = '\0';
        str_toupper(entry.object_name);
    }
    {
        size_t tlen = strlen(file_type);
        if (tlen > NDFS_TYPE_MAX) tlen = NDFS_TYPE_MAX;
        memcpy(entry.type, file_type, tlen);
        entry.type[tlen] = '\0';
        str_toupper(entry.type);
    }
    entry.user_index = user->user_index;
    /* The number SINTRAN shows is NOT the physical position: a user's files can
     * span several object blocks, each a whole index block apart. Compute it here
     * as well as on load, or a freshly created file reports number 0 until the
     * image is reloaded. */
    entry.file_number = (uint32_t)ndfs_oe_compute_file_number(entry.object_index,
                                                              entry.user_index);
    memcpy(entry.user_name, user->user_name, NDFS_NAME_MAX + 1);

    /* Unlike an indexed file, allocated and logical extent are the same here,
     * and both equal the reservation -- a contiguous run has no holes. */
    entry.pages_in_file = pages;

    /* Created empty: the pages exist, but no byte of them is file content yet.
     * This is what the live machine reports for a fresh contiguous file. */
    entry.bytes_in_file = bytes_in_file;

    /* Points straight at the first data page. There is no index block to walk:
     * page N of the file is simply block (file_pointer.block_id + N). */
    entry.file_pointer.block_id = start_block;
    entry.file_pointer.type = NDFS_PTR_CONTIGUOUS;
    entry.access_bits = NDFS_ACCESS_DEFAULT;

    /* Contiguous, not indexed -- the flag SINTRAN prints as "CONTINUOUS FILE". */
    entry.file_type_flags = NDFS_FT_CONTIGUOUS;
    entry.disk_object_index = (uint16_t)obj_index;
    entry.next_version = (uint16_t)obj_index;
    entry.prev_version = (uint16_t)obj_index;

    add_object(fs, &entry);

    user->pages_used += pages;

    write_object_page(fs, obj_index);
    write_user_page(fs, user->user_index);
    write_bit_file(fs);
}

ndfs_error_t ndfs_create_contiguous_file(ndfs_filesystem_t *fs,
                                         const char *path,
                                         uint32_t pages)
{
    char user_name[NDFS_NAME_MAX + 1];
    char obj_name[NDFS_NAME_MAX + 1];
    char file_type[NDFS_TYPE_MAX + 1];
    int user_slot;
    uint32_t obj_index;
    uint32_t start_block;
    uint32_t i;
    ndfs_user_entry_t *user;
    ndfs_error_t err;

    if (!fs || !path) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;
    if (pages == 0) return NDFS_ERR_INVALID_ARG;

    parse_path(path, user_name, sizeof(user_name),
               obj_name, sizeof(obj_name),
               file_type, sizeof(file_type));

    if (obj_name[0] == '\0') return NDFS_ERR_INVALID_ARG;

    if (user_name[0] != '\0') {
        user_slot = find_user_by_name(fs, user_name);
    } else {
        /* Default to the first user, same rule as ndfs_write_file(). */
        size_t u;
        user_slot = -1;
        for (u = 0; u < MAX_INTERNAL_USERS; u++) {
            if (fs->user_valid[u]) { user_slot = (int)u; break; }
        }
    }
    if (user_slot < 0) return NDFS_ERR_NOT_FOUND;
    user = &fs->users[user_slot];

    if (find_object(fs, path) >= 0) return NDFS_ERR_ALREADY_EXISTS;

    /* Every page of a contiguous file is real disk, so the whole run is
     * charged to the user's quota at once -- there are no holes to discount,
     * unlike an indexed file. */
    {
        int32_t avail = (int32_t)user->pages_reserved - (int32_t)user->pages_used;
        if (avail < (int32_t)pages) {
            uint32_t expansion = pages - (avail > 0 ? (uint32_t)avail : 0);
            if (ndfs_bf_count_free(&fs->bit_file) < expansion) return NDFS_ERR_NO_SPACE;
            user->pages_reserved += expansion;
        }
    }

    {
        int alloc_err = allocate_user_object_slot(fs, user, &obj_index);
        if (alloc_err != NDFS_OK) return alloc_err;
    }
    if (ensure_object_dir_page(fs, obj_index) == 0) return NDFS_ERR_NO_SPACE;

    /* The defining property: one run, no index block. ndfs_bf_find_free_range()
     * is the only allocator that can promise consecutive blocks -- the sparse
     * allocator picks each page independently and would scatter them. */
    if (pages == 1) {
        /* A file of one page does not take the top-down run rule: SINTRAN gives it the
         * page the data page allocator (RSPAG) would give. docs/NDFS-ALLOCATION-RULES.md section 3. */
        err = ndfs_bf_find_free_block(&fs->bit_file, &start_block);
    } else {
        err = ndfs_bf_find_free_range(&fs->bit_file, pages, &start_block);
    }
    if (err != NDFS_OK) return NDFS_ERR_NO_SPACE;

    err = ndfs_bf_allocate(&fs->bit_file, start_block, pages);
    if (err != NDFS_OK) return err;

    /* Zero the run. A freshly created file must not expose whatever a deleted
     * file left behind on those pages. */
    for (i = 0; i < pages; i++) {
        uint8_t *page = write_page_ptr(fs, start_block + i);
        if (!page) {
            ndfs_bf_free_range(&fs->bit_file, start_block, pages);
            return NDFS_ERR_IO;
        }
        memset(page, 0, NDFS_PAGE_SIZE);
    }

    register_contiguous_run(fs, user, obj_index, obj_name, file_type, start_block, pages, 0);

    return commit_writes(fs, NDFS_OK);
}

ndfs_error_t ndfs_delete_file(ndfs_filesystem_t *fs, const char *path)
{
    int idx, user_slot;
    ndfs_object_entry_t *obj;
    uint32_t real_pages;

    if (!fs || !path) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    idx = find_object(fs, path);
    if (idx < 0) return NDFS_ERR_NOT_FOUND;
    obj = &fs->objects[idx];

    /* Real (non-sparse) data pages this file actually occupies on disk right
     * now, computed BEFORE freeing -- see count_real_data_pages_in_object().
     * This is the exact refund: sparse holes were never charged against
     * quota in the first place, and index/sub-index structural blocks are
     * filesystem overhead, not user data, so they are never refunded either
     * (mirrors create_new_file/update_existing_file). */
    real_pages = count_charged_pages_in_object(fs, obj);

    /* Free blocks */
    if (obj->file_pointer.block_id > 0) {
        free_file_blocks(fs, obj);
    }

    /* Update user pages used */
    user_slot = find_user_by_index(fs, obj->user_index);
    if (user_slot >= 0) {
        if (fs->users[user_slot].pages_used >= real_pages)
            fs->users[user_slot].pages_used -= real_pages;
        else
            fs->users[user_slot].pages_used = 0;
    }

    /* Surgical writes (RetroCommander): capture the index/owner before the
     * array removal invalidates `obj`, drop the entry, then rewrite the freed
     * object's page (zero-fill clears the slot so it does not reappear), the
     * owner's user page, and the allocation bitmap. */
    {
        uint32_t freed_index = obj->object_index;
        uint8_t  owner       = obj->user_index;

        if ((size_t)idx < fs->object_count - 1) {
            memmove(&fs->objects[idx], &fs->objects[idx + 1],
                    (fs->object_count - (size_t)idx - 1) * sizeof(ndfs_object_entry_t));
        }
        fs->object_count--;

        write_object_page(fs, freed_index);
        write_user_page(fs, owner);
        write_bit_file(fs);
    }

    return commit_writes(fs, NDFS_OK);
}

ndfs_error_t ndfs_rename(ndfs_filesystem_t *fs,
                         const char *old_path,
                         const char *new_path)
{
    int idx;
    char user_name[NDFS_NAME_MAX + 1];
    char obj_name[NDFS_NAME_MAX + 1];
    char file_type[NDFS_TYPE_MAX + 1];
    size_t nlen, tlen;

    if (!fs || !old_path || !new_path) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    idx = find_object(fs, old_path);
    if (idx < 0) return NDFS_ERR_NOT_FOUND;

    parse_path(new_path, user_name, sizeof(user_name),
               obj_name, sizeof(obj_name),
               file_type, sizeof(file_type));

    if (obj_name[0] == '\0') return NDFS_ERR_INVALID_ARG;

    nlen = strlen(obj_name);
    if (nlen > NDFS_NAME_MAX) nlen = NDFS_NAME_MAX;
    memcpy(fs->objects[idx].object_name, obj_name, nlen);
    fs->objects[idx].object_name[nlen] = '\0';
    str_toupper(fs->objects[idx].object_name);

    tlen = strlen(file_type);
    if (tlen > NDFS_TYPE_MAX) tlen = NDFS_TYPE_MAX;
    memcpy(fs->objects[idx].type, file_type, tlen);
    fs->objects[idx].type[tlen] = '\0';
    str_toupper(fs->objects[idx].type);

    /* Rename touches only this file's object entry. */
    return commit_writes(fs, write_object_page(fs, fs->objects[idx].object_index));
}

/* ── User management ─────────────────────────────────────────────── */

ndfs_error_t ndfs_get_users(const ndfs_filesystem_t *fs,
                            ndfs_user_entry_t **out_users,
                            size_t *out_count)
{
    ndfs_user_entry_t *arr;
    size_t count = 0;
    size_t i;

    if (!fs || !out_users || !out_count) return NDFS_ERR_NULL_PTR;

    /* Count valid users */
    for (i = 0; i < MAX_INTERNAL_USERS; i++) {
        if (fs->user_valid[i]) count++;
    }

    if (count == 0) {
        *out_users = NULL;
        *out_count = 0;
        return NDFS_OK;
    }

    arr = (ndfs_user_entry_t *)calloc(count, sizeof(ndfs_user_entry_t));
    if (!arr) return NDFS_ERR_ALLOC;

    {
        size_t idx = 0;
        for (i = 0; i < MAX_INTERNAL_USERS; i++) {
            if (fs->user_valid[i]) {
                arr[idx++] = fs->users[i];
            }
        }
    }

    *out_users = arr;
    *out_count = count;
    return NDFS_OK;
}

void ndfs_free_users(ndfs_user_entry_t *users)
{
    free(users);
}

ndfs_error_t ndfs_add_user(ndfs_filesystem_t *fs,
                           const char *name,
                           uint32_t reserved_pages)
{
    int slot;
    ndfs_user_entry_t user;
    size_t i, nlen;

    if (!fs || !name) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    /* Check if already exists */
    if (find_user_by_name(fs, name) >= 0) return NDFS_ERR_ALREADY_EXISTS;

    /* Find next available index */
    slot = -1;
    for (i = 0; i < MAX_INTERNAL_USERS; i++) {
        if (!fs->user_valid[i]) { slot = (int)i; break; }
    }
    if (slot < 0) return NDFS_ERR_NO_SLOTS;

    ndfs_ue_init(&user);
    nlen = strlen(name);
    if (nlen > NDFS_NAME_MAX) nlen = NDFS_NAME_MAX;
    memcpy(user.user_name, name, nlen);
    user.user_name[nlen] = '\0';
    str_toupper(user.user_name);
    user.user_index = (uint8_t)slot;
    user.pages_reserved = reserved_pages;

    fs->users[slot] = user;
    fs->user_valid[slot] = true;
    fs->user_count++;

    /* Guarantee the UserFile data page for this slot exists on disk before
     * writing it -- a fresh/small image may only have its first UserFile
     * page pre-allocated, and write_user_page() silently no-ops if the
     * page's index-block pointer is unset (see write_user_page's comment).
     * ensure_user_dir_page() may allocate a bitmap block, so persist the
     * updated bitmap/master-block free-page count afterwards too, exactly
     * like create_new_file()/update_file_data() do after
     * ensure_object_dir_page(). */
    ensure_user_dir_page(fs, (uint32_t)slot);
    write_user_page(fs, (uint32_t)slot);
    write_bit_file(fs);

    return commit_writes(fs, NDFS_OK);
}

ndfs_error_t ndfs_remove_user(ndfs_filesystem_t *fs, uint8_t index)
{
    int slot;

    if (!fs) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    slot = find_user_by_index(fs, index);
    if (slot < 0) return NDFS_ERR_NOT_FOUND;

    /* Check if user has files */
    if (objects_for_user_by_index(fs, index) > 0) return NDFS_ERR_HAS_FILES;

    fs->user_valid[slot] = false;
    fs->user_count--;

    /* Remove-user touches this user's UserFile page (slot zeroed) and gives its quota back to the
     * Unreserved Pages pool in the master block. */
    persist_master_block(fs);
    return commit_writes(fs, write_user_page(fs, index));
}

ndfs_error_t ndfs_update_user_quota(ndfs_filesystem_t *fs,
                                    uint8_t index,
                                    uint32_t new_pages)
{
    int slot;

    if (!fs) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    slot = find_user_by_index(fs, index);
    if (slot < 0) return NDFS_ERR_NOT_FOUND;

    fs->users[slot].pages_reserved = new_pages;

    /* The quota is taken from the Unreserved Pages pool, so the master block changes too. */
    persist_master_block(fs);
    return commit_writes(fs, write_user_page(fs, index));
}

ndfs_error_t ndfs_clear_user_password(ndfs_filesystem_t *fs, const char *name)
{
    int slot;

    if (!fs || !name) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    slot = find_user_by_name(fs, name);
    if (slot < 0) return NDFS_ERR_NOT_FOUND;

    fs->users[slot].password = 0;

    return commit_writes(fs, write_user_page(fs, fs->users[slot].user_index));
}

ndfs_error_t ndfs_clear_user_password_by_index(ndfs_filesystem_t *fs,
                                               uint8_t index)
{
    int slot;

    if (!fs) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    slot = find_user_by_index(fs, index);
    if (slot < 0) return NDFS_ERR_NOT_FOUND;

    fs->users[slot].password = 0;

    return commit_writes(fs, write_user_page(fs, index));
}

/* ── Friends ─────────────────────────────────────────────────────── */

/* Resolve a user reference (name or decimal index 0-255) to the in-memory
 * slot (== user index). A purely-numeric reference is treated as an index.
 * Returns the slot, or -1 if no such valid user. */
static int resolve_user_ref(const struct ndfs_filesystem *fs, const char *ref)
{
    const char *p = ref;
    bool numeric;

    if (!ref || !ref[0]) return -1;

    numeric = true;
    for (p = ref; *p; p++) {
        if (*p < '0' || *p > '9') { numeric = false; break; }
    }
    if (numeric) {
        long v = atol(ref);
        if (v >= 0 && v <= 255) {
            int slot = find_user_by_index(fs, (uint8_t)v);
            if (slot >= 0) return slot;
        }
        return -1;
    }
    return find_user_by_name(fs, ref);
}

/* Resolve a friend reference to a user index. A numeric reference is taken
 * as a literal index (the friend need not be a named user); a name must
 * resolve to an existing user. Returns NDFS_OK + *out_index, else an error. */
static ndfs_error_t resolve_friend_index(const struct ndfs_filesystem *fs,
                                         const char *ref, uint8_t *out_index)
{
    const char *p;
    bool numeric;

    if (!ref || !ref[0]) return NDFS_ERR_INVALID_ARG;

    numeric = true;
    for (p = ref; *p; p++) {
        if (*p < '0' || *p > '9') { numeric = false; break; }
    }
    if (numeric) {
        long v = atol(ref);
        if (v < 0 || v > 255) return NDFS_ERR_INVALID_ARG;
        *out_index = (uint8_t)v;
        return NDFS_OK;
    } else {
        int slot = find_user_by_name(fs, ref);
        if (slot < 0) return NDFS_ERR_NOT_FOUND;
        *out_index = fs->users[slot].user_index;
        return NDFS_OK;
    }
}

ndfs_error_t ndfs_list_friends(const ndfs_filesystem_t *fs, const char *user_ref,
                               ndfs_friend_info_t **out_friends, size_t *out_count)
{
    int owner_slot;
    const ndfs_user_entry_t *owner;
    ndfs_friend_info_t *arr;
    size_t n = 0, i;

    if (!fs || !user_ref || !out_friends || !out_count) return NDFS_ERR_NULL_PTR;
    *out_friends = NULL;
    *out_count = 0;

    owner_slot = resolve_user_ref(fs, user_ref);
    if (owner_slot < 0) return NDFS_ERR_NOT_FOUND;
    owner = &fs->users[owner_slot];

    /* Count active friends first. */
    for (i = 0; i < NDFS_MAX_FRIENDS; i++) {
        if (ndfs_uf_is_active(&owner->friends[i])) n++;
    }
    if (n == 0) return NDFS_OK;

    arr = (ndfs_friend_info_t *)calloc(n, sizeof(*arr));
    if (!arr) return NDFS_ERR_ALLOC;

    {
        size_t k = 0;
        for (i = 0; i < NDFS_MAX_FRIENDS; i++) {
            const ndfs_user_friend_t *uf = &owner->friends[i];
            uint8_t fidx;
            int fslot;
            if (!ndfs_uf_is_active(uf)) continue;
            fidx = ndfs_uf_friend_index(uf);
            arr[k].index = fidx;
            arr[k].bits  = uf->bits;
            ndfs_uf_permission_string(uf, arr[k].perms);
            fslot = find_user_by_index(fs, fidx);
            if (fslot >= 0) {
                memcpy(arr[k].name, fs->users[fslot].user_name, NDFS_NAME_MAX + 1);
            } else {
                arr[k].name[0] = '\0';
            }
            k++;
        }
    }

    *out_friends = arr;
    *out_count = n;
    return NDFS_OK;
}

void ndfs_free_friends(ndfs_friend_info_t *friends)
{
    free(friends);
}

ndfs_error_t ndfs_add_friend(ndfs_filesystem_t *fs, const char *user_ref,
                             const char *friend_ref, const char *perms)
{
    int owner_slot;
    uint8_t friend_index;
    uint8_t perm_bits;
    ndfs_error_t err;

    if (!fs || !user_ref || !friend_ref) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    owner_slot = resolve_user_ref(fs, user_ref);
    if (owner_slot < 0) return NDFS_ERR_NOT_FOUND;

    err = resolve_friend_index(fs, friend_ref, &friend_index);
    if (err != NDFS_OK) return err;

    /* Default permissions: RWA (read/write/append). */
    if (!perms || !perms[0]) {
        perm_bits = 0x07; /* R|W|A */
    } else {
        err = ndfs_uf_parse_permissions(perms, &perm_bits);
        if (err != NDFS_OK) return err;
    }

    if (ndfs_ue_is_friend(&fs->users[owner_slot], friend_index))
        return NDFS_ERR_ALREADY_EXISTS;

    if (!ndfs_ue_add_friend(&fs->users[owner_slot], friend_index, perm_bits))
        return NDFS_ERR_NO_SLOTS;

    return commit_writes(fs, write_user_page(fs, fs->users[owner_slot].user_index));
}

ndfs_error_t ndfs_remove_friend(ndfs_filesystem_t *fs, const char *user_ref,
                                const char *friend_ref)
{
    int owner_slot;
    uint8_t friend_index;
    ndfs_error_t err;

    if (!fs || !user_ref || !friend_ref) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    owner_slot = resolve_user_ref(fs, user_ref);
    if (owner_slot < 0) return NDFS_ERR_NOT_FOUND;

    err = resolve_friend_index(fs, friend_ref, &friend_index);
    if (err != NDFS_OK) return err;

    if (!ndfs_ue_remove_friend(&fs->users[owner_slot], friend_index))
        return NDFS_ERR_NOT_FOUND;

    return commit_writes(fs, write_user_page(fs, fs->users[owner_slot].user_index));
}

/* ── Read operations (extended) ──────────────────────────────────── */

ndfs_error_t ndfs_get_file_blocks(const ndfs_filesystem_t *fs, const char *path,
                                  uint32_t **out_blocks, size_t *out_count)
{
    int idx;
    const ndfs_object_entry_t *obj;
    uint32_t *blocks;
    uint32_t logical_pages;
    size_t cap, n = 0;

    if (!fs || !path || !out_blocks || !out_count) return NDFS_ERR_NULL_PTR;
    *out_blocks = NULL;
    *out_count = 0;

    idx = find_object(fs, path);
    if (idx < 0) return NDFS_ERR_NOT_FOUND;
    obj = &fs->objects[idx];

    /* Bound the walk by the file's LOGICAL extent, holes included -- not by
     * pages_in_file, which counts only the pages really allocated.
     *
     * FIXED 2026-07-30. Measured on (SYSTEM)S3-CONFIG-E01:PROG in
     * BIGDISK0-K-103.IMG: pages_in_file is 89, the last used index slot is 98,
     * and slots 54..63 are holes. Walking 89 slots stopped at 88, dropping the
     * last 10 real pages while still reporting the 10 holes -- 79 real blocks
     * instead of 89, and every page past slot 53 at the wrong offset. Confirmed
     * on real hardware: an ND-100 transferring that file sends 89 messages
     * carrying page numbers 0..53 then 64..98.
     *
     * The read paths above (ndfs_read_file and friends) already bound themselves
     * by bytes_in_file and were never affected; only this block-listing helper
     * was. The allocation is sized from the same logical extent -- sizing it from
     * pages_in_file while writing logical_pages entries would overrun the heap. */
    logical_pages = (uint32_t)((obj->bytes_in_file + NDFS_PAGE_SIZE - 1) / NDFS_PAGE_SIZE);

    cap = logical_pages ? logical_pages : 1;
    if (obj->pages_in_file > cap) cap = obj->pages_in_file;
    blocks = (uint32_t *)malloc(cap * sizeof(uint32_t));
    if (!blocks) return NDFS_ERR_ALLOC;

    if (obj->file_pointer.type == NDFS_PTR_CONTIGUOUS) {
        uint32_t i;
        /* A contiguous run has no holes, so its own page count is the honest
         * bound and is kept. */
        for (i = 0; i < obj->pages_in_file; i++) {
            blocks[n++] = obj->file_pointer.block_id + i;
        }
    } else if (obj->file_pointer.type == NDFS_PTR_INDEXED) {
        const uint8_t *ib = read_page(fs, obj->file_pointer.block_id);
        uint32_t i;
        if (!ib) { free(blocks); return NDFS_ERR_CORRUPT; }
        for (i = 0; i < logical_pages && i < 512; i++) {
            ndfs_block_pointer_t p = ndfs_bp_from_bytes(ib, i * 4);
            blocks[n++] = p.block_id;
        }
    } else if (obj->file_pointer.type == NDFS_PTR_SUBINDEXED) {
        const uint8_t *sib = read_page(fs, obj->file_pointer.block_id);
        uint32_t remaining = logical_pages;
        uint32_t si;
        if (!sib) { free(blocks); return NDFS_ERR_CORRUPT; }
        /* sib held across each group-index read -- pin it. */
        cache_pin(fs, obj->file_pointer.block_id);
        for (si = 0; si < 512 && remaining > 0; si++) {
            ndfs_block_pointer_t ip = ndfs_bp_from_bytes(sib, si * 4);
            const uint8_t *ib;
            uint32_t j;
            if (ip.block_id == 0) {
                /* An empty entry is a hole of 512 pages, not the end of the file: SINTRAN
                 * III leaves the entries of groups with no page in them empty (a domain of
                 * the ND Linker B01, 2026-10-14: entries 0, 2, 4, 6 set). */
                for (j = 0; j < 512 && remaining > 0; j++) {
                    blocks[n++] = 0;
                    remaining--;
                }
                continue;
            }
            ib = read_page(fs, ip.block_id);
            if (!ib) { cache_unpin(fs, obj->file_pointer.block_id); free(blocks); return NDFS_ERR_CORRUPT; }
            for (j = 0; j < 512 && remaining > 0; j++) {
                ndfs_block_pointer_t dp = ndfs_bp_from_bytes(ib, j * 4);
                blocks[n++] = dp.block_id;
                remaining--;
            }
        }
        cache_unpin(fs, obj->file_pointer.block_id);
    }

    *out_blocks = blocks;
    *out_count = n;
    return NDFS_OK;
}

ndfs_error_t ndfs_patch_file_region(ndfs_filesystem_t *fs, const char *path,
                                    size_t file_offset,
                                    const uint8_t *data, size_t len)
{
    uint32_t *blocks;
    size_t count;
    ndfs_error_t err;
    size_t end, start_page, end_page, pg;

    if (!fs || !path || !data) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;
    if (len == 0) return NDFS_OK;

    err = ndfs_get_file_blocks(fs, path, &blocks, &count);
    if (err != NDFS_OK) return err;

    end = file_offset + len;
    start_page = file_offset / NDFS_PAGE_SIZE;
    end_page = (end - 1) / NDFS_PAGE_SIZE;

    /* Validate the whole span up front so an out-of-range region never leaves a
     * partial write behind. */
    if (end_page >= count) { free(blocks); return NDFS_ERR_OUT_OF_RANGE; }

    for (pg = start_page; pg <= end_page; pg++) {
        uint8_t *page;
        size_t page_start, ov_start, ov_end;

        if (pg >= count) { free(blocks); return NDFS_ERR_OUT_OF_RANGE; }
        if (blocks[pg] == 0) { free(blocks); return NDFS_ERR_CORRUPT; }

        page = write_page_ptr(fs, blocks[pg]);
        if (!page) { free(blocks); return NDFS_ERR_CORRUPT; }

        page_start = pg * NDFS_PAGE_SIZE;
        ov_start = file_offset > page_start ? file_offset : page_start;
        ov_end = end < page_start + NDFS_PAGE_SIZE ? end : page_start + NDFS_PAGE_SIZE;

        memcpy(page + (ov_start - page_start),
               data + (ov_start - file_offset),
               ov_end - ov_start);
    }

    free(blocks);
    /* Commit the patched data pages to the backend immediately. */
    return commit_writes(fs, NDFS_OK);
}

ndfs_error_t ndfs_file_exists(const ndfs_filesystem_t *fs, const char *path,
                              bool *out_exists)
{
    int idx;

    if (!fs || !path || !out_exists) return NDFS_ERR_NULL_PTR;

    idx = find_object(fs, path);
    *out_exists = (idx >= 0);
    return NDFS_OK;
}

ndfs_error_t ndfs_set_file_access(ndfs_filesystem_t *fs, const char *path,
                                  uint16_t access_bits)
{
    int idx;

    if (!fs || !path) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;

    idx = find_object(fs, path);
    if (idx < 0) return NDFS_ERR_NOT_FOUND;

    fs->objects[idx].access_bits = access_bits & 0x7FFF;
    /* Access change touches only this file's object entry. */
    return commit_writes(fs, write_object_page(fs, fs->objects[idx].object_index));
}

ndfs_error_t ndfs_get_metadata(const ndfs_filesystem_t *fs, const char *path,
                               ndfs_file_entry_t *out_entry)
{
    int idx;
    const ndfs_object_entry_t *obj;

    if (!fs || !path || !out_entry) return NDFS_ERR_NULL_PTR;

    idx = find_object(fs, path);
    if (idx < 0) return NDFS_ERR_NOT_FOUND;

    obj = &fs->objects[idx];

    memset(out_entry, 0, sizeof(*out_entry));
    memcpy(out_entry->name, obj->object_name, NDFS_NAME_MAX + 1);
    memcpy(out_entry->type, obj->type, NDFS_TYPE_MAX + 1);
    ndfs_oe_full_name(obj, out_entry->full_name, sizeof(out_entry->full_name));
    memcpy(out_entry->user_name, obj->user_name, NDFS_NAME_MAX + 1);
    out_entry->size = obj->bytes_in_file;
    out_entry->pages = obj->pages_in_file;
    out_entry->is_directory = false;

    return NDFS_OK;
}

/* ── User query (extended) ──────────────────────────────────────── */

ndfs_error_t ndfs_get_user(const ndfs_filesystem_t *fs, uint8_t index,
                           ndfs_user_entry_t *out_user)
{
    int slot;

    if (!fs || !out_user) return NDFS_ERR_NULL_PTR;

    slot = find_user_by_index(fs, index);
    if (slot < 0) return NDFS_ERR_NOT_FOUND;

    *out_user = fs->users[slot];
    return NDFS_OK;
}

/* ── Object blocks: the @GIVE-OBJECT-BLOCKS equivalent ───────────── */

ndfs_error_t ndfs_give_object_blocks(ndfs_filesystem_t *fs, const char *user_name,
                                     uint32_t count, uint8_t *out_max)
{
    int slot;
    uint32_t new_max;
    ndfs_error_t err;

    if (!fs || !user_name) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;
    if (count < 1) return NDFS_ERR_INVALID_ARG;

    slot = find_user_by_name(fs, user_name);
    if (slot < 0) return NDFS_ERR_NOT_FOUND;

    new_max = (uint32_t)fs->users[slot].max_object_blocks + count;
    if (new_max > NDFS_MAX_OBJECT_BLOCKS) {
        /* Refuse rather than clamp: clamping would leave the caller believing
         * it received what it asked for, and the shortfall would only surface
         * much later as a premature "user is full". */
        return NDFS_ERR_OUT_OF_RANGE;
    }

    fs->users[slot].max_object_blocks = (uint8_t)new_max;

    err = write_user_page(fs, fs->users[slot].user_index);
    if (err != NDFS_OK) {
        /* Put the entry back so the in-memory view does not claim a ceiling the
         * image does not have. */
        fs->users[slot].max_object_blocks = (uint8_t)(new_max - count);
        return err;
    }

    if (out_max) *out_max = (uint8_t)new_max;
    return NDFS_OK;
}

ndfs_error_t ndfs_take_object_blocks(ndfs_filesystem_t *fs, const char *user_name,
                                     uint32_t count, uint8_t *out_max)
{
    int slot;
    int32_t new_max;
    ndfs_error_t err;

    if (!fs || !user_name) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;
    if (count < 1) return NDFS_ERR_INVALID_ARG;

    slot = find_user_by_name(fs, user_name);
    if (slot < 0) return NDFS_ERR_NOT_FOUND;

    new_max = (int32_t)fs->users[slot].max_object_blocks - (int32_t)count;

    /* Never below the blocks actually allocated - those hold files, and a file's
     * number is derived from the block it sits in, so removing one would orphan
     * every file in it. Never below one either. */
    if (new_max < (int32_t)fs->users[slot].allocated_object_blocks) return NDFS_ERR_OUT_OF_RANGE;
    if (new_max < 1) return NDFS_ERR_OUT_OF_RANGE;

    fs->users[slot].max_object_blocks = (uint8_t)new_max;

    err = write_user_page(fs, fs->users[slot].user_index);
    if (err != NDFS_OK) {
        fs->users[slot].max_object_blocks = (uint8_t)(new_max + (int32_t)count);
        return err;
    }

    if (out_max) *out_max = (uint8_t)new_max;
    return NDFS_OK;
}

ndfs_error_t ndfs_get_object_block_info(const ndfs_filesystem_t *fs,
                                        const char *user_name,
                                        ndfs_object_block_info_t *out_info)
{
    int slot;
    size_t i;
    const ndfs_user_entry_t *user;

    if (!fs || !user_name || !out_info) return NDFS_ERR_NULL_PTR;

    slot = find_user_by_name(fs, user_name);
    if (slot < 0) return NDFS_ERR_NOT_FOUND;

    user = &fs->users[slot];
    memset(out_info, 0, sizeof(*out_info));

    strncpy(out_info->user_name, user->user_name, NDFS_NAME_MAX);
    out_info->user_name[NDFS_NAME_MAX] = '\0';
    out_info->user_index              = user->user_index;
    out_info->allocated_object_blocks = user->allocated_object_blocks;
    out_info->max_object_blocks       = user->max_object_blocks;
    out_info->max_files       = (uint32_t)user->max_object_blocks * NDFS_FILES_PER_OBJECT_BLOCK;
    out_info->allocated_files = (uint32_t)user->allocated_object_blocks * NDFS_FILES_PER_OBJECT_BLOCK;
    out_info->grantable_blocks =
        (uint8_t)(NDFS_MAX_OBJECT_BLOCKS - user->max_object_blocks);

    /* Counted live rather than read from a stored total: SINTRAN never compacts
     * the object table, so deleting a file leaves a hole and any cached figure
     * would drift. */
    for (i = 0; i < fs->object_count; i++) {
        if (fs->objects[i].user_index == user->user_index) out_info->files_in_use++;
    }

    return NDFS_OK;
}

/* ── Bitmap queries ──────────────────────────────────────────────── */

bool ndfs_is_block_used(const ndfs_filesystem_t *fs, uint32_t block_id)
{
    if (!fs) return false;
    return ndfs_bf_is_used(&fs->bit_file, block_id);
}

bool ndfs_is_unaligned(const ndfs_filesystem_t *fs)
{
    if (!fs) return false;
    return fs->unaligned;
}

ndfs_error_t ndfs_get_free_pages(const ndfs_filesystem_t *fs, uint32_t *out)
{
    if (!fs || !out) return NDFS_ERR_NULL_PTR;
    *out = ndfs_bf_count_free(&fs->bit_file);
    return NDFS_OK;
}

ndfs_error_t ndfs_get_used_pages(const ndfs_filesystem_t *fs, uint32_t *out)
{
    if (!fs || !out) return NDFS_ERR_NULL_PTR;
    *out = ndfs_bf_count_used(&fs->bit_file);
    return NDFS_OK;
}

ndfs_error_t ndfs_get_damage_report(const ndfs_filesystem_t *fs,
                                    ndfs_damage_report_t *out)
{
    if (!fs || !out) return NDFS_ERR_NULL_PTR;
    *out = fs->damage;
    return NDFS_OK;
}

/* ── Low-level object access ────────────────────────────────────── */

ndfs_error_t ndfs_get_object_entries(const ndfs_filesystem_t *fs,
                                     ndfs_object_entry_t **out_entries,
                                     size_t *out_count)
{
    ndfs_object_entry_t *arr;

    if (!fs || !out_entries || !out_count) return NDFS_ERR_NULL_PTR;

    if (fs->object_count == 0) {
        *out_entries = NULL;
        *out_count = 0;
        return NDFS_OK;
    }

    arr = (ndfs_object_entry_t *)malloc(
        fs->object_count * sizeof(ndfs_object_entry_t));
    if (!arr) return NDFS_ERR_ALLOC;

    memcpy(arr, fs->objects,
           fs->object_count * sizeof(ndfs_object_entry_t));

    *out_entries = arr;
    *out_count = fs->object_count;
    return NDFS_OK;
}

ndfs_error_t ndfs_get_object_entry(const ndfs_filesystem_t *fs,
                                   const char *name,
                                   const char *user_name,
                                   ndfs_object_entry_t *out_entry)
{
    char search_path[256];

    if (!fs || !name || !out_entry) return NDFS_ERR_NULL_PTR;

    /* Build a path string that find_object can parse */
    if (user_name && user_name[0] != '\0') {
        snprintf(search_path, sizeof(search_path), "%s/%s", user_name, name);
    } else {
        snprintf(search_path, sizeof(search_path), "%s", name);
    }

    {
        int idx = find_object(fs, search_path);
        if (idx < 0) return NDFS_ERR_NOT_FOUND;
        *out_entry = fs->objects[idx];
    }

    return NDFS_OK;
}

void ndfs_free_object_entries(ndfs_object_entry_t *entries)
{
    free(entries);
}

/* ── Diagnostics ─────────────────────────────────────────────────── */

ndfs_error_t ndfs_verify_integrity(const ndfs_filesystem_t *fs, bool *out_ok)
{
    size_t i;

    if (!fs || !out_ok) return NDFS_ERR_NULL_PTR;

    if (!ndfs_mb_is_valid(&fs->master_block)) {
        *out_ok = false;
        return NDFS_OK;
    }

    for (i = 0; i < fs->object_count; i++) {
        const ndfs_object_entry_t *obj = &fs->objects[i];
        if (!ndfs_bp_is_valid(&obj->file_pointer)) continue;
        if (!ndfs_bf_is_used(&fs->bit_file, obj->file_pointer.block_id)) {
            *out_ok = false;
            return NDFS_OK;
        }
    }

    *out_ok = true;
    return NDFS_OK;
}

/* -- fsck -----------------------------------------------------------
 *
 * Phase 2 builds a map "which structure or file uses each block" that mirrors the PROVEN
 * read/free enumerators (read_indexed_data / read_subindexed_data / free_file_blocks and the
 * load_structures walks), so fsck sees exactly the blocks the rest of the code treats as live.
 * An earlier fsck used a second, incomplete walk that missed whole classes of blocks (subindexed
 * files, the multi page bitmap, the object and user file DATA pages) and reported them as
 * phantom orphans.
 *
 * The map keeps, per block, the FIRST owner and the position inside that owner, so a block used
 * twice can be reported with both owners. See docs/FSCK-FIX-PLAN.md. */

#define FSCK_OWNER_NONE    0u   /* the image as a whole (master block, pointers) */
#define FSCK_OWNER_OBJ     1u   /* object file structure */
#define FSCK_OWNER_USR     2u   /* user file structure */
#define FSCK_OWNER_BIT     3u   /* bit file */
#define FSCK_OWNER_FILE0  16u   /* + index into fs->objects */
#define FSCK_POS_STRUCT   0xFFFFFFFFu  /* the index or sub-index block itself, not a data page */
#define FSCK_MAX_RECORDS  100000u

typedef enum {
    FSCK_KIND_MASTER = 0,   /* master block or one of its pointers */
    FSCK_KIND_PTR,          /* object entry with an unusable pointer */
    FSCK_KIND_OOB,          /* pointer outside the device */
    FSCK_KIND_UNREADABLE,   /* index or sub-index block that cannot be read */
    FSCK_KIND_CROSS,        /* block used by two owners */
    FSCK_KIND_FREE          /* block used but marked free in the bit file */
} fsck_kind_t;

typedef struct {
    fsck_kind_t kind;
    uint32_t    owner;
    uint32_t    pos;
    uint32_t    block;
    char        reason[160];
} fsck_rec_t;

typedef struct {
    uint32_t block;
    uint32_t a_owner, a_pos;
    uint32_t b_owner, b_pos;
} fsck_cross_t;

typedef struct {
    uint32_t  *v;
    size_t     n, cap;
} fsck_list_t;

typedef struct {
    const struct ndfs_filesystem *fs;
    uint32_t       total;
    uint8_t       *refcount;
    uint32_t      *owner;
    uint32_t      *pos;
    uint32_t       cur_owner;
    uint32_t       cur_pos;
    fsck_cross_t  *cross;
    size_t         n_cross, cap_cross;
    fsck_rec_t    *rec;
    size_t         n_rec, cap_rec, dropped_rec;
    uint32_t       n_oob;
    uint32_t       n_reserved;     /* pointers into blocks 1..6, ignored */
    uint32_t       n_unreadable;
    bool           oom;
} fsck_map_t;

typedef struct {
    char   *buf;
    size_t  len, cap;
    bool    oom;
} fsck_text_t;

static void fsck_say(fsck_text_t *t, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (t->oom) return;
    for (;;) {
        va_start(ap, fmt);
        n = vsnprintf(t->buf + t->len, t->cap - t->len, fmt, ap);
        va_end(ap);
        if (n < 0) { t->oom = true; return; }
        if ((size_t)n < t->cap - t->len) { t->len += (size_t)n; return; }
        {
            size_t ncap = t->cap * 2 + (size_t)n + 64;
            char *nb = (char *)realloc(t->buf, ncap);
            if (!nb) { t->oom = true; return; }
            t->buf = nb;
            t->cap = ncap;
        }
    }
}

static void fsck_list_add(fsck_list_t *l, uint32_t v, bool *oom)
{
    if (l->n == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 256;
        uint32_t *nv = (uint32_t *)realloc(l->v, ncap * sizeof(uint32_t));
        if (!nv) { *oom = true; return; }
        l->v = nv;
        l->cap = ncap;
    }
    l->v[l->n++] = v;
}

static void fsck_owner_name(const struct ndfs_filesystem *fs, uint32_t owner, char *out, size_t n)
{
    if (owner == FSCK_OWNER_OBJ) { snprintf(out, n, "(object file)"); return; }
    if (owner == FSCK_OWNER_USR) { snprintf(out, n, "(user file)"); return; }
    if (owner == FSCK_OWNER_BIT) { snprintf(out, n, "(bit file)"); return; }
    if (owner >= FSCK_OWNER_FILE0 && owner - FSCK_OWNER_FILE0 < fs->object_count) {
        const ndfs_object_entry_t *o = &fs->objects[owner - FSCK_OWNER_FILE0];
        snprintf(out, n, "%s/%s:%s", o->user_name, o->object_name, o->type);
        return;
    }
    snprintf(out, n, "(image)");
}

static void fsck_fail(fsck_map_t *m, fsck_kind_t kind, uint32_t owner, uint32_t pos,
                      uint32_t block, const char *fmt, ...)
{
    va_list ap;
    fsck_rec_t *r;

    if (m->n_rec >= FSCK_MAX_RECORDS) { m->dropped_rec++; return; }
    if (m->n_rec == m->cap_rec) {
        size_t ncap = m->cap_rec ? m->cap_rec * 2 : 64;
        fsck_rec_t *nr = (fsck_rec_t *)realloc(m->rec, ncap * sizeof(fsck_rec_t));
        if (!nr) { m->oom = true; return; }
        m->rec = nr;
        m->cap_rec = ncap;
    }
    r = &m->rec[m->n_rec++];
    r->kind = kind;
    r->owner = owner;
    r->pos = pos;
    r->block = block;
    va_start(ap, fmt);
    vsnprintf(r->reason, sizeof(r->reason), fmt, ap);
    va_end(ap);
}

/* Count one reference to a block (bounds and saturation checked).
 *
 * Block 0 is a sparse hole and has no disk page. Blocks 1..6 are the reserved range (the
 * master block and its fixed system structures); a pointer INTO that range is garbage,
 * typically a stale byte pattern in an unused index slot on a rewritten pack. It is counted
 * in n_reserved and never in refcount: counting it used to manufacture phantom "cross-linked"
 * reports on blocks 1..6. A pointer at or above the device size is a real error. */
static void fsck_ref(fsck_map_t *m, uint32_t block)
{
    if (block == 0) return;
    if (block >= m->total) {
        m->n_oob++;
        fsck_fail(m, FSCK_KIND_OOB, m->cur_owner, m->cur_pos, block,
                  "pointer to block %u, outside the device (%u pages)", block, m->total);
        return;
    }
    if (block < NDFS_FIRST_ALLOC_BLOCK) { m->n_reserved++; return; }

    if (m->refcount[block] > 0) {
        if (m->n_cross < FSCK_MAX_RECORDS) {
            fsck_cross_t *c;
            if (m->n_cross == m->cap_cross) {
                size_t ncap = m->cap_cross ? m->cap_cross * 2 : 64;
                fsck_cross_t *nc = (fsck_cross_t *)realloc(m->cross, ncap * sizeof(fsck_cross_t));
                if (!nc) { m->oom = true; return; }
                m->cross = nc;
                m->cap_cross = ncap;
            }
            c = &m->cross[m->n_cross++];
            c->block = block;
            c->a_owner = m->owner[block];
            c->a_pos = m->pos[block];
            c->b_owner = m->cur_owner;
            c->b_pos = m->cur_pos;
        }
    } else {
        m->owner[block] = m->cur_owner;
        m->pos[block] = m->cur_pos;
    }
    if (m->refcount[block] < 255) m->refcount[block]++;
}

/* Mark every real (non-hole) DATA pointer stored in ONE index block. `max_ptrs` bounds the scan:
 * NDFS_MAX_OBJECT_FILE_PTRS for object/data index blocks, NDFS_MAX_USER_FILE_PTRS for the user
 * file index block. Mirrors read_indexed_data() / count_real_data_pages_in_object(). The index
 * block ITSELF is the caller's responsibility to count. `base_pos` is the position (page in the
 * file) of slot 0. */
static void fsck_ref_index_data(fsck_map_t *m, uint32_t index_block_id, size_t max_ptrs,
                                uint32_t base_pos)
{
    const struct ndfs_filesystem *fs = m->fs;
    const uint8_t *idx_page;
    size_t k;

    if (index_block_id == 0 || index_block_id >= m->total) return;
    idx_page = read_page(fs, index_block_id);
    if (!idx_page) {
        m->n_unreadable++;
        fsck_fail(m, FSCK_KIND_UNREADABLE, m->cur_owner, FSCK_POS_STRUCT, index_block_id,
                  "index block %u cannot be read", index_block_id);
        return;
    }
    /* idx_page is held across up to max_ptrs reads of it -- pin its slot. */
    cache_pin(fs, index_block_id);
    for (k = 0; k < max_ptrs; k++) {
        ndfs_block_pointer_t p = ndfs_bp_from_bytes(idx_page, k * 4);
        if (p.block_id > 0) {
            m->cur_pos = base_pos + (uint32_t)k;
            fsck_ref(m, p.block_id);
        }
    }
    cache_unpin(fs, index_block_id);
}

/* Fully walk a SUBINDEXED structure: the sub-index block points at group-index blocks (each a
 * structural block), and each group-index block points at data pages. Mirrors
 * read_subindexed_data() / free_file_blocks()'s subindexed arm. The sub-index block ITSELF is
 * the caller's responsibility to count. */
static void fsck_ref_subindexed(fsck_map_t *m, uint32_t sub_index_block_id)
{
    const struct ndfs_filesystem *fs = m->fs;
    const uint8_t *sub_page;
    size_t si;

    if (sub_index_block_id == 0 || sub_index_block_id >= m->total) return;
    sub_page = read_page(fs, sub_index_block_id);
    if (!sub_page) {
        m->n_unreadable++;
        fsck_fail(m, FSCK_KIND_UNREADABLE, m->cur_owner, FSCK_POS_STRUCT, sub_index_block_id,
                  "sub-index block %u cannot be read", sub_index_block_id);
        return;
    }
    /* sub_page held across the whole walk -- pin it. */
    cache_pin(fs, sub_index_block_id);
    for (si = 0; si < NDFS_MAX_OBJECT_FILE_PTRS; si++) {
        ndfs_block_pointer_t idx_ptr = ndfs_bp_from_bytes(sub_page, si * 4);
        if (!ndfs_bp_is_valid(&idx_ptr)) continue;
        /* the group-index block is itself a real, allocated structural block */
        m->cur_pos = FSCK_POS_STRUCT;
        fsck_ref(m, idx_ptr.block_id);
        /* ...and the data pages it points at */
        fsck_ref_index_data(m, idx_ptr.block_id, NDFS_MAX_OBJECT_FILE_PTRS,
                            (uint32_t)(si * NDFS_MAX_OBJECT_FILE_PTRS));
    }
    cache_unpin(fs, sub_index_block_id);
}

/* ---- analysis state ------------------------------------------------ */

typedef struct {
    int          errors;
    int          warnings;
    fsck_map_t   map;
    fsck_list_t  orphans;          /* marked used, no owner, below the declared capacity */
    fsck_list_t  free_ref;         /* used by a file or structure, marked free */
    uint32_t     spare_marked_used;
    uint32_t     multiply_referenced;
    uint32_t     reserved_unmarked;/* blocks 0..6 not marked used */
    bool         ext_checksum_bad;
    bool         map_complete;     /* no master/pointer error, no outside pointer, no unreadable block */
    bool         oom;
} fsck_state_t;

static void fsck_state_free(fsck_state_t *st)
{
    free(st->map.refcount);
    free(st->map.owner);
    free(st->map.pos);
    free(st->map.cross);
    free(st->map.rec);
    free(st->orphans.v);
    free(st->free_ref.v);
    memset(st, 0, sizeof(*st));
}

static bool fsck_page_all_zero(const struct ndfs_filesystem *fs, uint32_t block)
{
    const uint8_t *p = read_page(fs, block);
    size_t i;
    if (!p) return false;
    for (i = 0; i < NDFS_PAGE_SIZE; i++)
        if (p[i]) return false;
    return true;
}

/* Print up to `limit` run lines "first..last (n blocks)" for a sorted block list. */
static void fsck_say_runs(fsck_text_t *t, const fsck_list_t *l, size_t limit)
{
    size_t i = 0, shown = 0;
    while (i < l->n) {
        size_t j = i;
        while (j + 1 < l->n && l->v[j + 1] == l->v[j] + 1) j++;
        if (shown < limit) {
            if (j == i) fsck_say(t, "      block %u\n", l->v[i]);
            else        fsck_say(t, "      blocks %u..%u (%u blocks)\n", l->v[i], l->v[j],
                                 (unsigned)(j - i + 1));
        }
        shown++;
        i = j + 1;
    }
    if (shown > limit) fsck_say(t, "      ... and %u more runs\n", (unsigned)(shown - limit));
}

static void fsck_say_pos(fsck_text_t *t, uint32_t pos)
{
    if (pos == FSCK_POS_STRUCT) fsck_say(t, "index block");
    else                        fsck_say(t, "page %u", pos);
}

static ndfs_error_t fsck_analyze(const struct ndfs_filesystem *fs, bool verbose,
                                 fsck_text_t *t, fsck_state_t *st)
{
    fsck_map_t *m = &st->map;
    const ndfs_master_block_t *mb = &fs->master_block;
    uint32_t total_pages = (uint32_t)(fs->size / NDFS_PAGE_SIZE);
    uint32_t block;
    size_t i, j;
    bool master_ok = true;

    memset(st, 0, sizeof(*st));
    m->fs = fs;
    m->total = total_pages;

    fsck_say(t, "NDFS Filesystem Check\n");
    fsck_say(t, "=====================\n");
    fsck_say(t, "Volume: %s\n", mb->directory_name);
    fsck_say(t, "Total pages: %u\n\n", total_pages);

    /* --- Phase 1: Master block validation --- */
    fsck_say(t, "Phase 1: Master block\n");
    if (!ndfs_mb_is_valid(mb)) {
        fsck_say(t, "  ERROR: Master block is invalid\n");
        fsck_fail(m, FSCK_KIND_MASTER, FSCK_OWNER_NONE, FSCK_POS_STRUCT, 0, "master block is invalid");
        st->errors++;
        master_ok = false;
    } else {
        fsck_say(t, "  OK: Master block valid, directory name '%s'\n", mb->directory_name);
    }
    if (!ndfs_bp_is_valid(&mb->object_file_ptr)) {
        fsck_say(t, "  ERROR: Object file pointer invalid\n");
        fsck_fail(m, FSCK_KIND_MASTER, FSCK_OWNER_NONE, FSCK_POS_STRUCT, 0, "object file pointer invalid");
        st->errors++;
        master_ok = false;
    }
    if (!ndfs_bp_is_valid(&mb->user_file_ptr)) {
        fsck_say(t, "  ERROR: User file pointer invalid\n");
        fsck_fail(m, FSCK_KIND_MASTER, FSCK_OWNER_NONE, FSCK_POS_STRUCT, 0, "user file pointer invalid");
        st->errors++;
        master_ok = false;
    }
    if (!ndfs_bp_is_valid(&mb->bit_file_ptr)) {
        fsck_say(t, "  ERROR: Bit file pointer invalid\n");
        fsck_fail(m, FSCK_KIND_MASTER, FSCK_OWNER_NONE, FSCK_POS_STRUCT, 0, "bit file pointer invalid");
        st->errors++;
        master_ok = false;
    }
    /* A wrong Extended Info Block checksum loses no data (SINTRAN rebuilds it on enter).
     * Only reported when the block is there: a floppy has none (all words zero), and a
     * FLOMON boot area is no Extended Info Block. */
    if (!mb->has_flomon && mb->checksum_state == NDFS_CHECKSUM_INVALID &&
        (mb->ext_checksum || mb->ext_reserved1 || mb->ext_reserved2 || mb->ext_reserved3 ||
         mb->ext_flag_word || mb->ext_last_system_number || mb->ext_pages_available)) {
        fsck_say(t, "  WARNING: Extended Info Block checksum is %04X, the words give %04X\n",
                 mb->ext_checksum, mb->ext_calculated_checksum);
        st->warnings++;
        st->ext_checksum_bad = true;
    }

    /* --- Phase 2: Build reference count map --- */
    fsck_say(t, "\nPhase 2: Block reference analysis\n");

    m->refcount = (uint8_t *)calloc(total_pages, sizeof(uint8_t));
    m->owner    = (uint32_t *)calloc(total_pages, sizeof(uint32_t));
    m->pos      = (uint32_t *)calloc(total_pages, sizeof(uint32_t));
    if (!m->refcount || !m->owner || !m->pos) { fsck_state_free(st); return NDFS_ERR_ALLOC; }

    /* Object file: its index/sub-index block + all its data (object) pages. */
    if (ndfs_bp_is_valid(&mb->object_file_ptr)) {
        uint32_t ob = mb->object_file_ptr.block_id;
        m->cur_owner = FSCK_OWNER_OBJ;
        m->cur_pos = FSCK_POS_STRUCT;
        fsck_ref(m, ob);
        if (mb->object_file_ptr.type == NDFS_PTR_SUBINDEXED)
            fsck_ref_subindexed(m, ob);
        else
            fsck_ref_index_data(m, ob, NDFS_MAX_OBJECT_FILE_PTRS, 0);
    }

    /* User file: its index block + up to 8 user data pages (see load_structures). */
    if (ndfs_bp_is_valid(&mb->user_file_ptr)) {
        uint32_t ub = mb->user_file_ptr.block_id;
        m->cur_owner = FSCK_OWNER_USR;
        m->cur_pos = FSCK_POS_STRUCT;
        fsck_ref(m, ub);
        fsck_ref_index_data(m, ub, NDFS_MAX_USER_FILE_PTRS, 0);
    }

    /* Bit file: the bitmap spans several CONTIGUOUS pages, not just block_id. */
    if (ndfs_bp_is_valid(&mb->bit_file_ptr)) {
        uint32_t bitmap_pages = bit_file_disk_pages(mb, total_pages);
        uint32_t bb = mb->bit_file_ptr.block_id;
        uint32_t p;
        m->cur_owner = FSCK_OWNER_BIT;
        for (p = 0; p < bitmap_pages; p++) {
            m->cur_pos = p;
            fsck_ref(m, bb + p);
        }
    }

    /* Walk all file pointers */
    for (i = 0; i < fs->object_count; i++) {
        const ndfs_object_entry_t *obj = &fs->objects[i];
        uint32_t fp_block;

        if (!ndfs_bp_is_valid(&obj->file_pointer)) continue;
        fp_block = obj->file_pointer.block_id;
        m->cur_owner = FSCK_OWNER_FILE0 + (uint32_t)i;

        if (fp_block >= total_pages) {
            fsck_say(t, "  ERROR: File '%s:%s' (user %s) pointer block %u out of range\n",
                     obj->object_name, obj->type, obj->user_name, fp_block);
            fsck_fail(m, FSCK_KIND_PTR, m->cur_owner, FSCK_POS_STRUCT, fp_block,
                      "file pointer to block %u, outside the device (%u pages)", fp_block, total_pages);
            st->errors++;
            continue;
        }

        if (obj->file_pointer.type == NDFS_PTR_INDEXED) {
            /* fp_block is a real, allocated structural index block: count it once, then each
             * real data page it points at. The WHOLE index (all 512 slots) is scanned and only
             * non-hole pointers counted, exactly like count_real_data_pages_in_object(): the
             * index, not bytes_in_file, is the truth of what pages a file physically owns. */
            m->cur_pos = FSCK_POS_STRUCT;
            fsck_ref(m, fp_block);
            fsck_ref_index_data(m, fp_block, NDFS_MAX_OBJECT_FILE_PTRS, 0);
        } else if (obj->file_pointer.type == NDFS_PTR_CONTIGUOUS) {
            /* For a CONTIGUOUS file, fp_block IS the first data page; there is no separate
             * index block, so it must not be counted twice. */
            uint32_t last = fp_block + (obj->pages_in_file ? obj->pages_in_file - 1u : 0u);
            if (obj->pages_in_file && (last >= total_pages || last < fp_block)) {
                fsck_say(t, "  ERROR: File '%s:%s' (user %s) runs past the end of the device\n",
                         obj->object_name, obj->type, obj->user_name);
                fsck_fail(m, FSCK_KIND_OOB, m->cur_owner, total_pages > fp_block ? total_pages - fp_block : 0u,
                          total_pages, "contiguous run of %u pages from block %u passes the end of the device (%u pages)",
                          obj->pages_in_file, fp_block, total_pages);
                st->errors++;
            }
            for (j = 0; j < obj->pages_in_file; j++) {
                uint32_t db = fp_block + (uint32_t)j;
                if (db >= total_pages) break;
                m->cur_pos = (uint32_t)j;
                fsck_ref(m, db);
            }
        } else if (obj->file_pointer.type == NDFS_PTR_SUBINDEXED) {
            m->cur_pos = FSCK_POS_STRUCT;
            fsck_ref(m, fp_block);
            fsck_ref_subindexed(m, fp_block);
        }
    }

    /* Pointers outside the device and index blocks that could not be read, one line each. */
    {
        size_t shown = 0;
        for (i = 0; i < m->n_rec; i++) {
            const fsck_rec_t *r = &m->rec[i];
            char who[96];
            if (r->kind != FSCK_KIND_OOB && r->kind != FSCK_KIND_UNREADABLE) continue;
            if (r->kind == FSCK_KIND_OOB && r->owner >= FSCK_OWNER_FILE0 &&
                strncmp(r->reason, "contiguous run", 14) == 0) continue;   /* already printed */
            fsck_owner_name(fs, r->owner, who, sizeof(who));
            if (shown < 100)
                fsck_say(t, "  ERROR: %s: %s\n", who, r->reason);
            shown++;
            st->errors++;
        }
        if (shown > 100) fsck_say(t, "  ... and %u more\n", (unsigned)(shown - 100));
    }
    if (m->n_reserved > 0)
        fsck_say(t, "  INFO: %u pointers into the reserved blocks 1-6 were ignored\n", m->n_reserved);

    /* --- Phase 3: Bitmap vs reference check --- */
    fsck_say(t, "\nPhase 3: Bitmap consistency\n");
    {
        /* WHY pages at or above the declared capacity are not orphans.
         *
         * The "declared capacity" is the 32-bit Pages Available value in the Extended Info
         * Block on page 0 (offset 0x07DC, SINTRAN words 1756B-1757B). It is read on mount into
         * bit_file.alloc_ceiling. The bitmap covers the whole PHYSICAL device (e.g. 38400
         * pages), but the directory only hands out pages below the declared capacity (e.g.
         * 36945). The pages from capacity to the end of the device are the drive's spare area
         * for bad sectors. No file can legitimately own them, so "marked used, no owner" is
         * meaningless there.
         *
         * Measured on real images (read only): BIGDISK0-K-100.IMG leaves every bit in that area
         * clear, but BIGDISK0-M-103.IMG (1455 pages) and WD0-SINTRAN-M.IMG (2004 pages) have
         * EVERY bit in that area set. The old test reported all of them as orphans. Who sets
         * those bits is NOT known; they are counted separately and printed as an INFO line.
         * See docs/FSCK-ANALYSIS-AND-PLAN.md, Update 3.
         *
         * alloc_ceiling equals total_pages when no valid Extended Info Block exists
         * (floppies), which makes this area empty. */
        for (block = 0; block < total_pages; block++) {
            bool in_bitmap = ndfs_bf_is_used(&fs->bit_file, block);
            bool referenced = (m->refcount[block] > 0);

            if (block < NDFS_FIRST_ALLOC_BLOCK && !in_bitmap) st->reserved_unmarked++;

            if (in_bitmap && !referenced && block >= NDFS_FIRST_ALLOC_BLOCK) {
                if (block >= fs->bit_file.alloc_ceiling) st->spare_marked_used++;
                else fsck_list_add(&st->orphans, block, &st->oom);
            }
            if (!in_bitmap && referenced) fsck_list_add(&st->free_ref, block, &st->oom);
            if (m->refcount[block] > 1) st->multiply_referenced++;
        }

        if (st->orphans.n > 0) {
            fsck_say(t, "  WARNING: %u orphaned blocks (marked used but not referenced by any file)\n",
                     (unsigned)st->orphans.n);
            st->warnings++;
        } else {
            fsck_say(t, "  OK: No orphaned blocks\n");
        }

        if (st->spare_marked_used > 0) {
            fsck_say(t, "  INFO: %u pages at or above the declared capacity (%u) are marked used in the bitmap (spare area, not orphans)\n",
                     st->spare_marked_used, fs->bit_file.alloc_ceiling);
        }

        /* Not a finding: a directory made by SINTRAN M's CREATE-DIRECTORY leaves blocks 1-6 clear
         * in the bit file (measured once, on a SCSI disk, 2026-10-21), while the real system disks
         * WD0-SINTRAN-M.IMG and BIGDISK0-M-103.IMG have them set. R2 of docs/FSCK-FIX-PLAN.md
         * therefore does not repair this. */
        if (st->reserved_unmarked > 0) {
            fsck_say(t, "  INFO: %u of the reserved blocks 0-6 are not marked used in the bitmap (not repaired: SINTRAN M leaves blocks 1-6 clear itself)\n",
                     st->reserved_unmarked);
        }

        if (st->free_ref.n > 0) {
            fsck_say(t, "  ERROR: %u blocks referenced by files but marked FREE in bitmap!\n",
                     (unsigned)st->free_ref.n);
            st->errors++;
            for (i = 0; i < st->free_ref.n; i++) {
                uint32_t b = st->free_ref.v[i];
                fsck_fail(m, FSCK_KIND_FREE, m->owner[b], m->pos[b], b,
                          "block %u is in use but marked free in the bit file", b);
            }
        } else {
            fsck_say(t, "  OK: All referenced blocks marked used in bitmap\n");
        }

        if (st->multiply_referenced > 0) {
            fsck_say(t, "  ERROR: %u blocks referenced by multiple files (cross-linked)\n",
                     st->multiply_referenced);
            st->errors++;
            for (i = 0; i < m->n_cross; i++) {
                const fsck_cross_t *c = &m->cross[i];
                char na[96], nb[96];
                fsck_owner_name(fs, c->a_owner, na, sizeof(na));
                fsck_owner_name(fs, c->b_owner, nb, sizeof(nb));
                fsck_fail(m, FSCK_KIND_CROSS, c->a_owner, c->a_pos, c->block,
                          "block %u is also used by %s", c->block, nb);
                fsck_fail(m, FSCK_KIND_CROSS, c->b_owner, c->b_pos, c->block,
                          "block %u is also used by %s", c->block, na);
            }
        } else {
            fsck_say(t, "  OK: No cross-linked blocks\n");
        }
    }

    /* --- Phase 4: User quota verification --- */
    fsck_say(t, "\nPhase 4: User quota verification\n");
    for (i = 0; i < MAX_INTERNAL_USERS; i++) {
        const ndfs_user_entry_t *user;
        uint32_t actual_pages = 0;
        if (!fs->user_valid[i]) continue;
        user = &fs->users[i];

        /* Same basis pages_used is maintained with: count_real_data_pages_in_object() walks
         * each file's resolved block structure and counts only real (non-hole) pointers. */
        for (j = 0; j < fs->object_count; j++) {
            if (fs->objects[j].user_index == user->user_index)
                actual_pages += count_charged_pages_in_object(fs, &fs->objects[j]);
        }
        actual_pages += count_user_object_pages(fs, user->user_index);

        if (actual_pages != user->pages_used) {
            fsck_say(t, "  WARNING: User '%s' pages_used=%u but actual file pages=%u\n",
                     user->user_name, user->pages_used, actual_pages);
            st->warnings++;
        }
        if (user->pages_used > user->pages_reserved && user->pages_reserved > 0) {
            fsck_say(t, "  WARNING: User '%s' over quota: using %u, reserved %u\n",
                     user->user_name, user->pages_used, user->pages_reserved);
            st->warnings++;
        }
    }

    /* --- Phase 5: File structure validation --- */
    fsck_say(t, "\nPhase 5: File structure validation\n");
    {
        uint32_t zero_byte_files = 0;

        for (i = 0; i < fs->object_count; i++) {
            const ndfs_object_entry_t *obj = &fs->objects[i];

            if (obj->bytes_in_file == 0 && obj->pages_in_file > 0) zero_byte_files++;
            if (!ndfs_bp_is_valid(&obj->file_pointer) &&
                (obj->pages_in_file > 0 ||
                 (obj->file_pointer.type == NDFS_PTR_RESERVED && obj->file_pointer.block_id != 0))) {
                fsck_say(t, "  ERROR: File '%s:%s' has %u pages but invalid pointer\n",
                         obj->object_name, obj->type, obj->pages_in_file);
                fsck_fail(m, FSCK_KIND_PTR, FSCK_OWNER_FILE0 + (uint32_t)i, FSCK_POS_STRUCT,
                          obj->file_pointer.block_id,
                          "%u pages in file but the file pointer is not usable (block %u, type %u)",
                          obj->pages_in_file, obj->file_pointer.block_id, (unsigned)obj->file_pointer.type);
                st->errors++;
            }
        }
        if (zero_byte_files > 0)
            fsck_say(t, "  INFO: %u files with 0 bytes but allocated pages (device/system files)\n",
                     zero_byte_files);
    }

    /* The map is complete when nothing was skipped: a block that looks unreferenced may
     * belong to a structure that could not be followed. */
    st->map_complete = master_ok && m->n_oob == 0 && m->n_unreadable == 0;

    if (verbose) {
        fsck_say(t, "\nDetails (-v)\n");
        fsck_say(t, "  Orphan runs: %u blocks\n", (unsigned)st->orphans.n);
        fsck_say_runs(t, &st->orphans, 50);
        fsck_say(t, "  Blocks used twice: %u\n", st->multiply_referenced);
        for (i = 0; i < m->n_cross && i < 100; i++) {
            const fsck_cross_t *c = &m->cross[i];
            char na[96], nb[96];
            fsck_owner_name(fs, c->a_owner, na, sizeof(na));
            fsck_owner_name(fs, c->b_owner, nb, sizeof(nb));
            fsck_say(t, "    block %u: %s ", c->block, na);
            fsck_say_pos(t, c->a_pos);
            fsck_say(t, " and %s ", nb);
            fsck_say_pos(t, c->b_pos);
            fsck_say(t, "\n");
        }
        if (m->n_cross > 100) fsck_say(t, "    ... and %u more\n", (unsigned)(m->n_cross - 100));
        fsck_say(t, "  Quota per user (pages): reserved, stored used, counted from files, object pages\n");
        for (i = 0; i < MAX_INTERNAL_USERS; i++) {
            uint32_t files_pages = 0, obj_pages;
            if (!fs->user_valid[i]) continue;
            for (j = 0; j < fs->object_count; j++)
                if (fs->objects[j].user_index == fs->users[i].user_index)
                    files_pages += count_charged_pages_in_object(fs, &fs->objects[j]);
            obj_pages = count_user_object_pages(fs, fs->users[i].user_index);
            fsck_say(t, "    %-16s reserved %6u  stored %6u  files %6u  object pages %4u  difference %d\n",
                     fs->users[i].user_name, fs->users[i].pages_reserved, fs->users[i].pages_used,
                     files_pages, obj_pages,
                     (int)fs->users[i].pages_used - (int)(files_pages + obj_pages));
        }
        fsck_say(t, "  Files with 0 bytes but allocated pages:\n");
        {
            size_t shown = 0, total_zero = 0;
            for (i = 0; i < fs->object_count; i++) {
                const ndfs_object_entry_t *o = &fs->objects[i];
                if (o->bytes_in_file == 0 && o->pages_in_file > 0) {
                    if (shown < 50) { fsck_say(t, "    %s/%s:%s (%u pages)\n", o->user_name, o->object_name, o->type, o->pages_in_file); shown++; }
                    total_zero++;
                }
            }
            if (total_zero > shown) fsck_say(t, "    ... and %u more\n", (unsigned)(total_zero - shown));
        }
    }

    st->oom = st->oom || m->oom;
    return NDFS_OK;
}

/* ---- verdict, failed files ----------------------------------------- */

static ndfs_fsck_verdict_t fsck_verdict(const fsck_state_t *st)
{
    if (st->errors > 0) return NDFS_FSCK_FAILED;
    if (st->warnings > 0) return NDFS_FSCK_DAMAGED;
    return NDFS_FSCK_CLEAN;
}

static const char *fsck_verdict_name(ndfs_fsck_verdict_t v)
{
    return v == NDFS_FSCK_FAILED ? "FAILED" : v == NDFS_FSCK_DAMAGED ? "DAMAGED" : "CLEAN";
}

static void fsck_say_summary(fsck_text_t *t, const fsck_state_t *st)
{
    fsck_say(t, "\n");
    fsck_say(t, "========================================\n");
    if (st->errors == 0 && st->warnings == 0)
        fsck_say(t, "Filesystem is CLEAN. No errors, no warnings.\n");
    else
        fsck_say(t, "Found %d error(s) and %d warning(s).\n", st->errors, st->warnings);
}

/* The last lines of the report: the verdict, and every failed file with the first reason. */
static void fsck_say_verdict(fsck_text_t *t, const struct ndfs_filesystem *fs,
                             const fsck_state_t *st, bool machine_lines)
{
    const fsck_map_t *m = &st->map;
    ndfs_fsck_verdict_t v = fsck_verdict(st);
    size_t i, n_owner = 0, nslots = fs->object_count + FSCK_OWNER_FILE0 + 1;
    uint8_t *seen = (uint8_t *)calloc(nslots, 1);

    fsck_say(t, "\nIMAGE VERDICT: %s\n", fsck_verdict_name(v));
    if (v != NDFS_FSCK_FAILED) { free(seen); return; }

    for (i = 0; i < m->n_rec; i++) {
        const fsck_rec_t *r = &m->rec[i];
        if (r->owner < nslots && seen && !seen[r->owner]) { seen[r->owner] = 1; n_owner++; }
    }
    fsck_say(t, "FAILED FILES: %u\n", (unsigned)n_owner);
    if (seen) memset(seen, 0, nslots);
    for (i = 0; i < m->n_rec; i++) {
        const fsck_rec_t *r = &m->rec[i];
        char who[96];
        if (seen && r->owner < nslots) { if (seen[r->owner]) continue; seen[r->owner] = 1; }
        fsck_owner_name(fs, r->owner, who, sizeof(who));
        fsck_say(t, "  %s  ", who);
        if (r->pos == FSCK_POS_STRUCT) fsck_say(t, "(index block");
        else fsck_say(t, "(data page %u", r->pos);
        fsck_say(t, ", block %u): %s\n", r->block, r->reason);
    }
    if (m->dropped_rec) fsck_say(t, "  ... and %u more findings not kept\n", (unsigned)m->dropped_rec);

    if (machine_lines) {
        fsck_say(t, "\nFAILED|USER/NAME:TYPE|page-in-file|block|reason\n");
        for (i = 0; i < m->n_rec; i++) {
            const fsck_rec_t *r = &m->rec[i];
            char who[96];
            fsck_owner_name(fs, r->owner, who, sizeof(who));
            if (r->pos == FSCK_POS_STRUCT) fsck_say(t, "FAILED|%s|-|%u|%s\n", who, r->block, r->reason);
            else fsck_say(t, "FAILED|%s|%u|%u|%s\n", who, r->pos, r->block, r->reason);
        }
    }
    free(seen);
}

/* ---- repairs ------------------------------------------------------- */

#define FSCK_UNDO_MAGIC "NDFSUND1"
#define FSCK_UNDO_MAGIC_LEN 8u

static void undo_capture(struct ndfs_filesystem *fs, uint32_t block, const uint8_t *old)
{
    size_t need;

    if (!fs->undo.active || !fs->undo.seen || block >= fs->total_pages) return;
    if (fs->undo.seen[block]) return;
    fs->undo.seen[block] = 1;
    need = fs->undo.len + 4u + NDFS_PAGE_SIZE;
    if (need > fs->undo.cap) {
        size_t ncap = fs->undo.cap ? fs->undo.cap * 2 : 64u * 1024u;
        uint8_t *nd;
        while (ncap < need) ncap *= 2;
        nd = (uint8_t *)realloc(fs->undo.data, ncap);
        if (!nd) { fs->undo.failed = true; return; }
        fs->undo.data = nd;
        fs->undo.cap = ncap;
    }
    fs->undo.data[fs->undo.len + 0] = (uint8_t)(block & 0xFF);
    fs->undo.data[fs->undo.len + 1] = (uint8_t)((block >> 8) & 0xFF);
    fs->undo.data[fs->undo.len + 2] = (uint8_t)((block >> 16) & 0xFF);
    fs->undo.data[fs->undo.len + 3] = (uint8_t)((block >> 24) & 0xFF);
    memcpy(fs->undo.data + fs->undo.len + 4, old, NDFS_PAGE_SIZE);
    fs->undo.len += 4u + NDFS_PAGE_SIZE;
}

/* One run of orphan pages that hold data, to become a contiguous file. */
static ndfs_error_t fsck_recover_run(struct ndfs_filesystem *fs, int user_slot,
                                     uint32_t start, uint32_t pages)
{
    ndfs_user_entry_t *user = &fs->users[user_slot];
    char name[NDFS_NAME_MAX + 1];
    char path[NDFS_NAME_MAX * 2 + NDFS_TYPE_MAX + 8];
    uint32_t obj_index;

    snprintf(name, sizeof(name), "ORPHAN-%u", start);
    snprintf(path, sizeof(path), "%s/%s:DATA", user->user_name, name);
    if (find_object(fs, path) >= 0) return NDFS_ERR_ALREADY_EXISTS;

    {
        int32_t avail = (int32_t)user->pages_reserved - (int32_t)user->pages_used;
        if (avail < (int32_t)pages)
            user->pages_reserved += pages - (avail > 0 ? (uint32_t)avail : 0u);
    }
    {
        int alloc_err = allocate_user_object_slot(fs, user, &obj_index);
        if (alloc_err != NDFS_OK) return (ndfs_error_t)alloc_err;
    }
    if (ensure_object_dir_page(fs, obj_index) == 0) return NDFS_ERR_NO_SPACE;

    register_contiguous_run(fs, user, obj_index, name, "DATA", start, pages,
                            pages * (uint32_t)NDFS_PAGE_SIZE);
    return NDFS_OK;
}

static void fsck_say_block_line(fsck_text_t *t, bool dry, const char *rule, uint32_t block,
                                const char *what, size_t *shown, size_t limit)
{
    if (*shown < limit)
        fsck_say(t, "  %s%s block %u: %s\n", dry ? "would " : "", rule, block, what);
    (*shown)++;
}

/* Run the repairs R1 to R5 of docs/FSCK-FIX-PLAN.md. `st` is the analysis of the image as it is.
 * Returns the number of repairs done (or listed, in a dry run). */
static ndfs_error_t fsck_repair(struct ndfs_filesystem *fs, const ndfs_fsck_options_t *o,
                                fsck_state_t *st, fsck_text_t *t, int *n_repairs, bool *refused)
{
    const bool dry = o->dry_run;
    const fsck_map_t *m = &st->map;
    const ndfs_master_block_t *mb = &fs->master_block;
    size_t i, shown;
    int count = 0;
    bool bits_changed = false;
    bool wrote_page0 = false;

    *n_repairs = 0;
    *refused = false;
    fsck_say(t, "\nRepairs%s\n=======\n", dry ? " (dry run, nothing is written)" : "");

    if (!dry) {
        if (fs->read_only) {
            fsck_say(t, "  Not done: the image was opened read only.\n");
            *refused = true;
            return NDFS_ERR_READ_ONLY;
        }
        if (!o->force) {
            fsck_say(t, "  Not done: stop any emulator or SINTRAN that has this image entered, then run again with -f.\n");
            if (mb->ext_flag_word & NDFS_EXT_FLAG_ENTERED)
                fsck_say(t, "  The Extended Info Block says the directory was left entered by system %u.\n",
                         (unsigned)mb->ext_last_system_number);
            *refused = true;
            return NDFS_OK;
        }
        /* From here every page written is copied first, for --fsck-undo. */
        fs->undo.seen = (uint8_t *)calloc(fs->total_pages, 1);
        if (!fs->undo.seen) return NDFS_ERR_ALLOC;
        fs->undo.data = NULL; fs->undo.len = 0; fs->undo.cap = 0; fs->undo.failed = false;
        /* the header is written when the undo data is handed out */
        fs->undo.active = true;
    }

    /* R1: a page that a file or structure uses, marked free in the bit file. */
    shown = 0;
    for (i = 0; i < st->free_ref.n; i++) {
        uint32_t b = st->free_ref.v[i];
        char who[96], what[160];
        fsck_owner_name(fs, m->owner[b], who, sizeof(who));
        snprintf(what, sizeof(what), "bit set (used by %s, was free)", who);
        fsck_say_block_line(t, dry, "R1", b, what, &shown, 200);
        if (!dry) { ndfs_bf_mark_used(&fs->bit_file, b); bits_changed = true; }
        count++;
    }
    /* R4 and R5 use the reference map, so they run only when it is complete. */
    if (st->orphans.n > 0 && !st->map_complete) {
        fsck_say(t, "  Orphan repairs R4 and R5 skipped: the reference map is incomplete (%u pointers outside the device, %u unreadable index blocks, or a master block error), so a block that looks unreferenced may belong to a structure that could not be followed.\n",
                 st->map.n_oob, st->map.n_unreadable);
    } else if (st->orphans.n > 0) {
        fsck_list_t data_orphans;
        bool oom = false;
        memset(&data_orphans, 0, sizeof(data_orphans));
        /* R4: orphan page with all 2048 bytes zero: clear the bit. */
        shown = 0;
        for (i = 0; i < st->orphans.n; i++) {
            uint32_t b = st->orphans.v[i];
            if (fsck_page_all_zero(fs, b)) {
                fsck_say_block_line(t, dry, "R4", b, "bit cleared (was used, page all zero)", &shown, 200);
                if (!dry) { ndfs_bf_mark_free(&fs->bit_file, b); bits_changed = true; }
                count++;
            } else {
                fsck_list_add(&data_orphans, b, &oom);
            }
        }
        if (shown > 200) fsck_say(t, "  ... and %u more R4 repairs\n", (unsigned)(shown - 200));
        /* R5: orphan pages with data are never freed. */
        if (data_orphans.n > 0) {
            if (!o->recover_orphans_user) {
                fsck_say(t, "  %u orphan pages hold data and are kept as they are (use --recover-orphans USER to copy them into files).\n",
                         (unsigned)data_orphans.n);
            } else {
                int user_slot = find_user_by_name(fs, o->recover_orphans_user);
                if (user_slot < 0) {
                    fsck_say(t, "  R5 not done: user '%s' does not exist.\n", o->recover_orphans_user);
                } else {
                    size_t a = 0;
                    while (a < data_orphans.n) {
                        size_t b2 = a;
                        uint32_t start, pages;
                        while (b2 + 1 < data_orphans.n && data_orphans.v[b2 + 1] == data_orphans.v[b2] + 1) b2++;
                        start = data_orphans.v[a];
                        pages = (uint32_t)(b2 - a + 1);
                        if (dry) {
                            fsck_say(t, "  would R5 blocks %u..%u (%u pages): new file %s/ORPHAN-%u:DATA\n",
                                     start, start + pages - 1u, pages, o->recover_orphans_user, start);
                            count++;
                        } else {
                            ndfs_error_t e = fsck_recover_run(fs, user_slot, start, pages);
                            if (e == NDFS_OK) {
                                fsck_say(t, "  R5 blocks %u..%u (%u pages): new file %s/ORPHAN-%u:DATA, the pages stay where they are\n",
                                         start, start + pages - 1u, pages, o->recover_orphans_user, start);
                                count++;
                            } else {
                                fsck_say(t, "  R5 blocks %u..%u: not done (%s)\n", start, start + pages - 1u, ndfs_strerror(e));
                            }
                        }
                        a = b2 + 1;
                    }
                }
            }
        }
        free(data_orphans.v);
        if (oom) return NDFS_ERR_ALLOC;
    }
    /* R3: Extended Info Block checksum. */
    if (st->ext_checksum_bad) {
        fsck_say(t, "  %sR3 Extended Info Block checksum %04X -> %04X\n", dry ? "would " : "",
                 mb->ext_checksum, mb->ext_calculated_checksum);
        if (!dry) {
            uint8_t *page0 = write_page_ptr(fs, 0);
            if (page0) {
                ndfs_mb_write_extended(&fs->master_block, page0);
                fs->master_block.ext_checksum = fs->master_block.ext_calculated_checksum;
                fs->master_block.checksum_state = NDFS_CHECKSUM_VALID;
                wrote_page0 = true;
            }
        }
        count++;
    }
    (void)wrote_page0;

    if (!dry) {
        if (bits_changed) write_bit_file_pages(fs);
        cache_flush_all(fs);
        fs->undo.active = false;
        free(fs->undo.seen);
        fs->undo.seen = NULL;
        if (fs->undo.failed) {
            fsck_say(t, "  WARNING: the undo data could not be kept completely (out of memory).\n");
        }
    }
    if (count == 0) fsck_say(t, "  Nothing to repair.\n");
    else if (!dry) fsck_say(t, "  %d repair(s) done. The undo data lists every page changed.\n", count);
    *n_repairs = count;
    return NDFS_OK;
}

/* ---- public API ---------------------------------------------------- */

void ndfs_fsck_result_free(ndfs_fsck_result_t *r)
{
    if (!r) return;
    free(r->report);
    free(r->failures);
    free(r->undo_data);
    memset(r, 0, sizeof(*r));
}

ndfs_error_t ndfs_fsck_ex(ndfs_filesystem_t *fs, const ndfs_fsck_options_t *opts,
                          ndfs_fsck_result_t *out)
{
    ndfs_fsck_options_t o;
    fsck_text_t t;
    fsck_state_t st;
    ndfs_error_t err;
    int n_repairs = 0;
    bool refused = false;
    size_t i;

    if (!fs || !out) return NDFS_ERR_NULL_PTR;
    memset(out, 0, sizeof(*out));
    memset(&o, 0, sizeof(o));
    if (opts) o = *opts;

    memset(&t, 0, sizeof(t));
    t.cap = 8192;
    t.buf = (char *)malloc(t.cap);
    if (!t.buf) return NDFS_ERR_ALLOC;
    t.buf[0] = '\0';

    err = fsck_analyze(fs, o.verbose, &t, &st);
    if (err != NDFS_OK) { free(t.buf); return err; }
    fsck_say_summary(&t, &st);

    if (o.fix) {
        err = fsck_repair(fs, &o, &st, &t, &n_repairs, &refused);
        if (err != NDFS_OK && err != NDFS_ERR_READ_ONLY) {
            fsck_state_free(&st);
            free(t.buf);
            return err;
        }
        out->repairs = n_repairs;
        if (!o.dry_run && !refused && n_repairs > 0) {
            /* Check again on the repaired image: the verdict and the exit code are those of this run. */
            fsck_state_free(&st);
            fsck_say(&t, "\n\nCheck after repair\n-----------------\n");
            err = fsck_analyze(fs, false, &t, &st);
            if (err != NDFS_OK) { free(t.buf); return err; }
            fsck_say_summary(&t, &st);

            if (fs->undo.data) {
                out->undo_data = (uint8_t *)malloc(FSCK_UNDO_MAGIC_LEN + fs->undo.len);
                if (out->undo_data) {
                    memcpy(out->undo_data, FSCK_UNDO_MAGIC, FSCK_UNDO_MAGIC_LEN);
                    memcpy(out->undo_data + FSCK_UNDO_MAGIC_LEN, fs->undo.data, fs->undo.len);
                    out->undo_size = FSCK_UNDO_MAGIC_LEN + fs->undo.len;
                }
            }
        }
        free(fs->undo.data);
        fs->undo.data = NULL; fs->undo.len = 0; fs->undo.cap = 0;
    }
    out->refused = refused;

    fsck_say_verdict(&t, fs, &st, o.verbose);

    if (t.oom) { fsck_state_free(&st); free(t.buf); return NDFS_ERR_ALLOC; }

    out->errors = st.errors;
    out->warnings = st.warnings;
    out->verdict = fsck_verdict(&st);

    /* The failure list for callers: every finding, with the file it belongs to. */
    if (st.map.n_rec > 0) {
        out->failures = (ndfs_fsck_failure_t *)calloc(st.map.n_rec, sizeof(ndfs_fsck_failure_t));
        if (out->failures) {
            out->n_failures = st.map.n_rec;
            for (i = 0; i < st.map.n_rec; i++) {
                const fsck_rec_t *r = &st.map.rec[i];
                ndfs_fsck_failure_t *f = &out->failures[i];
                fsck_owner_name(fs, r->owner, f->path, sizeof(f->path));
                f->page_in_file = r->pos;
                f->block = r->block;
                snprintf(f->reason, sizeof(f->reason), "%s", r->reason);
            }
        }
    }
    out->report = t.buf;
    fsck_state_free(&st);
    return NDFS_OK;
}

ndfs_error_t ndfs_fsck(const ndfs_filesystem_t *fs, char **out_report, int *out_errors)
{
    ndfs_fsck_result_t r;
    ndfs_error_t err;

    if (!fs || !out_report || !out_errors) return NDFS_ERR_NULL_PTR;
    /* Read only: no option asks for a repair, so the handle is not written. */
    err = ndfs_fsck_ex((ndfs_filesystem_t *)(uintptr_t)fs, NULL, &r);
    if (err != NDFS_OK) return err;
    *out_report = r.report;
    r.report = NULL;
    *out_errors = r.errors;
    ndfs_fsck_result_free(&r);
    return NDFS_OK;
}

ndfs_error_t ndfs_fsck_undo(ndfs_filesystem_t *fs, const uint8_t *undo, size_t size,
                            uint32_t *out_pages)
{
    size_t off = FSCK_UNDO_MAGIC_LEN;
    uint32_t n = 0;

    if (!fs || !undo) return NDFS_ERR_NULL_PTR;
    if (fs->read_only) return NDFS_ERR_READ_ONLY;
    if (size < FSCK_UNDO_MAGIC_LEN || memcmp(undo, FSCK_UNDO_MAGIC, FSCK_UNDO_MAGIC_LEN) != 0)
        return NDFS_ERR_INVALID_ARG;
    if ((size - FSCK_UNDO_MAGIC_LEN) % (4u + NDFS_PAGE_SIZE) != 0) return NDFS_ERR_INVALID_ARG;

    /* Check every record before the first write. */
    while (off < size) {
        uint32_t block = (uint32_t)undo[off] | ((uint32_t)undo[off + 1] << 8) |
                         ((uint32_t)undo[off + 2] << 16) | ((uint32_t)undo[off + 3] << 24);
        if (block >= fs->total_pages) return NDFS_ERR_INVALID_ARG;
        off += 4u + NDFS_PAGE_SIZE;
    }
    for (off = FSCK_UNDO_MAGIC_LEN; off < size; off += 4u + NDFS_PAGE_SIZE) {
        uint32_t block = (uint32_t)undo[off] | ((uint32_t)undo[off + 1] << 8) |
                         ((uint32_t)undo[off + 2] << 16) | ((uint32_t)undo[off + 3] << 24);
        uint8_t *page = write_page_ptr(fs, block);
        if (!page) return NDFS_ERR_IO;
        memcpy(page, undo + off + 4, NDFS_PAGE_SIZE);
        n++;
    }
    if (out_pages) *out_pages = n;
    /* The in-memory structures are stale now: close the handle and open the image again. */
    return cache_flush_all(fs);
}

ndfs_error_t ndfs_generate_report(const ndfs_filesystem_t *fs, char **out_report)
{
    char *report;
    size_t cap = 4096;
    size_t len = 0;
    uint32_t total_pages, used_pages, free_pages;
    size_t user_count = 0;
    size_t i;
    int n;

    if (!fs || !out_report) return NDFS_ERR_NULL_PTR;

    report = (char *)malloc(cap);
    if (!report) return NDFS_ERR_ALLOC;

    total_pages = (uint32_t)(fs->size / NDFS_PAGE_SIZE);
    used_pages  = ndfs_bf_count_used(&fs->bit_file);
    free_pages  = ndfs_bf_count_free(&fs->bit_file);

    for (i = 0; i < MAX_INTERNAL_USERS; i++) {
        if (fs->user_valid[i]) user_count++;
    }

#define APPEND(...) do { \
    n = snprintf(report + len, cap - len, __VA_ARGS__); \
    if (n > 0) len += (size_t)n; \
    if (len + 256 > cap) { \
        cap *= 2; \
        report = (char *)realloc(report, cap); \
        if (!report) return NDFS_ERR_ALLOC; \
    } \
} while(0)

    APPEND("NDFS Filesystem Report\n");
    APPEND("======================\n");
    APPEND("Volume: %s\n", fs->master_block.directory_name);
    APPEND("Total pages: %u\n", total_pages);
    APPEND("Used pages: %u\n", used_pages);
    APPEND("Free pages: %u\n", free_pages);
    APPEND("Users: %u\n", (unsigned)user_count);
    APPEND("Files: %u\n\n", (unsigned)fs->object_count);

    APPEND("Users:\n");
    for (i = 0; i < MAX_INTERNAL_USERS; i++) {
        if (!fs->user_valid[i]) continue;
        APPEND("  [%u] %s - Reserved: %u, Used: %u\n",
               fs->users[i].user_index,
               fs->users[i].user_name,
               fs->users[i].pages_reserved,
               fs->users[i].pages_used);
    }

    APPEND("\nFiles:\n");
    for (i = 0; i < fs->object_count; i++) {
        const ndfs_object_entry_t *o = &fs->objects[i];
        APPEND("  %s/%s:%s - %u bytes (%u pages)\n",
               o->user_name, o->object_name, o->type,
               o->bytes_in_file, o->pages_in_file);
    }

#undef APPEND

    *out_report = report;
    return NDFS_OK;
}

void ndfs_free_string(char *str)
{
    free(str);
}

ndfs_error_t ndfs_to_buffer(const ndfs_filesystem_t *fs,
                            uint8_t **out_data,
                            size_t *out_size)
{
    uint8_t *copy;
    uint32_t i;

    if (!fs || !out_data || !out_size) return NDFS_ERR_NULL_PTR;

    copy = (uint8_t *)malloc(fs->size);
    if (!copy) return NDFS_ERR_ALLOC;

    /* Read every page through the seam rather than memcpy'ing a resident image
       (there no longer is one).  read_page returns the current contents of each
       page -- a dirty cache slot if the page was written this session, else a
       fresh backend load -- so the snapshot reflects all pending writes. Each
       returned pointer is used and dropped immediately (no pin needed). */
    for (i = 0; i < fs->total_pages; i++) {
        const uint8_t *page = read_page(fs, i);
        if (!page) { free(copy); return NDFS_ERR_IO; }
        memcpy(copy + (size_t)i * NDFS_PAGE_SIZE, page, NDFS_PAGE_SIZE);
    }

    *out_data = copy;
    *out_size = fs->size;
    return NDFS_OK;
}

/* ── XAT support ────────────────────────────────────────────────── */

ndfs_error_t ndfs_get_file_properties(const ndfs_filesystem_t *fs,
                                      const char *path,
                                      ndfs_xat_properties_t *out)
{
    int idx;
    ndfs_error_t err;

    if (!fs || !path || !out) return NDFS_ERR_NULL_PTR;

    idx = find_object(fs, path);
    if (idx < 0) return NDFS_ERR_NOT_FOUND;

    err = ndfs_xat_from_object(&fs->objects[idx], out);
    if (err != NDFS_OK) return err;

    /* Record where the sparse holes are. Only the filesystem can: they live in
     * the file's index, not in the object entry. Without this the sidecar says a
     * file IS sparse (bytes / 2048 exceeds pages_in_file) but not where, so a
     * copy out and back would return it with a different layout. */
    {
        uint32_t *blocks = NULL;
        size_t count = 0;

        if (ndfs_get_file_blocks(fs, path, &blocks, &count) == NDFS_OK) {
            uint32_t *holes = NULL;
            size_t holes_n = 0, i;

            if (count) {
                holes = (uint32_t *)malloc(count * sizeof(uint32_t));
                if (!holes) { free(blocks); return NDFS_ERR_ALLOC; }
            }
            for (i = 0; i < count; i++) {
                if (blocks[i] == 0) holes[holes_n++] = (uint32_t)i;
            }

            ndfs_xat_set_holes(out, holes, holes_n);

            free(holes);
            free(blocks);
        }
        /* On failure the hole list stays "not determined" and the key is omitted,
         * rather than claiming the file is solid. */
    }

    return NDFS_OK;
}

const char *ndfs_strerror(ndfs_error_t err)
{
    switch (err) {
        case NDFS_OK:               return "Success";
        case NDFS_ERR_NULL_PTR:     return "Null pointer";
        case NDFS_ERR_INVALID_ARG:  return "Invalid argument";
        case NDFS_ERR_TOO_SMALL:    return "Buffer too small";
        case NDFS_ERR_NOT_ALIGNED:  return "Size not aligned to page boundary";
        case NDFS_ERR_INVALID_IMAGE:return "Invalid NDFS image";
        case NDFS_ERR_OUT_OF_RANGE: return "Block ID out of range";
        case NDFS_ERR_NOT_FOUND:    return "Not found";
        case NDFS_ERR_ALREADY_EXISTS:return "Already exists";
        case NDFS_ERR_NO_SPACE:     return "No free space";
        case NDFS_ERR_READ_ONLY:    return "Filesystem is read-only";
        case NDFS_ERR_NO_SLOTS:     return "No available slots";
        case NDFS_ERR_HAS_FILES:    return "User has files";
        case NDFS_ERR_ALLOC:        return "Memory allocation failed";
        case NDFS_ERR_CORRUPT:      return "Filesystem data corrupt";
        case NDFS_ERR_IO:           return "Block backend I/O failed";
        default:                    return "Unknown error";
    }
}
