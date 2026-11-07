/**
 * Tests for contiguous file creation.
 *
 * Added 2026-07-30. Until then every create path in the library produced an indexed
 * file, so SINTRAN's `@CREATE-FILE name pages` -- the form that makes the machine report
 * CONTINUOUS FILE -- had nothing behind it, and the COSMOS file-access server's
 * Create-file operation could not be answered when it carried a page count.
 *
 * A contiguous file allocates all of its pages up front, consecutive on the pack, and
 * cannot grow: the pages after the run belong to other files. A write past the
 * reservation therefore fails rather than silently converting the file to an indexed
 * one, which is what SINTRAN itself does.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 */
import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ImageTemplate, PointerType } from '../src/types.js';
import { NDFS_PAGE_SIZE } from '../src/constants.js';
import { FT_CONTIGUOUS, FT_INDEXED, ObjectEntry } from '../src/object-entry.js';

function createFS(pages: number = 200): NdfsFileSystem {
  return NdfsFileSystem.createImage({
    template: ImageTemplate.Custom,
    customPages: pages,
    directoryName: 'CONTIG',
  });
}

/** Find a file's object entry by name. */
function entryOf(fs: NdfsFileSystem, objectName: string): ObjectEntry | null {
  const entries = fs.getObjectEntries();
  for (let i = 0; i < entries.length; i++) {
    if (entries[i].objectName.toUpperCase() === objectName.toUpperCase()) {
      return entries[i];
    }
  }
  return null;
}

describe('CreateContiguousFile', () => {
  // A new file gets its owner's default access, indexed and contiguous alike, and an empty
  // file stays 0 bytes. Measured 2026-11-04 on nd100x: with SYSTEM's default set to
  // N,RW,RWACD, @CREATE-FILE ACCTEST:SYMB,0 and @CREATE-FILE ACCCON:DATA,4 both stored 0x007F,
  // with 0xFFFFFFFF (0 bytes) in the bytes field.
  it("gives new files the owner's default access and keeps an empty file at 0 bytes", () => {
    const fs = createFS();
    fs.setDefaultFileAccess('SYSTEM', 0x007f);
    fs.writeFile('SYSTEM/NEWIDX:SYMB', new Uint8Array([0x58, 0x0d, 0x0a]));
    fs.writeFile('SYSTEM/EMPTY:SYMB', new Uint8Array(0));
    fs.createContiguousFile('SYSTEM/NEWCON:DATA', 4);
    expect(entryOf(fs, 'NEWIDX')!.accessBits).toBe(0x007f);
    expect(entryOf(fs, 'NEWCON')!.accessBits).toBe(0x007f);
    expect(entryOf(fs, 'NEWCON')!.bytesInFile).toBe(0);
    expect(entryOf(fs, 'EMPTY')!.bytesInFile).toBe(0);
  });

  it('flags the entry contiguous and sizes it to the reservation', () => {
    // The three fields a SINTRAN client reads back off the object entry -- together
    // they are what make the machine print "CONTINUOUS FILE" with zero bytes.
    const fs = createFS();
    fs.createContiguousFile('SYSTEM/RESERVED:DATA', 8);

    const obj = entryOf(fs, 'RESERVED');
    expect(obj).not.toBeNull();
    expect(obj!.fileTypeFlags & FT_CONTIGUOUS).not.toBe(0);
    expect(obj!.fileTypeFlags & FT_INDEXED).toBe(0);
    expect(obj!.pagesInFile).toBe(8);
    expect(obj!.bytesInFile).toBe(0);
  });

  it('points straight at the first data page with a contiguous pointer', () => {
    // There is no index block: page N of the file is simply block start+N.
    const fs = createFS();
    fs.createContiguousFile('SYSTEM/RUN:DATA', 6);

    const obj = entryOf(fs, 'RUN')!;
    expect(obj.filePointer!.type).toBe(PointerType.Contiguous);
    expect(obj.filePointer!.blockId).toBeGreaterThan(0);
  });

  it('allocates the run as one unbroken range in the bitmap', () => {
    // The property the whole file kind exists for, checked against the bitmap rather
    // than the pointer: a run that is not actually allocated would be handed out again.
    const fs = createFS();
    fs.createContiguousFile('SYSTEM/RUN:DATA', 6);

    const start = entryOf(fs, 'RUN')!.filePointer!.blockId;
    for (let i = 0; i < 6; i++) {
      expect(fs.isBlockUsed(start + i)).toBe(true);
    }
  });

  it('costs exactly the reservation and no index block', () => {
    // A contiguous file has no structural overhead, unlike an indexed file.
    const fs = createFS();
    const freeBefore = fs.getFreePages();

    fs.createContiguousFile('SYSTEM/COST:DATA', 7);

    expect(freeBefore - fs.getFreePages()).toBe(7);
  });

  it('rejects a zero-page reservation', () => {
    // A file with no pages has no run to point at.
    const fs = createFS();
    expect(() => fs.createContiguousFile('SYSTEM/EMPTY:DATA', 0)).toThrow();
  });

  it('rejects creating over an existing name', () => {
    const fs = createFS();
    fs.createContiguousFile('SYSTEM/ONCE:DATA', 2);
    expect(() => fs.createContiguousFile('SYSTEM/ONCE:DATA', 2)).toThrow();
  });

  it('rejects a run larger than the pack', () => {
    // The allocator must refuse rather than hand back a truncated run.
    const fs = createFS(50);
    expect(() => fs.createContiguousFile('SYSTEM/HUGE:DATA', 10000)).toThrow();
  });
});

