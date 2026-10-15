/**
 * Tests for the SINTRAN initial-command buffer (sintran + fs methods).
 *
 * The pure parse/encode cases mirror ndfs-c/tests/test_sintran.c and the
 * RetroFS.NDFS C# tests, whose expected values were captured on live K/L/M
 * systems. The locate test builds a synthetic self-describing segment table; the
 * patch test exercises the in-place write path end-to-end on a created image.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */
import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ImageTemplate } from '../src/types.js';
import * as sintran from '../src/sintran.js';

/** '|' marks a NUL pad byte; other chars are literal; padded to 64 bytes. */
function fixture(spec: string): Uint8Array {
  const out = new Uint8Array(64);
  for (let i = 0; i < spec.length && i < 64; i++) {
    out[i] = spec[i] === '|' ? 0x00 : spec.charCodeAt(i);
  }
  return out;
}

const THREE = ['ENTER-DIR,,DI-75-1,0', 'ENTER-DIR,,DI-74-1,0', 'SET-AVAIL'];

describe('SINTRAN parser', () => {
  it('two commands with pad', () => {
    expect(sintran.parseBuffer(fixture("AB'|CD'|'"), 0, 64)).toEqual(['AB', 'CD']);
  });
  it('even command needs no pad', () => {
    expect(sintran.parseBuffer(fixture("SET-AVAIL'CC HELLO'|'"), 0, 64)).toEqual(['SET-AVAIL', 'CC HELLO']);
  });
  it('lone quote terminates immediately', () => {
    expect(sintran.parseBuffer(fixture("'"), 0, 64)).toEqual([]);
  });
  it('parity bit stripped', () => {
    expect(sintran.parseBuffer(Uint8Array.from([0xc1, 0xc2, 0x27, 0x00, 0x27]), 0, 5)).toEqual(['AB']);
  });
  it('non-printable payload rejected', () => {
    expect(sintran.parseBuffer(Uint8Array.from([0x01, 0x02, 0x27, 0x27]), 0, 4)).toBeNull();
  });
});

describe('SINTRAN encoder', () => {
  it('three real commands -> textLen 54', () => {
    expect(sintran.encodeBuffer(THREE).textLen).toBe(54);
  });
  it('+ CC OFFLINE WRITE OK -> textLen 74', () => {
    expect(sintran.encodeBuffer([...THREE, 'CC OFFLINE WRITE OK']).textLen).toBe(74);
  });
  it('uppercases', () => {
    const { bytes } = sintran.encodeBuffer(['cc hi']);
    expect(Array.from(bytes.subarray(0, 5))).toEqual(Array.from(new TextEncoder().encode('CC HI')));
  });
  it('254 accepted, 256 rejected', () => {
    expect(sintran.encodeBuffer(['C'.repeat(253)]).textLen).toBe(254);
    expect(() => sintran.encodeBuffer(['C'.repeat(255)])).toThrow(/overflow/);
  });
  it('embedded quote rejected', () => {
    expect(() => sintran.encodeBuffer(["BAD'CMD"])).toThrow(/invalid/);
  });
  it('encode -> parse round trip', () => {
    const cmds = [...THREE, 'CC OFFLINE WRITE OK'];
    const { bytes, textLen } = sintran.encodeBuffer(cmds);
    expect(sintran.parseBuffer(bytes, 0, textLen)).toEqual(cmds);
  });
});

describe('SINTRAN self-consistency', () => {
  it('rejects a coincidental parse and accepts a real buffer', () => {
    expect(sintran.reencodeMatches(['B,'], 2)).toBe(false); // re-encodes to 4
    expect(sintran.reencodeMatches(THREE, 54)).toBe(true);
  });
});

describe('SINTRAN locate (synthetic segment table)', () => {
  function putWord(b: Uint8Array, off: number, val: number): void {
    b[off] = (val >> 8) & 0xff;
    b[off + 1] = val & 0xff;
  }
  function putSegEntry(b: Uint8Array, page: number, n: number, logad: number, segle: number, madr: number, flag: number, sgsta: number): void {
    const o = page * 2048 + n * 16;
    putWord(b, o + 4, logad);
    putWord(b, o + 6, segle);
    putWord(b, o + 8, madr);
    putWord(b, o + 10, flag);
    putWord(b, o + 12, sgsta);
  }

  it('locates a real buffer via the segment table', () => {
    const PAGES = 64, TABLE_PAGE = 5, BUF_MADR = 20;
    const inibuL = 0o74123;
    const segBase = 12 * 1024;
    const seg = new Uint8Array(PAGES * 2048);

    putSegEntry(seg, TABLE_PAGE, 1, 12, 52, BUF_MADR, 1, 0xe000);
    putSegEntry(seg, TABLE_PAGE, 2, 0, 1, TABLE_PAGE, 1, 0xe000);
    for (let n = 3; n <= 40; n++) putSegEntry(seg, TABLE_PAGE, n, 0, 1, 1, 1, 0xe000);

    const byteOffset = BUF_MADR * 2048 + (inibuL - segBase) * 2;
    const { bytes, textLen } = sintran.encodeBuffer(THREE);
    seg.set(bytes, byteOffset);
    putWord(seg, byteOffset + sintran.LEN_CELL_BYTES, textLen);

    expect(sintran.findSegmentTablePage(seg)).toBe(TABLE_PAGE);
    const loc = sintran.locate(seg);
    expect(loc).not.toBeNull();
    expect(loc!.version).toBe('L');
    expect(loc!.byteOffset).toBe(byteOffset);
    expect(loc!.textLength).toBe(54);
    expect(loc!.commands).toEqual(THREE);
  });
});

describe('patchFileRegion (write path)', () => {
  function createFS(pages = 200): NdfsFileSystem {
    return NdfsFileSystem.createImage({ template: ImageTemplate.Custom, customPages: pages, directoryName: 'PATCHT' });
  }

  it('patches across a page boundary and leaves the rest intact', () => {
    const fs = createFS();
    fs.writeFile('SYSTEM/BIG:DATA', new Uint8Array(3000).fill(0x41)); // 'A'

    fs.patchFileRegion('SYSTEM/BIG:DATA', 2000, new Uint8Array(100).fill(0x42)); // 'B'

    const back = fs.readFile('SYSTEM/BIG:DATA');
    expect(back.length).toBe(3000);
    for (let i = 0; i < 3000; i++) {
      expect(back[i]).toBe(i >= 2000 && i < 2100 ? 0x42 : 0x41);
    }

    // A region past the file's allocated pages is rejected.
    expect(() => fs.patchFileRegion('SYSTEM/BIG:DATA', 4050, new Uint8Array(100))).toThrow();
  });
});
