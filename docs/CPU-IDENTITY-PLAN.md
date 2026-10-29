# Plan: SINTRAN CPU identity and segments on a pack, in all four ports

**Next:** C#: `SintranCpuIdentity` and `SintranSegments` in RetroIO.Storage.Ndfs.

Specification and all measured facts: NDInsight `SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md`
and chapters 11 and 22 (sibling repository). The C implementation is `ndfs-c/src/sintran_identity.c`
(`ndtool --cpu-identity` / `--set-cpu`, proven by booting patched copies of a K and an L pack on
2026-10-29) and `ndfs-c/src/sintran_segments.c` (`ndtool --segments` / `--export-segment` /
`--import-segment`, checked against the file view on the same packs). The Python port
(`ndfs-py/src/ndfs/sintran_identity.py`, `sintran_segments.py`) and the TypeScript port
(`ndfs-ts/src/sintran-identity.ts`, `sintran-segments.ts`) are done and checked against the same
two packs; the images they write are byte-identical. This file holds only the work that is still
open; finished items are removed, not ticked.

## Order of work

1. **C#**
   - `RetroIO.Storage.Ndfs` (sibling repository RetroIO, `src/RetroIO.Storage.Ndfs`): validate the
     existing `MasterBlock` extended-info checksum (additive, kernel-verified), `ClearEnteredDirectory`
     and `SintranInitialCommands` against chapters 28 and 26; add `SintranCpuIdentity` and
     `SintranSegments` (read/write) next to `SintranInitialCommands`; tests in `RetroIO.Tests`.
   - RetroCore `Emulated.Utilities\ND\FileSystem\FSObject\MasterBlock.cs`: the extended-info
     checksum is still the XOR model the kernel disproves (`MasterBlock.cs:181-187`). Fix to the
     additive sum. RetroCommander gets "show / set CPU identity" through the `RetroIO.Storage.Ndfs`
     package from the local feed.
2. **nd100x**: bump the ndfs submodule to the commit with the TypeScript port and rebuild the
   browser bundle (`make ndfs-build`). Web front end (`template-glass/js`): identity view and
   segment list in the Browse Disk window, and a set-cpu form that works only on an ejected unit
   (the running emulator owns a mounted buffer) and stores the patched image back through the SMD
   storage. The PROM default is already fixed: signature off until an identity variable is set.
3. **On a booted patched pack** (any emulator): type `@LIST-TITLE` and `@ENTER-DIRECTORY` of a
   second pack to close the two "not done" items in chapter 28 §9.
4. **Segment files 1-3 on a real pack**: no pack on hand defines them (DBLST 1-3 are 0 on K and L).
   Define one on a booted copy (`@ALLOCATE-FILE` + RT-loader `*DEFINE-SEGMENT-FILE Y Y 1 ...`),
   load a segment onto it, then check `ndtool --segments` finds it from DBLST(1). Until then the
   file 1-3 path is covered by the synthetic test only.
5. **Import proven by boot**: import an exported, modified `S3SDPIT` into a copy and boot it, the
   way `--set-cpu` was proven; today import is checked by read-back and by the file view only.

## Open questions carried from chapter 28

- What reads `FCPUN` (no reader in the NPL subset; the two L packs where FCPUN and SYSNO
  disagree ran COSMOS).
- What word 107017 is in the L builds.
- How NEW-SYSTEM turns its answers into `33CPN` / `33CPU` and what it does with `PRFLAG`.