describe('WritingIntoAContiguousFile', () => {
  it('reads written data back byte for byte', () => {
    const fs = createFS();
    fs.createContiguousFile('SYSTEM/PAYLOAD:DATA', 4);

    const payload = new TextEncoder().encode('contiguous payload written in place');
    fs.writeFile('SYSTEM/PAYLOAD:DATA', payload);

    const read = fs.readFile('SYSTEM/PAYLOAD:DATA');
    expect(read.length).toBe(payload.length);
    for (let i = 0; i < payload.length; i++) {
      expect(read[i]).toBe(payload[i]);
    }
  });

  it('does not convert the file or move its run', () => {
    const fs = createFS();
    fs.createContiguousFile('SYSTEM/STABLE:DATA', 4);
    const startBefore = entryOf(fs, 'STABLE')!.filePointer!.blockId;

    fs.writeFile('SYSTEM/STABLE:DATA', new TextEncoder().encode('some data'));

    const obj = entryOf(fs, 'STABLE')!;
    expect(obj.fileTypeFlags & FT_CONTIGUOUS).not.toBe(0);
    expect(obj.filePointer!.blockId).toBe(startBefore);
    expect(obj.pagesInFile).toBe(4);
  });

  it('accepts a write that exactly fills the reservation', () => {
    // Guards the boundary from the side that must always fit.
    const fs = createFS();
    fs.createContiguousFile('SYSTEM/EXACT:DATA', 3);

    const exact = new Uint8Array(3 * NDFS_PAGE_SIZE);
    for (let i = 0; i < exact.length; i++) exact[i] = i & 0xff;

    fs.writeFile('SYSTEM/EXACT:DATA', exact);

    const read = fs.readFile('SYSTEM/EXACT:DATA');
    expect(read.length).toBe(exact.length);
    for (let i = 0; i < exact.length; i++) {
      expect(read[i]).toBe(exact[i]);
    }
  });

  it('fails a write beyond the reservation', () => {
    // The deliberate design choice: fail, never grow and never convert.
    const fs = createFS();
    fs.createContiguousFile('SYSTEM/SMALL:DATA', 2);

    expect(() =>
      fs.writeFile('SYSTEM/SMALL:DATA', new Uint8Array(2 * NDFS_PAGE_SIZE + 1)),
    ).toThrow();

    const obj = entryOf(fs, 'SMALL')!;
    expect(obj.bytesInFile).toBe(0);
    expect(obj.pagesInFile).toBe(2);
  });

  it('does not leave the old tail behind on a shorter rewrite', () => {
    // The run is not reallocated, so the write path has to clear the rest.
    const fs = createFS();
    fs.createContiguousFile('SYSTEM/SHRINK:DATA', 3);

    const longData = new Uint8Array(3 * NDFS_PAGE_SIZE).fill(0xab);
    fs.writeFile('SYSTEM/SHRINK:DATA', longData);

    const shortData = new TextEncoder().encode('short');
    fs.writeFile('SYSTEM/SHRINK:DATA', shortData);

    const read = fs.readFile('SYSTEM/SHRINK:DATA');
    expect(read.length).toBe(shortData.length);
    for (let i = 0; i < read.length; i++) {
      expect(read[i]).toBe(shortData[i]);
    }
  });

  it('allocates and frees nothing on a write', () => {
    // The pages were charged at creation; a write must not move disk usage at all.
    const fs = createFS();
    fs.createContiguousFile('SYSTEM/FIXED:DATA', 5);
    const freeAfterCreate = fs.getFreePages();

    fs.writeFile('SYSTEM/FIXED:DATA', new Uint8Array(100).fill(0x41));

    expect(fs.getFreePages()).toBe(freeAfterCreate);
  });
});
