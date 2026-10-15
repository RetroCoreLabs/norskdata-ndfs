# ndtool

Command-line tool for reading, writing, and managing NDFS (Norsk Data File System) disk images from Sintran-III minicomputers.

Built on [libndfs](../../README.md), the C99 NDFS library.

## Build

```bash
cd ndfs-c   # from the repository root
mkdir -p build && cd build
cmake ..
make ndtool
```

The binary is at `build/ndtool`.

## Quick Reference

```
ndtool [options] [args] <image>

LISTING:
  ndtool -t <image>                          List all files
  ndtool -t -u SYSTEM <image>                List one user's files
  ndtool -u <image>                          List users with quotas
  ndtool -i <image>                          Filesystem info
  ndtool -i -v <image>                       Verbose info with bitmap map

EXTRACT:
  ndtool -x <image>                          Extract all files
  ndtool -x -d <image>                       Extract into USER/ subdirectories
  ndtool -x -p -l <image>                    Extract with parity stripped, lowercase
  ndtool -x -o /tmp/out -d -l <image>        Extract to directory
  ndtool -x -p -F 'SYSTEM/*:MODE' -o out/ <image>   Extract matching files (glob)
  ndtool -x -F '*/*:SYMB' -o out/ <image>    All users' :SYMB files

COPY IN:
  ndtool --put readme.txt SYSTEM/README:TEXT <image>
  ndtool -p --put source.c SYSTEM/SOURCE:C <image>    (with parity)
  ndtool --put '*.NPL' --dest TEST -p <image>         Import matching files (glob)

WILDCARDS (-F pattern / --put glob):
  * matches any run of chars, ? matches one. Quote patterns so the shell
  leaves them for ndtool. -F splits on '/' into USER-glob/FILE-glob.

OVERWRITE POLICY (import and extract):
  (default)        Skip targets that already exist (safe)
  --overwrite, -f  Replace existing targets
  --interactive    Prompt before each overwrite (skips when no TTY)
  -n               Dry run: preview create / overwrite / skip per file

FILE DETAILS:
  ndtool --stat SYSTEM/LOAD:MODE <image>     Full metadata (RetroCommander-style)
  ndtool --stat -v SYSTEM/LOAD:MODE <image>  ...also lists the file's data blocks
  (shows access tiers, dates, versioning, device, open counts, file pointer)

PERMISSIONS:
  ndtool --chmod 'OWN+WD,PUBLIC-A' SYSTEM/LOAD:MODE <image>
  ndtool --chmod 'OWN=RWACD,FRIEND=RW' SYSTEM/X:MODE <image>
  ndtool --chmod 0x03FF SYSTEM/X:MODE <image>   (raw 15-bit access word)
  ndtool --chmod 'PUBLIC+R' SYSTEM/X:MODE -n <image>   (preview before -> after)
    TIER  = OWN | FRIEND | PUBLIC | ALL (or O | F | P)
    OP    = '=' set   '+' add   '-' remove
    RIGHT = R read  W write  A append  C common  D directory
  New files default to OWN+FRIEND all rights, PUBLIC none (0x03FF).

DELETE:
  ndtool --rm SYSTEM/OLD-FILE:DATA <image>

USERS:
  ndtool --useradd NEWUSER 500 <image>       Add user with 500-page quota
  ndtool --userdel NEWUSER <image>           Remove user (must have no files)
  ndtool --quotaadd SYSTEM 200 <image>       Add 200 pages to SYSTEM's quota
  ndtool --quotadel SYSTEM 100 <image>       Remove 100 pages from quota
  ndtool --passwd SYSTEM <image>             Clear user password

OBJECT BLOCKS (how many FILES a user may hold - not how much space):
  ndtool --objblocks BIGMAN <image>          Show blocks, files held, room to grant
  ndtool --giveobjblocks BIGMAN 3 <image>    Give 3 more blocks (256 -> 1024 files)
  ndtool --takeobjblocks BIGMAN 1 <image>    Lower the maximum by 1

FRIENDS (a user grants another user rights to its files):
  ndtool --friends ALICE <image>             List ALICE's friends and rights
  ndtool --friendadd ALICE:BOB:RWA <image>   Add BOB as ALICE's friend (rights RWA)
  ndtool --frienddel ALICE:BOB <image>       Remove BOB from ALICE's friends
  ndtool --stat ALICE <image>                Show ALICE's details + friend list
    OWNER/FRIEND may be a user name or index (0-255). Rights letters:
    R read  W write  A append  C common  D directory  (default RWA)

DIAGNOSTICS:
  ndtool --fsck <image>                      Full filesystem check

CREATE:
  ndtool --create floppy360 --name MYDISK new.ndfs
  ndtool --create smd75 --name HARDDISK new.ndfs
  ndtool --create custom --pages 500 --name TEST new.ndfs

INTERACTIVE:
  ndtool --shell <image>                     Interactive shell
  ndtool --editor vim --shell <image>        Shell with custom editor
```

