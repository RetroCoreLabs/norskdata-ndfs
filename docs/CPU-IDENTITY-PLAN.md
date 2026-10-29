# Plan: SINTRAN CPU identity and segments on a pack, in all four ports

**Next:** a segment on segment file 1 to 3 (item 1); everything else is proven (chapter 28 §9).

Specification and all measured facts: NDInsight `SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md`
(§8.2a lists where it is implemented) and chapters 11 and 22 (sibling repository). Done and
checked against copies of the same K and L packs: ndfs-c + ndtool, ndfs-py, ndfs-ts, the nd100x
web window, RetroIO.Storage.Ndfs with the RetroCommander tab, and the RetroCore legacy checksum
fix. This file holds only the work that is still open; finished items are removed, not ticked.

## Order of work

1. **Segment files 1-3 on a real pack**: no pack on hand defines them (DBLST 1-3 are 0 on K and L).
   Define one on a booted copy (`@ALLOCATE-FILE` + RT-loader `*DEFINE-SEGMENT-FILE Y Y 1 ...`),
   load a segment onto it, then check `ndtool --segments` finds it from DBLST(1). Until then the
   file 1-3 path is covered by the synthetic test only.
2. **`@ENTER-DIRECTORY` of a released pack on a second SMD unit**: on 2026-10-29 the command never
   returned in RetroCore (unit 1, L copy, six minutes, emulator alive). The stamp verdicts are in
   (boot refuses a pack stamped by another system, `@ENTER-DIRECTORY` refuses one too, a released
   pack boots clean), so this is an emulator / multi-unit question to take to RetroCore, not an
   ndfs one.

## Open questions carried from chapter 28

- What reads `FCPUN` (no reader in the NPL subset; the two L packs where FCPUN and SYSNO
  disagree ran COSMOS).
- What word 107017 is in the L builds.
- How NEW-SYSTEM turns its answers into `33CPN` / `33CPU` and what it does with `PRFLAG`.
