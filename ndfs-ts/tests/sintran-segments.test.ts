/**
 * Tests for the SINTRAN segment lister / exporter / importer (sintran-segments.ts).
 *
 * Mirrors ndfs-c/tests/test_sintran_segments.c. A synthetic pack is built with the
 * image creator: a contiguous SEGFIL0:DATA with a self-describing segment table, a DPIT
 * image copy whose kernel tables DBLST / ABLPAGE / MASSNO point at the real disk pages
 * of SEGFIL0 and of a second contiguous segment file SEGFIL1:DATA, an RTFIL:DATA with
 * the name records, and a symbol list with BLST / ABLPA / MASSN / INIBU. Shapes follow
 * the K and L packs measured on 2026-10-29.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */
import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ImageTemplate } from '../src/types.js';
import { CpuIdentityError } from '../src/sintran-identity.js';
import * as ss from '../src/sintran-segments.js';
import { putNameRecord, putSegEntry, putWord, writeTextFile } from './sintran-identity.test.js';

const PAGE = 2048;
const TABLE_PAGE = 5;
const SEGFIL0_PAGES = 64;
const SEGFIL1_PAGES = 16;

// The L07 symbol values.
const BLST_SYM = 0o4023;
const ABLPA_SYM = 0o4461;
const MASSN_SYM = 0o4441;

const SEG_IDPIT = 0o44;
const SEG_SDPIT = 0o53;
const SEG_FS = 0o06; // a code segment on segment file 0
const SEG_USER = 0o101; // a user segment on segment file 1
const SEG_MEMORY = 0o23; // S3DPIT, memory only
const SEG_SAVE4 = 0o54; // a save segment on segment file 4 (absolute)
const SEG_IMAGE = 0o02; // S3IMAGE: file 0, MADR 0, real pages

const IDPIT_MADR = 20, SDPIT_MADR = 40, FS_MADR = 10, USER_MADR = 2, SAVE4_MADR = 30;
const DPIT_LOGAD = 0o000102; // base page 2 = 4000B

function contiguous(blocks: number[]): boolean {
  for (let i = 1; i < blocks.length; i++) if (blocks[i] !== blocks[i - 1] + 1) return false;
  return true;
}

