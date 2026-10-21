# libndfs (C)

Read and write NDFS (Norsk Data File System) disk images from Sintran-III minicomputers.

Pure C99 library with no external dependencies. Suitable for embedded systems and as a base for FFI bindings.

108 tests. Includes [ndtool](tools/ndtool/) CLI.

## Build

```bash
mkdir build && cd build
cmake ..
make
ctest --output-on-failure
```

### Build Options

```bash
cmake .. -DBUILD_SHARED_LIBS=ON    # Build shared library instead of static
cmake .. -DCMAKE_BUILD_TYPE=Debug  # Debug build with symbols
```

## Quick Start

```c
#include <ndfs/ndfs.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    // Read image from file
    FILE *fp = fopen("disk.ndfs", "rb");
    fseek(fp, 0, SEEK_END);
    size_t size = ftell(fp);
    rewind(fp);
    uint8_t *buf = malloc(size);
    fread(buf, 1, size, fp);
    fclose(fp);

    // Open filesystem
    ndfs_filesystem_t *fs = NULL;
    ndfs_error_t err = ndfs_open_buffer_copy(buf, size, false, &fs);
    free(buf);  // safe - library made a copy
    if (err != NDFS_OK) {
        fprintf(stderr, "Error: %s\n", ndfs_strerror(err));
        return 1;
    }

    // Get volume name
    char name[NDFS_NAME_MAX + 1];
    ndfs_get_directory_name(fs, name, sizeof(name));
    printf("Volume: %s\n", name);

    // List root directory (users)
    ndfs_file_entry_t *entries = NULL;
    size_t count = 0;
    ndfs_list_directory(fs, "", &entries, &count);
    for (size_t i = 0; i < count; i++) {
        printf("  User: %s\n", entries[i].name);
    }
    ndfs_free_entries(entries);

    // Read a file
    uint8_t *data = NULL;
    size_t data_size = 0;
    err = ndfs_read_file(fs, "SYSTEM/README:TEXT", &data, &data_size);
    if (err == NDFS_OK) {
        printf("Content: %.*s\n", (int)data_size, data);
        ndfs_free_data(data);
    }

    // Write a file
    const char *msg = "Hello NDFS!";
    ndfs_write_file(fs, "SYSTEM/HELLO:DATA",
                    (const uint8_t *)msg, strlen(msg));

    // Export modified image
    uint8_t *out_data = NULL;
    size_t out_size = 0;
    ndfs_to_buffer(fs, &out_data, &out_size);
    // ... write out_data to file ...
    ndfs_free_data(out_data);

    ndfs_close(fs);
    return 0;
}
```

## Create a New Image

```c
#include <ndfs/ndfs.h>

ndfs_filesystem_t *fs = NULL;
ndfs_image_options_t opts;
ndfs_image_options_init(&opts);

opts.template_type = NDFS_TMPL_FLOPPY_360KB;
snprintf(opts.directory_name, sizeof(opts.directory_name), "MY-DISK");

ndfs_error_t err = ndfs_create_image(&fs, &opts);
if (err == NDFS_OK) {
    ndfs_write_file(fs, "SYSTEM/TEST:DATA", data, data_len);
    // ...
    ndfs_close(fs);
}
```

## API Reference

### Lifecycle

| Function | Description |
|----------|-------------|
| `ndfs_open_buffer(data, size, ro, &fs)` | Open from caller-owned buffer (no copy) |
| `ndfs_open_buffer_copy(data, size, ro, &fs)` | Open by copying the buffer |
| `ndfs_open_file(path, ro, &fs)` | Open straight from a host file (streams; host builds only) |
| `ndfs_open_block(&io, total_pages, ro, &fs)` | Open over a custom block backend (see below) |
| `ndfs_create_image(&fs, &opts)` | Create new image from template |
| `ndfs_close(fs)` | Close and free all resources |
| `ndfs_to_buffer(fs, &data, &size)` | Export image as malloc'd buffer |

### Storage backends (the block-IO seam)

The library never touches a flat image buffer directly — every page is fetched
and stored through one small vtable (`ndfs_block_io`), so the *same* filesystem
logic runs over three backends:

| Backend | Opener | Use |
|---------|--------|-----|
| In-RAM buffer | `ndfs_open_buffer` / `ndfs_open_buffer_copy` | Fast host use; whole image in RAM |
| Host file | `ndfs_open_file` | Streams a big image page-by-page from disk (host builds) |
| Custom / embedded | `ndfs_open_block` | Consumer supplies `read_block`/`write_block` (e.g. an SD card) |

Internally a small **pinned page cache** (16 KiB, 8 pages) keeps only a handful
of pages resident, so an image far larger than RAM can be mounted. Writes are
write-back and flushed on `ndfs_close`. The host-file backend is compiled only
when `NDFS_WITH_STDIO_BACKEND` is defined (default **ON** for host builds, OFF
for freestanding targets). See **[docs/BLOCK-IO-SEAM.md](../docs/BLOCK-IO-SEAM.md)**
for the backend contract and a worked example.

### Read Operations