## Listing Files

List all files on a disk image. Each file shows its octal object ID, creation date, owning user, name:type, size, and page count:

```
$ ndtool -t BIGDISK0-L.IMG
USER: SYSTEM
  [0000]  1988-12-16 09:39:59  (SYSTEM)SINTRAN:DATA;1                        0 bytes     63 pages
  [0001]  1988-12-16 09:40:00  (SYSTEM)MACM-AREA:DATA;1                      0 bytes     64 pages
  [0002]  1988-12-16 09:40:45  (SYSTEM)SEGFIL0:DATA;1                 17618944 bytes  10000 pages
  [0003]  1988-12-16 09:42:28  (SYSTEM)MAILBOX:DATA;1                     6144 bytes      3 pages
  [0004]  1988-12-16 09:42:47  (SYSTEM)RTFIL:DATA;1                       3122 bytes      2 pages
  ...
USER: RONNY
  [1400]  1995-10-30 05:30:08  (RONNY)CAT:C;1                               814 bytes      1 pages
  [1401]  1995-10-30 05:30:08  (RONNY)CAT:PROG;1                         141312 bytes     11 pages
  ...
```

Filter by user:

```
$ ndtool -t -u RONNY BIGDISK0-L.IMG
USER: RONNY
  [1400]  1995-10-30 05:30:08  (RONNY)CAT:C;1                               814 bytes      1 pages
  [1401]  1995-10-30 05:30:08  (RONNY)CAT:PROG;1                         141312 bytes     11 pages
```

## Listing Users

Users are shown with their octal index, file count, and quota info:

```
$ ndtool -u BIGDISK0-L.IMG
Users: 15
  [000]  SYSTEM             85 files  Reserved: 15000  Used: 11774  Free:  3226
  [001]  FLOPPY-USER         0 files  Reserved:     0  Used:     0  Free:     0
  [002]  UTILITY            17 files  Reserved:  1000  Used:   368  Free:   632
  [003]  BPUN-FILES         30 files  Reserved:  1000  Used:   687  Free:   313
  [004]  SCRATCH            25 files  Reserved:  2500  Used:   227  Free:  2273
  [006]  RONNY              13 files  Reserved:  1000  Used:    87  Free:   913
  [010]  GUEST               0 files  Reserved:   500  Used:     0  Free:   500
  [012]  C-INCLUDE           7 files  Reserved:   300  Used:    15  Free:   285
  ...
```

## Filesystem Info

Basic info:

```
$ ndtool -i BIGDISK0-L.IMG
Volume:        PACK-ONE
Total pages:   38400
Used pages:    14861
Free pages:    23539
Users:         15
Files:         293
Boot format:   None
```

Verbose mode adds block pointers, extended info, bitmap visualization, and integrity check:

```
$ ndtool -i -v BIGDISK0-L.IMG
Volume:        PACK-ONE
Total pages:   38400
Used pages:    14861
Free pages:    23539
Users:         15
Files:         293
Boot format:   None

Object file:   Block 18684 (Indexed)
User file:     Block 18686 (Indexed)
Bit file:      Block 18468 (Contiguous)
Extended info: Valid (System 341, Checksum Valid)
  Pages avail: 36945
  Flag word:   0x8000

Bitmap (38400 pages, 14861 used, 23539 free):
  Legend: █=full  ▓=75%+  ▒=25-75%  ░=<25%  ·=free

  Block 0       4800    9600    14400   19200   24000   28800   33600
      0 ██████████████████▒········░█████░··························▒▒··

Bitmap Validation:
  ✓ Bitmap is consistent with file allocation
  Bitmap used:       14861 pages
  Bitmap free:       23539 pages
  File data pages:   14618 pages (across 293 files)
  Overhead:          243 pages (system + index blocks)
```

