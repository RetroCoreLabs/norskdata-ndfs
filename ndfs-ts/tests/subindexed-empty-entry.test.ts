import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { ImageTemplate, PointerType } from '../src/types.js';
import { NDFS_PAGE_SIZE } from '../src/constants.js';

// SINTRAN III leaves the sub-index entry of a group with no page in it EMPTY (a domain of
// the ND Linker B01, 2026-10-14: entries 0, 2, 4, 6 set). Our writer always makes every
// group's index block, so the file is written and entry 1 is then cleared, as SINTRAN
// would have it. Every page behind the empty entry must stay at its own place.
describe('SubIndexedEmptyEntry', () => {
  it('reads the pages behind an empty sub-index entry at their own place', () => {
    const fs = NdfsFileSystem.createImage({
      template: ImageTemplate.Custom,
      customPages: 2000,
      directoryName: 'SUBEMPTY',
    });
    const pages = 1500;
    const data = new Uint8Array(NDFS_PAGE_SIZE * pages);
    for (const pg of [3, 1030]) data.fill(0xa5, pg * NDFS_PAGE_SIZE, (pg + 1) * NDFS_PAGE_SIZE);
    fs.writeFile('SYSTEM/GAP:DATA', data, 'none',
      Array.from({ length: pages }, (_, i) => i).filter((p) => p !== 3 && p !== 1030));
    const entry = fs.getObjectEntry('GAP', 'SYSTEM');
    expect(entry).not.toBeNull();
    expect(entry!.filePointer.type).toBe(PointerType.SubIndexed);

    const image = fs.toBuffer();
    image.fill(0, entry!.filePointer.blockId * NDFS_PAGE_SIZE + 4, entry!.filePointer.blockId * NDFS_PAGE_SIZE + 8);
    const again = new NdfsFileSystem(image, true);

    expect(again.readFile('SYSTEM/GAP:DATA')).toEqual(data);
    const blocks = again.getFileBlocks('SYSTEM/GAP:DATA');
    expect(blocks.length).toBe(pages);
    expect(blocks[3]).not.toBe(0);
    expect(blocks[600]).toBe(0);
    expect(blocks[1030]).not.toBe(0);
  });
});
