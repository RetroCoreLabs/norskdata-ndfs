/**
 * A SINTRAN file is identified by NAME:TYPE, not by NAME alone.
 *
 * Real packs carry several files that share a base name and differ only in
 * type: (TCP-IP)TCP-IP-LO-D02 exists as :MODE and :LIST, (TCP-IP)TCP-START-D02
 * as :MODE and :LIST, and (TCP-IP)SKP-C00 as :DEFS, :IMPT and :INTL at once.
 *
 * findObject used to match on the name only, so writeFile believed AAA:MODE
 * was the same file as an existing AAA:LIST. The second write updated the LIST
 * entry's data and left its type alone, which meant the MODE file never
 * appeared and the LIST file silently held the MODE file's bytes. Measured
 * 2026-08-19 while installing the COSMOS TCP/IP kit onto a SINTRAN M pack:
 * both TCP-IP-LO-D02:MODE and TCP-START-D02:MODE were lost that way, and the
 * loss was only visible by comparing file sizes against the source pack.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */
import { describe, it, expect } from 'vitest';

import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ImageTemplate } from '../src/types.js';

function makeFs(): NdfsFileSystem {
  return NdfsFileSystem.createImage({
    template: ImageTemplate.Floppy12MB,
    directoryName: 'TESTD',
  });
}

/** [type, bytes] for every entry with this object name, sorted by type. */
function entriesNamed(fs: NdfsFileSystem, name: string): Array<[string, number]> {
  const out: Array<[string, number]> = [];
  for (const e of fs.getObjectEntries()) {
    if (e.objectName.trim().toUpperCase() === name.toUpperCase()) {
      out.push([e.type.trim().toUpperCase(), e.bytesInFile]);
    }
  }
  out.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0));
  return out;
}

function filled(byte: number, length: number): Uint8Array {
  return new Uint8Array(length).fill(byte);
}

describe('same name, different type', () => {
  it('lets files that differ only in type coexist', () => {
    const fs = makeFs();
    fs.writeFile('SYSTEM/AAA:LIST', filled(0x4c, 100));
    fs.writeFile('SYSTEM/AAA:MODE', filled(0x4d, 200));
    fs.writeFile('SYSTEM/AAA:DEFS', filled(0x44, 300));

    expect(entriesNamed(fs, 'AAA')).toEqual([
      ['DEFS', 300],
      ['LIST', 100],
      ['MODE', 200],
    ]);
  });

  it('reads each type back with its own content', () => {
    const fs = makeFs();
    fs.writeFile('SYSTEM/AAA:LIST', filled(0x4c, 100));
    fs.writeFile('SYSTEM/AAA:MODE', filled(0x4d, 200));

    expect(fs.readFile('SYSTEM/AAA:LIST').slice(0, 100)).toEqual(filled(0x4c, 100));
    expect(fs.readFile('SYSTEM/AAA:MODE').slice(0, 200)).toEqual(filled(0x4d, 200));
  });

  it('leaves the other types alone when one is overwritten', () => {
    const fs = makeFs();
    fs.writeFile('SYSTEM/AAA:LIST', filled(0x4c, 100));
    fs.writeFile('SYSTEM/AAA:MODE', filled(0x4d, 200));
    fs.writeFile('SYSTEM/AAA:DEFS', filled(0x44, 300));

    fs.writeFile('SYSTEM/AAA:MODE', filled(0x6d, 50));

    expect(entriesNamed(fs, 'AAA')).toEqual([
      ['DEFS', 300],
      ['LIST', 100],
      ['MODE', 50],
    ]);
    expect(fs.readFile('SYSTEM/AAA:LIST').slice(0, 100)).toEqual(filled(0x4c, 100));
    expect(fs.readFile('SYSTEM/AAA:DEFS').slice(0, 300)).toEqual(filled(0x44, 300));
  });

  it('honours the type in fileExists and deleteFile', () => {
    const fs = makeFs();
    fs.writeFile('SYSTEM/AAA:LIST', filled(0x4c, 100));

    expect(fs.fileExists('SYSTEM/AAA:LIST')).toBe(true);
    expect(fs.fileExists('SYSTEM/AAA:MODE')).toBe(false);

    fs.writeFile('SYSTEM/AAA:MODE', filled(0x4d, 200));
    fs.deleteFile('SYSTEM/AAA:MODE');

    expect(fs.fileExists('SYSTEM/AAA:LIST')).toBe(true);
    expect(fs.fileExists('SYSTEM/AAA:MODE')).toBe(false);
  });

  it('does not let a contiguous file collide with another type', () => {
    const fs = makeFs();
    fs.writeFile('SYSTEM/BBB:PROG', filled(0x50, 100));

    // A different type is a different file, so this must be allowed.
    fs.createContiguousFile('SYSTEM/BBB:DATA', 2);
    expect(fs.fileExists('SYSTEM/BBB:PROG')).toBe(true);
    expect(fs.fileExists('SYSTEM/BBB:DATA')).toBe(true);

    // The same NAME:TYPE really is a duplicate, and must still be refused.
    expect(() => fs.createContiguousFile('SYSTEM/BBB:DATA', 2)).toThrow();
  });

  it('handles the real case from the TCP/IP kit', () => {
    const fs = makeFs();
    const files: Array<[string, string, number]> = [
      ['TCP-IP-LO-D02', 'LIST', 4848],
      ['TCP-IP-LO-D02', 'MODE', 2307],
      ['TCP-START-D02', 'LIST', 684],
      ['TCP-START-D02', 'MODE', 581],
      ['SKP-C00', 'DEFS', 17260],
      ['SKP-C00', 'IMPT', 7759],
      ['SKP-C00', 'INTL', 7149],
    ];
    for (const [name, type, size] of files) {
      fs.writeFile(`SYSTEM/${name}:${type}`, filled(0x41, size));
    }

    expect(entriesNamed(fs, 'TCP-IP-LO-D02')).toEqual([
      ['LIST', 4848],
      ['MODE', 2307],
    ]);
    expect(entriesNamed(fs, 'TCP-START-D02')).toEqual([
      ['LIST', 684],
      ['MODE', 581],
    ]);
    expect(entriesNamed(fs, 'SKP-C00')).toEqual([
      ['DEFS', 17260],
      ['IMPT', 7759],
      ['INTL', 7149],
    ]);
  });
});
