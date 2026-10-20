# How SINTRAN allocates pages: users, files, index blocks, extending, deleting

This document says which page SINTRAN M picks for each kind of allocation, how a file grows or is
overwritten, how the quota is charged, and what is still unknown. The first block of a new directory
(bit file, first user page, first object page) is in `docs/CREATE-DIRECTORY-PLACEMENT.md`; this
document covers everything after that.

Labels used for every statement:

- **VERIFIED CODE** - read in the disassembly of the SINTRAN file system segment 006-S3FS of the M carve.
- **MEASURED** - read from the image after real SINTRAN M (in the nd100x emulator) did the command.
- **INFERRED** - fits all measurements, but no instruction was found that says so.
- **UNKNOWN** - not settled.

Page numbers are decimal. Code addresses are octal and marked `o`. All runs used the commands of the
SINTRAN command reference (`CREATE-FILE`, `COPY-FILE`, `EXPAND-FILE`, `ALLOCATE-FILE`, `DELETE-FILE`,
`CREATE-USER`, `GIVE-USER-SPACE`) with a SMD 75 MB image as the main test and a cold start on 12 formats.

## 1. Short version

| What | Rule | Label |
|---|---|---|
| Index blocks of indexed files, first user data page and index, object data pages and object index block | the RSBPG hint rule: hint `H = BA - 1`, start at page `H OR 15`, take the first free page going down | VERIFIED CODE and MEASURED |
| Data pages of indexed files, and a contiguous file of 1 page | the RSPAG block rule (section 3): search in 256-page blocks, lowest free page of the block, then the next block up, the next down, and so on | VERIFIED CODE (M06 `053440o`); the start block MEASURED on 12 formats |
| Contiguous file of 2 pages or more (`CREATE-FILE name,N`) | highest free run of N pages under the directory size; the file is placed at the top of that run | MEASURED (about 30 files); the routine is not read |
| `EXPAND-FILE` | only works on a file with no pages; the file becomes contiguous and is placed like `CREATE-FILE name,N`. On a file that has pages: `SPACE ALREADY ALLOCATED` | MEASURED |
| `ALLOCATE-FILE name,octal-page,N` | contiguous file at exactly that page | MEASURED |
| Overwriting a file with a shorter or longer file (`COPY-FILE` to an existing name) | the number of pages never shrinks; only the length in bytes changes; a longer file gets more pages by the data page rule | MEASURED |
| Quota used by a user | all file pages + all index and sub-index blocks + one page for each object data page | MEASURED (5 states, exact arithmetic) |
| Object entries | the object index block has 8 slots per user (user u uses slots 8u to 8u+7); each slot points to an object data page of 32 entries; a page is made when the first file needs it | MEASURED |

## 2. The RSBPG hint rule

Described in full in `docs/CREATE-DIRECTORY-PLACEMENT.md` section 2.2. In short: `H = BA - 1`; the first page
tested is the top bit of the bitmap word that holds H (page `H OR 15`); it goes down to the first free page;
every page it takes is marked used. It is used for:

| Allocation | Example (SMD 75 MB, BA 18468) | Label |
|---|---|---|
| user data page, then user index block (first CREATE-USER) | 18479, 18478 | MEASURED |
| object data page, then object index block (first file of a user) | 18477, 18476 | MEASURED |
| index block of every indexed file | 18475 (E1), 18474 (E2), 18473 (E3), 18471 for the next user's file | MEASURED |
| the object data page of the next user | 18472 (ALPHA, user 1) | MEASURED |
| the first sub-index blocks of a very large file | 18474, 18472, 18471 | MEASURED |

When all pages of the home word are taken, the code moves on to the neighbouring words (read in code,
never measured): the next index block would be 18463 and below.

## 3. The RSPAG block rule (data pages and 1-page files)

Routine RSPAG at M06 `053440o`, called by GPAGE (M06 `101540o`). VERIFIED CODE. The search unit is a **block of
256 pages** (16 bitmap words). The scanner TESTB (M06 `053707o`) takes the **lowest** free page in one block.

