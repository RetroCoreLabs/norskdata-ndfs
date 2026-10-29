/**
 * Tests for the SINTRAN CPU number / CPU type reader and writer (sintran-identity.ts).
 *
 * Mirrors ndfs-c/tests/test_sintran_identity.c case for case. A synthetic system pack
 * is built with the image creator: both symbol lists (with even parity, as the real
 * packs store them), an RTFIL:DATA with the RT-loader "DEFINED SEGMENT NAME" records,
 * a SEGFIL0:DATA holding a self-describing segment table and the two DPIT copies, and
 * for the K shape a SINTRAN:DATA that holds the save copy (segment file 4). The
 * segment numbers in the fixture are deliberately NOT the real 44B/53B in one case, so
 * a test proves the lookup goes by name. Shapes and addresses follow the real packs
 * measured on 2026-10-29 (NDInsight chapter SINTRAN/OS/28, sections 4 to 6; chapter
 * 22 section 6.5 for the records).
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */
import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ImageTemplate } from '../src/types.js';
import { setParity, stripParity } from '../src/parity.js';
import * as si from '../src/sintran-identity.js';

const PAGE = 2048;

// Fixture shape: logical base 4000B (LOGAD low bits = page 2, as on every real pack),
// 35 pages per copy as on the K pack so the K FCPUN at 105034B (page 32 of the copy)
// is inside. SEGFIL0 is 80 pages: table at page 5, image copy at page 20.
// L shape: image copy at SEGFIL0 page 20, save at page 44 (fits 44+35 = 79).
// K shape: save copy at page 10 of SINTRAN:DATA (segment file 4).
export const TABLE_PAGE = 5;
const IMAGE_MADR = 20;
const SAVE_MADR_L = 44;
const SAVE_MADR_K = 10;
const SEGLE = 35;
const LOGAD = 0o000102;
const SEGFIL_PAGES = 80;

const SYSNO = 0o4051;
const HWINF = 0o4052;
const REVLE = 0o4057;
const FCPUN_L = 0o4414;
const FCPUN_K = 0o105034;

interface Shape {
  kShape: boolean; // save copy on segment file 4 in SINTRAN:DATA
  withFcpunSymbol: boolean; // false: SYMBOL-2-LIST lacks FCPUN
  imageSegno: number; // segment numbers the name records give
  saveSegno: number;
  sintranDataPages: number; // K only; too short = refuse
  withNameRecords: boolean; // false: no RTFIL:DATA and no records in SEGFIL0
}

const lShape = (): Shape => ({ kShape: false, withFcpunSymbol: true, imageSegno: 0o44, saveSegno: 0o53, sintranDataPages: 0, withNameRecords: true });
const kShape = (): Shape => ({ kShape: true, withFcpunSymbol: true, imageSegno: 0o44, saveSegno: 0o53, sintranDataPages: 48, withNameRecords: true });

export function putWord(b: Uint8Array, off: number, val: number): void {
  b[off] = (val >> 8) & 0xff;
  b[off + 1] = val & 0xff;
}

export function putSegEntry(b: Uint8Array, page: number, n: number, logad: number, segle: number, madr: number, flag: number, sgsta: number): void {
  const o = page * PAGE + n * 16;
  putWord(b, o + 4, logad);
  putWord(b, o + 6, segle);
  putWord(b, o + 8, madr);
  putWord(b, o + 10, flag);
  putWord(b, o + 12, sgsta);
}

/** Pack a name the way the RT-loader does: SIXBIT, right-justified base 64. */
export function putNameRecord(b: Uint8Array, off: number, name: string, segno: number): void {
  let n = 0n;
  for (let i = 0; i < name.length; i++) n = (n << 6n) | BigInt(name.charCodeAt(i) & 0x3f);
  putWord(b, off + 0, Number((n >> 32n) & 0xffffn));
  putWord(b, off + 2, Number((n >> 16n) & 0xffffn));
  putWord(b, off + 4, Number(n & 0xffffn));
  putWord(b, off + 6, segno);
  putWord(b, off + 8, 0);
  putWord(b, off + 10, 0o25);
  putWord(b, off + 12, segno);
  putWord(b, off + 14, 0);
}

