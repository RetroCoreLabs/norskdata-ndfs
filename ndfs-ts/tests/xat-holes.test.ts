/**
 * Tests for the sparse-hole map in XAT sidecars.
 *
 * A sparse file has pages that were never allocated. Nothing in the sidecar said
 * WHERE they were: pagesInFile and bytesInFile together reveal THAT a file is
 * sparse, but not the positions, so a file copied out to a host and back returned
 * with a different layout. XAT_KEY_HOLES records them.
 *
 * The behaviour these tests pin was measured on real hardware, not invented: the
 * file (SYSTEM)S3-CONFIG-E01:PROG on a live pack has 89 real pages spread over a
 * 99-page extent with holes at 54..63, and transferring it between two ND-100s
 * sends 89 messages carrying page numbers 0..53 then 64..98.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */
import { describe, it, expect } from 'vitest';

import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ObjectEntry } from '../src/object-entry.js';
import { NDFS_PAGE_SIZE } from '../src/constants.js';
import { ImageTemplate } from '../src/types.js';
import {
  XAT_KEYS,
  XAT_KEY_HOLES,
  objectEntryToXat,
  serializeXat,
  deserializeXat,
} from '../src/xat.js';

/** Build file content of `pageCount` pages, non-zero only where asked. */
function sparseContent(pageCount: number, filledPages: number[]): Uint8Array {
  const data = new Uint8Array(pageCount * NDFS_PAGE_SIZE);
  for (const page of filledPages) {
    const start = page * NDFS_PAGE_SIZE;
    for (let i = start; i < start + NDFS_PAGE_SIZE; i++) data[i] = 0xa5;
  }
  return data;
}

/**
 * The hole list for content built by sparseContent().
 *
 * Holes are DECLARED, never inferred from zeros -- writing a hole where the
 * original merely had zeros produces a file SINTRAN cannot READ-BI, so the
 * writer no longer guesses. These tests state the same holes the old inference
 * would have produced, which is what keeps them testing holes.
 */
function holesFor(pageCount: number, filled: number[]): number[] {
  const set = new Set(filled);
  const out: number[] = [];
  for (let p = 0; p < pageCount; p++) if (!set.has(p)) out.push(p);
  return out;
}

function makeFs(): NdfsFileSystem {
  return NdfsFileSystem.createImage({
    template: ImageTemplate.Floppy360KB,
    directoryName: 'TESTDISK',
  });
}

describe('XAT sparse-hole map', () => {
  it('reports an empty list for a solid file', () => {
    const fs = makeFs();
    fs.writeFile('SYSTEM/SOLID:DATA', new Uint8Array(2 * NDFS_PAGE_SIZE).fill(0x11));

    const props = fs.getFileProperties('SYSTEM/SOLID:DATA')!;

    // Empty means "checked, and there are none" - not the same as absent.
    expect(props[XAT_KEY_HOLES]).toEqual([]);
  });

  it('reports where the holes are', () => {
    const fs = makeFs();
    // 6 pages, real data only on the first and last, so pages 1..4 hole out.
    fs.writeFile('SYSTEM/SPARSE:DATA', sparseContent(6, [0, 5]), 'none', holesFor(6, [0, 5]));

    const props = fs.getFileProperties('SYSTEM/SPARSE:DATA')!;

    expect(props[XAT_KEY_HOLES]).toEqual([1, 2, 3, 4]);
  });

  it('matches the zero entries in the file index', () => {
    const fs = makeFs();
    fs.writeFile('SYSTEM/GAPPY:DATA', sparseContent(8, [0, 3, 7]), 'none', holesFor(8, [0, 3, 7]));

    const props = fs.getFileProperties('SYSTEM/GAPPY:DATA')!;
    const blocks = fs.getFileBlocks('SYSTEM/GAPPY:DATA');

    const expected: number[] = [];
    for (let i = 0; i < blocks.length; i++) if (blocks[i] === 0) expected.push(i);

    expect(props[XAT_KEY_HOLES]).toEqual(expected);
  });

  it('lists holes ascending, without duplicates, inside the extent', () => {
    const fs = makeFs();
    fs.writeFile('SYSTEM/ORDER:DATA', sparseContent(8, [0, 3, 7]), 'none', holesFor(8, [0, 3, 7]));

    const props = fs.getFileProperties('SYSTEM/ORDER:DATA')!;
    const holes = props[XAT_KEY_HOLES] as number[];
    const logicalPages = Math.ceil((props[XAT_KEYS.BYTES_IN_FILE] as number) / NDFS_PAGE_SIZE);

    expect([...holes].sort((a, b) => a - b)).toEqual(holes);
    expect(new Set(holes).size).toBe(holes.length);
    for (const page of holes) {
      expect(page).toBeGreaterThanOrEqual(0);
      expect(page).toBeLessThan(logicalPages);
    }
  });

  it('carries the holes through readFileWithProperties too', () => {
    const fs = makeFs();
    fs.writeFile('SYSTEM/BOTH:DATA', sparseContent(6, [0, 5]), 'none', holesFor(6, [0, 5]));

    const { data, properties } = fs.readFileWithProperties('SYSTEM/BOTH:DATA');

    expect(properties[XAT_KEY_HOLES]).toEqual([1, 2, 3, 4]);
    // And the holes still read back as zeros, so the data is unaffected.
    for (let i = NDFS_PAGE_SIZE; i < 2 * NDFS_PAGE_SIZE; i++) expect(data[i]).toBe(0);
  });

  it('survives the JSON round trip as a flat array of numbers', () => {
    const fs = makeFs();
    fs.writeFile('SYSTEM/RTRIP:DATA', sparseContent(6, [0, 5]), 'none', holesFor(6, [0, 5]));

    const props = fs.getFileProperties('SYSTEM/RTRIP:DATA')!;
    const json = serializeXat(props);
    const restored = deserializeXat(json);

    // Flat array is the agreed on-disk shape: the C port stores runs internally
    // but expands them here, so all four sidecars are comparable.
    expect(JSON.parse(json)['ndfs.holes']).toEqual([1, 2, 3, 4]);
    expect(restored[XAT_KEY_HOLES]).toEqual(props[XAT_KEY_HOLES]);
  });

  it('omits the key when the holes were not determined', () => {
    // An object entry on its own cannot know where the holes are - they live in
    // the file's index. Omitting says "not checked", which differs from "none".
    const entry = new ObjectEntry();
    entry.objectName = 'LONE';
    entry.type = 'DATA';

    const props = objectEntryToXat(entry);

    expect(XAT_KEY_HOLES in props).toBe(false);
  });
});
