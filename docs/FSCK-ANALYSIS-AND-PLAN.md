# ndtool fsck: analysis of BIGDISK0-K-100.IMG and plan

Image analysed: a scratch copy of RonnyTest/HDLC1/BIGDISK0-K-100.IMG
Original: RonnyTest/HDLC1/BIGDISK0-K-100.IMG  
Independent parser used (reads only): a throwaway read-only Python parser (not in the repo)
fsck code: ndfs-c/src/filesystem.c lines 3596-3894

## Result of fsck on this image
- 0 errors, 12 warnings: 25 orphaned pages, 11 users where pages_used differs from counted pages.
- The independent parser (no library code) finds the same 25 orphans, 0 pages referenced-but-free, 0 cross-links.
  So ndtool reads the image correctly. The question is only what the numbers MEAN.

## Orphans (25 pages): real, but not a library bug
Pages 16099-16107, 16117, 16472, 16487, 16511, 16715, 17101, 18355, 20371-20372, 20376-20382.
They are marked used in the bitmap, hold real data (9 of them are PLANC source text with line numbers,
the rest look like program/binary pages), and NO object entry points at them, not even entries whose
in-use bit is clear. Unknown: how SINTRAN left them (crash while a file was open, or a delete that freed
the entry but not the pages). Open question, needs the SINTRAN delete / close code (MDLFI) or a test.

## Quota warnings: the library's rule is wrong for index blocks
The library says index blocks are never charged to the user (filesystem.c line 932). This image says
SINTRAN does charge them. For 8 of 11 users, stored pages_used equals EXACTLY:

    real data pages + index blocks of the user's files + object-entry pages the user holds (1 page per 32 files, rounded up)

| User | pages_used | data | index blks | object pages | sum |
|---|---|---|---|---|---|
| SECRET | 2 | 1 | 0 | 1 | 2 |
| OLAV | 2 | 1 | 0 | 1 | 2 |
| RONNY | 3 | 1 | 1 | 1 | 3 |
| BPUN-FILES | 108 | 100 | 7 | 1 | 108 |
| COSMOS-BASIC | 418 | 400 | 17 | 1 | 418 |
| RT | 2000 | 1989 | 5+1+4 group=10 | 1 | 2000 |
| ND5000-SEMICS | 851 | 785 | 64 | 2 | 851 |
| BIGMAN | 499 | 483 | 0 | 16 | 499 |

(RT: 5 indexed + 1 sub-index + 4 group-index = 10 index blocks.)
Not yet explained: SYSTEM (+18 too many), SCRATCH (+11 too many), UTILITY (5 too few).
UTILITY has exactly 5 files whose header is 0x8000 instead of SINTRAN's 0x9000 (XMP-B02:DEFS, XMP-B02:IMPT,
XMP-100-1-B02:BRF, XMP-100-2-B02:BRF, XMP-500-B02:NRF). That fits "written by a tool that did not charge the
index block", but it is INFERRED, not proven.

Conclusion: the 11 quota warnings are false positives caused by the wrong counting rule, at least 8 of them.
BUT: the data comes from one image. Same test is needed on the other images before changing the rule.

## Plan A: more detail (--fsck -v)
1. Add -v (already a global option) to --fsck. Without -v, output stays as now.
2. Phase 3 with -v: list every orphan page number as ranges (16099-16107), and for each range say
   first non-zero bytes look like text/binary.
3. Phase 3 with -v: list every cross-linked or referenced-free page and WHICH file(s) reference it
   (keep the file label next to each refcount instead of a bare count).
4. Phase 4 with -v: per user print data pages, index blocks, sub/group-index blocks, object-entry pages,
   computed total, stored total, and the difference. This is the table above.
5. Phase 5 with -v: list the files behind the "0 bytes but allocated pages" count, and list in-use entries
   with pointer block 0.
6. Print the ndtool version and the bitmap word order used at the top of the report.
7. Same change in ndfs-py and ndfs-ts is NOT needed (fsck is C only by design, see
   docs/HANDOFF-2026-08-23-FSCK-ACCESS-BITS-AUDIT.md).