/** Write one identity word set into a DPIT copy that starts at byte `base`. */
function putDpitWords(buf: Uint8Array, base: number, fcpunAddr: number, sysno: number, cpuType: number, revle: number, fcpun: number): void {
  putWord(buf, base + (SYSNO - 0o4000) * 2, sysno);
  putWord(buf, base + (HWINF + 2 - 0o4000) * 2, cpuType);
  putWord(buf, base + (REVLE - 0o4000) * 2, revle);
  putWord(buf, base + (fcpunAddr - 0o4000) * 2, fcpun);
}

export function writeTextFile(fs: NdfsFileSystem, path: string, text: string, parity = true): void {
  const data = new TextEncoder().encode(text);
  fs.writeFile(path, parity ? setParity(data) : data);
}

const SYM1_K = 'SYSNO=004051\r\nHWINF=004052\r\nREVLE=004057\r\nINIBU=067172\r\n';
const SYM2_K = 'FCPUN=105034\r\n33CPU=000156\r\n33CPN=000144\r\nSYSNO=004051\r\n';
const SYM2_K_NOFCPUN = '33CPU=000156\r\n33CPN=000144\r\nSYSNO=004051\r\n';
const SYM1_L = 'FCPUN=004414\r\nREVLE=004057\r\nSYSNO=004051\r\nHWINF=004052\r\nINIBU=074123\r\n';
const SYM2_L = 'GCPUN=061434\r\n33CPU=000000\r\n33CPN=000000\r\n';

function buildPack(sh: Shape): NdfsFileSystem {
  const fs = NdfsFileSystem.createImage({
    template: ImageTemplate.Smd75MB,
    directoryName: 'PACK-ONE',
    includeExtendedInfo: true,
    systemNumber: 100,
    flagWord: 0x8000, // "entered by system 100", as every real pack here
  });
  const fcpunAddr = sh.kShape ? FCPUN_K : FCPUN_L;

  // Segment table: entry 2 self-describing, filler so the page has >= 40 good entries,
  // then the two DPIT copies at the numbers the name records will give.
  const seg = new Uint8Array(SEGFIL_PAGES * PAGE);
  putSegEntry(seg, TABLE_PAGE, 2, 0, 1, TABLE_PAGE, 1, 0xe000);
  for (let n = 3; n <= 60; n++) {
    if (n !== sh.imageSegno && n !== sh.saveSegno) putSegEntry(seg, TABLE_PAGE, n, 0, 1, 1, 1, 0xe000);
  }
  putSegEntry(seg, TABLE_PAGE, sh.imageSegno, LOGAD, SEGLE, IMAGE_MADR, 0o000111, 0o162000);
  putSegEntry(seg, TABLE_PAGE, sh.saveSegno, LOGAD, SEGLE, sh.kShape ? SAVE_MADR_K : SAVE_MADR_L, sh.kShape ? 0o100111 : 0o000111, 0o162000);

  putDpitWords(seg, IMAGE_MADR * PAGE, fcpunAddr, 21056, 9883, 0o10200, 21056);
  if (!sh.kShape) putDpitWords(seg, SAVE_MADR_L * PAGE, fcpunAddr, 21056, 9883, 0o10200, 21056);
  fs.writeFile('SYSTEM/SEGFIL0:DATA', seg);

  if (sh.kShape) {
    const sd = new Uint8Array(sh.sintranDataPages * PAGE);
    if (sd.length >= (SAVE_MADR_K + SEGLE) * PAGE) putDpitWords(sd, SAVE_MADR_K * PAGE, fcpunAddr, 21056, 9883, 0o10200, 21056);
    fs.writeFile('SYSTEM/SINTRAN:DATA', sd);
  }

  if (sh.withNameRecords) {
    // RTFIL:DATA as on the real packs: records at page 0 and 1, other names around.
    const rtfil = new Uint8Array(PAGE * 2);
    putNameRecord(rtfil, 0 * 16, 'S3IMAGE', 0o02);
    putNameRecord(rtfil, 1 * 16, 'S3RTFIL', 0o10);
    putNameRecord(rtfil, 2 * 16, 'S3DPIT', 0o23);
    putNameRecord(rtfil, 3 * 16, 'S3ISGT', 0o27);
    putNameRecord(rtfil, PAGE + 0 * 16, 'S3PATCH', 0o43);
    putNameRecord(rtfil, PAGE + 1 * 16, 'S3IDPIT', sh.imageSegno);
    putNameRecord(rtfil, PAGE + 2 * 16, 'S3SDPIT', sh.saveSegno);
    fs.writeFile('SYSTEM/RTFIL:DATA', rtfil);
  }

  writeTextFile(fs, 'SYSTEM/SYMBOL-1-LIST:SYMB', sh.kShape ? SYM1_K : SYM1_L);
  writeTextFile(fs, 'SYSTEM/SYMBOL-2-LIST:SYMB', sh.kShape ? (sh.withFcpunSymbol ? SYM2_K : SYM2_K_NOFCPUN) : SYM2_L);
  return fs;
}

