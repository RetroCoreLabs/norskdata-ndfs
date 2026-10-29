/**
 * SINTRAN III segments on a system pack: list, export, import.
 *
 * A segment is a run of pages on one of up to five segment files. The segment
 * table (inside SEGFIL0) gives each segment its LOGAD, SEGLE, MADR and FLAG, and
 * bits 13-15 of FLAG name the segment file. Where a segment file begins on the
 * pack comes from the kernel's own tables in the resident data page (DBLST,
 * ABLPAGE, MASSNO, read from the S3IDPIT copy), with one rule the kernel applies
 * at every start-up: segment file 4 starts at disk page 1 (OPPSTART:751). Names
 * come from the RT-loader DEFINED SEGMENT NAME records.
 *
 * Ported behaviour-for-behaviour from ndfs-c/src/sintran_segments.c. The model is
 * in NDInsight chapters SINTRAN/OS/11 and 22, the measurements in chapter 28.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import type { NdfsFileSystem } from './ndfs-filesystem.js';
import { stripParity } from './parity.js';
import { findSegmentTablePage } from './sintran.js';
import { SEGNAME_IMAGE_DPIT, CpuIdentityError, findSymbol, findSystemFile, nameRecords, segmentWordOffset } from './sintran-identity.js';

/** SINTRAN knows segment files 0..4. */
export const SEGFILE_COUNT = 5;

const PAGE_BYTES = 2048;
const SEG_ENTRY_BYTES = 16;
const NAME_RECORD_BYTES = 16;
const INIBU_TO_VERSION: Readonly<Record<number, string>> = { 0o67172: 'K', 0o74123: 'L', 0o102327: 'M' };

/** One of the five segment files as the kernel tables describe it. */
export interface SegFile {
  defined: boolean;
  /** First disk block (DBLST), 32-bit. */
  dblst: number;
  /** ABLPAGE */
  blocksPerPage: number;
  /** MASSNO: which disk unit. */
  massno: number;
  onThisDisk: boolean;
  /** First disk page on this pack; -1 when unknown. */
  basePage: number;
  /** Where basePage came from. */
  source: string;
  /** NDFS file starting at basePage, for labelling. */
  filePath: string;
}

/** One segment table entry plus its name and its place on the pack. */
export interface Segment {
  number: number;
  name: string;
  segli: number;
  prese: number;
  logad: number;
  segle: number;
  madr: number;
  flag: number;
  sgsta: number;
  bpagl: number;
  /** (flag >> 13) & 7 */
  segfile: number;
  /** flag bit 0: has a disk address. */
  ok5: boolean;
  /** SEGLE 0: no pages on disk (S3DPIT, S3RPIT ...). */
  memoryOnly: boolean;
  /** First disk page, -1 when not on a known local file. */
  diskPage: number;
}

/** Result of {@link listSegments}. */
export interface SegmentList {
  version: string;
  segmentTablePage: number;
  segfilPath: string;
  namesPath: string;
  haveSymbols: boolean;
  blstSym: number;
  ablpaSym: number;
  massnSym: number;
  segfiles: SegFile[];
  segments: Segment[];
}

function newSegFile(): SegFile {
  return { defined: false, dblst: 0, blocksPerPage: 0, massno: 0, onThisDisk: true, basePage: -1, source: 'unknown', filePath: '' };
}

function wordBE(b: Uint8Array, o: number): number {
  return (b[o] << 8) | b[o + 1];
}

// -- Names -------------------------------------------------------------------

/** Every DEFINED SEGMENT NAME record in `buf`: segment number -> name. The first record for a number wins. */
export function collectSegmentNames(buf: Uint8Array): Map<number, string> {
  const names = new Map<number, string>();
  if (buf.length < NAME_RECORD_BYTES) return names;
  for (const r of nameRecords(buf)) {
    if (!names.has(r.segno)) names.set(r.segno, r.name);
  }
  return names;
}