/** Build the pack; returns the fs and where the two segment files landed. */
function buildPack(withSymbols: boolean): { fs: NdfsFileSystem; sf0: number; sf1: number } {
  const fs = NdfsFileSystem.createImage({
    template: ImageTemplate.Smd75MB, directoryName: 'PACK-ONE', includeExtendedInfo: true, systemNumber: 100, flagWord: 0x8000,
  });

  // Both segment files contiguous, as SINTRAN requires (@ALLOCATE-FILE).
  fs.createContiguousFile('SYSTEM/SEGFIL0:DATA', SEGFIL0_PAGES);
  fs.createContiguousFile('SYSTEM/SEGFIL1:DATA', SEGFIL1_PAGES);
  const blocks0 = fs.getFileBlocks('SYSTEM/SEGFIL0:DATA');
  expect(contiguous(blocks0)).toBe(true);
  const sf0 = blocks0[0];
  const sf1 = fs.getFileBlocks('SYSTEM/SEGFIL1:DATA')[0];

  const seg = new Uint8Array(SEGFIL0_PAGES * PAGE);
  const seg1 = new Uint8Array(SEGFIL1_PAGES * PAGE);

  // Segment table: self-describing entry 027 (S3ISGT on the real packs), filler,
  // then the real entries; entry 2 is S3IMAGE.
  putSegEntry(seg, TABLE_PAGE, 0o27, 0, 1, TABLE_PAGE, 1, 0xe000);
  const special = new Set([SEG_IDPIT, SEG_SDPIT, SEG_FS, SEG_MEMORY, SEG_SAVE4, SEG_IMAGE, 0o27]);
  for (let n = 3; n <= 60; n++) if (!special.has(n)) putSegEntry(seg, TABLE_PAGE, n, 0, 1, 1, 1, 0xe000);
  putSegEntry(seg, TABLE_PAGE, SEG_FS, 0o000026, 4, FS_MADR, 0o000111, 0o162000);
  putSegEntry(seg, TABLE_PAGE, SEG_MEMORY, DPIT_LOGAD, 0, 0, 0o000111, 0o162000);
  putSegEntry(seg, TABLE_PAGE, SEG_IMAGE, 0o000000, 2, 0, 0o000111, 0o162000);
  putSegEntry(seg, TABLE_PAGE, SEG_IDPIT, DPIT_LOGAD, 4, IDPIT_MADR, 0o000111, 0o162000);
  putSegEntry(seg, TABLE_PAGE, SEG_SDPIT, DPIT_LOGAD, 4, SDPIT_MADR, 0o000111, 0o162000);
  putSegEntry(seg, TABLE_PAGE, SEG_SAVE4, 0o000030, 2, SAVE4_MADR, 0o100111, 0o162000);
  putSegEntry(seg, TABLE_PAGE, SEG_USER, 0o000060, 3, USER_MADR, 0o020111, 0o162000);

  // Recognisable content: every page of a segment carries its number and page index.
  for (let p = 0; p < 4; p++) putWord(seg, (FS_MADR + p) * PAGE, 0x5300 | p);
  for (let p = 0; p < 2; p++) putWord(seg, p * PAGE, 0x0200 | p); // S3IMAGE at MADR 0
  for (let p = 0; p < 4; p++) putWord(seg, (IDPIT_MADR + p) * PAGE, 0x4400 | p);
  for (let p = 0; p < 4; p++) putWord(seg, (SDPIT_MADR + p) * PAGE, 0x5300 | p);
  for (let p = 0; p < 3; p++) putWord(seg1, (USER_MADR + p) * PAGE, 0xa100 | p);

  // The kernel tables in the DPIT image copy (base 4000B): DBLST in blocks, 2 per page.
  const base = IDPIT_MADR * PAGE;
  const dblst0 = sf0 * 2, dblst1 = sf1 * 2;
  putWord(seg, base + (BLST_SYM + 0 - 0o4000) * 2, dblst0 >>> 16);
  putWord(seg, base + (BLST_SYM + 1 - 0o4000) * 2, dblst0 & 0xffff);
  putWord(seg, base + (BLST_SYM + 2 - 0o4000) * 2, dblst1 >>> 16);
  putWord(seg, base + (BLST_SYM + 3 - 0o4000) * 2, dblst1 & 0xffff);
  // segment file 2: defined but on another disk unit
  putWord(seg, base + (BLST_SYM + 5 - 0o4000) * 2, 2000);
  putWord(seg, base + (ABLPA_SYM + 0 - 0o4000) * 2, 2);
  putWord(seg, base + (ABLPA_SYM + 1 - 0o4000) * 2, 2);
  putWord(seg, base + (MASSN_SYM + 0 - 0o4000) * 2, 0o1100);
  putWord(seg, base + (MASSN_SYM + 1 - 0o4000) * 2, 0o1100);
  putWord(seg, base + (MASSN_SYM + 2 - 0o4000) * 2, 0o1500);
  putWord(seg, base + (0o4051 - 0o4000) * 2, 100); // SYSNO, for show

  fs.writeFile('SYSTEM/SEGFIL0:DATA', seg);
  fs.writeFile('SYSTEM/SEGFIL1:DATA', seg1);

  // Segment file 4 is absolute: disk page 1 + MADR. Page 31 and 32 of a fresh 75 MB pack
  // are free, so writing them raw harms nothing.
  const page = new Uint8Array(PAGE);
  putWord(page, 0, 0x5400);
  fs.writeDiskPage(1 + SAVE4_MADR, page);
  putWord(page, 0, 0x5401);
  fs.writeDiskPage(2 + SAVE4_MADR, page);

  const rtfil = new Uint8Array(PAGE);
  putNameRecord(rtfil, 0 * 16, 'S3IMAGE', SEG_IMAGE);
  putNameRecord(rtfil, 1 * 16, 'S3FS', SEG_FS);
  putNameRecord(rtfil, 2 * 16, 'S3DPIT', SEG_MEMORY);
  putNameRecord(rtfil, 3 * 16, 'S3IDPIT', SEG_IDPIT);
  putNameRecord(rtfil, 4 * 16, 'S3SDPIT', SEG_SDPIT);
  putNameRecord(rtfil, 5 * 16, 'S3SPRMA', SEG_SAVE4);
  putNameRecord(rtfil, 6 * 16, 'USERSEG', SEG_USER);
  fs.writeFile('SYSTEM/RTFIL:DATA', rtfil);

  if (withSymbols) {
    writeTextFile(fs, 'SYSTEM/SYMBOL-1-LIST:SYMB',
      'BLST=004023\r\nDBLST=004023\r\nMASSN=004441\r\nMASSU=004451\r\nABLPA=004461\r\nSYSNO=004051\r\nINIBU=074123\r\n');
  }
  return { fs, sf0, sf1 };
}