## Plan B: fixing what fsck reports (--fsck --fix, dry run by default with -n)
Order of safety:
1. FIRST decide the quota rule (above). Until then do not touch pages_used.
2. Referenced but marked free: set the bit (safe, cannot lose data).
3. Orphans: do NOT free by default. Option --fsck-orphans=report|free|recover. recover copies each run into
   a lost+found file under a chosen user, the way Unix fsck does, so the source text is not lost.
4. Cross-linked: report only, never auto-fix.
5. pages_used: rewrite from the computed total once the rule is confirmed.
6. Every fix: write only the blocks touched, refuse to run on an image that is not a copy unless -f given,
   and print a before/after line for each change.

## Open questions before any code change
1. Where does SINTRAN charge index blocks and object-entry pages? (CUSED call sites; only GPAGE is verified.)
2. Do the other BIGDISK images (L, M, 5000, 102, 103) show the same sum rule?
3. How are the 25 orphans produced (open file at stop, or delete)?

## Update 2: 8 more images probed (read only, in place under the RonnyTest image folder)
Images: HDLC1/BIGDISK0-K-100-old.IMG, HDLC1/BIGDISK0-K-5000.IMG, HDLC1/BIGDISK0-L-SEMICS.IMG, HDLC1/BIGDISK0-L2-100.IMG,
HDLC2/BIGDISK0-K-102.IMG, HDLC3/BIGDISK0-K-103.IMG, HDLC3M/BIGDISK0-M-103.IMG, TCP-M/WD0-SINTRAN-M.IMG.
Not found anywhere under RonnyTest: SMD0.IMG.

- Sum rule holds exactly for small stable users in every SINTRAN-written image:
  BPUN-FILES 107+1=108 (K-100-old, K-5000, K-103, L2-100), COSMOS-BASIC 417+1=418 (all K images), UTILITY 704+1=705 (K-100-old, WD0),
  SCRATCH in K-100-old 130+2=132. The "+N" is the user's object-entry pages.
- Users written by an outside tool break the rule the other way (pages_used = data only, no index blocks):
  ND5000-SEMICS in L-SEMICS used=1594=real, TCP-IP in WD0 used=895=real, UTILITY in L2-100 off by 74.
  Inferred (not proven): these users were filled by ndtool/RetroFS, which does not charge index blocks. This is
  the same library rule seen from the writing side.