1. Bit file page number `BP = page div 16384`; block inside the bit file page `BL = (page mod 16384) div 256` (0 to 63).
2. TESTP (M06 `053622o`) reads bit file page BP and calls TESTB for block BL.
3. If the block has no free page, the loop tries block `BL + 1`, then `BL - 1`, then `BL + 2`, `BL - 2` and so on,
   across bit file pages when it runs out (read in code). The first block with a free page wins.
4. The page found is marked used and its offset is saved in the descriptor of the bit file buffer (word 27o), so
   the next call starts in the block of the last allocation.

### 3.1 Where the search starts (VERIFIED CODE for the logic, MEASURED for the cold start)

RSPAG asks for the bit file buffer that is in use (routine FBFBU, M06 `035210o`):

- If no buffer with a saved offset exists, the start is `BP = (BA + 1) div 16384`, `BL = ((BA + 1) mod 16384) div 256`
  (the block of the hint BA + 1).
- If a buffer exists for bit file page P with the saved offset W (W = -1 means none): when W is set, the start is page
  P, block `W div 256`. When W is -1 the start depends on P against `BP`: `P > BP`: block 0; `P = BP`: block of BA + 1;
  `P < BP`: block 63.

### 3.2 Cold start: measured on 12 formats

Cold start = SINTRAN just booted and `ENTER-DIRECTORY` as the first thing, then `CREATE-FILE N1:DATA 1`,
`CREATE-FILE N2:DATA 1`, an empty file N3 filled by `COPY-FILE` with a 3 page source. The buffer in use is the one of
the **last bit file page** (the page that holds `capacity - 1`), which gives this rule:

```
lastPage = (capacity - 1) div 16384
BP       = (BA + 1) div 16384
start block = lastPage > BP  ->  block 0 of lastPage
              lastPage = BP  ->  the block of BA + 1
```

| Device | Capacity | BA | lastPage | BP | First 1 page file (measured) | Rule gives |
|---|---|---|---|---|---|---|
| DISC-38MB | 18486 | 9243 | 1 | 0 | 16384 | block 0 of page 1 = 16384 |
| DISC-70MB | 34765 | 17379 | 2 | 1 | 32768 | 32768 |
| DISC-75MB | 36945 | 18468 | 2 | 1 | 32768 | 32768 |
| DISC-140MB-F | 69530 | 34758 | 4 | 2 | 65536 | 65536 |
| DISC-288MB-R | 140391 | 70191 | 8 | 4 | 131072 | 131072 |
| DISC-450MB-F | 220584 | 110286 | 13 | 6 | 212992 | 212992 |
| DISC-45MB | 22032 | 11016 | 1 | 0 | 16384 | 16384 |
| DISC-74MB | 36396 | 18198 | 2 | 1 | 32768 | 32768 |
| DISC-14MB | 6912 | 3456 | 0 | 0 | 3328 | block of 3457 = 3328 |
| DISC-21MB | 10728 | 5364 | 0 | 0 | 5120 | 5120 |
| DISC-23MB | 11016 | 5508 | 0 | 0 | 5376 | 5376 |
| DISC-28MB | 13648 | 6822 | 0 | 0 | 6656 | 6656 |

The second 1 page file follows directly (N2 = N1 + 1) and the pages of the indexed file N3 follow after that
(for DISC-75MB: 32768, 32769, then 32770-32772). All 12 rows match. MEASURED.

### 3.3 Warm start: the start depends on the history

After the first allocations, which bit file buffer is "in use" depends on what SINTRAN touched last. Measured on the
75 MB disk, one SINTRAN session (`CREATE-FILE` of 1 page = single, others as listed):

| Step | Page given | Comment |
|---|---|---|
| single C3, C4 after contiguous files near the top | 32768, 32769 | buffer = last bit file page |
| indexed data of E1, E2, E3, AE1 | 32770-32772, 32773, 32774-32897, 32898-32900 | continues |
| `DELETE-FILE` of an indexed file (frees index block 18471 and data), then single Y1 | 18432 | block of BA + 1 (bit file page 1 in use) |
| single Y2, Y3 after that | 32898, 32899 | back to the last page |
| `DELETE-FILE` E9 (indexed), then single Y4 | 18433 | again the block of BA + 1 |
| `DELETE-FILE` of a contiguous file, then single Y5 | 32900 | no switch |
| indexed T1, first copy | 18434-18436 | block of BA + 1 |
| T1 grown to 124 pages | 18438-18466, 18480-18571 | the free pages of the block, skipping the bit file and user pages |