| Function | Description |
|----------|-------------|
| `ndfs_get_master_block(fs, &mb)` | Get parsed master block |
| `ndfs_get_directory_name(fs, buf, len)` | Get volume name |
| `ndfs_list_directory(fs, path, &entries, &count)` | List directory contents |
| `ndfs_read_file(fs, path, &data, &size)` | Read file contents |
| `ndfs_file_exists(fs, path, &exists)` | Check if file exists |
| `ndfs_get_metadata(fs, path, &entry)` | Get file metadata |

### Write Operations

| Function | Description |
|----------|-------------|
| `ndfs_write_file(fs, path, data, size)` | Create or overwrite a file |
| `ndfs_delete_file(fs, path)` | Delete a file |
| `ndfs_rename(fs, old_path, new_path)` | Rename a file |

### User Management

| Function | Description |
|----------|-------------|
| `ndfs_get_users(fs, &users, &count)` | Get all users |
| `ndfs_get_user(fs, index, &user)` | Get user by index |
| `ndfs_add_user(fs, name, pages)` | Add a user with quota |
| `ndfs_remove_user(fs, index)` | Remove user (no files) |
| `ndfs_update_user_quota(fs, index, pages)` | Update quota |
| `ndfs_clear_user_password(fs, name)` | Clear password by name |
| `ndfs_clear_user_password_by_index(fs, idx)` | Clear password by index |

### Bitmap & Boot

| Function | Description |
|----------|-------------|
| `ndfs_is_block_used(fs, id)` | Check block allocation |
| `ndfs_get_free_pages(fs, &count)` | Free page count |
| `ndfs_get_used_pages(fs, &count)` | Used page count |
| `ndfs_detect_boot_format(fs, &fmt)` | Detect boot format |
| `ndfs_load_boot_code(fs, &code)` | Extract boot code |
| `ndfs_is_bootable(fs, &bootable)` | Has valid boot code |

### Low-Level Access

| Function | Description |
|----------|-------------|
| `ndfs_get_object_entries(fs, &entries, &count)` | Get all file entries |
| `ndfs_get_object_entry(fs, name, user, &entry)` | Find specific file entry |

### Diagnostics

| Function | Description |
|----------|-------------|
| `ndfs_verify_integrity(fs, &ok)` | Basic integrity check |
| `ndfs_fsck(fs, &report, &errors)` | Full 5-phase filesystem check (read only) |
| `ndfs_fsck_ex(fs, &options, &result)` | The same with the verdict (CLEAN, DAMAGED, FAILED), the failed files, and on request the safe repairs R1, R3, R4, R5 with undo data |
| `ndfs_fsck_undo(fs, data, size, &pages)` | Put back the pages a repair saved |
| `ndfs_generate_report(fs, &report)` | Text report |
| `ndfs_strerror(err)` | Error code to string |

### XAT Metadata (Extended Attributes)

| Function | Description |
|----------|-------------|
| `ndfs_get_file_properties(fs, path, &xat)` | Get all NDFS metadata for a file |
| `ndfs_xat_from_object(&entry, &xat)` | Extract XAT properties from an object entry |
| `ndfs_xat_to_object(&xat, &entry)` | Apply XAT properties to an object entry |
| `ndfs_xat_serialize(&xat, &json)` | Serialize to JSON string (caller frees) |
| `ndfs_xat_deserialize(json, &xat)` | Parse JSON string to properties |
| `ndfs_xat_filename(datafile, buf, len)` | Get .xat filename for a data file |
| `ndfs_xat_is_xat_file(filename)` | Check if filename ends with .xat |

