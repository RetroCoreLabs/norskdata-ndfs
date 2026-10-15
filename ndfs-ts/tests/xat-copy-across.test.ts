/**
 * Tests for XAT (Extended Attribute) copy-across scenarios.
 *
 * Validates that XAT metadata and sparse data are preserved when
 * copying files across NDFS filesystem images.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { describe, it, expect } from 'vitest';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { NDFS_PAGE_SIZE } from '../src/constants.js';
import { ImageTemplate } from '../src/types.js';
import {
  XAT_KEYS,
  XatProperties,
  objectEntryToXat,
  xatToObjectEntry,
  serializeXat,
  deserializeXat,
  getXatFileName,
  isXatFile,
} from '../src/xat.js';
import { ObjectEntry } from '../src/object-entry.js';

/**
 * Helper: create a writable NDFS image via the image creator API.
 */
function createFs(dirName: string = 'TESTDISK'): NdfsFileSystem {
  return NdfsFileSystem.createImage({
    template: ImageTemplate.Floppy360KB,
    directoryName: dirName,
  });
}

describe('XAT Copy-Across Scenarios', () => {

  // 1. Basic XAT round-trip
  it('basic XAT round-trip: write -> extract -> serialize -> deserialize -> write -> verify', () => {
    const fs1 = createFs('DISK1');
    const content = new Uint8Array([10, 20, 30, 40, 50]);
    fs1.writeFile('SYSTEM/HELLO:TEXT', content);

    // Extract with properties
    const { data, properties } = fs1.readFileWithProperties('SYSTEM/HELLO:TEXT');

    // Serialize to JSON and back
    const json = serializeXat(properties);
    const restored = deserializeXat(json);

    // Create new image, write with restored properties
    const fs2 = createFs('DISK2');
    fs2.writeFileWithProperties('SYSTEM/HELLO:TEXT', data, restored);

    // Verify all fields
    const props2 = fs2.getFileProperties('SYSTEM/HELLO:TEXT');
    expect(props2).not.toBeNull();
    expect(props2![XAT_KEYS.OBJECT_NAME]).toBe(properties[XAT_KEYS.OBJECT_NAME]);
    expect(props2![XAT_KEYS.TYPE]).toBe(properties[XAT_KEYS.TYPE]);
    expect(props2![XAT_KEYS.USER_NAME]).toBe(properties[XAT_KEYS.USER_NAME]);
    expect(props2![XAT_KEYS.FILE_TYPE]).toBe(properties[XAT_KEYS.FILE_TYPE]);

    // Verify data
    const readBack = fs2.readFile('SYSTEM/HELLO:TEXT');
    expect(readBack.length).toBe(5);
    for (let i = 0; i < 5; i++) {
      expect(readBack[i]).toBe(content[i]);
    }
  });

  // 2. Access bits preservation
  it('preserves access_bits through XAT round-trip', () => {
    const fs1 = createFs('DISK1');
    fs1.writeFile('SYSTEM/SECURE:DATA', new Uint8Array([1, 2, 3]));
    fs1.writeFileWithProperties('SYSTEM/SECURE:DATA', new Uint8Array([1, 2, 3]), {
      [XAT_KEYS.ACCESS_BITS]: 0x7FFF,
    });

    const { data, properties } = fs1.readFileWithProperties('SYSTEM/SECURE:DATA');
    expect(properties[XAT_KEYS.ACCESS_BITS]).toBe(0x7FFF);

    const json = serializeXat(properties);
    const restored = deserializeXat(json);

    const fs2 = createFs('DISK2');
    fs2.writeFileWithProperties('SYSTEM/SECURE:DATA', data, restored);

    const props2 = fs2.getFileProperties('SYSTEM/SECURE:DATA');
    expect(props2).not.toBeNull();
    expect(props2![XAT_KEYS.ACCESS_BITS]).toBe(0x7FFF);
  });

  // 3. File type flags preservation
  it('preserves file_type_flags through XAT round-trip', () => {
    const fs1 = createFs();
    const content = new Uint8Array([0xAA, 0xBB]);
    fs1.writeFile('SYSTEM/FLAGS:DATA', content);

    // objectEntryToXat sets file_type_flags to 0 by default;
    // we set it manually in the XAT to simulate IndexedFile | AllocatedFile = 0x28
    const { data, properties } = fs1.readFileWithProperties('SYSTEM/FLAGS:DATA');
    properties[XAT_KEYS.FILE_TYPE_FLAGS] = 0x28;

    const json = serializeXat(properties);
    const restored = deserializeXat(json);
    expect(restored[XAT_KEYS.FILE_TYPE_FLAGS]).toBe(0x28);
  });

  // 4. File type code preservation
  it('preserves file_type code through XAT round-trip (e.g., SYMB = 2)', () => {
    const fs1 = createFs();
    fs1.writeFile('SYSTEM/SYMBOLS:SYMB', new Uint8Array([1, 2, 3, 4]));
    fs1.writeFileWithProperties('SYSTEM/SYMBOLS:SYMB', new Uint8Array([1, 2, 3, 4]), {
      [XAT_KEYS.FILE_TYPE]: 2,
    });

    const { data, properties } = fs1.readFileWithProperties('SYSTEM/SYMBOLS:SYMB');
    expect(properties[XAT_KEYS.FILE_TYPE]).toBe(2);

    const json = serializeXat(properties);
    const restored = deserializeXat(json);

    const fs2 = createFs();
    fs2.writeFileWithProperties('SYSTEM/SYMBOLS:SYMB', data, restored);

    const props2 = fs2.getFileProperties('SYSTEM/SYMBOLS:SYMB');
    expect(props2).not.toBeNull();
    expect(props2![XAT_KEYS.FILE_TYPE]).toBe(2);
  });

  // 5. User association preservation
  it('preserves user_index and user_name through XAT round-trip', () => {
    const fs1 = NdfsFileSystem.createImage({
      template: ImageTemplate.Floppy360KB,
      directoryName: 'USRDISK',
      users: [
        { name: 'SYSTEM', reservedPages: 100 },
        { name: 'GUEST', reservedPages: 50 },
      ],
    });

    fs1.writeFile('GUEST/MYFILE:TEXT', new Uint8Array([65, 66, 67]));

    const { data, properties } = fs1.readFileWithProperties('GUEST/MYFILE:TEXT');
    expect(properties[XAT_KEYS.USER_NAME]).toBe('GUEST');
    const guestIndex = properties[XAT_KEYS.USER_INDEX];
    expect(typeof guestIndex).toBe('number');
    expect(guestIndex).toBeGreaterThan(0); // GUEST is not the first user

    const json = serializeXat(properties);
    const restored = deserializeXat(json);
    expect(restored[XAT_KEYS.USER_NAME]).toBe('GUEST');
    expect(restored[XAT_KEYS.USER_INDEX]).toBe(guestIndex);
  });

  // 6. Date fields preservation
  it('preserves date_created and last_write_date through XAT round-trip', () => {
    const fs1 = createFs();
    fs1.writeFile('SYSTEM/DATED:DATA', new Uint8Array([1]));
    fs1.writeFileWithProperties('SYSTEM/DATED:DATA', new Uint8Array([1]), {
      [XAT_KEYS.DATE_CREATED]: 12345,
      [XAT_KEYS.LAST_WRITE_DATE]: 67890,
    });

    const { data, properties } = fs1.readFileWithProperties('SYSTEM/DATED:DATA');
    expect(properties[XAT_KEYS.DATE_CREATED]).toBe(12345);
    expect(properties[XAT_KEYS.LAST_WRITE_DATE]).toBe(67890);

    const json = serializeXat(properties);
    const restored = deserializeXat(json);

    const fs2 = createFs();
    fs2.writeFileWithProperties('SYSTEM/DATED:DATA', data, restored);

    const props2 = fs2.getFileProperties('SYSTEM/DATED:DATA');
    expect(props2).not.toBeNull();
    expect(props2![XAT_KEYS.DATE_CREATED]).toBe(12345);
    expect(props2![XAT_KEYS.LAST_WRITE_DATE]).toBe(67890);
  });

  // 7. Sparse file with XAT (4 pages, pages 1 and 3 all zeros)
  it('sparse file with XAT: zero pages preserved in round-trip', () => {
    const fs1 = createFs();

    // Build 4-page file: page 0 has data, page 1 zeros, page 2 data, page 3 zeros
    const fileSize = 4 * NDFS_PAGE_SIZE;
    const content = new Uint8Array(fileSize);
    // Page 0: fill with 0xAA
    for (let i = 0; i < NDFS_PAGE_SIZE; i++) content[i] = 0xAA;
    // Page 1: all zeros (default)
    // Page 2: fill with 0xBB
    for (let i = 2 * NDFS_PAGE_SIZE; i < 3 * NDFS_PAGE_SIZE; i++) content[i] = 0xBB;
    // Page 3: all zeros (default)

    // Pages 1 and 3 are DECLARED holes -- being zero would not make them so.
    fs1.writeFile('SYSTEM/SPARSE:DATA', content, 'none', [1, 3]);

    const { data, properties } = fs1.readFileWithProperties('SYSTEM/SPARSE:DATA');
    // pagesInFile counts the pages ACTUALLY ALLOCATED, not the logical extent: pages 0
    // and 2 hold data, pages 1 and 3 are sparse holes that occupy no disk. Was 4 before
    // the 2026-07-30 correction, which matched SINTRAN -- a real pack records 89 for a
    // file whose index spans 99 slots with 10 holes. The logical extent is not lost, it
    // is ceil(bytesInFile / NDFS_PAGE_SIZE).
    expect(properties[XAT_KEYS.PAGES_IN_FILE]).toBe(2);
    expect(properties[XAT_KEYS.BYTES_IN_FILE]).toBe(fileSize);

    // Copy to new image
    const fs2 = createFs();
    fs2.writeFileWithProperties('SYSTEM/SPARSE:DATA', data, properties);

    // Read back and verify byte-for-byte
    const readBack = fs2.readFile('SYSTEM/SPARSE:DATA');
    expect(readBack.length).toBe(fileSize);

    // Check page 0: 0xAA
    for (let i = 0; i < NDFS_PAGE_SIZE; i++) {
      expect(readBack[i]).toBe(0xAA);
    }
    // Check page 1: zeros
    for (let i = NDFS_PAGE_SIZE; i < 2 * NDFS_PAGE_SIZE; i++) {
      expect(readBack[i]).toBe(0);
    }
    // Check page 2: 0xBB
    for (let i = 2 * NDFS_PAGE_SIZE; i < 3 * NDFS_PAGE_SIZE; i++) {
      expect(readBack[i]).toBe(0xBB);
    }
    // Check page 3: zeros
    for (let i = 3 * NDFS_PAGE_SIZE; i < 4 * NDFS_PAGE_SIZE; i++) {
      expect(readBack[i]).toBe(0);
    }
  });

  // 8. Large sparse file XAT (10 pages, only first and last have data)
  it('large sparse file: 10 pages, data only on first and last', () => {
    const fs1 = createFs();

    const fileSize = 10 * NDFS_PAGE_SIZE;
    const content = new Uint8Array(fileSize);
    // First page: 0x11
    for (let i = 0; i < NDFS_PAGE_SIZE; i++) content[i] = 0x11;
    // Last page: 0xFF
    for (let i = 9 * NDFS_PAGE_SIZE; i < 10 * NDFS_PAGE_SIZE; i++) content[i] = 0xFF;
    // Pages 1-8: zeros

    // Pages 1..8 are DECLARED holes; only 0 and 9 carry data.
    fs1.writeFile('SYSTEM/BIGSPARSE:DATA', content, 'none', [1, 2, 3, 4, 5, 6, 7, 8]);

    const { data, properties } = fs1.readFileWithProperties('SYSTEM/BIGSPARSE:DATA');
    // Only the first and last pages hold data; the 8 in between are sparse holes
    // occupying no disk. pagesInFile counts allocated pages, so 2 -- see the note in the
    // sparse round-trip test above.
    expect(properties[XAT_KEYS.PAGES_IN_FILE]).toBe(2);

    // Read back and verify zeros in middle
    const readBack = fs1.readFile('SYSTEM/BIGSPARSE:DATA');
    expect(readBack.length).toBe(fileSize);

    // First page
    for (let i = 0; i < NDFS_PAGE_SIZE; i++) {
      expect(readBack[i]).toBe(0x11);
    }
    // Middle pages should be zero
    for (let i = NDFS_PAGE_SIZE; i < 9 * NDFS_PAGE_SIZE; i++) {
      expect(readBack[i]).toBe(0);
    }
    // Last page
    for (let i = 9 * NDFS_PAGE_SIZE; i < 10 * NDFS_PAGE_SIZE; i++) {
      expect(readBack[i]).toBe(0xFF);
    }
  });

  // 9. Mixed content sparse
  it('mixed content sparse: alternating data/zero pages round-trip', () => {
    const fs1 = createFs();

    const numPages = 6;
    const fileSize = numPages * NDFS_PAGE_SIZE;
    const content = new Uint8Array(fileSize);
    // Even pages: data, Odd pages: zeros
    for (let p = 0; p < numPages; p++) {
      if (p % 2 === 0) {
        const fill = (p + 1) * 0x10;
        for (let i = 0; i < NDFS_PAGE_SIZE; i++) {
          content[p * NDFS_PAGE_SIZE + i] = fill & 0xFF;
        }
      }
    }

    fs1.writeFile('SYSTEM/MIXED:DATA', content);
    const { data, properties } = fs1.readFileWithProperties('SYSTEM/MIXED:DATA');

    const json = serializeXat(properties);
    const restored = deserializeXat(json);

    const fs2 = createFs();
    fs2.writeFileWithProperties('SYSTEM/MIXED:DATA', data, restored);

    const readBack = fs2.readFile('SYSTEM/MIXED:DATA');
    expect(readBack.length).toBe(fileSize);

    for (let i = 0; i < fileSize; i++) {
      expect(readBack[i]).toBe(content[i]);
    }
  });

  // 10. XAT with overwrite
  it('XAT with overwrite: apply file A XAT to file B', () => {
    const fs1 = createFs();
    const dataA = new Uint8Array([1, 2, 3]);
    fs1.writeFile('SYSTEM/FILEA:TEXT', dataA);
    fs1.writeFileWithProperties('SYSTEM/FILEA:TEXT', dataA, {
      [XAT_KEYS.ACCESS_BITS]: 0x1234,
      [XAT_KEYS.FILE_TYPE]: 3,
    });

    const propsA = fs1.getFileProperties('SYSTEM/FILEA:TEXT');
    expect(propsA).not.toBeNull();

    // Write file B with different content
    const dataB = new Uint8Array([10, 20, 30, 40, 50]);
    fs1.writeFile('SYSTEM/FILEB:TEXT', dataB);

    // Apply A's XAT to B
    fs1.writeFileWithProperties('SYSTEM/FILEB:TEXT', dataB, propsA!);

    const propsB = fs1.getFileProperties('SYSTEM/FILEB:TEXT');
    expect(propsB).not.toBeNull();
    expect(propsB![XAT_KEYS.ACCESS_BITS]).toBe(0x1234);
    expect(propsB![XAT_KEYS.FILE_TYPE]).toBe(3);

    // But data should be B's data
    const readBack = fs1.readFile('SYSTEM/FILEB:TEXT');
    expect(readBack.length).toBe(5);
    expect(readBack[0]).toBe(10);
    expect(readBack[4]).toBe(50);
  });

  // 11. Copy between two images
  it('copy between two images preserves all metadata', () => {
    const fs1 = createFs('SRC');
    fs1.writeFile('SYSTEM/FILE1:TEXT', new Uint8Array([65, 66]));
    fs1.writeFileWithProperties('SYSTEM/FILE1:TEXT', new Uint8Array([65, 66]), {
      [XAT_KEYS.ACCESS_BITS]: 999,
      [XAT_KEYS.FILE_TYPE]: 1,
      [XAT_KEYS.DATE_CREATED]: 100,
    });

    fs1.writeFile('SYSTEM/FILE2:DATA', new Uint8Array([67, 68, 69]));
    fs1.writeFileWithProperties('SYSTEM/FILE2:DATA', new Uint8Array([67, 68, 69]), {
      [XAT_KEYS.ACCESS_BITS]: 511,
      [XAT_KEYS.FILE_TYPE]: 2,
      [XAT_KEYS.DATE_CREATED]: 200,
    });

    const fs2 = createFs('DST');

    // Copy file1
    const r1 = fs1.readFileWithProperties('SYSTEM/FILE1:TEXT');
    fs2.writeFileWithProperties('SYSTEM/FILE1:TEXT', r1.data, r1.properties);

    // Copy file2
    const r2 = fs1.readFileWithProperties('SYSTEM/FILE2:DATA');
    fs2.writeFileWithProperties('SYSTEM/FILE2:DATA', r2.data, r2.properties);

    // Verify file1 on fs2
    const p1 = fs2.getFileProperties('SYSTEM/FILE1:TEXT');
    expect(p1).not.toBeNull();
    expect(p1![XAT_KEYS.ACCESS_BITS]).toBe(999);
    expect(p1![XAT_KEYS.FILE_TYPE]).toBe(1);
    expect(p1![XAT_KEYS.DATE_CREATED]).toBe(100);

    const d1 = fs2.readFile('SYSTEM/FILE1:TEXT');
    expect(d1[0]).toBe(65);
    expect(d1[1]).toBe(66);

    // Verify file2 on fs2
    const p2 = fs2.getFileProperties('SYSTEM/FILE2:DATA');
    expect(p2).not.toBeNull();
    expect(p2![XAT_KEYS.ACCESS_BITS]).toBe(511);
    expect(p2![XAT_KEYS.FILE_TYPE]).toBe(2);
    expect(p2![XAT_KEYS.DATE_CREATED]).toBe(200);

    const d2 = fs2.readFile('SYSTEM/FILE2:DATA');
    expect(d2[0]).toBe(67);
    expect(d2[2]).toBe(69);
  });

  // 12. Status bits survive re-write
  it('status bits survive re-write with same XAT', () => {
    const fs1 = createFs();
    fs1.writeFile('SYSTEM/PERSIST:DATA', new Uint8Array([1, 2, 3]));
    fs1.writeFileWithProperties('SYSTEM/PERSIST:DATA', new Uint8Array([1, 2, 3]), {
      [XAT_KEYS.ACCESS_BITS]: 0x0FFF,
      [XAT_KEYS.DATE_CREATED]: 42,
    });

    const propsOrig = fs1.getFileProperties('SYSTEM/PERSIST:DATA');
    expect(propsOrig![XAT_KEYS.ACCESS_BITS]).toBe(0x0FFF);

    // Overwrite file with new content but same XAT
    const newData = new Uint8Array([10, 20, 30, 40]);
    fs1.deleteFile('SYSTEM/PERSIST:DATA');
    fs1.writeFileWithProperties('SYSTEM/PERSIST:DATA', newData, propsOrig!);

    const propsAfter = fs1.getFileProperties('SYSTEM/PERSIST:DATA');
    expect(propsAfter).not.toBeNull();
    expect(propsAfter![XAT_KEYS.ACCESS_BITS]).toBe(0x0FFF);
    expect(propsAfter![XAT_KEYS.DATE_CREATED]).toBe(42);

    // Verify new data
    const readBack = fs1.readFile('SYSTEM/PERSIST:DATA');
    expect(readBack.length).toBe(4);
    expect(readBack[0]).toBe(10);
  });

  // 13. XAT filename convention
  it('XAT filename convention', () => {
    expect(getXatFileName('README.TEXT')).toBe('README.TEXT.xat');
    expect(getXatFileName('foo')).toBe('foo.xat');
    expect(isXatFile('README.TEXT.xat')).toBe(true);
    expect(isXatFile('README.TEXT')).toBe(false);
  });
});
