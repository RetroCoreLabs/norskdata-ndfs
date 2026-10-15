/**
 * XAT (Extended Attribute) sidecar file support for NDFS.
 *
 * Preserves NDFS metadata (access bits, dates, file type flags, user info, etc.)
 * when files are copied to/from host filesystems.
 *
 * When extracting a file from NDFS, a companion `.xat` JSON file is created
 * alongside the data file containing all the NDFS metadata that would
 * otherwise be lost. When copying back in, the `.xat` file is read to
 * restore the metadata.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { ObjectEntry } from './object-entry.js';

/** Property key constants for XAT files. */
export const XAT_KEYS = {
  OBJECT_NAME: 'ndfs.object_name',
  TYPE: 'ndfs.type',
  USER_NAME: 'ndfs.user_name',
  USER_INDEX: 'ndfs.user_index',
  ACCESS_BITS: 'ndfs.access_bits',
  FILE_TYPE_FLAGS: 'ndfs.file_type_flags',
  FILE_TYPE: 'ndfs.file_type',
  DEVICE_NUMBER: 'ndfs.device_number',
  NEXT_VERSION: 'ndfs.next_version',
  PREV_VERSION: 'ndfs.prev_version',
  PAGES_IN_FILE: 'ndfs.pages_in_file',
  BYTES_IN_FILE: 'ndfs.bytes_in_file',
  DATE_CREATED: 'ndfs.date_created',
  LAST_READ_DATE: 'ndfs.last_read_date',
  LAST_WRITE_DATE: 'ndfs.last_write_date',
} as const;

/**
 * Ascending list of the file's sparse holes, as LOGICAL page indices. Empty when
 * the file is solid; absent when the holes were not determined.
 *
 * Deliberately NOT part of XAT_KEYS. Those are the keys every sidecar carries,
 * and this one is conditional - an ObjectEntry alone cannot know where the holes
 * are, because they live in the file's index. Emitting an empty list in that case
 * would assert the file is solid, a claim the caller cannot make.
 *
 * Nothing else in the sidecar can express WHERE the holes are. `pagesInFile` and
 * `bytesInFile` together reveal THAT a file is sparse - if `bytesInFile / 2048`
 * exceeds `pagesInFile` there must be holes - but not their positions. Without
 * this a sparse file copied out to a host and back returns with a different
 * layout from the one it left with.
 *
 * It also preserves a distinction the data alone cannot carry: a hole and a page
 * of genuine zeros read back identically, but they differ on disk.
 */
export const XAT_KEY_HOLES = 'ndfs.holes';

/** XAT property bag: string-keyed record of primitives. */
export type XatProperties = Record<string, string | number | boolean | number[]>;

/** XAT file extension. */
export const XAT_EXTENSION = '.xat';

/**
 * Serialize an ObjectEntry to XAT properties. Captures all NDFS metadata fields.
 *
 * @param entry Entry to serialize.
 * @param holes Ascending logical page indices of the file's sparse holes, or
 *   undefined when the caller cannot determine them. The entry alone cannot: the
 *   holes live in the file's index, so only the filesystem can supply them. When
 *   undefined the key is omitted rather than guessed at, so an empty array means
 *   "checked, none" while a missing key means "not checked".
 */
export function objectEntryToXat(entry: ObjectEntry, holes?: number[]): XatProperties {
  const props: XatProperties = {};
  props[XAT_KEYS.OBJECT_NAME] = entry.objectName;
  props[XAT_KEYS.TYPE] = entry.type;
  props[XAT_KEYS.USER_NAME] = entry.userName;
  props[XAT_KEYS.USER_INDEX] = entry.userIndex;
  props[XAT_KEYS.ACCESS_BITS] = entry.accessBits;
  props[XAT_KEYS.FILE_TYPE_FLAGS] = entry.fileTypeFlags;
  props[XAT_KEYS.FILE_TYPE] = entry.fileType;
  props[XAT_KEYS.DEVICE_NUMBER] = entry.deviceNumber;
  props[XAT_KEYS.NEXT_VERSION] = entry.nextVersion;
  props[XAT_KEYS.PREV_VERSION] = entry.prevVersion;
  props[XAT_KEYS.PAGES_IN_FILE] = entry.pagesInFile;
  props[XAT_KEYS.BYTES_IN_FILE] = entry.bytesInFile;
  props[XAT_KEYS.DATE_CREATED] = entry.dateCreated;
  props[XAT_KEYS.LAST_READ_DATE] = entry.lastDateRead;
  props[XAT_KEYS.LAST_WRITE_DATE] = entry.lastDateWritten;
  if (holes !== undefined) {
    props[XAT_KEY_HOLES] = [...holes];
  }
  return props;
}

/**
 * Apply XAT properties to an ObjectEntry (for copy-in / restore).
 * Only updates fields that are present in the XAT properties.
 */