See [XAT Sidecar Files](../docs/NDFS-FORMAT.md#xat-sidecar-files) in the format spec for the full explanation and file format.

### Memory Management

All functions that allocate output must be freed by the caller:

| Allocator | Free Function |
|-----------|---------------|
| `ndfs_list_directory` | `ndfs_free_entries(entries)` |
| `ndfs_read_file` | `ndfs_free_data(data)` |
| `ndfs_to_buffer` | `ndfs_free_data(data)` |
| `ndfs_get_users` | `ndfs_free_users(users)` |
| `ndfs_get_object_entries` | `ndfs_free_object_entries(entries)` |
| `ndfs_generate_report` | `ndfs_free_string(report)` |
| `ndfs_fsck` | `ndfs_free_string(report)` |
| `ndfs_fsck_ex` | `ndfs_fsck_result_free(&result)` |
| `ndfs_load_boot_code` | `ndfs_boot_code_destroy(&code)` |

All free functions are no-ops when passed NULL.

### Error Codes

| Code | Value | Description |
|------|-------|-------------|
| `NDFS_OK` | 0 | Success |
| `NDFS_ERR_NULL_PTR` | -1 | NULL pointer argument |
| `NDFS_ERR_INVALID_ARG` | -2 | Invalid argument |
| `NDFS_ERR_TOO_SMALL` | -3 | Image too small |
| `NDFS_ERR_NOT_ALIGNED` | -4 | Image size not page-aligned |
| `NDFS_ERR_INVALID_IMAGE` | -5 | Not a valid NDFS image |
| `NDFS_ERR_NOT_FOUND` | -7 | File or user not found |
| `NDFS_ERR_ALREADY_EXISTS` | -8 | User already exists |
| `NDFS_ERR_NO_SPACE` | -9 | Disk full |
| `NDFS_ERR_READ_ONLY` | -10 | Write attempted on read-only image |
| `NDFS_ERR_NO_SLOTS` | -11 | No free user slots |
| `NDFS_ERR_HAS_FILES` | -12 | User has files (cannot delete) |
| `NDFS_ERR_ALLOC` | -13 | Memory allocation failed |
| `NDFS_ERR_CORRUPT` | -14 | Filesystem corruption detected |

## Image Templates

| Template | Constant | Pages | Size |
|----------|----------|-------|------|
| Floppy 360KB | `NDFS_TMPL_FLOPPY_360KB` | 154 | 315 KB |
| Floppy 1.2MB | `NDFS_TMPL_FLOPPY_12MB` | 616 | 1.2 MB |
| SMD 75MB | `NDFS_TMPL_SMD_75MB` | 36,945 | 75 MB |
| Winchester 74MB | `NDFS_TMPL_WINCHESTER_74MB` | 36,396 declared, 36,864 in the file | 72 MB |
| SCSI 126MB | `NDFS_TMPL_SCSI_126MB` | 61,036 declared, 64,656 in the file | 126 MB |
| Custom | `NDFS_TMPL_CUSTOM` | User-defined | Varies |

SINTRAN disk type names (`DISC-14MB` ... `DISC-450MB-F`, 40 names with controller family, drive model and a "created with real SINTRAN" flag) are available through
`ndfs_disk_type_count()`, `ndfs_disk_type_get()`, `ndfs_disk_type_by_name()` and `ndfs_disk_type_find()`; pass the size to `NDFS_TMPL_CUSTOM`.
The sizes come from SINTRAN's device-name table (see `docs/DISK-GEOMETRY-AND-BOOT-FINDINGS.md`).

### What a created image contains (measured with the real SINTRAN M)

`ndfs_create_image` follows what SINTRAN's `@CREATE-DIRECTORY`, the first `@CREATE-USER` and the first `@CREATE-FILE` write (see `docs/CREATE-DIRECTORY-ORACLE.md`):
the label and the Extended Info Block (hard disks only; floppies have none), the bit file at the address SINTRAN would choose (`bit_file_answer`: 0 = default,
-1 = end of disc, n = a page address), the pages above the declared capacity marked used up to the next multiple of 256, the user file and object file pages found
by SINTRAN's free-page search from the bit file, and the Unreserved Pages count by the measured formula. Page 0 bytes 0 to 1999 get a real boot area
(`boot_choice`, or `boot_data` for your own); the SCSI template also gets the real last page SINTRAN needs.
Not verified: nobody has booted an image made this way, and the boot areas come from different SINTRAN generations.
## Test Coverage

137 tests covering:

- On-disk format (endian, block pointers, master block, entries)
- Image creation (all 5 templates, pointer placement, bitmap)
- Boot loader detection (BPUN, FLOMON, Binary, None)
- SINTRAN initial-command buffer: read / write / repair (`ndfs_read_initial_commands`, `ndfs_write_initial_commands`, `ndfs_repair_initial_commands`; see [../../docs/SINTRAN-INITIAL-COMMANDS-SPEC.md](../../docs/SINTRAN-INITIAL-COMMANDS-SPEC.md))
- Read/write operations (small, multi-page, overwrite, delete, rename)
- Write persistence (export/reimport round-trips)
- Sparse files, user management, bitmap queries
- XAT metadata (serialization, deserialization, all 12 keys, edge cases)
- XAT copy-across (access bits, file type, user, dates, sparse data preservation)
- Stress testing (20 users x 5 files, write/delete cycles)
- Filesystem check (fsck with block reference analysis)

## ndtool CLI

A full-featured CLI tool is included at [tools/ndtool/](tools/ndtool/). Features:

- List files and users (`-t`, `-u`)
- Extract files with parity handling and name conversion (`-x`)
- Copy files in (`--put`) with ND-100 even parity
- Delete, rename, user management, quota management
- Create new images from templates (`--create`)
- Full filesystem check (`--fsck`)
- Interactive shell (`--shell`) with edit, cat, hexdump, bitmap, fsck, stat
- Bitmap visualization with Unicode block characters

See the [ndtool README](tools/ndtool/README.md) for full documentation.

## On-Disk Format

See [NDFS-FORMAT.md](../docs/NDFS-FORMAT.md) for the complete binary format specification.

## Related

- [norskdata-ndfs (TypeScript)](../ndfs-ts/) -- Same API in TypeScript
- [norskdata-ndfs (Python)](../ndfs-py/) -- Same API in Python
- [ndtool](tools/ndtool/) -- CLI tool built on this library

## License

MIT - Copyright (c) 1985-2026 Ronny Hansen, RetroCore Labs
