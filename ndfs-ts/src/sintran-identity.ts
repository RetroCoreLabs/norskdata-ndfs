/**
 * SINTRAN III CPU number and CPU type on a system pack: read / change / verify.
 *
 * A SINTRAN pack carries its CPU number twice (SYSNO and FCPUN) and its CPU type
 * once (HWINF+2) in the resident data page. That page is stored on disk in two
 * segments: the image copy (segment named S3IDPIT, used at a warm start) and the
 * save copy (S3SDPIT, used at a cold start). Nothing here is hard-coded:
 *
 *  - the word addresses come from the pack's own (SYSTEM)SYMBOL-1-LIST:SYMB and
 *    SYMBOL-2-LIST:SYMB (FCPUN moves from build to build);
 *  - the segment numbers come from the RT-loader "DEFINED SEGMENT NAME" records
 *    (type 25B) in (SYSTEM)RTFIL:DATA or their copies inside SEGFIL0;
 *  - the disk pages and the segment file come from the segment table in SEGFIL0.
 *
 * Ported behaviour-for-behaviour from ndfs-c/src/sintran_identity.c (the Python
 * port ndfs-py/src/ndfs/sintran_identity.py is the same shape). The measurements
 * and the derivation are in NDInsight, chapter
 * SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import type { NdfsFileSystem } from './ndfs-filesystem.js';
import { MasterBlock } from './master-block.js';
import { setParity, stripParity } from './parity.js';
import { findSegmentTablePage } from './sintran.js';

/** Segment names of the two disk copies of the resident data page. */
export const SEGNAME_IMAGE_DPIT = 'S3IDPIT';
export const SEGNAME_SAVE_DPIT = 'S3SDPIT';

/** RT-loader record type of a "DEFINED SEGMENT NAME" record (octal 25). */
export const RTFIL_TYPE_SEGMENT_NAME = 0o25;

/** A segment name is at most 8 SIXBIT characters (three 16-bit words). */
export const SEGNAME_MAX = 8;

const PAGE_BYTES = 2048;
const SEG_ENTRY_BYTES = 16;
const NAME_RECORD_BYTES = 16;

/** INIBU per version, as sintran.ts. Only used to name the version letter. */
const INIBU_TO_VERSION: Readonly<Record<number, string>> = { 0o67172: 'K', 0o74123: 'L', 0o102327: 'M' };

/**
 * A pack does not hold what the reader or writer needs (no symbol list, no name
 * records, a copy whose file does not hold the word, ...). `identity` carries what
 * was found before the refusal.
 */
export class CpuIdentityError extends Error {
  identity: CpuIdentity | null;
  constructor(message: string, identity: CpuIdentity | null = null) {
    super(message);
    this.name = 'CpuIdentityError';
    this.identity = identity;
  }
}

/** Word addresses read from the pack's symbol lists; -1 when absent. */
export interface SintranSymbols {
  sysno: number;
  hwinf: number;
  revle: number;
  fcpun: number;
  /** 33CPN: the CPU number the generation was asked for. */
  cpn33: number;
  /** 33CPU: the CPU type the generation was asked for. */
  cpu33: number;
  inibu: number;
}

/** One disk copy of the resident data page and the identity words in it. */
export interface DpitCopy {
  name: string;
  /** "image" or "save". */
  role: 'image' | 'save';
  /** From the name records; -1 when unresolved. */
  segmentNumber: number;
  /** Table entry has pages, the 5OK bit and covers SYSNO. */
  present: boolean;
  logad: number;
  segle: number;
  madr: number;
  flag: number;
  /** (flag >> 13) & 7 */
  segmentFile: number;
  /** NDFS file holding the segment file, "" when unknown. */
  filePath: string;
  fileFound: boolean;
  /** Byte offsets inside filePath; -1 = not reachable. */
  sysnoOffset: number;
  fcpunOffset: number;
  typeOffset: number;
  revleOffset: number;
  sysno: number;
  fcpun: number;
  cpuType: number;
  revle: number;
}

