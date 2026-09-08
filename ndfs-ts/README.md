# norskdata-ndfs (TypeScript)

Read and write NDFS (Norsk Data File System) disk images from Sintran-III minicomputers.

232 tests. Zero runtime dependencies.

## Install

```bash
npm install norskdata-ndfs
```

## Quick Start

```typescript
import { NdfsFileSystem, ImageTemplate } from 'norskdata-ndfs';
import * as fs from 'fs';

// Open an existing image
const imageData = new Uint8Array(fs.readFileSync('disk.ndfs'));
const ndfs = new NdfsFileSystem(imageData);

// List users (root directory)
for (const entry of ndfs.listDirectory()) {
  console.log(entry.name);  // SYSTEM, USER1, ...
}

// List files for a user
for (const file of ndfs.listDirectory('SYSTEM')) {
  console.log(`${file.fullName}  ${file.size} bytes`);
}

// Read a file
const data = ndfs.readFile('SYSTEM/README:TEXT');
console.log(new TextDecoder().decode(data));

// Write a file
ndfs.writeFile('SYSTEM/HELLO:DATA', new TextEncoder().encode('Hello NDFS!'));

// Save the modified image
fs.writeFileSync('disk-modified.ndfs', ndfs.toBuffer());
```

## Create a New Image

```typescript
import { NdfsFileSystem, ImageTemplate } from 'norskdata-ndfs';

// Create a 360KB floppy image
const ndfs = NdfsFileSystem.createImage({
  template: ImageTemplate.Floppy360KB,
  directoryName: 'MY-DISK',
});

// Create a 75MB SMD hard disk with extended info
const hd = NdfsFileSystem.createImage({
  template: ImageTemplate.Smd75MB,
  directoryName: 'HARDDISK',
  includeExtendedInfo: true,
  systemNumber: 100,
});

// Create a custom-size image
const custom = NdfsFileSystem.createImage({
  template: ImageTemplate.Custom,
  directoryName: 'CUSTOM',
  customPages: 500,
});
```

## API Reference

### Constructor

```typescript
new NdfsFileSystem(data: Uint8Array | ArrayBuffer, readOnly?: boolean)
```

Opens an existing NDFS disk image. The buffer must be page-aligned (multiple of 2048 bytes).

### Static Methods

| Method | Description |
|--------|-------------|
| `createImage(options)` | Create a new disk image from template |

### Read Operations

| Method | Returns | Description |
|--------|---------|-------------|
| `getMasterBlock()` | `MasterBlock` | Parsed master block with pointers and extended info |
| `getDirectoryName()` | `string` | Volume/directory name |
| `listDirectory(path?)` | `FileEntry[]` | List users (root) or files (user path) |
| `readFile(path)` | `Uint8Array` | Read file contents |
| `getMetadata(path)` | `FileEntry \| null` | File metadata without reading content |
| `fileExists(path)` | `boolean` | Check if file exists |

### Write Operations

| Method | Description |
|--------|-------------|
| `writeFile(path, data)` | Create or overwrite a file (supports sparse allocation) |
| `deleteFile(path)` | Delete a file and free its blocks |
| `rename(oldPath, newPath)` | Rename a file |

### User Management

| Method | Returns | Description |
|--------|---------|-------------|
| `getUsers()` | `UserEntry[]` | All user entries |
| `getUser(index)` | `UserEntry \| null` | User by index |
| `addUser(name, reservedPages)` | `boolean` | Add a new user with quota |
| `removeUser(index)` | `boolean` | Remove user (must have no files) |
| `updateUserQuota(index, pages)` | `boolean` | Change user's page quota |
| `clearUserPassword(indexOrName)` | `boolean` | Set password to zero |

### Bitmap Queries

| Method | Returns | Description |
|--------|---------|-------------|
| `isBlockUsed(blockId)` | `boolean` | Check block allocation status |
| `getFreePages()` | `number` | Free page count |
| `getUsedPages()` | `number` | Used page count |

### Boot Loader

| Method | Returns | Description |
|--------|---------|-------------|
| `detectBootFormat()` | `BootFormat` | Detect boot code format (BPUN, FLOMON, Binary, None) |
| `loadBootCode()` | `BootCode \| null` | Extract boot code with checksum validation |
| `isBootable()` | `boolean` | Has valid boot code |

### XAT Metadata (Extended Attributes)

| Method | Returns | Description |
|--------|---------|-------------|
| `getFileProperties(path)` | `XatProperties \| null` | Get all NDFS metadata for a file |
| `readFileWithProperties(path)` | `{ data, properties }` | Read file data and metadata together |
| `writeFileWithProperties(path, data, props)` | `void` | Write file with metadata from XAT properties |

