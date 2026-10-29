# Plan: SINTRAN CPU identity and segments on a pack, in all four ports

**Next:** the boot-side proofs (items 1 to 3), which need a running machine.

Specification and all measured facts: NDInsight `SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md`
(§8.2a lists where it is implemented) and chapters 11 and 22 (sibling repository). Done and
checked against copies of the same K and L packs: ndfs-c + ndtool, ndfs-py, ndfs-ts, the nd100x
web window, RetroIO.Storage.Ndfs with the RetroCommander tab, and the RetroCore legacy checksum
fix. This file holds only the work that is still open; finished items are removed, not ticked.

## Order of work

1. **On a booted patched pack** (any emulator): type `@LIST-TITLE` and `@ENTER-DIRECTORY` of a
   second pack to close the two "not done" items in chapter 28 §9.
2. **Segment files 1-3 on a real pack**: no pack on hand defines them (DBLST 1-3 are 0 on K and L).
   Define one on a booted copy (`@ALLOCATE-FILE` + RT-loader `*DEFINE-SEGMENT-FILE Y Y 1 ...`),
   load a segment onto it, then check `ndtool --segments` finds it from DBLST(1). Until then the
   file 1-3 path is covered by the synthetic test only.
3. **Import proven by boot**: import an exported, modified `S3SDPIT` into a copy and boot it, the
   way `--set-cpu` was proven; today import is checked by read-back and by the file view only.

## Open questions carried from chapter 28

- What reads `FCPUN` (no reader in the NPL subset; the two L packs where FCPUN and SYSNO
  disagree ran COSMOS).
- What word 107017 is in the L builds.
- How NEW-SYSTEM turns its answers into `33CPN` / `33CPU` and what it does with `PRFLAG`.
