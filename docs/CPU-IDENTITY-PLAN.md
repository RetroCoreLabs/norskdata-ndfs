# Plan: SINTRAN CPU identity and segments on a pack, in all four ports

**Next:** port `sintran_identity` and `sintran_segments` to ndfs-py.

Specification and all measured facts: NDInsight `SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md`
and chapters 11 and 22 (sibling repository). The C implementation is `ndfs-c/src/sintran_identity.c`
(`ndtool --cpu-identity` / `--set-cpu`, proven by booting patched copies of a K and an L pack on
2026-10-29) and `ndfs-c/src/sintran_segments.c` (`ndtool --segments` / `--export-segment` /
`--import-segment`, checked against the file view on the same packs). This file holds only the
work that is still open; finished items are removed, not ticked.

## Order of work

1. **ndfs-py**: port `sintran_identity` and `sintran_segments` behaviour for behaviour into
   `src/ndfs/sintran_identity.py` and `sintran_segments.py` (symbol parsing, SIXBIT name decode,
   name-record scan, segment-table entry, DBLST/ABLPAGE/MASSNO segment-file bases incl. the file-4
   start-up rule, per-word file check, both DPIT copies, 33CPN/33CPU rewrite with parity, stamp
   clear, read-back; list / export / import by disk page), tests mirroring the two C test files,
   CLI exposure if the Python CLI has one.
2. **ndfs-ts**: same ports and tests; the nd100x web front end (`template-glass/js`) gets a
   read-only segment list and identity view first, write once the C path has been used for a while.
3. **C#**
   - `RetroIO.Storage.Ndfs` (sibling repository RetroIO, `src/RetroIO.Storage.Ndfs`): validate the
     existing `MasterBlock` extended-info checksum (additive, kernel-verified), `ClearEnteredDirectory`
     and `SintranInitialCommands` against chapters 28 and 26; add `SintranCpuIdentity` (read/write)
     next to `SintranInitialCommands`; tests in `RetroIO.Tests`.
   - RetroCore `Emulated.Utilities\ND\FileSystem\FSObject\MasterBlock.cs`: the extended-info
     checksum is still the XOR model the kernel disproves (`MasterBlock.cs:181-187`). Fix to the
     additive sum, or replace the whole legacy NDFS layer with `RetroIO.Storage.Ndfs`; RetroCommander
     then gets "show / set CPU identity".
4. **nd100x**: expose the same read/write through the ndfs submodule in the web front end
   (the PROM default is already fixed: signature off until an identity variable is set).
5. **On a booted patched pack** (any emulator): type `@LIST-TITLE` and `@ENTER-DIRECTORY` of a
   second pack to close the two "not done" items in chapter 28 §9.
6. **Segment files 1-3 on a real pack**: no pack on hand defines them (DBLST 1-3 are 0 on K and L).
   Define one on a booted copy (`@ALLOCATE-FILE` + RT-loader `*DEFINE-SEGMENT-FILE Y Y 1 ...`),
   load a segment onto it, then check `ndtool --segments` finds it from DBLST(1). Until then the
   file 1-3 path is covered by the synthetic test only.
7. **Import proven by boot**: import an exported, modified `S3SDPIT` into a copy and boot it, the
   way `--set-cpu` was proven; today import is checked by read-back and by the file view only.

## Open questions carried from chapter 28

- What reads `FCPUN` (no reader in the NPL subset; the two L packs where FCPUN and SYSNO
  disagree ran COSMOS).
- What word 107017 is in the L builds.
- How NEW-SYSTEM turns its answers into `33CPN` / `33CPU` and what it does with `PRFLAG`.