/** Everything the reader found. Filled as far as the pack allows, so a refusal still reports what was there. */
export interface CpuIdentity {
  version: string;
  sym: SintranSymbols;
  symbol1Path: string;
  symbol2Path: string;
  /** Where the segment numbers came from. */
  namesPath: string;
  /** Byte offset of the 33CPN digits in symbol2Path; -1 when absent. */
  cpn33ValueOffset: number;
  cpu33ValueOffset: number;
  cpn33ValueWidth: number;
  cpu33ValueWidth: number;
  symbol2HasParity: boolean;
  segmentTablePage: number;
  /** [0] = image copy (S3IDPIT), [1] = save copy (S3SDPIT). */
  copies: DpitCopy[];
  dirExtValid: boolean;
  dirEntered: boolean;
  dirSystemNumber: number;
}

function newCopy(name: string, role: 'image' | 'save'): DpitCopy {
  return {
    name, role, segmentNumber: -1, present: false,
    logad: 0, segle: 0, madr: 0, flag: 0, segmentFile: 0, filePath: '', fileFound: false,
    sysnoOffset: -1, fcpunOffset: -1, typeOffset: -1, revleOffset: -1,
    sysno: -1, fcpun: -1, cpuType: -1, revle: -1,
  };
}

function newIdentity(): CpuIdentity {
  return {
    version: '?',
    sym: { sysno: -1, hwinf: -1, revle: -1, fcpun: -1, cpn33: -1, cpu33: -1, inibu: -1 },
    symbol1Path: '', symbol2Path: '', namesPath: '',
    cpn33ValueOffset: -1, cpu33ValueOffset: -1, cpn33ValueWidth: 0, cpu33ValueWidth: 0,
    symbol2HasParity: false, segmentTablePage: -1,
    copies: [newCopy(SEGNAME_IMAGE_DPIT, 'image'), newCopy(SEGNAME_SAVE_DPIT, 'save')],
    dirExtValid: false, dirEntered: false, dirSystemNumber: 0,
  };
}

// -- Pure helpers ------------------------------------------------------------

function wordBE(b: Uint8Array, o: number): number {
  return (b[o] << 8) | b[o + 1];
}

/**
 * Byte offset of word address `addr` inside the segment file, or -1 when the segment
 * does not cover it. The logical base is the low six bits of LOGAD in pages:
 * byte = MADR*2048 + 2*(addr - base).
 */
export function segmentWordOffset(logad: number, segle: number, madr: number, addr: number): number {
  const base = (logad & 0o77) * 1024;
  if (addr < base || addr >= base + segle * 1024) return -1;
  return madr * PAGE_BYTES + (addr - base) * 2;
}

/** Result of {@link findSymbol}: the octal value, where its first digit is, how many digits. */
export interface SymbolHit {
  value: number;
  offset: number;
  width: number;
}

/**
 * Find `NAME=OOOOOO` at the start of a line (leading blanks allowed) in a
 * parity-stripped symbol list. The name must be followed by '=' so "CPN" never
 * matches "33CPN". Null when absent.
 */
export function findSymbol(text: Uint8Array, name: string): SymbolHit | null {
  if (name.length === 0) return null;
  const nlen = name.length;
  const n = text.length;
  let i = 0;
  while (i < n) {
    let p = i;
    while (p < n && (text[p] === 0x20 || text[p] === 0x09)) p++;
    if (p + nlen < n && text[p + nlen] === 0x3d) {
      let same = true;
      for (let k = 0; k < nlen; k++) {
        if (text[p + k] !== name.charCodeAt(k)) { same = false; break; }
      }
      if (same) {
        const d = p + nlen + 1;
        let value = 0;
        let width = 0;
        while (d + width < n && text[d + width] >= 0x30 && text[d + width] <= 0x37) {
          value = value * 8 + (text[d + width] - 0x30);
          width++;
        }
        if (width > 0) return { value, offset: d, width };
      }
    }
    while (i < n && text[i] !== 0x0a && text[i] !== 0x0d) i++;
    while (i < n && (text[i] === 0x0a || text[i] === 0x0d)) i++;
  }
  return null;
}

/**
 * Decode an RT-loader packed segment name: SIXBIT characters, right-justified
 * base 64 across three words. Null when the words are not a name.
 */