So: after a delete of an indexed file (which frees an index block in bit file page 1) the next page comes from the block of
BA + 1; otherwise from the last bit file page. The exact buffer rule (FBFBU) was not read. The libraries use the cold
start rule, then continue in the block of the last allocated page. INFERRED for the continuation.

## 4. Contiguous files

`CREATE-FILE name,N` with N of 2 or more. MEASURED on the 75 MB disk, about 30 files:

- The first free run of N pages from the top, placed with its **top** at the top of the run: C1 10 pages at 36935-36944
  (capacity 36945), C2 20 pages at 36915-36934, C5 2 pages at 36913-36914, then 3, 5, 8, 9, 16, 17 and 300 pages in
  a stack downward. The top of the stack is `capacity - 1`, not the end of the device.
- A hole is used when the file fits: after `DELETE-FILE` of a 10 page file at 36545-36554, a 4 page file went to
  36551-36554 (the top of the hole), a 3 page file later to 36548-36550, and a 7 page file did not fit the rest of the
  hole (6 pages) and went below to 36533-36539.
- A file of 1 page does not use this rule; it uses the block rule of section 3.

`EXPAND-FILE name,N` on a file with 0 pages (made by `CREATE-FILE name,0`) makes it contiguous at the same place a
`CREATE-FILE name,N` would put it (5 pages at 36540-36544, directly below the earlier file). On any file that already has
pages it stops with `SPACE ALREADY ALLOCATED`, also when the pages above the file are free (4 page file at 8192, free
8196 to 8199, `EXPAND-FILE` to 8 pages). MEASURED.

`ALLOCATE-FILE name,octal-page,N` makes the file at that page (octal 10000 = page 4096).

## 5. Indexed files

A file made by `CREATE-FILE name,0` has no pages. The first write (`COPY-FILE`) makes it indexed:

- pointer type 1 (index block), the index block from the RSBPG hint rule (section 2), the data pages from the
  block rule (section 3);
- pages 1 to 512: one index block with up to 512 data page pointers (4 bytes each);
- pointer type 2 above 512 pages: a top index block with up to 512 sub-index blocks, each with up to 512 data page
  pointers. A 10000 page file (SEGFIL0 copied) has 20 sub-index blocks and 1 top block: 19 full sub-index blocks and one
  with 272 pointers. The first sub-index blocks were taken by the hint rule (18474, 18472, 18471, with the top block at
  18473); the later ones from the data stream, one for each 512 pages (16895, 17407, 17919, 18431, 18956, ...). The order of
  the sub-index blocks in the top block was not the page order: 18474, 18472, 18471, 18431, 18956, 17919, ... MEASURED.
  The exact interleaving is not modelled.

## 6. Overwriting, shortening and extending an existing file

`COPY-FILE` into an existing file (MEASURED, destination S1:DATA):

| Step | Pages | Bytes |
|---|---|---|
| copy a 124 page source into the empty file | 124 | 253356 |
| copy a 3 page source (4495 bytes) over it | 124 | 4495 |
| copy the 124 page source again | 124 | 253356 |

The allocation is never given back and the length in bytes is set to the new length. A file that is longer than its
allocation gets more pages: T1 3 pages (index 18467, data 18434-18436) became 124 pages with the extra 121 pages
at 18438-18466 and 18480-18571 (section 3.3); the old pages stay where they were.

`DELETE-FILE` frees all data pages, all index and sub-index blocks. The object entry slot is freed (a later file reuses
the slot number: slot 5 was reused by Y5 after V1 was deleted). The quota counter is lowered by the freed pages.

### Not measured