// -- Symbols -----------------------------------------------------------------

function readSymbol(fs: NdfsFileSystem, path: string, name: string): number | null {
  const hit = findSymbol(stripParity(fs.readFile(path)), name);
  return hit === null ? null : hit.value;
}

// -- Segment table -----------------------------------------------------------

function readEntry(seg: Uint8Array, page: number, n: number): Segment {
  const o = page * PAGE_BYTES + n * SEG_ENTRY_BYTES;
  const flag = wordBE(seg, o + 10);
  const segle = wordBE(seg, o + 6) & 0x3ff;
  return {
    number: n,
    name: '',
    segli: wordBE(seg, o + 0),
    prese: wordBE(seg, o + 2),
    logad: wordBE(seg, o + 4),
    segle,
    madr: wordBE(seg, o + 8),
    flag,
    sgsta: wordBE(seg, o + 12),
    bpagl: wordBE(seg, o + 14),
    segfile: (flag >> 13) & 7,
    ok5: (flag & 1) !== 0,
    // No pages: S3DPIT, S3RPIT ... MADR 0 is a real offset (S3IMAGE, S3SAVE).
    memoryOnly: segle === 0,
    diskPage: -1,
  };
}

/** The NDFS file whose first disk page is `page`, for labelling only. */
function labelFileAtPage(fs: NdfsFileSystem, page: number): string {
  if (page < 0) return '';
  const objs = fs.getObjectEntries();
  for (let i = 0; i < objs.length; i++) {
    const obj = objs[i];
    if (obj.pagesInFile === 0) continue;
    const t = (obj.type ?? '').replace(/['\s]+$/, '');
    const candidate = `${obj.userName}/${obj.objectName}:${t}`;
    let blocks: number[];
    try {
      blocks = fs.getFileBlocks(candidate);
    } catch {
      continue;
    }
    if (blocks.length > 0 && blocks[0] === page) return candidate;
  }
  return '';
}

/** Segment-file bases from the kernel tables inside the S3IDPIT copy. */
function readSegfileTables(fs: NdfsFileSystem, seg: Uint8Array, out: SegmentList, idpit: Segment | null): void {
  for (const sf of out.segfiles) {
    sf.defined = false;
    sf.basePage = -1;
    sf.source = 'unknown';
    sf.filePath = '';
    sf.onThisDisk = true;
  }

  // Where the DPIT image copy is inside SEGFIL0 (segment file 0 by construction:
  // the table and the DPIT image are both in SEGFIL0:DATA).
  const dpitOk = idpit !== null && !idpit.memoryOnly && out.haveSymbols && (idpit.madr + idpit.segle) * PAGE_BYTES <= seg.length;

  if (dpitOk && idpit !== null) {
    let ablpage0 = 0;
    let massno0 = 0;
    for (let n = 0; n < SEGFILE_COUNT; n++) {
      const sf = out.segfiles[n];
      const bl = segmentWordOffset(idpit.logad, idpit.segle, idpit.madr, out.blstSym + 2 * n);
      const ab = segmentWordOffset(idpit.logad, idpit.segle, idpit.madr, out.ablpaSym + n);
      const mn = segmentWordOffset(idpit.logad, idpit.segle, idpit.madr, out.massnSym + n);
      if (bl < 0 || ab < 0 || mn < 0) continue;
      sf.dblst = wordBE(seg, bl) * 0x10000 + wordBE(seg, bl + 2);
      sf.blocksPerPage = wordBE(seg, ab);
      sf.massno = wordBE(seg, mn);
      if (n === 0) {
        ablpage0 = sf.blocksPerPage;
        massno0 = sf.massno;
      }
      if (sf.blocksPerPage === 0) sf.blocksPerPage = ablpage0;
      sf.onThisDisk = sf.massno === 0 || sf.massno === massno0;
      if (sf.dblst !== 0 && sf.blocksPerPage > 0) {
        sf.defined = true;
        sf.basePage = Math.floor(sf.dblst / sf.blocksPerPage);
        sf.source = 'DBLST / ABLPAGE in S3IDPIT';
      } else if (n === 4 && ablpage0 > 0) {
        // Set at every start-up, not stored: BLST(11B) := ABLPAGE(0) + XXSWTYPE
        // (PH-P2-OPPSTART.NPL:751), i.e. one page when XXSWTYPE is 0. Measured on
        // the K packs: the save copy at MADR 27 is disk page 28.
        sf.defined = true;
        sf.blocksPerPage = ablpage0;
        sf.basePage = 1;
        sf.source = 'start-up rule, ABLPAGE(0) blocks (OPPSTART:751)';
      }
    }
  }

  // Without the tables, segment file 0 is where SEGFIL0:DATA begins.
  if (!out.segfiles[0].defined) {
    const blocks = fs.getFileBlocks(out.segfilPath);
    if (blocks.length > 0) {
      out.segfiles[0].defined = true;
      out.segfiles[0].basePage = blocks[0];
      out.segfiles[0].blocksPerPage = 0;
      out.segfiles[0].source = 'first page of SEGFIL0:DATA (no symbol list)';
    }
  }

  for (const sf of out.segfiles) {
    if (sf.defined && sf.onThisDisk) sf.filePath = labelFileAtPage(fs, sf.basePage);
  }
}

// -- List --------------------------------------------------------------------

/**
 * Every segment the table uses or the name records name, with the segment files
 * and each segment's first disk page. Throws CpuIdentityError when the pack has no
 * SEGFIL0 or no segment table.
 */
export function listSegments(fs: NdfsFileSystem): SegmentList {
  const out: SegmentList = {
    version: '?', segmentTablePage: -1, segfilPath: '', namesPath: '', haveSymbols: false,
    blstSym: -1, ablpaSym: -1, massnSym: -1,
    segfiles: [newSegFile(), newSegFile(), newSegFile(), newSegFile(), newSegFile()],
    segments: [],
  };

  // Symbols: INIBU for the version letter, BLST / ABLPA / MASSN for the segment files.
  const symPath = findSystemFile(fs, 'SYMBOL-1-LIST', 'SYMB');
  if (symPath !== null) {
    const inibu = readSymbol(fs, symPath, 'INIBU');
    if (inibu !== null) out.version = INIBU_TO_VERSION[inibu] ?? '?';
    const blst = readSymbol(fs, symPath, 'BLST');
    const ablpa = readSymbol(fs, symPath, 'ABLPA');
    const massn = readSymbol(fs, symPath, 'MASSN');
    if (blst !== null && ablpa !== null && massn !== null) {
      out.haveSymbols = true;
      out.blstSym = blst;
      out.ablpaSym = ablpa;
      out.massnSym = massn;
    }
  }

  // SEGFIL0 and its segment table.
  const segfil = findSystemFile(fs, 'SEGFIL0', 'DATA') ?? findSystemFile(fs, 'SEGFIL', 'DATA');
  if (segfil === null) throw new CpuIdentityError('no SEGFIL0:DATA');
  out.segfilPath = segfil;
  const seg = fs.readFile(segfil);
  out.segmentTablePage = findSegmentTablePage(seg);
  if (out.segmentTablePage < 0) throw new CpuIdentityError('no segment table found in SEGFIL0');

  // Names: RTFIL:DATA first, then the loader table copies inside SEGFIL0.
  let names = new Map<number, string>();
  const rtPath = findSystemFile(fs, 'RTFIL', 'DATA');
  if (rtPath !== null) {
    names = collectSegmentNames(fs.readFile(rtPath));
    if (names.size > 0) out.namesPath = rtPath;
  }
  if (out.namesPath === '') {
    names = collectSegmentNames(seg);
    if (names.size > 0) out.namesPath = 'SEGFIL0 (loader table copy)';
  }

  // Every entry that is used or named.
  let idpit: Segment | null = null;
  for (let n = 1; n < 256; n++) {
    const s = readEntry(seg, out.segmentTablePage, n);
    if (s.segle === 0 && s.madr === 0 && s.logad === 0 && !names.has(n)) continue;
    s.name = names.get(n) ?? '';
    if (s.name === SEGNAME_IMAGE_DPIT) idpit = s;
    out.segments.push(s);
  }

  readSegfileTables(fs, seg, out, idpit);

  // Disk page of every segment with a disk copy on a known, local segment file.
  for (const s of out.segments) {
    const sf = out.segfiles[s.segfile];
    if (s.memoryOnly || s.segle === 0) continue;
    if (!s.ok5 || !sf.defined || !sf.onThisDisk) continue;
    s.diskPage = sf.basePage + s.madr;
  }
  return out;
}

// -- Lookup ------------------------------------------------------------------

/**
 * Index into `list.segments` of the segment named, or numbered (octal with a
 * trailing B, decimal without), case-insensitive; -1 when absent.
 */
export function findSegment(list: SegmentList, nameOrNumber: string): number {
  if (!nameOrNumber) return -1;
  let text = nameOrNumber;
  let base = 10;
  const last = text[text.length - 1];
  if (last === 'B' || last === 'b') {
    base = 8;
    text = text.slice(0, -1);
  }
  const digits = base === 8 ? /^[0-7]+$/ : /^[0-9]+$/;
  if (digits.test(text)) {
    const v = parseInt(text, base);
    for (let i = 0; i < list.segments.length; i++) {
      if (list.segments[i].number === v) return i;
    }
    return -1;
  }
  const want = nameOrNumber.toUpperCase();
  for (let i = 0; i < list.segments.length; i++) {
    const s = list.segments[i];
    if (s.name !== '' && s.name.toUpperCase() === want) return i;
  }
  return -1;
}

// -- Export / import ---------------------------------------------------------

function checkPlace(fs: NdfsFileSystem, list: SegmentList, index: number): Segment {
  if (index < 0 || index >= list.segments.length) throw new RangeError('segment index out of range');
  const s = list.segments[index];
  if (s.memoryOnly || s.segle === 0 || s.diskPage < 0) throw new CpuIdentityError(`segment ${s.number} has no disk copy on this pack`);
  if (s.diskPage + s.segle > fs.totalPages) throw new RangeError(`segment ${s.number} runs past the end of the image`);
  return s;
}

/** The segment's pages as they are on the pack (SEGLE * 2048 bytes). */
export function exportSegment(fs: NdfsFileSystem, list: SegmentList, index: number): Uint8Array {
  const s = checkPlace(fs, list, index);
  const out = new Uint8Array(s.segle * PAGE_BYTES);
  for (let p = 0; p < s.segle; p++) out.set(fs.readDiskPage(s.diskPage + p), p * PAGE_BYTES);
  return out;
}

/**
 * Write `data` over the segment's pages. Up to SEGLE pages; a last partial page
 * keeps its remaining bytes. Throws RangeError when `data` is longer than the segment.
 */
export function importSegment(fs: NdfsFileSystem, list: SegmentList, index: number, data: Uint8Array): void {
  const s = checkPlace(fs, list, index);
  const size = data.length;
  if (size > s.segle * PAGE_BYTES) throw new RangeError('data is longer than the segment');
  let done = 0;
  while (done < size) {
    const pg = s.diskPage + Math.floor(done / PAGE_BYTES);
    const chunk = Math.min(size - done, PAGE_BYTES);
    const page = chunk < PAGE_BYTES ? fs.readDiskPage(pg) : new Uint8Array(PAGE_BYTES);
    page.set(data.subarray(done, done + chunk), 0);
    fs.writeDiskPage(pg, page);
    done += chunk;
  }
}
