import { describe, it, expect } from 'vitest';
import { ObjectEntry } from '../src/object-entry.js';
import { NdfsFileSystem } from '../src/ndfs-filesystem.js';
import { NDFS_PAGE_SIZE, NDFS_NAME_TERMINATOR } from '../src/constants.js';
import { writeUint32BE } from '../src/endian.js';
import { BlockPointer } from '../src/block-pointer.js';
import { PointerType } from '../src/types.js';
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

/**
 * Create a minimal valid NDFS test image (same helper as ndfs-filesystem.test.ts).
 */
function createTestImage(totalPages: number = 50, dirName: string = 'TESTDISK'): Uint8Array {
  const imageSize = totalPages * NDFS_PAGE_SIZE;
  const image = new Uint8Array(imageSize);

  const mbOff = 2016;
  for (let i = 0; i < dirName.length; i++) image[mbOff + i] = dirName.charCodeAt(i);
  if (dirName.length < 16) image[mbOff + dirName.length] = NDFS_NAME_TERMINATOR;

  const objPtr = new BlockPointer(7, PointerType.Indexed);
  objPtr.toBytes(image, mbOff + 0x10);

  const usrPtr = new BlockPointer(9, PointerType.Indexed);
  usrPtr.toBytes(image, mbOff + 0x14);

  const bitPtr = new BlockPointer(11, PointerType.Contiguous);
  bitPtr.toBytes(image, mbOff + 0x18);

  writeUint32BE(image, mbOff + 0x1c, totalPages - 12);

  const objIdxOff = 7 * NDFS_PAGE_SIZE;
  const objDataPtr = new BlockPointer(8, PointerType.Contiguous);
  objDataPtr.toBytes(image, objIdxOff);

  const usrIdxOff = 9 * NDFS_PAGE_SIZE;
  const usrDataPtr = new BlockPointer(10, PointerType.Contiguous);
  usrDataPtr.toBytes(image, usrIdxOff);

  const usrDataOff = 10 * NDFS_PAGE_SIZE;
  image[usrDataOff + 0] = 0x81;
  const sysName = 'SYSTEM';
  for (let i = 0; i < sysName.length; i++) image[usrDataOff + 2 + i] = sysName.charCodeAt(i);
  image[usrDataOff + 2 + sysName.length] = NDFS_NAME_TERMINATOR;
  writeUint32BE(image, usrDataOff + 28, 1000);
  writeUint32BE(image, usrDataOff + 32, 0);
  image[usrDataOff + 37] = 0;

  const bitmapOff = 11 * NDFS_PAGE_SIZE;
  // Word addressed: pages 0-7 are in the second byte of the pair, pages 8-15 in the first.
  image[bitmapOff + 1] = 0xff;
  image[bitmapOff] = 0x0f;

  return image;
}