The bitmap map uses Unicode block characters to show disk usage at a glance:
- `█` Full block -- all pages in group used
- `▓` Dark shade -- 75%+ used
- `▒` Medium shade -- 25-75% used
- `░` Light shade -- <25% used
- `·` Middle dot -- all free

## Extracting Files

Extract all files to the current directory:

```
$ ndtool -x disk.ndfs
```

Extract into per-user subdirectories with lowercase names:

```
$ ndtool -x -d -l -o /tmp/extracted disk.ndfs
```

This creates:
```
/tmp/extracted/
  system/
    readme.text
    program.prog
  ronny/
    hello.mode
```

Extract a single file:

```
$ ndtool -x -F SYSTEM/README:TEXT disk.ndfs
```

### Parity Handling on Extract

ND-100 text files (:MODE, :SYMB, :TEXT, :C, etc.) use even parity -- bit 7 is set on each byte so the total number of 1-bits is even. Use `-p` to strip parity when extracting:

```
$ ndtool -x -p -l disk.ndfs
```

Without `-p`, text files will have high-bit characters. With `-p`, they become clean 7-bit ASCII.

### XAT Metadata Preservation

NDFS files carry metadata that has no equivalent on modern filesystems: 3-tier access permissions, file type flags, ND-100 timestamps, user ownership, and file type codes. When you extract a file to Windows or Linux, all of this is lost. If you copy the file back later, it arrives as a plain DATA file with default permissions.

The `--xat` flag solves this. On extract, it creates a companion `.xat` JSON file alongside each data file containing all the NDFS metadata. On copy-in, it reads the `.xat` file and restores the metadata.

```
$ ndtool -x --xat disk.ndfs
  Extracted SYSTEM/README:TEXT -> README.TEXT (1024 bytes)
  Wrote XAT README.TEXT.xat
```

The `.xat` file contains:
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

When copying back in with `--xat`, the metadata is restored:

```
$ ndtool --xat --put README.TEXT SYSTEM/README:TEXT disk.ndfs
```

This is essential for archival round-trips (extract, analyze, reconstitute) and development workflows (edit source on a modern system, transfer back to an ND-100 emulator) where preserving the file's original identity matters.