const text = (s: string): Uint8Array => new TextEncoder().encode(s);

function indexOf(hay: Uint8Array, needle: Uint8Array): number {
  outer: for (let i = 0; i + needle.length <= hay.length; i++) {
    for (let k = 0; k < needle.length; k++) if (hay[i + k] !== needle[k]) continue outer;
    return i;
  }
  return -1;
}

describe('SINTRAN CPU identity: pure helpers', () => {
  it('find_symbol', () => {
    const txt = text('SYSNO=004051\r\n   FCPUN=004414\r\n33CPN=000000\r\nX=7\n');
    expect(si.findSymbol(txt, 'SYSNO')).toEqual({ value: 0o4051, offset: 6, width: 6 });
    expect(si.findSymbol(txt, 'FCPUN')!.value).toBe(0o4414);
    expect(si.findSymbol(txt, '33CPN')!.value).toBe(0);
    // A name that is only the tail of another ("CPN" inside "33CPN") must not match.
    expect(si.findSymbol(txt, 'CPN')).toBeNull();
    expect(si.findSymbol(txt, 'HWINF')).toBeNull();
  });

  it('decode_packed_name', () => {
    // The two worked examples of NDInsight chapter 22 section 6.5.
    expect(si.decodePackedName(0x0004, 0xf325, 0x31d4)).toBe('S3ISGT');
    expect(si.decodePackedName(0x013c, 0xd250, 0x10c3)).toBe('S3RTACC');
    expect(si.decodePackedName(0, 0, 0)).toBeNull();
    // A code outside A-Z / 0-9 (0x3F) is not a name.
    expect(si.decodePackedName(0, 0, 0x003f)).toBeNull();
  });

  it('find_segment_by_name', () => {
    const buf = new Uint8Array(256);
    putNameRecord(buf, 16, 'S3IDPIT', 0o44);
    putNameRecord(buf, 48, 'S3SDPIT', 0o53);
    // A record with the wrong type must be ignored even with the right name.
    putNameRecord(buf, 80, 'S3DPIT', 0o23);
    putWord(buf, 80 + 10, 0o24);
    expect(si.findSegmentByName(buf, 'S3IDPIT')).toBe(0o44);
    expect(si.findSegmentByName(buf, 'S3SDPIT')).toBe(0o53);
    expect(si.findSegmentByName(buf, 'S3DPIT')).toBe(-1);
    expect(si.findSegmentByName(buf, 'S3FS')).toBe(-1);
  });

  it('segment_word_offset', () => {
    // L pack, S3SDPIT: MADR 64, SEGLE 45, LOGAD 000102 -> SYSNO at 64*2048 + 41*2 = 131154.
    expect(si.segmentWordOffset(0o000102, 45, 64, 0o4051)).toBe(131154);
    expect(si.segmentWordOffset(0o000102, 45, 64, 0o4054)).toBe(131160);
    // Below the base or past the end: -1.
    expect(si.segmentWordOffset(0o000102, 45, 64, 0o3777)).toBe(-1);
    expect(si.segmentWordOffset(0o000102, 45, 64, 0o4000 + 45 * 1024)).toBe(-1);
    // A segment based elsewhere does not contain 4051B.
    expect(si.segmentWordOffset(0o000114, 45, 64, 0o4051)).toBe(-1);
  });
});