const word = (d: Uint8Array, o: number): number => (d[o] << 8) | d[o + 1];

describe('SINTRAN segments', () => {
  it('collect_names', () => {
    const buf = new Uint8Array(128);
    putNameRecord(buf, 0, 'S3FS', 0o06);
    putNameRecord(buf, 16, 'S3SDPIT', 0o53);
    putNameRecord(buf, 32, 'S3SDPIT', 0o54); // a second record for a name: first wins
    const names = ss.collectSegmentNames(buf);
    expect(names.size).toBe(3);
    expect(names.get(0o06)).toBe('S3FS');
    expect(names.get(0o53)).toBe('S3SDPIT');
    expect(names.get(0o54)).toBe('S3SDPIT');
    expect(names.has(1)).toBe(false);
  });

  it('raw_page_api', () => {
    const fs = NdfsFileSystem.createImage({ template: ImageTemplate.Floppy12MB, directoryName: 'FLOPPY' });
    const total = fs.totalPages;
    expect(total).toBeGreaterThan(100); // the floppy template, whatever its exact size
    const page = new Uint8Array(PAGE).fill(0x5a);
    fs.writeDiskPage(100, page);
    expect(Array.from(fs.readDiskPage(100))).toEqual(Array.from(page));
    expect(() => fs.readDiskPage(total)).toThrow(RangeError);
    expect(() => fs.writeDiskPage(total, page)).toThrow(RangeError);
    expect(() => fs.writeDiskPage(100, new Uint8Array(5))).toThrow();
  });

  it('list_segments', () => {
    const { fs, sf0, sf1 } = buildPack(true);
    const l = ss.listSegments(fs);
    expect(l.version).toBe('L');
    expect(l.segmentTablePage).toBe(TABLE_PAGE);
    expect(l.haveSymbols).toBe(true);
    expect(l.namesPath).toBe('SYSTEM/RTFIL:DATA');

    // Segment files from the kernel tables.
    expect(l.segfiles[0].defined).toBe(true);
    expect(l.segfiles[0].basePage).toBe(sf0);
    expect(l.segfiles[0].filePath).toBe('SYSTEM/SEGFIL0:DATA');
    expect(l.segfiles[1].defined).toBe(true);
    expect(l.segfiles[1].basePage).toBe(sf1);
    expect(l.segfiles[1].filePath).toBe('SYSTEM/SEGFIL1:DATA');
    expect(l.segfiles[2].defined).toBe(true);
    expect(l.segfiles[2].onThisDisk).toBe(false);
    expect(l.segfiles[3].defined).toBe(false);
    expect(l.segfiles[4].defined).toBe(true);
    expect(l.segfiles[4].basePage).toBe(1);

    // The segments.
    let i = ss.findSegment(l, 'S3FS');
    expect(i).toBeGreaterThanOrEqual(0);
    expect(l.segments[i].number).toBe(SEG_FS);
    expect(l.segments[i].diskPage).toBe(sf0 + 10);
    expect(l.segments[i].segle).toBe(4);
    expect(l.segments[i].segfile).toBe(0);
    i = ss.findSegment(l, 'userseg'); // case-insensitive
    expect(i).toBeGreaterThanOrEqual(0);
    expect(l.segments[i].segfile).toBe(1);
    expect(l.segments[i].diskPage).toBe(sf1 + 2);
    i = ss.findSegment(l, 'S3DPIT');
    expect(i).toBeGreaterThanOrEqual(0);
    expect(l.segments[i].memoryOnly).toBe(true);
    expect(l.segments[i].diskPage).toBe(-1);
    i = ss.findSegment(l, 'S3IMAGE'); // MADR 0 is the first page of file 0
    expect(i).toBeGreaterThanOrEqual(0);
    expect(l.segments[i].memoryOnly).toBe(false);
    expect(l.segments[i].diskPage).toBe(sf0);
    i = ss.findSegment(l, 'S3SPRMA');
    expect(i).toBeGreaterThanOrEqual(0);
    expect(l.segments[i].segfile).toBe(4);
    expect(l.segments[i].diskPage).toBe(31);
    // By number: octal with B, decimal without.
    expect(ss.findSegment(l, '53B')).toBe(ss.findSegment(l, 'S3SDPIT'));
    expect(ss.findSegment(l, '43')).toBe(ss.findSegment(l, 'S3SDPIT'));
    expect(ss.findSegment(l, 'NOSUCH')).toBe(-1);
    expect(ss.findSegment(l, '60B')).toBeGreaterThanOrEqual(0); // filler entry 060B: listed, unnamed
  });

  it('list_without_symbols_falls_back_to_segfil0_file', () => {
    const { fs, sf0 } = buildPack(false);
    const l = ss.listSegments(fs);
    expect(l.haveSymbols).toBe(false);
    expect(l.version).toBe('?');
    expect(l.segfiles[0].defined).toBe(true);
    expect(l.segfiles[0].basePage).toBe(sf0);
    expect(l.segfiles[1].defined).toBe(false);
    expect(l.segfiles[4].defined).toBe(false);
    let i = ss.findSegment(l, 'S3FS');
    expect(l.segments[i].diskPage).toBe(sf0 + 10);
    i = ss.findSegment(l, 'USERSEG');
    expect(l.segments[i].diskPage).toBe(-1); // segment file 1 unknown
  });

  it('export_matches_file_bytes', () => {
    const { fs } = buildPack(true);
    const l = ss.listSegments(fs);

    // S3FS: 4 pages at SEGFIL0 page 10 - must equal the file's bytes.
    let i = ss.findSegment(l, 'S3FS');
    let data = ss.exportSegment(fs, l, i);
    expect(data.length).toBe(4 * PAGE);
    const file = fs.readFile('SYSTEM/SEGFIL0:DATA');
    expect(Array.from(data)).toEqual(Array.from(file.subarray(10 * PAGE, 14 * PAGE)));
    expect(word(data, 2 * PAGE)).toBe(0x5302);

    // USERSEG on segment file 1.
    i = ss.findSegment(l, 'USERSEG');
    data = ss.exportSegment(fs, l, i);
    expect(data.length).toBe(3 * PAGE);
    expect(word(data, PAGE)).toBe(0xa101);

    // The absolute save segment on file 4.
    i = ss.findSegment(l, 'S3SPRMA');
    data = ss.exportSegment(fs, l, i);
    expect(word(data, 0)).toBe(0x5400);
    expect(word(data, PAGE)).toBe(0x5401);

    // S3IMAGE at MADR 0: the first pages of SEGFIL0.
    i = ss.findSegment(l, 'S3IMAGE');
    data = ss.exportSegment(fs, l, i);
    expect(word(data, PAGE)).toBe(0x0201);

    // Memory-only: refused.
    i = ss.findSegment(l, 'S3DPIT');
    expect(() => ss.exportSegment(fs, l, i)).toThrow(CpuIdentityError);
  });

  it('import_round_trip_and_refusals', () => {
    const { fs } = buildPack(true);
    const l = ss.listSegments(fs);
    let i = ss.findSegment(l, 'S3FS');

    // Modify the export and write it back; the file view must show it.
    const data = ss.exportSegment(fs, l, i);
    data[100] = 0xde;
    data[101] = 0xad;
    data[3 * PAGE + 7] = 0x77;
    ss.importSegment(fs, l, i, data);
    expect(Array.from(ss.exportSegment(fs, l, i))).toEqual(Array.from(data));
    let back = fs.readFile('SYSTEM/SEGFIL0:DATA');
    expect(back[10 * PAGE + 100]).toBe(0xde);
    expect(back[13 * PAGE + 7]).toBe(0x77);

    // A shorter import keeps the tail of the last page.
    data[0] = 0x11;
    data[1] = 0x22;
    ss.importSegment(fs, l, i, data.subarray(0, 10));
    back = ss.exportSegment(fs, l, i);
    expect(back[0]).toBe(0x11);
    expect(back[100]).toBe(0xde); // untouched tail of page 0

    // Too long: refused, nothing written.
    const big = new Uint8Array(5 * PAGE).fill(0xee);
    expect(() => ss.importSegment(fs, l, i, big)).toThrow(RangeError);
    expect(ss.exportSegment(fs, l, i)[0]).toBe(0x11);

    // Memory-only: refused.
    i = ss.findSegment(l, 'S3DPIT');
    expect(() => ss.importSegment(fs, l, i, big.subarray(0, 10))).toThrow(CpuIdentityError);
  });
});