export function decodePackedName(w0: number, w1: number, w2: number): string | null {
  // 48 bits do not fit a JS number's safe bit operations, so use BigInt.
  let n = (BigInt(w0) << 32n) | (BigInt(w1) << 16n) | BigInt(w2);
  if (n === 0n) return null;
  const chars: string[] = [];
  while (n !== 0n) {
    const c6 = Number(n & 0x3fn);
    n >>= 6n;
    if (chars.length >= SEGNAME_MAX) return null;
    // SIXBIT: 1..26 are A..Z (ASCII & 0x3F), 0x30..0x39 are the digits.
    if (c6 >= 1 && c6 <= 26) chars.push(String.fromCharCode(0x41 + c6 - 1));
    else if (c6 >= 0x30 && c6 <= 0x39) chars.push(String.fromCharCode(c6));
    else return null;
  }
  chars.reverse();
  return chars.join('');
}

/** Every well-formed DEFINED SEGMENT NAME record in `buf`, scanning on word boundaries. */
export function* nameRecords(buf: Uint8Array): Generator<{ segno: number; name: string }> {
  const size = buf.length;
  for (let o = 0; o + NAME_RECORD_BYTES <= size; o += 2) {
    if (wordBE(buf, o + 10) !== RTFIL_TYPE_SEGMENT_NAME) continue;
    if (wordBE(buf, o + 8) !== 0 || wordBE(buf, o + 14) !== 0) continue;
    const segno = wordBE(buf, o + 6);
    if (segno === 0 || segno > 0o377 || segno !== wordBE(buf, o + 12)) continue;
    const name = decodePackedName(wordBE(buf, o), wordBE(buf, o + 2), wordBE(buf, o + 4));
    if (name === null) continue;
    yield { segno, name };
  }
}

/** Segment number the first DEFINED SEGMENT NAME record for `name` gives, or -1. */
export function findSegmentByName(buf: Uint8Array, name: string): number {
  if (buf.length < NAME_RECORD_BYTES) return -1;
  for (const r of nameRecords(buf)) {
    if (r.name === name) return r.segno;
  }
  return -1;
}