See the [NDFS Format Specification](../../../docs/NDFS-FORMAT.md#xat-sidecar-files) for the full XAT file format documentation.

## Copying Files In

Copy a local file into the image:

```
$ ndtool --put readme.txt SYSTEM/README:TEXT disk.ndfs
```

The NDFS path specifies `USER/FILENAME:TYPE`. If you omit the NDFS path, the local filename is converted automatically (`.` becomes `:`, name is uppercased):

```
$ ndtool --put hello.txt disk.ndfs
# Creates SYSTEM/HELLO:TXT (using first user)
```

### Parity on Copy-In

For text files that will be read on an ND-100, use `-p` to set proper even parity:

```
$ ndtool -p --put startup.mode SYSTEM/STARTUP:MODE disk.ndfs
```

This calculates even parity per byte: bit 7 is set when needed so the total number of 1-bits in each byte is even. This matches the ND-100 convention for text files.

**Parity applies to**: :MODE, :SYMB, :TEXT, :C, :BATC, :OUT, :LOG, :LIST, :FADM, :BASM, :FORT, :NPL, :COBO, :PASC, :PLAN, :BAS, :MAC, :EDIT

**Parity does NOT apply to**: :PROG, :BPUN, :DATA, :VTM (binary files)

## Deleting Files

```
$ ndtool --rm SYSTEM/OLD-FILE:DATA disk.ndfs
Delete SYSTEM/OLD-FILE:DATA? [y/N] y
Deleted SYSTEM/OLD-FILE:DATA
```

Use `-f` to skip the confirmation:

```
$ ndtool -f --rm SYSTEM/OLD-FILE:DATA disk.ndfs
```

## User Management

### Add a User

```
$ ndtool --useradd NEWUSER 500 disk.ndfs
Added user 'NEWUSER' with quota 500 pages
```

The quota argument is optional (defaults to 100 pages).

### Remove a User

```
$ ndtool --userdel NEWUSER disk.ndfs
Removed user 'NEWUSER' (index 3)
```

The user must have no files. Delete their files first.

### Quota Management

Add pages to a user's quota (checks disk space first):

```
$ ndtool --quotaadd SYSTEM 200 disk.ndfs
User 'SYSTEM' quota: 500 -> 700 pages (+200)
```

Remove pages from quota (checks that user isn't using more than the new limit):

```
$ ndtool --quotadel SYSTEM 100 disk.ndfs
User 'SYSTEM' quota: 700 -> 600 pages (-100)
```

### Object Blocks — raising how many FILES a user may hold

A SINTRAN user area holds **256 files per object block**: one block when the user is created, up to
**16 blocks (4096 files)** once granted. This is a separate limit from disk quota — object blocks
cap the *number* of files, `--quotaadd` caps their *total size*. A user can run out of either one
independently, and running out of object blocks is the one that looks like a mysterious failure,
because the file being written is not large.

`--giveobjblocks` is the `@GIVE-OBJECT-BLOCKS` equivalent: it **adds** blocks to the maximum rather
than setting a total.

```
$ ndtool --objblocks BIGMAN disk.ndfs
User               : BIGMAN (index 1)
Object blocks      : 1 allocated of 1 max
Files in use       : 0 of 256 maximum
Without new block  : 256 files
Can still be given : 15 more block(s)

$ ndtool --giveobjblocks BIGMAN 3 disk.ndfs
User 'BIGMAN' object blocks: 1 -> 4  (max files 256 -> 1024)
```

Granting moves only the **maximum**. Blocks are allocated on demand as files are created, so
"allocated" stays at 1 until the 257th file needs a second block. No disk pages are reserved.

When a user fills up, the report names the remedy — or says plainly that there is none:

```
$ ndtool --objblocks BIGMAN disk.ndfs
Files in use       : 4096 of 4096 maximum

*** FULL. This is the SINTRAN maximum of 4096 files; no more object blocks can be granted.
    Delete files, or use another user area.
```

`--takeobjblocks` lowers the maximum again. **It has no SINTRAN equivalent** — no command to remove
object blocks is documented, which follows from the structure, since a file's number is derived
from the block it sits in. It exists to undo a grant on an image you are building, and refuses to
drop the maximum below the blocks actually holding files.

**Note:** growing into a second block needs a **SubIndexed** object file. Block 1 starts at
object-file page 512, which a plain Indexed object file cannot address. Granting blocks on such a
pack succeeds, but the 257th file then fails.

See `docs/NDFS-OBJECT-BLOCKS-SPEC.md` for the on-disk layout and how it was verified.

### Clear Password

```
$ ndtool --passwd SYSTEM disk.ndfs
Password cleared for 'SYSTEM'
```

## Friends

A user has 0 or more *friends* stored in its own user entry. A friend grants
one specific other user a set of rights to this user's files — independent of
the file's own "friend tier" access bits. The rights are **R**ead, **W**rite,
**A**ppend, **C**ommon, **D**irectory. Both the owner and the friend may be
given by **user name or by decimal index** (0-255).

> This is distinct from a user's **default file access**
> (`SET-DEFAULT-FILE-ACCESS` in SINTRAN, stored in the user entry): that value
> is the OWN/FRIEND/PUBLIC access copied onto *new files the user creates*.
> `chmod` edits an individual file's access. The friend list, by contrast,
> names *which users* get rights to this user's files.

```
$ ndtool --friendadd ALICE:BOB:RWA disk.ndfs
Added friend 'BOB' to 'ALICE' (RWA)

$ ndtool --friends ALICE disk.ndfs
Friends of 'ALICE': 1
  [  2]  BOB               RWA--

$ ndtool --frienddel ALICE:BOB disk.ndfs
Removed friend 'BOB' from 'ALICE'
```

If you omit the rights on `--friendadd`, the friend gets **RWA** by default.
`--stat USER` (a name with no `/`) shows a user's quota, dates, default file
access, and full friend list. A user may have at most 8 friends.

In the interactive shell the same operations are space-separated:

```
ndtool> friendadd ALICE BOB RWA
ndtool> friends ALICE
ndtool> frienddel ALICE BOB
ndtool> stat ALICE
```

## Editing Files

The `edit` shell command extracts a file to a temporary location, opens it in an external editor, and re-imports it when the editor closes:

```
ndtool> edit RONNY/CAT:C
Opening 'RONNY/CAT:C' in editor...
Re-applied even parity (text file type).
Updated 'RONNY/CAT:C' (814 bytes).
```

The workflow:
1. File is extracted to `/tmp/ndtool_ronny_cat.c` with parity stripped
2. Editor opens the temp file (waits for it to close)
3. If the file was modified, it's written back to the image
4. For text file types (:C, :MODE, :SYMB, etc.), even parity is automatically re-applied
5. Temp file is deleted

### Editor Selection

The editor is chosen in this order:
1. `--editor CMD` command-line flag
2. `$EDITOR` environment variable
3. `$VISUAL` environment variable
4. `code --wait` (VS Code, default)

Examples:

```bash
# Use VS Code (default)
ndtool --shell disk.ndfs

# Use vim
ndtool --editor vim --shell disk.ndfs

# Use nano
ndtool --editor nano --shell disk.ndfs

# Use Notepad++ on Windows/WSL
ndtool --editor "notepad++.exe -multiInst -notabbar -nosession" --shell disk.ndfs
```

## Filesystem Check (fsck)

Run a full 5-phase filesystem check:

```
$ ndtool --fsck BIGDISK0-L.IMG
NDFS Filesystem Check
=====================
Volume: PACK-ONE
Total pages: 38400

Phase 1: Master block
  OK: Master block valid, directory name 'PACK-ONE'

Phase 2: Block reference analysis

Phase 3: Bitmap consistency
  WARNING: 328 orphaned blocks (marked used but not referenced by any file)
  ERROR: 34 blocks referenced by files but marked FREE in bitmap!
  ERROR: 73 blocks referenced by multiple files (cross-linked)

Phase 4: User quota verification
  WARNING: User 'SYSTEM' pages_used=11774 but actual file pages=12077
  WARNING: User 'UTILITY' pages_used=368 but actual file pages=367
  WARNING: User 'BPUN-FILES' pages_used=687 but actual file pages=694

Phase 5: File structure validation
  INFO: 2 files with 0 bytes but allocated pages (device/system files)

========================================
Found 2 error(s) and 11 warning(s).
```

The check performs:
- **Phase 1**: Master block structure validation
- **Phase 2**: Walk all file pointers and build a reference count per block
- **Phase 3**: Compare bitmap against references -- finds orphaned blocks (marked used but unreferenced), missing allocations (referenced but marked free), and cross-linked blocks (referenced by multiple files)
- **Phase 4**: Verify each user's `pages_used` matches the actual sum of their file allocations
- **Phase 5**: Validate file structure integrity (pointers vs page counts)

Also available in the interactive shell as `fsck`.

## Creating New Images

Create from a predefined template:

```
$ ndtool --create floppy360 --name MYDISK new.ndfs
Created floppy360 image: new.ndfs
```

Available templates:

| Template | Size | Pages | Description |
|----------|------|-------|-------------|
| `floppy360` | 315 KB | 154 | ND floppy 360 KB |
| `floppy12` | 1.2 MB | 616 | ND floppy 1.2 MB |
| `smd75` | 75 MB | 38,400 | SMD hard disk |
| `winchester74` | 74 MB | 36,360 | Winchester hard disk |
| `custom` | variable | `--pages N` | Custom page count |

Custom size example:

```
$ ndtool --create custom --pages 500 --name TESTDISK test.ndfs
```

All new images include a SYSTEM user with a default quota.

## Interactive Shell

```
$ ndtool --shell disk.ndfs
ndtool shell - type 'help' for commands

ndtool> ls
  [000]  SYSTEM             12 files  Reserved:  1000  Used:    53
  [006]  RONNY               3 files  Reserved:   500  Used:    12

ndtool> ls SYSTEM
  [0000]  1988-12-16 09:39:59  (SYSTEM)README:TEXT                 1024 bytes      1 pages
  [0001]  1988-12-16 09:40:00  (SYSTEM)STARTUP:MODE                4096 bytes      2 pages
  [0002]  1988-12-16 09:40:45  (SYSTEM)PROGRAM:PROG              102400 bytes     50 pages

ndtool> ls system/:MODE
  [0001]  1988-12-16 09:40:00  (SYSTEM)STARTUP:MODE                4096 bytes      2 pages

ndtool> ls system/S*
  [0001]  1988-12-16 09:40:00  (SYSTEM)STARTUP:MODE                4096 bytes      2 pages

ndtool> cat SYSTEM/README:TEXT
This is a test file on the ND-100 filesystem.

ndtool> put myfile.txt SYSTEM/MYFILE:TEXT
Copied myfile.txt -> SYSTEM/MYFILE:TEXT (256 bytes)

ndtool> rm SYSTEM/MYFILE:TEXT
Deleted SYSTEM/MYFILE:TEXT

ndtool> users
Users: 2
  [000]  SYSTEM             12 files  Reserved:  1000  Used:    53  Free:   947
  [006]  RONNY               3 files  Reserved:   500  Used:    12  Free:   488

ndtool> stat SYSTEM/README:TEXT
  Name:   README:TEXT
  User:   SYSTEM
  Size:   1024 bytes
  Pages:  1
  Alloc:  Indexed (block 42)
  Index:  0

ndtool> fsck
NDFS Filesystem Check
=====================
...
Filesystem is CLEAN. No errors, no warnings.

ndtool> quotaadd RONNY 100
User 'RONNY' quota: 500 -> 600 pages (+100)

ndtool> save
Image saved to disk.ndfs

ndtool> quit
```

### Shell Commands

Two kinds of path appear below:

- **`PATH`** — a file **inside the image** (a SINTRAN file), written `USER/NAME:TYPE`, e.g. `SYSTEM/STARTUP:MODE`. This is *not* a file on your computer.
- **`HOSTFILE`** — a file **on your computer** (the host OS), e.g. `./startup.txt`.

> **The shell edits a copy of the image in memory.** Commands like `put`, `rm`, `chmod`, and `useradd` change that in-memory copy only — nothing is written to the `.ndfs` file until you run **`save`**. Quitting without saving discards your changes.

| Command | Description |
|---------|-------------|
| `ls [USER[/PATTERN]]` | List users, or a user's files (wildcards: `*`, `?`, `:TYPE` shorthand) |
| `cat PATH` | Display a file as text (parity stripped) |
| `hexdump PATH` | Hex dump of a file's content |
| `stat PATH` | Show detailed file metadata (size, allocation, block) |
| `edit PATH` | Edit a file in an external editor (extract, edit, re-import) |
| `get PATH [HOSTFILE]` | Copy a file **out of** the image onto your computer |
| `put HOSTFILE [PATH]` | Copy a file from your computer **into** the image |
| `rm PATH` | Delete a file inside the image |
| `mv OLD NEW` | Rename a file inside the image |
| `info` | Filesystem summary |
| `bitmap` | Bitmap visualization with Unicode block map |
| `fsck` | Full 5-phase filesystem check |
| `users` | List users with quotas |
| `useradd NAME [QUOTA]` | Add user (default quota: 100) |
| `userdel NAME` | Remove user (must have no files) |
| `quotaadd NAME PAGES` | Add pages to user quota |
| `quotadel NAME PAGES` | Remove pages from quota |
| `passwd NAME` | Clear user password |
| `friends USER` | List a user's friends and their rights |
| `friendadd OWNER FRIEND [RWACD]` | Add a friend (default rights RWA) |
| `frienddel OWNER FRIEND` | Remove a friend |
| `stat USER` | Show a user's details + friend list (a name with no `/`) |
| `save [HOSTFILE]` | Save changes to disk (optional save-as) |
| `help` | Show command list |
| `quit` / `exit` | Exit (warns if unsaved changes) |

## Global Options

| Option | Description |
|--------|-------------|
| `-v` | Verbose output (more detail in listings and info) |
| `-l` | Convert names to lowercase on extract (`:` becomes `.`) |
| `-p` | Even parity: strip on extract, calculate and set on copy-in |
| `-d` | Create per-user subdirectories on extract |
| `-o DIR` | Output directory for extract |
| `-F FILE` | Filter by specific file path |
| `-n` | Dry run (show what would happen without writing) |
| `-f` | Force (skip confirmation prompts) |
| `--editor CMD` | External editor for `edit` command (default: `$EDITOR` or `code --wait`) |
| `-h` | Show help |
| `-V` | Show version |

## ND-100 Even Parity

The ND-100 stores text files with even parity. Each byte has bit 7 set so that the total number of 1-bits in the byte is even:

```
'H' = 0x48 (01001000) -> 2 ones (even) -> bit 7 = 0 -> 0x48
'e' = 0x65 (01100101) -> 4 ones (even) -> bit 7 = 0 -> 0x65
' ' = 0x20 (00100000) -> 1 one  (odd)  -> bit 7 = 1 -> 0xA0
'W' = 0x57 (01010111) -> 5 ones (odd)  -> bit 7 = 1 -> 0xD7
'd' = 0x64 (01100100) -> 3 ones (odd)  -> bit 7 = 1 -> 0xE4
```

This is **not** simply setting bit 7 on every byte. It is calculated even parity, matching the convention used by SINTRAN-III for text file types.

## Filename Conversion

### NDFS to Host (on extract)

NDFS filenames use `:` as the type separator and are uppercase:

```
NDFS:  SYSTEM/README:TEXT
Host:  README.TEXT           (without -l)
Host:  readme.text           (with -l)
Host:  SYSTEM/README.TEXT    (with -d)
Host:  system/readme.text    (with -d -l)
```

The `:` is always converted to `.` on extract because `:` is not valid in filenames on Windows.

### Host to NDFS (on copy-in)

Local filenames are converted automatically:

```
Host:  readme.txt     ->  NDFS: README:TXT
Host:  startup.mode   ->  NDFS: STARTUP:MODE
Host:  data.dat       ->  NDFS: DATA:DAT
```

The last `.` becomes `:`, and the name is uppercased. You can also specify the NDFS name explicitly:

```
$ ndtool --put myfile.txt SYSTEM/CUSTOM-NAME:TEXT disk.ndfs
```

## On-Disk Format

See [NDFS-FORMAT.md](../../../docs/NDFS-FORMAT.md) for the complete binary format specification covering master blocks, block pointers, user/object entries, allocation bitmaps, and boot sector formats.

## Comparison with Tor Arntsen's ndfs

ndtool is inspired by [Tor Arntsen's ndfs tool](https://www.ndwiki.org/wiki/User:TArntsen) and supports the same listing and extraction features, plus:

| Feature | Tor's ndfs | ndtool |
|---------|-----------|--------|
| List files (`-t`) | Yes | Yes |
| List users (`-u`) | Yes | Yes |
| Extract files (`-x`) | Yes | Yes |
| Lowercase names (`-l`) | Yes | Yes |
| User subdirectories (`-d`) | Yes | Yes |
| Verbose mode (`-v`) | Yes | Yes |
| Copy files in | No | Yes (`--put`) |
| Delete files | No | Yes (`--rm`) |
| User management | No | Yes (`--useradd`, `--userdel`) |
| Quota management | No | Yes (`--quotaadd`, `--quotadel`) |
| Object blocks (files per user) | No | Yes (`--objblocks`, `--giveobjblocks`, `--takeobjblocks`) |
| Friend management | No | Yes (`--friends`, `--friendadd`, `--frienddel`) |
| Password clearing | No | Yes (`--passwd`) |
| Image creation | No | Yes (`--create`) |
| Interactive shell | No | Yes (`--shell`) |
| Even parity (correct) | Strip only | Strip and set |
| Bitmap visualization | No | Yes (`bitmap`, `-i -v`) |
| Filesystem check (fsck) | No | Yes (`--fsck`, `fsck`) |
| File metadata (stat) | No | Yes (`stat`) |
| Edit file in-place | No | Yes (`edit`, launches external editor) |
| XAT metadata preservation | No | Yes (`--xat`, `.xat` JSON sidecar files) |
| Dry run mode | No | Yes (`-n`) |

## License

MIT - Copyright (c) 1985-2026 Ronny Hansen, RetroCore Labs
