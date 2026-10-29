# Plan: SINTRAN CPU identity and segments on a pack, in all four ports

**Next:** multi-unit SMD validation (item 1); every ndfs-side proof is in (chapter 28 §9).

Specification and all measured facts: NDInsight `SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md`
(§8.2a lists where it is implemented) and chapters 11 and 22 (sibling repository). Done and
checked against copies of the same K and L packs: ndfs-c + ndtool, ndfs-py, ndfs-ts, the nd100x
web window, RetroIO.Storage.Ndfs with the RetroCommander tab, and the RetroCore legacy checksum
fix. This file holds only the work that is still open; finished items are removed, not ticked.

## Order of work

1. **SINTRAN with more than one SMD unit is not validated in the emulators.** On 2026-10-29
   `@ENTER-DIRECTORY PACK-TWO,DISC-75MB-1,1` of a released L copy on unit 1 never returned in
   RetroCore (six minutes, emulator alive), while unit 2's stamp check answered at once. Ronny's
   read: probably bugs in the SMD disk controller for unit numbers other than 0, and SINTRAN with
   several disks has never been checked. The work, in this order:
   - unit tests that read and write across several SMD units, on temporary copies of a valid
     pack (never the originals), first in nd100x, then the same cases ported to the C# RetroCore
     controller;
   - then the TPE disk tools on a booted system with two or three units as the real-system check;
   - then repeat the `@ENTER-DIRECTORY` of a released pack on unit 1 (the one proof still open
     from chapter 28 §9).

## Open questions carried from chapter 28

- What reads `FCPUN` (no reader in the NPL subset; the two L packs where FCPUN and SYSNO
  disagree ran COSMOS).
- What word 107017 is in the L builds.
- How NEW-SYSTEM turns its answers into `33CPN` / `33CPU` and what it does with `PRFLAG`.