- Still unexplained: SCRATCH +11 in BOTH K-100 and K-5000 even though their file sets differ (SCRATCH03-09 differ);
  SYSTEM +18 in K-100, ~0 in K-5000. SCRATCH01 header is 0xd001 (write-open bit) in K-100.
  Decisive experiment (needs a running SINTRAN, Ronny's call): on a copy, note pages_used (@USER-STATISTICS), create,
  expand and delete a file, note it again.
- Cross-links: K-100-old and K-103 both show the same 4 pages (36944, 36559, 36558, 36557) shared by SYSTEM files
  (e.g. FILSYS-SYMBOLS:SYMB with SET-CPU-K:MODE index block). L-SEMICS has 138 cross-links, 20 referenced-but-free and
  130 orphans; L2-100 has 4 and 9 and 14. Not analysed; may be real damage from a stopped/crashed emulator or tool writes.
- Images with huge orphan counts (M-103: 1455 starting at page 36945; WD0: 2004 starting at 36396) are in the
  spare region ABOVE the declared capacity. docs/NDFS-FORMAT.md says pages above declared capacity are never handed out,
  so fsck must not call those orphans (probable false positive in fsck Phase 3; fix = stop the orphan scan at declared capacity
  from the Extended Info Block, words 1756B-1757B).

## Update 3: capacity orphans PROVEN (Extended Info Block word 1756-1757 read from page 0 offset 0x7DC)
| Image | declared capacity | device pages | orphans | orphans below capacity | orphans at/above capacity | used bits at/above capacity |
|---|---|---|---|---|---|---|
| HDLC3M/BIGDISK0-M-103.IMG | 36945 | 38400 | 1455 | 0 | 1455 | 1455 (all of them) |
| TCP-M/WD0-SINTRAN-M.IMG | 36396 | 38400 | 2004 | 0 | 2004 | 2004 (all of them) |
| HDLC1/BIGDISK0-K-100.IMG | 36945 | 38400 | 25 | 25 | 0 | 0 |
So on M-103 and WD0 every page from the declared capacity to the end of the device is marked USED in the bitmap and
none of those is a real orphan. K-100 leaves that region free. Both conventions exist, so fsck Phase 3 must
not count pages >= declared capacity as orphans (fix point: orphan test at filesystem.c line 3784).
Verified count: M-103 = 38400-36945 = 1455, WD0 = 38400-36396 = 2004, exactly the reported orphans.
The 25 K-100 orphans are all below capacity, so they remain real findings.

## Update 4: the 4 cross-linked pages in BIGDISK0-K-100-old.IMG and BIGDISK0-K-103.IMG are REAL DAMAGE
Read from RonnyTest/HDLC3/BIGDISK0-K-103.IMG; the same page contents appear in RonnyTest/HDLC1/BIGDISK0-K-100-old.IMG.

| Page | Owner 1 (SINTRAN-style, header 0x9000) | Owner 2 (header 0x8000, dates 0) | What is stored there now |
|---|---|---|---|
| 36944 | SYSTEM/FILSYS-SYMBOLS:SYMB, data page 0 | SYSTEM/SET-CPU-K:MODE, its INDEX block | 2 non-zero bytes = an index block |
| 36559 | SYSTEM/REFERENCE:FADM, last data page | SYSTEM/SET-CPU-K:MODE, its data page | the SET-CPU-K text (576 bytes) |
| 36558 | SYSTEM/PATCH-FILE:OUT, data page 4 | SYSTEM/PATCH-CPU-NR:MODE, its INDEX block | 2 non-zero bytes = an index block |
| 36557 | SYSTEM/PATCH-FILE:OUT, data page 5 | SYSTEM/PATCH-CPU-NR:MODE, its data page | the PATCH-CPU-NR text (707 bytes) |

- The two MODE files carry creation date 0 and header 0x8000, which is how files written by an outside tool look
  on these images (same marks as the XMP-* files in UTILITY). Inferred: an outside tool wrote them onto pages SINTRAN
  already owned (or SINTRAN then handed out pages the tool had taken; the dates cannot say which came first).
- Result is real data loss: page 0 of FILSYS-SYMBOLS, the last page of REFERENCE:FADM, and pages 4-5 of PATCH-FILE:OUT
  now hold the other files' bytes. fsck Phase 3 "cross-linked" ERROR is therefore correct on these two images.
- Bitmap shows all four pages used, so "referenced but free" does not catch it; only the cross-link count does.
- BIGDISK0-K-100.IMG (the SINTRAN-only image) has 0 cross-links.
- Still not looked at: the 138 cross-links in L-SEMICS and 4 in L2-100 (L2-100 pairs SYSTEM/RONNY-T1:SYMB with
  COSMOS-BASIC/COS-FA-SERV-E04:PROG and SCRATCH/SCRATCH01:DATA, again a SYSTEM file Ronny made).
- Plan impact (Plan A, -v): the cross-link report must name both owners and the page index inside each file, as in
  this table. Plan B: never auto-fix; offer to copy the damaged file out and list which of its pages are shared.

## Update 5: the cross-links match the old byte-swapped bitmap bug (fixed in 0.0.5, 2026-08-02)
Swapped bitmap = page N read/written at byte N>>3 instead of (N>>3)^1, i.e. a tool of that age sees page N as page N^8.
Test (read only): take every page of files marked as outside-tool (header 0x8000 and creation date 0), look at page N^8.
- RonnyTest/HDLC1/BIGDISK0-K-100-old.IMG (file date 2026-07-28, before the fix): the 4 outside-tool pages are exactly
  the 4 cross-linked pages (36557, 36558, 36559, 36944), and for ALL 4 the page N^8 is an orphan (bit set, no owner).
  That is the signature of a tool that set bit N^8 instead of N, then later saw page N as free and reused it.
- RonnyTest/HDLC3/BIGDISK0-K-103.IMG: identical, 4 of 4.
- RonnyTest/HDLC1/BIGDISK0-K-100.IMG (current, SINTRAN-only): 39 outside-tool pages, 0 with an orphan at N^8. No swap signature,
  so its 25 orphans are NOT explained by the old bitmap bug.
- Remaining orphans not explained by the swap: 16 in K-100-old, 15 in K-103, 25 in K-100.
- Conclusion for the cross-link ERROR: the damage is real on disk, its cause is the old bitmap bug (strongly supported, not
  byte-proven: the tool that wrote it is gone). fsck reported it correctly. A post-0.0.5 ndtool would not repeat it.

## Update 6: the 25 orphans in RonnyTest/HDLC1/BIGDISK0-K-100.IMG, by group
No swap signature (Update 5). Compared with RonnyTest/HDLC1/BIGDISK0-K-100-old.IMG, BIGDISK0-K-5000.IMG and BIGDISK0-K-103.IMG
by page number and by page-content hash (read only).

| Group | Pages | What is known |
|---|---|---|
| A | 16099-16107, 16117 | Compiler listing text (line numbers, PLANC code). Free and all zero in K-100-old and K-5000, so created after K-5000 (2026-08-22). No copy of the content in any other image. Owning file unknown. |
| B | 16472, 16487, 16511, 16715, 17101 | Single scattered pages, binary-looking. 16715 and 17101 were page 15 of SYSTEM/CHATSV:PROG and page 9 of SYSTEM/CHATSV:LIST in K-5000 (Ronny's PLANC chat server). In K-100 those two files live elsewhere (CHATSV:PROG now at pages 22104-22139). Of the 17 old PROG pages and 143 old LIST pages, all were freed and reused EXCEPT exactly 1 page of each. The content at 16715 and 17101 changed since K-5000, so the pages were reused before being lost. |
| C | 18355 | Oldest orphan: orphan in K-100-old, K-5000 and K-100. It sits between index slot 4 (page 18356) and slot 5 (page 18354) of SYSTEM/SYSTEM-OUTPUT-1:SYMB, whose pages are handed out downward one by one. 18355 is the one page skipped in that run. In K-5000 its content equals page 18354 (slot 5). |
| D | 20371-20372, 20376-20382 | Orphans in K-5000 too. Byte-identical to pages 7948-7955 of SYSTEM/SEGFIL0:DATA (pages 7820-7827 of that file, unchanged in K-100). They also lie in the downward allocation run of SYSTEM/SYSTEM-OUTPUT-1:SYMB (its slot 127 is page 20375, slots 154-155 are 20369 and 20368). |

- Groups C and D lie inside the allocation run of the same growing file, and a skipped page in the run is exactly what C shows.
  Inferred (not proven): while SYSTEM-OUTPUT-1:SYMB grows, SINTRAN sometimes allocates a page, writes it, and then does not enter it
  in the file's index. Not proven because no trace of the allocator was taken.
- Group B shows the same fingerprint for deletes: all pages of a deleted file are freed except one. Cause unknown.
- Nothing here needs a fix to the library. In fix mode, orphans stay report-only or "recover into a lost+found file" (Plan B step 3);
  group A and B contain readable source and binary, so freeing them by default would lose data.
- To prove or kill the inference: trace SINTRAN GPAGE/RLPAG during a file growth and during a delete in the emulator
  (Ronny's call: needs a running emulator).

## Update 7: are the group B pages directory index pages? NO
Each of the 25 orphans in RonnyTest/HDLC1/BIGDISK0-K-100.IMG was tested as (a) an index block (all 512 pointers in range, type bits not 11),
(b) a page of 32 object entries, (c) a page of 32 user entries. None passes any test: 0 object entries, 0 user entries, and every page
has 190 or more non-zero 4-byte words, i.e. not pointer-shaped. Group B pages are 50-53 percent printable bytes = program/binary data.
The real directory pages (object file, user file, bitmap) are already counted as referenced by fsck Phase 2, so they cannot be the orphans.

Two more facts found while checking:
- Extended Info Block flag word is 0x8000 on K-100, K-100-old, K-5000 and K-103 = "directory entered / not released". System number 0x64 (100)
  on K-100. So every one of these images was left entered, i.e. SINTRAN was stopped without releasing the directory. That fits leaked pages
  but is not proof (see T7 below). Checksum of the Extended Info Block is correct on all four.
- Master block "Unreserved Pages" (offset 0x1C) matches NO simple formula: not capacity minus bitmap-used, not capacity minus sum of quotas
  (K-100: 14208 stored, 16004 free in bitmap, negative for capacity minus sum of reserved). Its meaning is UNKNOWN. fsck must not check or fix it.

## TODO: settle groups B, C and D (and the quota residuals)
Each item says what to do, what result proves what. None of these changes an original image; all run on a copy.

- T1 Static: read the SINTRAN delete and grow paths. In the NDInsight repo, folder SINTRAN/NPL-SOURCE and
  SINTRAN/Filesystem/code-logic/allocation.md: follow MDLFI (delete) to DLPAG/DFPAG/RLPAG, and GPAGE/RSPAG (allocate) to CUSED.
  Question: does a delete free every page of an indexed file, in every case (last index slot, a page currently open, a page allocated but
  not yet entered in the index)? Answer decides whether B is a SINTRAN bug or a stop in the middle.
- T2 Dynamic, growth (groups C and D): in nd100x with the DAP debugger (skill nd100-debug), on a copy of the image set breakpoints at GPAGE
  76205B and CUSED 55206B. Grow a file page by page (copy a 200-page file with SINTRAN's own copy command). After each page record the page
  number GPAGE returns, and compare with the file's index slots. Proof of group C: a GPAGE return that never appears in the index.
- T3 Dynamic, delete (group B): same copy, breakpoint on RLPAG 50635B. Delete an indexed file of N pages made in T2. Count RLPAG calls and
  bitmap bits cleared. Proof of group B: N-1 RLPAG calls (or N calls but one bit set again).
- T4 Clean stop versus hard stop (settles the left-entered flag): run T2, then (a) release the directory cleanly and fsck the copy,
  (b) repeat but kill the emulator in the middle of the copy and fsck. If (a) has 0 orphans and (b) has some, the orphans are stop damage.
- T5 Identify group D: pages 20371-20382 equal SYSTEM/SEGFIL0:DATA pages 7820-7827 (K-5000). Find which program segments those pages hold
  (decode the segment table in SEGFIL0) and what was loaded in K-5000 at that time. Proof: a segment copy outside SEGFIL0 is a SINTRAN
  behaviour (segment dump/relocation) rather than a lost page.
- T6 Identify group A: 9 pages of compiler listing. Decode the ND timestamps of CHATSV:BRF and CHATSV:PROG (dates 0xc2448100 and 0xc24481ac in K-100)
  and of the files around it, to date the compile that produced the listing, then ask Ronny which listing file he deleted or overwrote then.
- T7 Quota residuals (SYSTEM +18, SCRATCH +11 in K-100; SCRATCH +11 in K-5000): hypothesis = pages that leaked stay charged to their owner.
  Test: in T2/T3 note pages_used (@USER-STATISTICS) before and after each step. If a leaked page stays charged, the residual equals the orphans.
  Known against it: the two sums do not match yet (18+11 = 29 against 25 orphans in K-100; 11 against 10 in K-5000).
- T8 Which user was charged for an object entry page and an index block: same T2/T3 notes; confirms the sum rule in Update 1 and 2 by experiment.
- T9 Unreserved Pages (master block offset 0x1C): in T2/T3 note the value before and after each step to find what moves it.
