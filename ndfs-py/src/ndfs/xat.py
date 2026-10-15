"""
XAT (Extended Attribute) sidecar file support for NDFS.

Preserves NDFS metadata (access bits, dates, file type flags, user info, etc.)
when files are copied to/from host filesystems.

When extracting a file from NDFS, a companion `.xat` JSON file is created
alongside the data file containing all the NDFS metadata that would
otherwise be lost. When copying back in, the `.xat` file is read to
restore the metadata.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

import json
from typing import Dict, List, Optional, Union

from ndfs.object_entry import ObjectEntry

# Property key constants
XAT_OBJECT_NAME = "ndfs.object_name"
XAT_TYPE = "ndfs.type"
XAT_USER_NAME = "ndfs.user_name"
XAT_USER_INDEX = "ndfs.user_index"
XAT_ACCESS_BITS = "ndfs.access_bits"
XAT_FILE_TYPE_FLAGS = "ndfs.file_type_flags"
XAT_FILE_TYPE = "ndfs.file_type"
XAT_DEVICE_NUMBER = "ndfs.device_number"
XAT_NEXT_VERSION = "ndfs.next_version"
XAT_PREV_VERSION = "ndfs.prev_version"
XAT_PAGES_IN_FILE = "ndfs.pages_in_file"
XAT_BYTES_IN_FILE = "ndfs.bytes_in_file"
XAT_DATE_CREATED = "ndfs.date_created"
XAT_LAST_READ_DATE = "ndfs.last_read_date"
XAT_LAST_WRITE_DATE = "ndfs.last_write_date"

# Ascending list of the file's sparse holes, as LOGICAL page indices. Empty when
# the file is solid.
#
# Why it is needed: nothing else in the sidecar can express WHERE the holes are.
# pages_in_file and bytes_in_file together reveal THAT a file is sparse -- if
# bytes_in_file / 2048 exceeds pages_in_file there must be holes -- but not their
# positions. Without this key a sparse file copied out to a host and back returns
# with a different layout from the one it left with.
#
# It also preserves a distinction the data alone cannot carry: a hole and a page
# of genuine zeros read back identically, but they are not the same thing on disk.
XAT_HOLES = "ndfs.holes"

ALL_XAT_KEYS = [
    XAT_OBJECT_NAME,
    XAT_TYPE,
    XAT_USER_NAME,
    XAT_USER_INDEX,
    XAT_ACCESS_BITS,
    XAT_FILE_TYPE_FLAGS,
    XAT_FILE_TYPE,
    XAT_DEVICE_NUMBER,
    XAT_NEXT_VERSION,
    XAT_PREV_VERSION,
    XAT_PAGES_IN_FILE,
    XAT_BYTES_IN_FILE,
    XAT_DATE_CREATED,
    XAT_LAST_READ_DATE,
    XAT_LAST_WRITE_DATE,
]

# Keys that appear only when the writer could determine them. XAT_HOLES is not in
# ALL_XAT_KEYS on purpose: those are the keys every sidecar carries, and this one
# is conditional. Emitting an empty list when the holes were never looked up would
# assert the file is solid, which is a claim the caller cannot make from an object
# entry alone.
OPTIONAL_XAT_KEYS = [
    XAT_HOLES,
]

XAT_EXTENSION = ".xat"


def object_entry_to_xat(entry: ObjectEntry, holes: Optional[List[int]] = None) -> dict:
    """Serialize an ObjectEntry to XAT properties dict.

    Captures all NDFS metadata fields.

    Args:
        entry: the object entry to serialize.
        holes: ascending logical page indices of the file's sparse holes, or None
            when the caller cannot determine them. The object entry alone cannot:
            the holes live in the file's index, so only the filesystem can supply
            them. When None the key is omitted rather than guessed at, so an empty
            list means "checked, and there are none" while a missing key means
            "not checked".
    """
    props: Dict[str, Union[str, int, bool, List[int]]] = {}
    props[XAT_OBJECT_NAME] = entry.object_name
    props[XAT_TYPE] = entry.type
    props[XAT_USER_NAME] = entry.user_name
    props[XAT_USER_INDEX] = entry.user_index
    props[XAT_ACCESS_BITS] = entry.access_bits
    props[XAT_FILE_TYPE_FLAGS] = entry.file_type_flags
    props[XAT_FILE_TYPE] = entry.file_type
    props[XAT_DEVICE_NUMBER] = entry.device_number
    props[XAT_NEXT_VERSION] = entry.next_version
    props[XAT_PREV_VERSION] = entry.prev_version
    props[XAT_PAGES_IN_FILE] = entry.pages_in_file
    props[XAT_BYTES_IN_FILE] = entry.bytes_in_file
    props[XAT_DATE_CREATED] = entry.date_created
    props[XAT_LAST_READ_DATE] = entry.last_date_read
    props[XAT_LAST_WRITE_DATE] = entry.last_date_written
    if holes is not None:
        props[XAT_HOLES] = list(holes)
    return props


def xat_to_object_entry(xat: dict, entry: ObjectEntry) -> None:
    """Apply XAT properties to an ObjectEntry (for copy-in / restore).

    Only updates fields that are present in the XAT properties.
    """
    if XAT_OBJECT_NAME in xat and isinstance(xat[XAT_OBJECT_NAME], str):
        entry.object_name = xat[XAT_OBJECT_NAME]
    if XAT_TYPE in xat and isinstance(xat[XAT_TYPE], str):
        entry.type = xat[XAT_TYPE]
    if XAT_USER_NAME in xat and isinstance(xat[XAT_USER_NAME], str):
        entry.user_name = xat[XAT_USER_NAME]
    if XAT_USER_INDEX in xat and isinstance(xat[XAT_USER_INDEX], int):
        entry.user_index = xat[XAT_USER_INDEX]
    if XAT_ACCESS_BITS in xat and isinstance(xat[XAT_ACCESS_BITS], int):
        entry.access_bits = xat[XAT_ACCESS_BITS]
    if XAT_FILE_TYPE in xat and isinstance(xat[XAT_FILE_TYPE], int):
        entry.file_type = xat[XAT_FILE_TYPE]
    if XAT_FILE_TYPE_FLAGS in xat and isinstance(xat[XAT_FILE_TYPE_FLAGS], int):
        entry.file_type_flags = xat[XAT_FILE_TYPE_FLAGS]
    if XAT_DEVICE_NUMBER in xat and isinstance(xat[XAT_DEVICE_NUMBER], int):
        entry.device_number = xat[XAT_DEVICE_NUMBER]
    # next_version / prev_version: recorded in XAT for fidelity but NOT restored.
    # They reference object-table slots that are reassigned on import.
    if XAT_PAGES_IN_FILE in xat and isinstance(xat[XAT_PAGES_IN_FILE], int):
        entry.pages_in_file = xat[XAT_PAGES_IN_FILE]
    if XAT_BYTES_IN_FILE in xat and isinstance(xat[XAT_BYTES_IN_FILE], int):
        entry.bytes_in_file = xat[XAT_BYTES_IN_FILE]
    if XAT_DATE_CREATED in xat and isinstance(xat[XAT_DATE_CREATED], int):
        entry.date_created = xat[XAT_DATE_CREATED]
    if XAT_LAST_READ_DATE in xat and isinstance(xat[XAT_LAST_READ_DATE], int):
        entry.last_date_read = xat[XAT_LAST_READ_DATE]
    if XAT_LAST_WRITE_DATE in xat and isinstance(xat[XAT_LAST_WRITE_DATE], int):
        entry.last_date_written = xat[XAT_LAST_WRITE_DATE]
    # holes: recorded for fidelity but NOT applied here, for the same reason as the
    # version pointers -- an ObjectEntry has nowhere to put them. The holes live in
    # the file's index, which only the write path can build. A writer that wants to
    # honour them must force a hole at each listed page instead of relying on the
    # all-zero detection, which cannot tell a hole from a page of real zeros.


def serialize_xat(props: dict) -> str:
    """Serialize XAT properties to a JSON string (pretty-printed)."""
    return json.dumps(props, indent=2)


def deserialize_xat(json_str: str) -> dict:
    """Deserialize XAT properties from a JSON string."""
    parsed = json.loads(json_str)
    if not isinstance(parsed, dict):
        raise ValueError("Invalid XAT data: expected a JSON object")
    return parsed


def get_xat_filename(data_file: str) -> str:
    """Get the .xat filename for a given data file.

    E.g., "README.TEXT" -> "README.TEXT.xat"
    """
    return data_file + XAT_EXTENSION


def is_xat_file(filename: str) -> bool:
    """Check if a filename is an XAT sidecar file."""
    return len(filename) > len(XAT_EXTENSION) and filename.lower().endswith(XAT_EXTENSION)
