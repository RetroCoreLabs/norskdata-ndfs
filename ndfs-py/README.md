# norskdata-ndfs (Python)

Read and write NDFS (Norsk Data File System) disk images from Sintran-III minicomputers.

271 tests. No external dependencies (stdlib only). Python >= 3.9.

## Install

```bash
pip install norskdata-ndfs
```

## Quick Start

```python
from ndfs import NdfsFileSystem, ImageTemplate, ImageCreationOptions

# Open an existing image
with open('disk.ndfs', 'rb') as f:
    ndfs = NdfsFileSystem(f.read())

# List users (root directory)
for entry in ndfs.list_directory():
    print(entry.name)  # SYSTEM, USER1, ...

# List files for a user
for file in ndfs.list_directory('SYSTEM'):
    print(f"{file.full_name}  {file.size} bytes")

# Read a file
data = ndfs.read_file('SYSTEM/README:TEXT')
print(data.decode('ascii'))

# Write a file
ndfs.write_file('SYSTEM/HELLO:DATA', b'Hello NDFS!')

# Save the modified image
with open('disk-modified.ndfs', 'wb') as f:
    f.write(ndfs.to_buffer())
```

## Create a New Image

```python
from ndfs import NdfsFileSystem, ImageTemplate, ImageCreationOptions

# Create a 360KB floppy image
ndfs = NdfsFileSystem.create_image(ImageCreationOptions(
    template=ImageTemplate.Floppy360KB,
    directory_name='MY-DISK',
))

# Create a 75MB SMD hard disk with extended info
hd = NdfsFileSystem.create_image(ImageCreationOptions(
    template=ImageTemplate.Smd75MB,
    directory_name='HARDDISK',
    include_extended_info=True,
    system_number=100,
))

# Create a custom-size image
custom = NdfsFileSystem.create_image(ImageCreationOptions(
    template=ImageTemplate.Custom,
    directory_name='CUSTOM',
    custom_pages=500,
))
```

## API Reference

### Constructor

```python
NdfsFileSystem(data: bytes | bytearray | memoryview, read_only: bool = False)
```

Opens an existing NDFS disk image. The buffer must be page-aligned (multiple of 2048 bytes).

### Class Methods

| Method | Description |
|--------|-------------|
| `create_image(options)` | Create a new disk image from template |

### Read Operations

| Method | Returns | Description |
|--------|---------|-------------|
| `get_master_block()` | `MasterBlock` | Parsed master block with pointers and extended info |
| `get_directory_name()` | `str` | Volume/directory name |
| `list_directory(path="")` | `list[FileEntry]` | List users (root) or files (user path) |
| `read_file(path)` | `bytes` | Read file contents |
| `get_metadata(path)` | `FileEntry \| None` | File metadata without reading content |
| `file_exists(path)` | `bool` | Check if file exists |

### Write Operations

| Method | Description |
|--------|-------------|
| `write_file(path, data)` | Create or overwrite a file (supports sparse allocation) |
| `delete_file(path)` | Delete a file and free its blocks |
| `rename(old_path, new_path)` | Rename a file |

### User Management

| Method | Returns | Description |
|--------|---------|-------------|
| `get_users()` | `list[UserEntry]` | All user entries |
| `get_user(index)` | `UserEntry \| None` | User by index |
| `add_user(name, reserved_pages)` | `bool` | Add a new user with quota |
| `remove_user(index)` | `bool` | Remove user (must have no files) |
| `update_user_quota(index, pages)` | `bool` | Change user's page quota |
| `clear_user_password(index_or_name)` | `bool` | Set password to zero (accepts int index or str name) |

### Bitmap Queries

| Method | Returns | Description |
|--------|---------|-------------|
| `is_block_used(block_id)` | `bool` | Check block allocation status |
| `get_free_pages()` | `int` | Free page count |
| `get_used_pages()` | `int` | Used page count |

### Boot Loader

| Method | Returns | Description |
|--------|---------|-------------|
| `detect_boot_format()` | `BootFormat` | Detect boot code format (BPUN, FLOMON, Binary, None) |
| `load_boot_code()` | `BootCode \| None` | Extract boot code with checksum validation |
| `is_bootable()` | `bool` | Has valid boot code |

