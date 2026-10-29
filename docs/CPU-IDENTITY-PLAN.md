# Plan: SINTRAN CPU identity and segments on a pack, in all four ports

**Next:** the TPE disk tools on a booted system with two or three SMD units (item 1).

Specification and all measured facts: NDInsight `SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md`
(§8.2a lists where it is implemented, §9 the boot-side proofs) and chapters 11 and 22 (sibling
repository). Done and checked against copies of the same K and L packs: ndfs-c + ndtool, ndfs-py,
ndfs-ts, the nd100x web window, RetroIO.Storage.Ndfs with the RetroCommander tab, the RetroCore
legacy checksum fix, and the multi-unit SMD controller work in nd100x and RetroCore (unit tests
on temporary pack copies in both, `@ENTER-DIRECTORY` of a released pack on SMD unit 1 proven in
both). This file holds only the work that is still open; finished items are removed, not ticked.

## Order of work

1. **The TPE disk tools on a booted system with two or three SMD units**, as the real-system
   check of the multi-unit controller work: copy, compare and verify between units with the
   tools SINTRAN ships, on temporary copies of the K and L packs. Both emulators now have a
   controller trace for this (nd100x `--pipe --log=smd:debug`, RetroCore
   `RETROCORE_SMD_TRACE=<file>`), and the driver's own sequence for a new unit is on record in
   chapter 28 §9.

## Open questions carried from chapter 28

- What reads `FCPUN` (no reader in the NPL subset; the two L packs where FCPUN and SYSNO
  disagree ran COSMOS).
- What word 107017 is in the L builds.
- How NEW-SYSTEM turns its answers into `33CPN` / `33CPU` and what it does with `PRFLAG`.
