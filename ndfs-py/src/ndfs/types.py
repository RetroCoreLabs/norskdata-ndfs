"""
Public type definitions for the NDFS library.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

import enum
from dataclasses import dataclass, field
from datetime import datetime
from typing import List, Optional


class PointerType(enum.IntEnum):
    """Block pointer type encoded in the top 2 bits."""
    Contiguous = 0
    Indexed = 1
    SubIndexed = 2
    Reserved = 3


class BootFormat(enum.Enum):
    """Boot code format detected in page 0."""
    NONE = "none"
    BINARY = "binary"
    BPUN = "bpun"
    FLOMON = "flomon"


class BootControllerType(enum.Enum):
    """Hard-disk controller family identified from IOX/IOXT instructions in a
    raw-binary bootstrap. Only meaningful when BootFormat is BINARY; BPUN/FLOMON
    boots are always floppy media and are not classified this way."""
    UNKNOWN = "unknown"
    SMD_ECC = "smd_ecc"
    WINCHESTER = "winchester"
    SCSI = "scsi"
    FLOPPY = "floppy"


class FileAccessType(enum.IntEnum):
    """Access level for a user relative to a file."""
    Own = 0
    Friend = 1
    Public = 2


class FileOperationType(enum.IntEnum):
    """File operation types for permission checks."""
    Read = 0
    Write = 1
    Append = 2
    Execute = 3
    Delete = 4
    List = 5


class ChecksumValidation(enum.Enum):
    """Checksum validation state for extended info.

    NOTE: there is deliberately no ``ValidLowByteOnly``. The SINTRAN kernel writes and
    compares the FULL 16-bit checksum (writer WXDIR 37702B, validator CHDSI 37763B);
    accepting a low-byte-only match was a reader-side heuristic SINTRAN never produces.
    """
    Valid = "valid"
    Invalid = "invalid"


class ImageTemplate(enum.Enum):
    """Disk image template for image creation."""
    Floppy360KB = "floppy_360kb"
    Floppy12MB = "floppy_12mb"
    Smd75MB = "smd_75mb"
    Winchester74MB = "winchester_74mb"
    Custom = "custom"


class FileTypeFlags(enum.IntFlag):
    """File type flags (bit field)."""
    NONE = 0
    TerminalFile = 1 << 0
    PeripheralFile = 1 << 1
    SpoolingFile = 1 << 2
    IndexedFile = 1 << 3
    ContiguousFile = 1 << 4
    AllocatedFile = 1 << 5
    MagneticTapeFile = 1 << 6
    LibraryFile = 1 << 7


@dataclass
class FileEntry:
    """A file/directory entry returned by list_directory()."""
    name: str = ""
    type: str = ""
    full_name: str = ""
    user_name: str = ""
    size: int = 0
    pages: int = 0
    is_directory: bool = False
    last_modified: Optional[datetime] = None


@dataclass
class BootCode:
    """Boot code extracted from page 0."""
    format: BootFormat = BootFormat.NONE
    controller_type: BootControllerType = BootControllerType.UNKNOWN
    start_address: int = 0
    boot_address: int = 0
    load_address: int = 0
    word_count: int = 0
    data: bytes = b""
    checksum_valid: bool = False


@dataclass
class UserCreationInfo:
    """User creation info for ImageCreationOptions."""
    name: str = ""
    reserved_pages: int = 0


@dataclass
class ImageCreationOptions:
    """Options for creating a new NDFS disk image."""
    template: ImageTemplate = ImageTemplate.Custom
    directory_name: Optional[str] = None
    custom_pages: Optional[int] = None
    include_extended_info: Optional[bool] = None
    system_number: Optional[int] = None
    flag_word: Optional[int] = None
    users: List[UserCreationInfo] = field(default_factory=list)