export function xatToObjectEntry(xat: XatProperties, entry: ObjectEntry): void {
  if (XAT_KEYS.OBJECT_NAME in xat && typeof xat[XAT_KEYS.OBJECT_NAME] === 'string') {
    entry.objectName = xat[XAT_KEYS.OBJECT_NAME] as string;
  }
  if (XAT_KEYS.TYPE in xat && typeof xat[XAT_KEYS.TYPE] === 'string') {
    entry.type = xat[XAT_KEYS.TYPE] as string;
  }
  if (XAT_KEYS.USER_NAME in xat && typeof xat[XAT_KEYS.USER_NAME] === 'string') {
    entry.userName = xat[XAT_KEYS.USER_NAME] as string;
  }
  if (XAT_KEYS.USER_INDEX in xat && typeof xat[XAT_KEYS.USER_INDEX] === 'number') {
    entry.userIndex = xat[XAT_KEYS.USER_INDEX] as number;
  }
  if (XAT_KEYS.ACCESS_BITS in xat && typeof xat[XAT_KEYS.ACCESS_BITS] === 'number') {
    entry.accessBits = xat[XAT_KEYS.ACCESS_BITS] as number;
  }
  if (XAT_KEYS.FILE_TYPE in xat && typeof xat[XAT_KEYS.FILE_TYPE] === 'number') {
    entry.fileType = xat[XAT_KEYS.FILE_TYPE] as number;
  }
  if (
    XAT_KEYS.FILE_TYPE_FLAGS in xat &&
    typeof xat[XAT_KEYS.FILE_TYPE_FLAGS] === 'number'
  ) {
    entry.fileTypeFlags = xat[XAT_KEYS.FILE_TYPE_FLAGS] as number;
  }
  if (XAT_KEYS.DEVICE_NUMBER in xat && typeof xat[XAT_KEYS.DEVICE_NUMBER] === 'number') {
    entry.deviceNumber = xat[XAT_KEYS.DEVICE_NUMBER] as number;
  }
  // nextVersion / prevVersion: recorded in XAT for fidelity but NOT restored.
  // They reference object-table slots that are reassigned on import.
  if (XAT_KEYS.PAGES_IN_FILE in xat && typeof xat[XAT_KEYS.PAGES_IN_FILE] === 'number') {
    entry.pagesInFile = xat[XAT_KEYS.PAGES_IN_FILE] as number;
  }
  if (XAT_KEYS.BYTES_IN_FILE in xat && typeof xat[XAT_KEYS.BYTES_IN_FILE] === 'number') {
    entry.bytesInFile = xat[XAT_KEYS.BYTES_IN_FILE] as number;
  }
  if (XAT_KEYS.DATE_CREATED in xat && typeof xat[XAT_KEYS.DATE_CREATED] === 'number') {
    entry.dateCreated = xat[XAT_KEYS.DATE_CREATED] as number;
  }
  if (XAT_KEYS.LAST_READ_DATE in xat && typeof xat[XAT_KEYS.LAST_READ_DATE] === 'number') {
    entry.lastDateRead = xat[XAT_KEYS.LAST_READ_DATE] as number;
  }
  // HOLES: recorded for fidelity but NOT applied here, for the same reason as the
  // version pointers - an ObjectEntry has nowhere to put them. The holes live in
  // the file's index, which only the write path can build. A writer that wants to
  // honour them must force a hole at each listed page instead of relying on
  // all-zero detection, which cannot tell a hole from a page of real zeros.
  if (XAT_KEYS.LAST_WRITE_DATE in xat && typeof xat[XAT_KEYS.LAST_WRITE_DATE] === 'number') {
    entry.lastDateWritten = xat[XAT_KEYS.LAST_WRITE_DATE] as number;
  }
}

/**
 * Serialize XAT properties to a JSON string (pretty-printed).
 */
export function serializeXat(props: XatProperties): string {
  return JSON.stringify(props, null, 2);
}

/**
 * Deserialize XAT properties from a JSON string.
 */
export function deserializeXat(json: string): XatProperties {
  const parsed = JSON.parse(json);
  if (typeof parsed !== 'object' || parsed === null || Array.isArray(parsed)) {
    throw new Error('Invalid XAT data: expected a JSON object');
  }
  return parsed as XatProperties;
}

/**
 * Get the .xat filename for a given data file.
 * E.g., "README.TEXT" -> "README.TEXT.xat"
 */
export function getXatFileName(dataFile: string): string {
  return dataFile + XAT_EXTENSION;
}

/**
 * Check if a filename is an XAT sidecar file.
 */
export function isXatFile(fileName: string): boolean {
  return fileName.length > XAT_EXTENSION.length &&
    fileName.toLowerCase().endsWith(XAT_EXTENSION);
}