describe('SINTRAN CPU identity: read', () => {
  it('read_l_shape', () => {
    const fs = buildPack(lShape());
    const id = si.readCpuIdentity(fs);
    expect(id.version).toBe('L');
    expect(id.sym.sysno).toBe(0o4051);
    expect(id.sym.fcpun).toBe(0o4414);
    expect(id.sym.cpn33).toBe(0);
    expect(id.segmentTablePage).toBe(TABLE_PAGE);
    expect(id.namesPath).toBe('SYSTEM/RTFIL:DATA');
    expect(id.copies[0].segmentNumber).toBe(0o44);
    expect(id.copies[1].segmentNumber).toBe(0o53);
    expect(id.copies[0].present && id.copies[0].fileFound).toBe(true);
    expect(id.copies[1].present && id.copies[1].fileFound).toBe(true);
    expect(id.copies[1].segmentFile).toBe(0);
    expect(id.copies[1].filePath).toBe('SYSTEM/SEGFIL0:DATA');
    expect(id.copies[0].sysno).toBe(21056);
    expect(id.copies[0].fcpun).toBe(21056);
    expect(id.copies[0].cpuType).toBe(9883);
    expect(id.copies[0].revle).toBe(0o10200);
    expect(id.copies[1].sysno).toBe(21056);
    expect(id.dirExtValid).toBe(true);
    expect(id.dirEntered).toBe(true);
    expect(id.dirSystemNumber).toBe(100);
  });

  it('read_k_shape_save_copy_in_sintran_data', () => {
    const fs = buildPack(kShape());
    const id = si.readCpuIdentity(fs);
    expect(id.version).toBe('K');
    expect(id.sym.fcpun).toBe(0o105034);
    expect(id.sym.cpn33).toBe(0o144);
    expect(id.sym.cpu33).toBe(0o156);
    expect(id.copies[1].segmentFile).toBe(4);
    expect(id.copies[1].filePath).toBe('SYSTEM/SINTRAN:DATA');
    expect(id.copies[1].fileFound).toBe(true);
    expect(id.copies[1].sysno).toBe(21056);
    expect(id.copies[1].fcpun).toBe(21056);
    expect(id.copies[1].cpuType).toBe(9883);
    expect(id.copies[0].fcpun).toBe(21056);
  });

  it('read_segments_found_by_name_not_number', () => {
    const sh = lShape();
    sh.imageSegno = 0o45; // not the real 44B/53B: only the name records say where
    sh.saveSegno = 0o54;
    const fs = buildPack(sh);
    const id = si.readCpuIdentity(fs);
    expect(id.copies[0].segmentNumber).toBe(0o45);
    expect(id.copies[1].segmentNumber).toBe(0o54);
    expect(id.copies[0].sysno).toBe(21056);
    expect(id.copies[1].sysno).toBe(21056);
  });

  it('read_names_from_segfil_when_rtfil_missing', () => {
    const sh = lShape();
    sh.withNameRecords = false;
    const fs = buildPack(sh);
    // No RTFIL:DATA and no records anywhere: refuse.
    let partial: si.CpuIdentity | null = null;
    try {
      si.readCpuIdentity(fs);
    } catch (e) {
      expect(e).toBeInstanceOf(si.CpuIdentityError);
      partial = (e as si.CpuIdentityError).identity;
    }
    expect(partial).not.toBeNull();
    expect(partial!.copies[0].segmentNumber).toBe(-1);
    // Put the loader table copy inside SEGFIL0 (page 60, as the S3RTFIL segment does).
    const rec = new Uint8Array(2 * 16);
    putNameRecord(rec, 0, 'S3IDPIT', 0o44);
    putNameRecord(rec, 16, 'S3SDPIT', 0o53);
    fs.patchFileRegion('SYSTEM/SEGFIL0:DATA', 60 * PAGE, rec);
    const id = si.readCpuIdentity(fs);
    expect(id.copies[0].segmentNumber).toBe(0o44);
    expect(id.copies[1].sysno).toBe(21056);
    expect(id.namesPath).toBe('SEGFIL0 (loader table copy)');
  });

  it('read_refuses_without_fcpun_symbol', () => {
    const sh = kShape();
    sh.withFcpunSymbol = false;
    const fs = buildPack(sh);
    let err: si.CpuIdentityError | null = null;
    try {
      si.readCpuIdentity(fs);
    } catch (e) {
      err = e as si.CpuIdentityError;
    }
    expect(err).toBeInstanceOf(si.CpuIdentityError);
    expect(err!.identity!.sym.sysno).toBe(0o4051); // what was found is still reported
    expect(err!.identity!.sym.fcpun).toBe(-1);
  });

  it('read_not_a_system_pack', () => {
    const fs = NdfsFileSystem.createImage({ template: ImageTemplate.Floppy12MB, directoryName: 'FLOPPY' });
    expect(() => si.readCpuIdentity(fs)).toThrow(si.CpuIdentityError);
  });
});

