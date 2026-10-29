# Plan: change a SINTRAN pack's CPU number and CPU type offline

**Next:** port `sintran_identity` to ndfs-py.

Specification and all measured facts: NDInsight `SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md`
(sibling repository). The C implementation is `ndfs-c/src/sintran_identity.c` with
`ndtool --cpu-identity` / `--set-cpu`, proven by booting patched copies of a K and an L pack on
2026-10-29 (chapter 28 §9). This file holds only the work that is still open; finished items are
removed, not ticked.

## Order of work

1. **ndfs-py**: port `sintran_identity` behaviour for behaviour into `src/ndfs/sintran_identity.py`
   (symbol parsing, SIXBIT name decode, name-record scan, segment-table entry, per-word file
   check, both copies, 33CPN/33CPU rewrite with parity, stamp clear, read-back), tests mirroring
   `ndfs-c/tests/test_sintran_identity.c`, CLI exposure if the Python CLI has one.
2. **ndfs-ts**: same port and tests; the nd100x web front end (`template-glass/js`) gets a
   read-only view first, write once the C path has been used on a real machine for a while.
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

## Open questions carried from chapter 28

- What reads `FCPUN` (no reader in the NPL subset; the two L packs where FCPUN and SYSNO
  disagree ran COSMOS).
- What word 107017 is in the L builds.
- How NEW-SYSTEM turns its answers into `33CPN` / `33CPU` and what it does with `PRFLAG`.