### XAT Metadata (Extended Attributes)

| Method | Returns | Description |
|--------|---------|-------------|
| `get_file_properties(path)` | `dict \| None` | Get all NDFS metadata for a file |
| `read_file_with_properties(path)` | `tuple[bytes, dict]` | Read file data and metadata together |
| `write_file_with_properties(path, data, props)` | `None` | Write file with metadata from XAT properties |

See [XAT Sidecar Files](#xat-sidecar-files) below for details.

### Low-Level Access

| Method | Returns | Description |
|--------|---------|-------------|
| `get_object_entries()` | `list[ObjectEntry]` | All file entries with full metadata |
| `get_object_entry(name, user_name)` | `ObjectEntry \| None` | Find specific file entry |

### Diagnostics

| Method | Returns | Description |
|--------|---------|-------------|
| `verify_integrity()` | `bool` | Validate filesystem structures |
| `generate_report()` | `str` | Text report of filesystem state |

### Export

| Method | Returns | Description |
|--------|---------|-------------|
| `to_buffer()` | `bytearray` | Export image as new buffer |

## XAT Sidecar Files

NDFS files carry rich metadata that has no equivalent on modern filesystems: 3-tier access permissions (Own/Friend/Public), file type flags (Indexed, Contiguous, Allocated, etc.), ND-100 timestamps, user ownership, and file type codes (DATA, PROG, SYMB, TEXT). When a file is copied out of an NDFS disk image, all of this metadata is lost. If the file is later copied back, it arrives as a plain DATA file with default permissions -- the original identity is gone.

This is a real problem for archival work (extracting disk images for analysis and reconstitution) and development workflows (editing source on a modern system, then transferring back to an ND-100 emulator).

XAT (Extended Attribute) sidecar files solve this by storing the metadata in a companion JSON file alongside each extracted data file. When the file is copied back, the `.xat` is read and all metadata is restored -- permissions, file type, ownership, dates, everything.

### How It Works

Extracting `SYSTEM/README:TEXT` produces two files:
- `README.TEXT` -- the file data
- `README.TEXT.xat` -- JSON metadata sidecar

When copying the file back into an NDFS image, the `.xat` file is read and the metadata is restored.

### XAT File Format

```json
{
  "ndfs.object_name": "README",
  "ndfs.type": "TEXT",
  "ndfs.user_name": "SYSTEM",
  "ndfs.user_index": 0,
  "ndfs.access_bits": 1279,
  "ndfs.file_type_flags": 8,
  "ndfs.file_type": 3,
  "ndfs.pages_in_file": 1,
  "ndfs.bytes_in_file": 1024,
  "ndfs.date_created": 0,
  "ndfs.last_read_date": 0,
  "ndfs.last_write_date": 0
}
```

### Properties Preserved

| Key | Type | Description |
|-----|------|-------------|
| `ndfs.object_name` | str | File name (max 16 chars) |
| `ndfs.type` | str | File type (max 4 chars: DATA, PROG, SYMB, TEXT, etc.) |
| `ndfs.user_name` | str | Owner user name |
| `ndfs.user_index` | int | Owner user index (0-255) |
| `ndfs.access_bits` | int | 15-bit permission encoding (Own/Friend/Public) |
| `ndfs.file_type_flags` | int | File attribute flags (Indexed, Contiguous, Allocated, etc.) |
| `ndfs.file_type` | int | File type code (0=DATA, 1=PROG, 2=SYMB, 3=TEXT) |
| `ndfs.pages_in_file` | int | File size in 2048-byte pages |
| `ndfs.bytes_in_file` | int | File size in bytes |
| `ndfs.date_created` | int | Creation date (ND timestamp format) |
| `ndfs.last_read_date` | int | Last read date (ND timestamp format) |
| `ndfs.last_write_date` | int | Last write date (ND timestamp format) |

### Usage

```python
from ndfs import NdfsFileSystem
from ndfs.xat import serialize_xat, deserialize_xat, get_xat_filename

ndfs = NdfsFileSystem(image_data)

# Extract with metadata
data, properties = ndfs.read_file_with_properties('SYSTEM/README:TEXT')
xat_json = serialize_xat(properties)
# Write data to README.TEXT and xat_json to README.TEXT.xat

# Import with metadata
xat = deserialize_xat(xat_json)
ndfs.write_file_with_properties('SYSTEM/README:TEXT', data, xat)
# Access bits, file type, dates are all restored
```

### Utility Functions

| Function | Description |
|----------|-------------|
| `object_entry_to_xat(entry)` | Extract XAT properties from an ObjectEntry |
| `xat_to_object_entry(xat, entry)` | Apply XAT properties to an ObjectEntry |
| `serialize_xat(props)` | Convert properties to JSON string |
| `deserialize_xat(json_str)` | Parse JSON string to properties |
| `get_xat_filename(data_file)` | Get .xat filename (e.g., `"README.TEXT"` -> `"README.TEXT.xat"`) |
| `is_xat_file(filename)` | Check if filename ends with `.xat` |

## Path Format

```
/                        Root (lists users as directories)
USERNAME                 User's file listing
USERNAME/FILENAME:TYPE   File reference (colon separates name and type)
USERNAME/FILENAME.TYPE   Also accepted (dot converted to colon)
```

## Image Templates

| Template | Pages | Size | Description |
|----------|-------|------|-------------|
| `Floppy360KB` | 154 | 315 KB | ND floppy 360 KB |
| `Floppy12MB` | 616 | 1.2 MB | ND floppy 1.2 MB |
| `Smd75MB` | 36,945 | 75 MB | SMD hard disk |
| `Winchester74MB` | 36,396 | 74 MB | Winchester hard disk |
| `Custom` | User-defined | Varies | Specify with `custom_pages` |

## Test Coverage

299 tests across 21 test files covering:

- On-disk format parsing (endian, block pointers, master block, names, timestamps)
- Data structures (user/object entries, friends, access permissions)
- Image creation (all 5 templates, custom sizes, pointer placement)
- Boot loader detection (BPUN, FLOMON, Binary, None)
- SINTRAN initial-command buffer: read / write / repair (`read_initial_commands`, `write_initial_commands`, `repair_initial_commands`; see [../docs/SINTRAN-INITIAL-COMMANDS-SPEC.md](../docs/SINTRAN-INITIAL-COMMANDS-SPEC.md))
- SINTRAN CPU number and type on a system pack (`ndfs.sintran_identity.read_cpu_identity` / `write_cpu_identity`: both DPIT copies, the 33CPN/33CPU symbols, the directory stamp; addresses from the pack's own symbol lists, segment numbers from the RT-loader name records) and SINTRAN segments (`ndfs.sintran_segments.list_segments` / `export_segment` / `import_segment`, disk pages from the kernel's DBLST/ABLPAGE/MASSNO tables). Synthetic K- and L-shaped packs, same cases as the C tests.
- Read operations (contiguous, indexed, sub-indexed, sparse holes)
- Write operations (small, 1-page, multi-page, overwrite, disk full, auto quota)
- XAT metadata (serialization, deserialization, round-trips, all 12 property keys)
- XAT copy-across (access bits, file type, user, dates, sparse data preservation)
- Write persistence (export/reimport round-trips)
- Sparse files (all-zero, mixed, block savings verification)
- User management (add, remove, quota, password, limits, duplicates)
- Access control (Own/Friend/Public tiers, friend override, permissions)
- Stress testing (50 users, 200 files, write/delete cycles)
- Real fixture files (empty.ndfs, withfiles.ndfs)

```bash
PYTHONPATH=src python -m pytest tests/ -v    # run all tests
PYTHONPATH=src python -m pytest tests/ -q    # quiet mode
```

## On-Disk Format

See [NDFS-FORMAT.md](../docs/NDFS-FORMAT.md) for the complete binary format specification.

## Related

- [norskdata-ndfs (TypeScript)](../ndfs-ts/) -- Same API in TypeScript
- [libndfs (C)](../ndfs-c/) -- Same API in C99
- [ndtool](../ndfs-c/tools/ndtool/) -- CLI tool built on libndfs

## License

MIT - Copyright (c) 1985-2026 Ronny Hansen, RetroCore Labs