function trimType(t: string): string {
  return t.replace(/['\s]+$/, '');
}

/** "USER/NAME:TYPE" of the first object owned by SYSTEM with this name and type (case-insensitive), or null. */
export function findSystemFile(fs: NdfsFileSystem, name: string, wantType: string): string | null {
  const objs = fs.getObjectEntries();
  for (let i = 0; i < objs.length; i++) {
    const obj = objs[i];
    if (obj.userName.toUpperCase() !== 'SYSTEM') continue;
    if (obj.objectName.toUpperCase() !== name.toUpperCase()) continue;
    const t = trimType(obj.type ?? '');
    if (t.toUpperCase() !== wantType.toUpperCase()) continue;
    return `${obj.userName}/${obj.objectName}:${t}`;
  }
  return null;
}

/** SEGFIL0:DATA, or the older name SEGFIL:DATA. */
function findSegfil(fs: NdfsFileSystem): string | null {
  return findSystemFile(fs, 'SEGFIL0', 'DATA') ?? findSystemFile(fs, 'SEGFIL', 'DATA');
}

// -- Symbol lists ------------------------------------------------------------

function readSymlist(fs: NdfsFileSystem, path: string): { text: Uint8Array; hasParity: boolean } {
  const raw = fs.readFile(path);
  let hasParity = false;
  for (let i = 0; i < raw.length; i++) {
    if (raw[i] & 0x80) { hasParity = true; break; }
  }
  return { text: stripParity(raw), hasParity };
}

function symOrMinusOne(a: Uint8Array, b: Uint8Array, name: string): number {
  const hit = findSymbol(a, name) ?? findSymbol(b, name);
  return hit === null ? -1 : hit.value;
}

// -- Segment names and table -------------------------------------------------

function segEntry(seg: Uint8Array, page: number, n: number): { logad: number; segle: number; madr: number; flag: number } {
  const o = page * PAGE_BYTES + n * SEG_ENTRY_BYTES;
  return { logad: wordBE(seg, o + 4), segle: wordBE(seg, o + 6) & 0x3ff, madr: wordBE(seg, o + 8), flag: wordBE(seg, o + 10) };
}

/** Both DPIT names to segment numbers: RTFIL:DATA first (small), then the copies of the loader table inside SEGFIL0. */
function resolveSegmentNames(fs: NdfsFileSystem, seg: Uint8Array, out: CpuIdentity): void {
  for (const c of out.copies) c.segmentNumber = -1;
  out.namesPath = '';

  const path = findSystemFile(fs, 'RTFIL', 'DATA');
  if (path !== null) {
    const rt = fs.readFile(path);
    for (const c of out.copies) c.segmentNumber = findSegmentByName(rt, c.name);
    if (out.copies.every((c) => c.segmentNumber > 0)) {
      out.namesPath = path;
      return;
    }
  }

  // Fall back to the S3RTFIL segment pages inside SEGFIL0.
  for (const c of out.copies) {
    if (c.segmentNumber <= 0) c.segmentNumber = findSegmentByName(seg, c.name);
  }
  if (out.copies.every((c) => c.segmentNumber > 0)) out.namesPath = 'SEGFIL0 (loader table copy)';
}

/**
 * The file a segment file number names. 0 is SEGFIL0; on K the save copy sits on
 * segment file 4, which the K release information places in SINTRAN:DATA. Other
 * numbers are reported and never written.
 */
function segmentFilePath(fs: NdfsFileSystem, segmentFile: number, segfilPath: string): string | null {
  if (segmentFile === 0) return segfilPath;
  if (segmentFile === 4) return findSystemFile(fs, 'SINTRAN', 'DATA');
  return null;
}

/**
 * A word offset is usable only when the file really holds it; otherwise -1. The K
 * pack's SINTRAN:DATA is 60 pages while S3SDPIT is listed as 35 pages from page 27,
 * so the segment's tail is past the file end but every identity word is inside it.
 * The check is therefore per word, not per segment.
 */
function offsetInFile(offset: number, fileSize: number): number {
  if (offset < 0 || offset + 2 > fileSize) return -1;
  return offset;
}

/** Read the four identity words of one copy out of its file. */
function readCopyWords(fs: NdfsFileSystem, sym: SintranSymbols, c: DpitCopy): void {
  c.sysno = c.fcpun = c.cpuType = c.revle = -1;
  c.sysnoOffset = segmentWordOffset(c.logad, c.segle, c.madr, sym.sysno);
  c.fcpunOffset = segmentWordOffset(c.logad, c.segle, c.madr, sym.fcpun);
  c.typeOffset = sym.hwinf >= 0 ? segmentWordOffset(c.logad, c.segle, c.madr, sym.hwinf + 2) : -1;
  c.revleOffset = sym.revle >= 0 ? segmentWordOffset(c.logad, c.segle, c.madr, sym.revle) : -1;
  c.fileFound = false;
  if (c.filePath === '') return;
  let data: Uint8Array;
  try {
    data = fs.readFile(c.filePath);
  } catch {
    return;
  }
  const size = data.length;
  // The file must at least reach the copy's first page; each word is then checked
  // against the file length on its own.
  if (size > c.madr * PAGE_BYTES) {
    c.fileFound = true;
    c.sysnoOffset = offsetInFile(c.sysnoOffset, size);
    c.fcpunOffset = offsetInFile(c.fcpunOffset, size);
    c.typeOffset = offsetInFile(c.typeOffset, size);
    c.revleOffset = offsetInFile(c.revleOffset, size);
    if (c.sysnoOffset >= 0) c.sysno = wordBE(data, c.sysnoOffset);
    if (c.fcpunOffset >= 0) c.fcpun = wordBE(data, c.fcpunOffset);
    if (c.typeOffset >= 0) c.cpuType = wordBE(data, c.typeOffset);
    if (c.revleOffset >= 0) c.revle = wordBE(data, c.revleOffset);
  }
}

// -- Read --------------------------------------------------------------------

/**
 * Read the CPU number and type from both DPIT copies, the generation symbols and
 * the directory stamp. Throws CpuIdentityError (with the partial result in
 * `.identity`) when the pack has no symbol lists, no SYSNO/FCPUN symbol, no
 * segment table or no name records.
 */
export function readCpuIdentity(fs: NdfsFileSystem): CpuIdentity {
  const out = newIdentity();

  // Directory stamp first: it needs no SINTRAN at all.
  const mb = fs.getMasterBlock();
  out.dirExtValid = mb.extValid;
  out.dirEntered = (mb.extFlagWord & MasterBlock.EXT_FLAG_ENTERED) !== 0;
  out.dirSystemNumber = mb.extLastSystemNumber;

  // The pack's own symbol lists.
  const p1 = findSystemFile(fs, 'SYMBOL-1-LIST', 'SYMB');
  const p2 = findSystemFile(fs, 'SYMBOL-2-LIST', 'SYMB');
  if (p1 === null || p2 === null) {
    throw new CpuIdentityError('not a SINTRAN system pack: no SYMBOL-1-LIST / SYMBOL-2-LIST under SYSTEM', out);
  }
  out.symbol1Path = p1;
  out.symbol2Path = p2;
  const s1 = readSymlist(fs, p1).text;
  const l2 = readSymlist(fs, p2);
  const s2 = l2.text;
  out.symbol2HasParity = l2.hasParity;

  out.sym.sysno = symOrMinusOne(s1, s2, 'SYSNO');
  out.sym.hwinf = symOrMinusOne(s1, s2, 'HWINF');
  out.sym.revle = symOrMinusOne(s1, s2, 'REVLE');
  out.sym.fcpun = symOrMinusOne(s2, s1, 'FCPUN'); // K keeps it in list 2
  out.sym.inibu = symOrMinusOne(s1, s2, 'INIBU');
  let hit = findSymbol(s2, '33CPN');
  if (hit !== null) {
    out.sym.cpn33 = hit.value;
    out.cpn33ValueOffset = hit.offset;
    out.cpn33ValueWidth = hit.width;
  }
  hit = findSymbol(s2, '33CPU');
  if (hit !== null) {
    out.sym.cpu33 = hit.value;
    out.cpu33ValueOffset = hit.offset;
    out.cpu33ValueWidth = hit.width;
  }

  out.version = INIBU_TO_VERSION[out.sym.inibu] ?? '?';

  // Without SYSNO and FCPUN nothing can be addressed safely.
  if (out.sym.sysno < 0 || out.sym.fcpun < 0) {
    throw new CpuIdentityError('SYSNO or FCPUN missing from the symbol lists', out);
  }

  // The segment table inside SEGFIL0.
  const segfilPath = findSegfil(fs);
  if (segfilPath === null) throw new CpuIdentityError('no SEGFIL0:DATA', out);
  const seg = fs.readFile(segfilPath);
  out.segmentTablePage = findSegmentTablePage(seg);
  if (out.segmentTablePage < 0) throw new CpuIdentityError('no segment table found in SEGFIL0', out);

  // The segment numbers, by name, from the pack's RT-loader records.
  resolveSegmentNames(fs, seg, out);
  if (out.copies.some((c) => c.segmentNumber <= 0)) {
    throw new CpuIdentityError('DEFINED SEGMENT NAME records for S3IDPIT / S3SDPIT not found', out);
  }

  for (const c of out.copies) {
    const e = segEntry(seg, out.segmentTablePage, c.segmentNumber);
    c.logad = e.logad;
    c.segle = e.segle;
    c.madr = e.madr;
    c.flag = e.flag;
    c.segmentFile = (e.flag >> 13) & 7;
    c.filePath = '';
    // Usable: a length, a disk address (the 5OK bit) and a range that covers SYSNO.
    // The logical base comes from the entry itself, not from a constant.
    c.present = e.segle > 0 && (e.flag & 1) !== 0 && segmentWordOffset(e.logad, e.segle, e.madr, out.sym.sysno) >= 0;
    if (!c.present) continue;
    const path = segmentFilePath(fs, c.segmentFile, segfilPath);
    if (path === null) continue;
    c.filePath = path;
    readCopyWords(fs, out.sym, c);
  }
  return out;
}

// -- Write -------------------------------------------------------------------

function patchWord(fs: NdfsFileSystem, path: string, offset: number, value: number): void {
  if (offset < 0) throw new CpuIdentityError(`${path}: word not reachable`);
  fs.patchFileRegion(path, offset, Uint8Array.from([(value >> 8) & 0xff, value & 0xff]));
}

/** Rewrite the octal digits of a `NAME=OOOOOO` symbol in place, same width, with the list's own parity convention. */
function patchSymbolValue(fs: NdfsFileSystem, path: string, offset: number, width: number, parity: boolean, value: number): void {
  if (offset < 0 || width <= 0 || width > 15) throw new CpuIdentityError(`${path}: symbol value not reachable`);
  // The value must fit the digits that are there; 6 digits hold any 16-bit word.
  if (width < 6 && value >= 1 << (3 * width)) throw new RangeError(`${value} does not fit ${width} octal digits`);
  const digits = new Uint8Array(width);
  let v = value;
  for (let i = width - 1; i >= 0; i--) {
    digits[i] = 0x30 + (v & 7);
    v >>= 3;
  }
  fs.patchFileRegion(path, offset, parity ? setParity(digits) : digits);
}

/**
 * Set the CPU number (SYSNO and FCPUN) in both DPIT copies, the CPU type (HWINF+2)
 * when given, rewrite 33CPN / 33CPU in SYMBOL-2-LIST so a later ND patch run keeps
 * the values, clear the directory's "entered by system N" stamp unless `keepStamp`,
 * then read everything back and compare.
 *
 * Nothing is written unless every target word is reachable. Throws RangeError for a
 * value outside 0..65535, CpuIdentityError when a target is missing, and Error when
 * the read-back does not match what was written.
 */
export function writeCpuIdentity(fs: NdfsFileSystem, cpuNumber: number, cpuType: number | null = null, keepStamp = false): CpuIdentity {
  if (!Number.isInteger(cpuNumber) || cpuNumber < 0 || cpuNumber > 0xffff) throw new RangeError('cpuNumber must be 0..65535');
  if (cpuType !== null && (!Number.isInteger(cpuType) || cpuType < 0 || cpuType > 0xffff)) throw new RangeError('cpuType must be 0..65535');

  const ident = readCpuIdentity(fs);

  // Everything must be addressable before the first byte is written.
  for (const c of ident.copies) {
    if (!c.present || !c.fileFound) throw new CpuIdentityError(`${c.name} (${c.role} copy) has no reachable disk copy`, ident);
    if (c.sysnoOffset < 0 || c.fcpunOffset < 0) throw new CpuIdentityError(`${c.name}: SYSNO or FCPUN is outside ${c.filePath}`, ident);
    if (cpuType !== null && c.typeOffset < 0) throw new CpuIdentityError(`${c.name}: HWINF+2 is outside ${c.filePath}`, ident);
  }

  for (const c of ident.copies) {
    patchWord(fs, c.filePath, c.sysnoOffset, cpuNumber);
    patchWord(fs, c.filePath, c.fcpunOffset, cpuNumber);
    if (cpuType !== null) patchWord(fs, c.filePath, c.typeOffset, cpuType);
  }

  // The generation symbols, so a later ND patch run keeps the new values.
  if (ident.cpn33ValueOffset >= 0) {
    patchSymbolValue(fs, ident.symbol2Path, ident.cpn33ValueOffset, ident.cpn33ValueWidth, ident.symbol2HasParity, cpuNumber);
  }
  if (cpuType !== null && ident.cpu33ValueOffset >= 0) {
    patchSymbolValue(fs, ident.symbol2Path, ident.cpu33ValueOffset, ident.cpu33ValueWidth, ident.symbol2HasParity, cpuType);
  }

  if (!keepStamp && (ident.dirEntered || ident.dirSystemNumber !== 0)) fs.releaseDirectory();

  // Read back and compare.
  const after = readCpuIdentity(fs);
  for (const c of after.copies) {
    if (c.sysno !== cpuNumber || c.fcpun !== cpuNumber) throw new Error(`read-back of ${c.name} does not show CPU number ${cpuNumber}`);
    if (cpuType !== null && c.cpuType !== cpuType) throw new Error(`read-back of ${c.name} does not show CPU type ${cpuType}`);
  }
  if (after.cpn33ValueOffset >= 0 && after.sym.cpn33 !== cpuNumber) throw new Error('read-back of 33CPN does not show the new CPU number');
  if (cpuType !== null && after.cpu33ValueOffset >= 0 && after.sym.cpu33 !== cpuType) throw new Error('read-back of 33CPU does not show the new CPU type');
  if (!keepStamp && (after.dirEntered || after.dirSystemNumber !== 0)) throw new Error('the directory stamp is still set after the release');
  return after;
}