See [XAT Sidecar Files](#xat-sidecar-files) below for details.

### Low-Level Access

| Method | Returns | Description |
|--------|---------|-------------|
| `getObjectEntries()` | `ObjectEntry[]` | All file entries with full metadata |
| `getObjectEntry(name, userName)` | `ObjectEntry \| null` | Find specific file entry |

### Diagnostics

| Method | Returns | Description |
|--------|---------|-------------|
| `verifyIntegrity()` | `boolean` | Validate filesystem structures |
| `generateReport()` | `string` | Text report of filesystem state |

### Export

| Method | Returns | Description |
|--------|---------|-------------|
| `toBuffer()` | `Uint8Array` | Export image as new buffer |

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
| `ndfs.object_name` | string | File name (max 16 chars) |
| `ndfs.type` | string | File type (max 4 chars: DATA, PROG, SYMB, TEXT, etc.) |
| `ndfs.user_name` | string | Owner user name |
| `ndfs.user_index` | number | Owner user index (0-255) |
| `ndfs.access_bits` | number | 15-bit permission encoding (Own/Friend/Public) |
| `ndfs.file_type_flags` | number | File attribute flags (Indexed, Contiguous, Allocated, etc.) |
| `ndfs.file_type` | number | File type code (0=DATA, 1=PROG, 2=SYMB, 3=TEXT) |
| `ndfs.pages_in_file` | number | File size in 2048-byte pages |
| `ndfs.bytes_in_file` | number | File size in bytes |
| `ndfs.date_created` | number | Creation date (ND timestamp format) |
| `ndfs.last_read_date` | number | Last read date (ND timestamp format) |
| `ndfs.last_write_date` | number | Last write date (ND timestamp format) |

### Usage

```typescript
import { NdfsFileSystem } from 'norskdata-ndfs';
import { serializeXat, deserializeXat, getXatFileName } from 'norskdata-ndfs';

const ndfs = new NdfsFileSystem(imageData);

// Extract with metadata
const { data, properties } = ndfs.readFileWithProperties('SYSTEM/README:TEXT');
const xatJson = serializeXat(properties);
// Write data to README.TEXT and xatJson to README.TEXT.xat

// Import with metadata
const xat = deserializeXat(xatJson);
ndfs.writeFileWithProperties('SYSTEM/README:TEXT', data, xat);
// Access bits, file type, dates are all restored
```

### Utility Functions

| Function | Description |
|----------|-------------|
| `objectEntryToXat(entry)` | Extract XAT properties from an ObjectEntry |
| `xatToObjectEntry(xat, entry)` | Apply XAT properties to an ObjectEntry |
| `serializeXat(props)` | Convert properties to JSON string |
| `deserializeXat(json)` | Parse JSON string to properties |
| `getXatFileName(dataFile)` | Get .xat filename (e.g., `"README.TEXT"` -> `"README.TEXT.xat"`) |
| `isXatFile(fileName)` | Check if filename ends with `.xat` |

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
| `Custom` | User-defined | Varies | Specify with `customPages` |

## Test Coverage

260 tests across 21 test files covering:

- On-disk format parsing (endian, block pointers, master block, names, timestamps)
- Data structures (user/object entries, friends, access permissions)
- Image creation (all 5 templates, custom sizes, pointer placement)
- Boot loader detection (BPUN, FLOMON, Binary, None)
- SINTRAN initial-command buffer: read / write / repair (`readInitialCommands`, `writeInitialCommands`, `repairInitialCommands`; see [../docs/SINTRAN-INITIAL-COMMANDS-SPEC.md](../docs/SINTRAN-INITIAL-COMMANDS-SPEC.md))
- Read operations (contiguous, indexed, sub-indexed, sparse holes)
- Write operations (small, 1-page, multi-page, >512-page sub-indexed, overwrite)
- Write persistence (export/reimport round-trips)
- Sparse files (all-zero, mixed, block savings verification)
- User management (add, remove, quota, password, limits, duplicates)
- Access control (Own/Friend/Public tiers, friend override, permissions)
- XAT metadata (serialization, deserialization, round-trips, all 12 property keys)
- XAT copy-across (access bits, file type, user, dates, sparse data preservation)
- Stress testing (50 users, 200 files, write/delete cycles)
- Real fixture files (empty.ndfs, withfiles.ndfs)

```bash
npm test              # run all tests
npm run test:watch    # watch mode
npm run typecheck     # TypeScript type checking
```

## On-Disk Format

See [NDFS-FORMAT.md](../docs/NDFS-FORMAT.md) for the complete binary format specification.

## Related

- [norskdata-ndfs (Python)](../ndfs-py/) -- Same API in Python
- [libndfs (C)](../ndfs-c/) -- Same API in C99
- [ndtool](../ndfs-c/tools/ndtool/) -- CLI tool built on libndfs

## License

MIT - Copyright (c) 1985-2026 Ronny Hansen, RetroCore Labs