describe('SINTRAN CPU identity: write', () => {
  it('write_l_shape_both_copies_symbols_and_stamp', () => {
    const fs = buildPack(lShape());
    const after = si.writeCpuIdentity(fs, 21238, 9883, false);
    expect(after.copies[0].sysno).toBe(21238);
    expect(after.copies[0].fcpun).toBe(21238);
    expect(after.copies[1].sysno).toBe(21238);
    expect(after.copies[1].fcpun).toBe(21238);
    expect(after.copies[1].cpuType).toBe(9883);
    expect(after.copies[1].revle).toBe(0o10200); // untouched
    expect(after.sym.cpn33).toBe(21238);
    expect(after.sym.cpu33).toBe(9883);
    expect(after.dirEntered).toBe(false);
    expect(after.dirSystemNumber).toBe(0);
    expect(after.dirExtValid).toBe(true); // checksum rewritten

    // The symbol list kept its line shape, "33CPN=051366", and the new digits carry
    // even parity like the rest of the file ('1' = 0x31 has three 1-bits, so 0xB1).
    const sym2 = fs.readFile('SYSTEM/SYMBOL-2-LIST:SYMB');
    const plain = stripParity(sym2);
    const at = indexOf(plain, text('33CPN=051366'));
    expect(at).toBeGreaterThanOrEqual(0);
    expect(Array.from(sym2.subarray(at + 6, at + 12))).toEqual(Array.from(setParity(text('051366'))));
    expect(indexOf(plain, text('33CPU=023233\r\n'))).toBeGreaterThanOrEqual(0);

    // The reopened image agrees: the stamp really went to disk with a valid checksum.
    const reopened = new NdfsFileSystem(fs.toBuffer(), true);
    const mb = reopened.getMasterBlock();
    expect(mb.extValid).toBe(true);
    expect(mb.isDirectoryEntered).toBe(false);
    expect(mb.extLastSystemNumber).toBe(0);
  });

  it('write_type_optional_and_keep_stamp', () => {
    const fs = buildPack(lShape());
    const after = si.writeCpuIdentity(fs, 7, null, true);
    expect(after.copies[0].sysno).toBe(7);
    expect(after.copies[0].cpuType).toBe(9883); // kept
    expect(after.sym.cpu33).toBe(0); // kept
    expect(after.dirEntered).toBe(true); // kept
    expect(after.dirSystemNumber).toBe(100);
  });

  it('write_k_shape_writes_sintran_data', () => {
    const fs = buildPack(kShape());
    const after = si.writeCpuIdentity(fs, 21238, null, false);
    expect(after.copies[0].sysno).toBe(21238);
    expect(after.copies[0].fcpun).toBe(21238);
    expect(after.copies[1].sysno).toBe(21238);
    expect(after.copies[1].fcpun).toBe(21238);
    expect(after.copies[1].cpuType).toBe(9883);
    expect(after.sym.cpn33).toBe(21238);
    expect(after.sym.cpu33).toBe(0o156); // type not given: kept
    // The save copy really went into SINTRAN:DATA page 10.
    const sd = fs.readFile('SYSTEM/SINTRAN:DATA');
    const o = SAVE_MADR_K * PAGE + (SYSNO - 0o4000) * 2;
    expect((sd[o] << 8) | sd[o + 1]).toBe(21238);
  });

  it('write_refuses_when_save_file_too_short', () => {
    const sh = kShape();
    sh.sintranDataPages = 16; // SYSNO (page 10) is inside, FCPUN (page 10 + 32) is not
    const fs = buildPack(sh);
    let id = si.readCpuIdentity(fs);
    expect(id.copies[1].present).toBe(true);
    expect(id.copies[1].fileFound).toBe(true);
    expect(id.copies[1].sysnoOffset).toBeGreaterThanOrEqual(0);
    expect(id.copies[1].fcpunOffset).toBe(-1);
    expect(id.copies[1].fcpun).toBe(-1);
    expect(() => si.writeCpuIdentity(fs, 21238, null, false)).toThrow(si.CpuIdentityError);
    id = si.readCpuIdentity(fs);
    expect(id.copies[0].sysno).toBe(21056); // nothing changed
    expect(id.dirEntered).toBe(true);
  });

  it('write_rejects_bad_values', () => {
    const fs = buildPack(lShape());
    expect(() => si.writeCpuIdentity(fs, -1)).toThrow(RangeError);
    expect(() => si.writeCpuIdentity(fs, 65536)).toThrow(RangeError);
    expect(() => si.writeCpuIdentity(fs, 1, 65536)).toThrow(RangeError);
    // Nothing was written.
    expect(si.readCpuIdentity(fs).copies[0].sysno).toBe(21056);
  });
});