- Shortening a file by deleting pages (monitor call 272 `DELPG`/`DeletePage`) and random access writes: no SINTRAN
  command does this; it needs a program that calls the monitor. UNKNOWN. What the C library does today for
  `ndfs_write_file` onto an existing file (free old pages, allocate new) is not what SINTRAN does for `COPY-FILE`
  (keep the allocation) - see section 9.
- Holes (sparse files) written by a random access write. UNKNOWN.

## 7. Quota (pages used)

The user entry has "reserved" (set by `GIVE-USER-SPACE`) and "used". "Used" is changed by CUSED (M06 `055206o`, VERIFIED
CODE that GPAGE calls it). Measured arithmetic, `used` read after `RELEASE-DIRECTORY`:

| State | Computed | `used` read |
|---|---|---|
| SYSTEM: 12 contiguous files and 3 indexed files (E1 3 pages, E2 1 page, E3 124 pages), 520 file pages in all, 3 index blocks, 1 object page | 520 + 3 + 1 | 524 |
| ALPHA: 12 files with 275 pages, 2 index blocks (S1, T1), 1 object page | 275 + 2 + 1 | 278 |
| 10000 page file with 20 sub-index blocks and a top block | 10000 + 21 | +10021 |

So the charge is: every data page (contiguous or indexed), every index block and sub-index block, and every object
data page (to the user whose entry caused the page). `EXPAND-FILE` and `ALLOCATE-FILE` pages are charged too (ALPHA's
278 includes the 5 + 5 pages of X1 and A1). The counter in the user entry is written back late (after the next
operations or at `RELEASE-DIRECTORY`), so reading it directly after one command can show the old number. MEASURED.

## 8. Users and object entries

- A new user (`CREATE-USER dir:name`, the user must exist in the main directory first) takes the next free slot of the user
  data page; no new page is needed until 32 users. The new entry has reserved 0 and used 0. MEASURED.
- The first file of a user makes its object data page: ALPHA (user 1) got a new page (18472) by the hint rule and the
  object index block got the pointer at slot 8 (`user * 8 + block`). The object entry slot inside the page is the
  next free of the 32; slots are reused after a delete. MEASURED.

## 9. What the libraries did before and what they do now

Before this change `ndfs_bf_find_free` took the highest free page for every allocation ("downward from the top"). That
matches SINTRAN only for contiguous files. It did not match for index blocks (hint rule), data pages and 1 page files
(block rule). The quota did not charge index blocks and object pages, and the bit file size was taken from the length of
the device. The C, Python and TypeScript libraries now follow sections 2 to 4 and 7 (tests `test_alloc_rules.c`,
`test_alloc_rules.py`, `alloc-rules.test.ts`, 14 + 41 + 42 tests). The C# libraries RetroFS.NDFS and RetroIO.Storage.Ndfs follow the
same rules (class `NdfsAllocationRulesTests`, 35 tests in each). Images made by the five libraries with the same options
are byte-identical (eight cases compared).

Notes:

* **Cold start per process.** The library has no memory of earlier sessions. The block rule starts at the cold start block
  each time an image is opened and continues from the block of the last page it gave while the image stays open. That is
  also what SINTRAN does after a restart (section 3.2). Writing several files in one session and writing them in
  separate `ndtool` runs can therefore put a data page in different places, for example the last 1 page file of a run of
  files: 33372 in one session, 32512 when it is the fourth `ndtool --put`.
* **Not modelled.** Section 6 (shortening, random access writes), the exact interleaving of the sub-index blocks of a file
  of more than 512 pages (the library takes all of them from the hint rule), and the warm start history of section 3.3.
* **Object page order.** `ndfs_write_file` makes the user's object data page before the file's index block; SINTRAN's
  order was only measured for a contiguous first file.

## 10. Open questions

1. Which bit file buffer RSPAG finds in use (FBFBU, M06 `035210o`), i.e. the exact warm start.
2. The routine that places contiguous files of 2 pages or more (highest run that fits); only the result is known.
3. Shortening by `DeletePage`, random access writes, writes past the end.
4. The interleaving of sub-index blocks in a file of more than 512 pages.
5. Behaviour when a block has no free page and the search spirals across blocks (read in code; the T1 growth is the one
   measured case, and it only skipped used pages inside the same block).