describe('XAT', () => {
  describe('objectEntryToXat', () => {
    it('serializes all fields from ObjectEntry', () => {
      const entry = new ObjectEntry();
      entry.objectName = 'README';
      entry.type = 'TEXT';
      entry.userName = 'SYSTEM';
      entry.userIndex = 0;
      entry.accessBits = 1279;
      entry.fileType = 3;
      entry.pagesInFile = 1;
      entry.bytesInFile = 1024;
      entry.dateCreated = 12345;
      entry.lastDateRead = 67890;
      entry.lastDateWritten = 11111;

      const xat = objectEntryToXat(entry);

      expect(xat[XAT_KEYS.OBJECT_NAME]).toBe('README');
      expect(xat[XAT_KEYS.TYPE]).toBe('TEXT');
      expect(xat[XAT_KEYS.USER_NAME]).toBe('SYSTEM');
      expect(xat[XAT_KEYS.USER_INDEX]).toBe(0);
      expect(xat[XAT_KEYS.ACCESS_BITS]).toBe(1279);
      expect(xat[XAT_KEYS.FILE_TYPE_FLAGS]).toBe(0);
      expect(xat[XAT_KEYS.FILE_TYPE]).toBe(3);
      expect(xat[XAT_KEYS.PAGES_IN_FILE]).toBe(1);
      expect(xat[XAT_KEYS.BYTES_IN_FILE]).toBe(1024);
      expect(xat[XAT_KEYS.DATE_CREATED]).toBe(12345);
      expect(xat[XAT_KEYS.LAST_READ_DATE]).toBe(67890);
      expect(xat[XAT_KEYS.LAST_WRITE_DATE]).toBe(11111);
    });

    it('includes all expected keys', () => {
      const entry = new ObjectEntry();
      const xat = objectEntryToXat(entry);

      const expectedKeys = Object.values(XAT_KEYS);
      for (let i = 0; i < expectedKeys.length; i++) {
        expect(xat).toHaveProperty(expectedKeys[i]);
      }
    });
  });

  describe('xatToObjectEntry', () => {
    it('applies XAT properties to ObjectEntry', () => {
      const xat: XatProperties = {
        [XAT_KEYS.OBJECT_NAME]: 'HELLO',
        [XAT_KEYS.TYPE]: 'PROG',
        [XAT_KEYS.USER_NAME]: 'ADMIN',
        [XAT_KEYS.USER_INDEX]: 2,
        [XAT_KEYS.ACCESS_BITS]: 511,
        [XAT_KEYS.FILE_TYPE]: 1,
        [XAT_KEYS.PAGES_IN_FILE]: 5,
        [XAT_KEYS.BYTES_IN_FILE]: 8192,
        [XAT_KEYS.DATE_CREATED]: 100,
        [XAT_KEYS.LAST_READ_DATE]: 200,
        [XAT_KEYS.LAST_WRITE_DATE]: 300,
      };

      const entry = new ObjectEntry();
      xatToObjectEntry(xat, entry);

      expect(entry.objectName).toBe('HELLO');
      expect(entry.type).toBe('PROG');
      expect(entry.userName).toBe('ADMIN');
      expect(entry.userIndex).toBe(2);
      expect(entry.accessBits).toBe(511);
      expect(entry.fileType).toBe(1);
      expect(entry.pagesInFile).toBe(5);
      expect(entry.bytesInFile).toBe(8192);
      expect(entry.dateCreated).toBe(100);
      expect(entry.lastDateRead).toBe(200);
      expect(entry.lastDateWritten).toBe(300);
    });

    it('only modifies fields present in XAT', () => {
      const entry = new ObjectEntry();
      entry.objectName = 'ORIGINAL';
      entry.accessBits = 999;

      const xat: XatProperties = {
        [XAT_KEYS.ACCESS_BITS]: 42,
      };

      xatToObjectEntry(xat, entry);

      expect(entry.objectName).toBe('ORIGINAL');
      expect(entry.accessBits).toBe(42);
    });
  });

  describe('JSON round-trip', () => {
    it('serialize then deserialize preserves all data', () => {
      const entry = new ObjectEntry();
      entry.objectName = 'TESTFILE';
      entry.type = 'DATA';
      entry.userName = 'SYSTEM';
      entry.userIndex = 0;
      entry.accessBits = 1279;
      entry.fileType = 0;
      entry.pagesInFile = 3;
      entry.bytesInFile = 5000;
      entry.dateCreated = 99;
      entry.lastDateRead = 100;
      entry.lastDateWritten = 101;

      const original = objectEntryToXat(entry);
      const json = serializeXat(original);
      const restored = deserializeXat(json);

      const keys = Object.values(XAT_KEYS);
      for (let i = 0; i < keys.length; i++) {
        expect(restored[keys[i]]).toEqual(original[keys[i]]);
      }
    });

    it('round-trip through ObjectEntry preserves metadata', () => {
      const entry = new ObjectEntry();
      entry.objectName = 'ROUNDTRIP';
      entry.type = 'SYMB';
      entry.userName = 'USER1';
      entry.userIndex = 1;
      entry.accessBits = 255;
      entry.fileType = 2;

      const xat = objectEntryToXat(entry);
      const json = serializeXat(xat);
      const restored = deserializeXat(json);

      const newEntry = new ObjectEntry();
      xatToObjectEntry(restored, newEntry);

      expect(newEntry.objectName).toBe('ROUNDTRIP');
      expect(newEntry.type).toBe('SYMB');
      expect(newEntry.userName).toBe('USER1');
      expect(newEntry.userIndex).toBe(1);
      expect(newEntry.accessBits).toBe(255);
      expect(newEntry.fileType).toBe(2);
    });

    it('deserialize rejects non-object JSON', () => {
      expect(() => deserializeXat('"just a string"')).toThrow();
      expect(() => deserializeXat('[1, 2, 3]')).toThrow();
      expect(() => deserializeXat('null')).toThrow();
    });
  });

  describe('getXatFileName', () => {
    it('appends .xat to data filename', () => {
      expect(getXatFileName('README.TEXT')).toBe('README.TEXT.xat');
      expect(getXatFileName('HELLO.PROG')).toBe('HELLO.PROG.xat');
      expect(getXatFileName('noext')).toBe('noext.xat');
    });
  });

  describe('isXatFile', () => {
    it('identifies .xat files', () => {
      expect(isXatFile('README.TEXT.xat')).toBe(true);
      expect(isXatFile('file.xat')).toBe(true);
      expect(isXatFile('FILE.XAT')).toBe(true);
    });

    it('rejects non-xat files', () => {
      expect(isXatFile('README.TEXT')).toBe(false);
      expect(isXatFile('xat')).toBe(false);
      expect(isXatFile('.xat')).toBe(false);
      expect(isXatFile('')).toBe(false);
    });
  });

  describe('NdfsFileSystem XAT integration', () => {
    it('getFileProperties returns XAT for existing file', () => {
      const image = createTestImage();
      const fs = new NdfsFileSystem(image);

      fs.writeFile('SYSTEM/HELLO:TEXT', new Uint8Array([72, 69, 76, 76, 79]));

      const props = fs.getFileProperties('SYSTEM/HELLO:TEXT');
      expect(props).not.toBeNull();
      expect(props![XAT_KEYS.OBJECT_NAME]).toBe('HELLO');
      expect(props![XAT_KEYS.TYPE]).toBe('TEXT');
      expect(props![XAT_KEYS.USER_NAME]).toBe('SYSTEM');
    });

    it('getFileProperties returns null for non-existent file', () => {
      const image = createTestImage();
      const fs = new NdfsFileSystem(image);
      expect(fs.getFileProperties('SYSTEM/NOFILE:DATA')).toBeNull();
    });

    it('readFileWithProperties returns data and properties', () => {
      const image = createTestImage();
      const fs = new NdfsFileSystem(image);

      const testData = new Uint8Array([1, 2, 3, 4, 5]);
      fs.writeFile('SYSTEM/MYFILE:DATA', testData);

      const result = fs.readFileWithProperties('SYSTEM/MYFILE:DATA');
      expect(result.data.length).toBe(5);
      expect(result.data[0]).toBe(1);
      expect(result.properties[XAT_KEYS.OBJECT_NAME]).toBe('MYFILE');
      expect(result.properties[XAT_KEYS.BYTES_IN_FILE]).toBe(5);
    });

    it('writeFileWithProperties applies metadata', () => {
      const image = createTestImage();
      const fs = new NdfsFileSystem(image);

      const testData = new Uint8Array([10, 20, 30]);
      const xatProps: XatProperties = {
        [XAT_KEYS.ACCESS_BITS]: 1279,
        [XAT_KEYS.FILE_TYPE]: 3,
        [XAT_KEYS.DATE_CREATED]: 42,
      };

      fs.writeFileWithProperties('SYSTEM/RESTORED:TEXT', testData, xatProps);

      const props = fs.getFileProperties('SYSTEM/RESTORED:TEXT');
      expect(props).not.toBeNull();
      expect(props![XAT_KEYS.ACCESS_BITS]).toBe(1279);
      expect(props![XAT_KEYS.FILE_TYPE]).toBe(3);
      expect(props![XAT_KEYS.DATE_CREATED]).toBe(42);

      // Verify data is correct
      const data = fs.readFile('SYSTEM/RESTORED:TEXT');
      expect(data.length).toBe(3);
      expect(data[0]).toBe(10);
    });

    it('full extract-then-restore round-trip', () => {
      const image = createTestImage();
      const fs = new NdfsFileSystem(image);

      // Write a file
      const originalData = new Uint8Array([65, 66, 67, 68]);
      fs.writeFile('SYSTEM/RTFILE:TEXT', originalData);

      // "Extract" with properties
      const { data, properties } = fs.readFileWithProperties('SYSTEM/RTFILE:TEXT');

      // Modify properties (simulate external edit)
      properties[XAT_KEYS.ACCESS_BITS] = 777;

      // Delete original
      fs.deleteFile('SYSTEM/RTFILE:TEXT');

      // "Restore" with properties
      fs.writeFileWithProperties('SYSTEM/RTFILE:TEXT', data, properties);

      // Verify
      const restored = fs.readFileWithProperties('SYSTEM/RTFILE:TEXT');
      expect(restored.data.length).toBe(4);
      expect(restored.data[0]).toBe(65);
      expect(restored.properties[XAT_KEYS.ACCESS_BITS]).toBe(777);
    });
  });

  describe('device_number and version pointers', () => {
    it('captures all three from ObjectEntry', () => {
      const entry = new ObjectEntry();
      entry.objectName = 'TEST';
      entry.type = 'DATA';
      entry.deviceNumber = 42;
      entry.nextVersion = 100;
      entry.prevVersion = 200;

      const xat = objectEntryToXat(entry);
      expect(xat[XAT_KEYS.DEVICE_NUMBER]).toBe(42);
      expect(xat[XAT_KEYS.NEXT_VERSION]).toBe(100);
      expect(xat[XAT_KEYS.PREV_VERSION]).toBe(200);
    });

    it('JSON round-trip preserves all three', () => {
      const entry = new ObjectEntry();
      entry.deviceNumber = 7;
      entry.nextVersion = 55;
      entry.prevVersion = 33;

      const xat = objectEntryToXat(entry);
      const json = serializeXat(xat);
      const restored = deserializeXat(json);

      expect(restored[XAT_KEYS.DEVICE_NUMBER]).toBe(7);
      expect(restored[XAT_KEYS.NEXT_VERSION]).toBe(55);
      expect(restored[XAT_KEYS.PREV_VERSION]).toBe(33);
    });

    it('restores device_number but NOT version pointers', () => {
      const xat: XatProperties = {
        [XAT_KEYS.DEVICE_NUMBER]: 42,
        [XAT_KEYS.NEXT_VERSION]: 100,
        [XAT_KEYS.PREV_VERSION]: 200,
      };
      const entry = new ObjectEntry();
      entry.deviceNumber = 0;
      entry.nextVersion = 999;  // sentinel
      entry.prevVersion = 888;  // sentinel

      xatToObjectEntry(xat, entry);

      expect(entry.deviceNumber).toBe(42);   // restored
      expect(entry.nextVersion).toBe(999);   // NOT changed
      expect(entry.prevVersion).toBe(888);   // NOT changed
    });
  });
});
