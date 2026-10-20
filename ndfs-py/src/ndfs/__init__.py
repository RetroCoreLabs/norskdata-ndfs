"""
NDFS (Norsk Data File System) Python library.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""

from ndfs.constants import (
    NDFS_PAGE_SIZE,
    NDFS_NAME_TERMINATOR,
    NDFS_NAME_MAX,
    NDFS_TYPE_MAX,
    ENTRIES_PER_PAGE,
    ENTRY_SIZE,
    MAX_USER_FILE_POINTERS,
    MAX_OBJECT_FILE_POINTERS,
    MAX_USERS,
    MAX_FRIENDS,
    MASTER_BLOCK_OFFSET,
    EXTENDED_INFO_OFFSET,
    MASTER_BLOCK_SIZE,
    EXTENDED_INFO_SIZE,
    FIRST_ALLOCATABLE_BLOCK,
    USER_ENTRY_FLAG,
    OBJECT_ENTRY_IN_USE,
)
from ndfs.types import (
    BootChoice,
    PointerType,
    BootFormat,
    BootControllerType,
    FileAccessType,
    FileOperationType,
    ChecksumValidation,
    ImageTemplate,
    FileTypeFlags,
    FileEntry,
    BootCode,
    UserCreationInfo,
    ImageCreationOptions,
)
from ndfs.endian import read_uint16_be, read_uint32_be, write_uint16_be, write_uint32_be
from ndfs.ndfs_name import read_ndfs_name, write_ndfs_name
from ndfs.nd_time import nd_time_to_date, date_to_nd_time
from ndfs.block_pointer import BlockPointer
from ndfs.master_block import MasterBlock
from ndfs.bit_file import BitFile
from ndfs.user_friend import UserFriend
from ndfs.user_entry import UserEntry
from ndfs.user_file import UserFile
from ndfs.errors import (
    NdfsError,
    ObjectBlockCollisionError,
    ObjectBlockLimitError,
)
from ndfs.object_entry import ObjectEntry
from ndfs.object_file import ObjectFile
from ndfs.access_permissions import (
    AccessPermissions,
    PERM_READ,
    PERM_WRITE,
    PERM_APPEND,
    PERM_EXECUTE,
    PERM_DELETE,
)
from ndfs.access_control import get_access_level, get_friend_permissions, check_access
from ndfs.image_creator import create_image, options_for_disk_type, TemplateSpec
from ndfs.disk_types import DiskType, DiskFamily, DISK_TYPES, disk_type_count, get_disk_type, find_disk_type
from ndfs.boot_blobs import boot_blob, SCSI_LAST_PAGE
from ndfs.boot_loader import load_boot_code, detect_boot_format, is_bootable, detect_controller_type
from ndfs.sintran import (
    InitialCommands,
    InitCmdCandidate,
    find_segment_table_page,
    parse_buffer,
    encode_buffer,
    reencode_matches,
    locate,
    locate_repair_target,
    enumerate_candidates,
)
from ndfs.xat import (
    object_entry_to_xat,
    xat_to_object_entry,
    serialize_xat,
    deserialize_xat,
    get_xat_filename,
    is_xat_file,
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
    ALL_XAT_KEYS,
    XAT_EXTENSION,
)
from ndfs.parity import strip_parity, set_parity, is_text_type
from ndfs.wildmatch import wildmatch
from ndfs.filesystem import NdfsFileSystem

__all__ = [
    # Constants
    "NDFS_PAGE_SIZE", "NDFS_NAME_TERMINATOR", "NDFS_NAME_MAX", "NDFS_TYPE_MAX",
    "ENTRIES_PER_PAGE", "ENTRY_SIZE", "MAX_USER_FILE_POINTERS", "MAX_OBJECT_FILE_POINTERS",
    "MAX_USERS", "MAX_FRIENDS", "MASTER_BLOCK_OFFSET", "EXTENDED_INFO_OFFSET",
    "MASTER_BLOCK_SIZE", "EXTENDED_INFO_SIZE", "FIRST_ALLOCATABLE_BLOCK",
    "USER_ENTRY_FLAG", "OBJECT_ENTRY_IN_USE",
    # Types / Enums
    "PointerType", "BootFormat", "BootControllerType", "FileAccessType", "FileOperationType",
    "ChecksumValidation", "ImageTemplate", "BootChoice", "FileTypeFlags",
    "FileEntry", "BootCode", "ImageCreationOptions", "UserCreationInfo",
    # Endian helpers
    "read_uint16_be", "read_uint32_be", "write_uint16_be", "write_uint32_be",
    # Name helpers
    "read_ndfs_name", "write_ndfs_name",
    # Time helpers
    "nd_time_to_date", "date_to_nd_time",
    # Core classes
    "BlockPointer", "MasterBlock", "BitFile",
    "UserFriend", "UserEntry", "UserFile",
    "ObjectEntry", "ObjectFile",
    "NdfsError", "ObjectBlockLimitError", "ObjectBlockCollisionError",
    # Access control
    "AccessPermissions", "PERM_READ", "PERM_WRITE", "PERM_APPEND", "PERM_EXECUTE", "PERM_DELETE",
    "get_access_level", "get_friend_permissions", "check_access",
    # Image creation
    "create_image", "options_for_disk_type", "TemplateSpec",
    "DiskType", "DiskFamily", "DISK_TYPES", "disk_type_count", "get_disk_type", "find_disk_type",
    "boot_blob", "SCSI_LAST_PAGE",
    # Boot loader
    "load_boot_code", "detect_boot_format", "is_bootable", "detect_controller_type",
    # SINTRAN initial commands
    "InitialCommands", "InitCmdCandidate", "find_segment_table_page", "parse_buffer",
    "encode_buffer", "reencode_matches", "locate", "locate_repair_target", "enumerate_candidates",
    # XAT support
    "object_entry_to_xat", "xat_to_object_entry", "serialize_xat", "deserialize_xat",
    "get_xat_filename", "is_xat_file",
    "XAT_OBJECT_NAME", "XAT_TYPE", "XAT_USER_NAME", "XAT_USER_INDEX",
    "XAT_ACCESS_BITS", "XAT_FILE_TYPE_FLAGS", "XAT_FILE_TYPE",
    "XAT_DEVICE_NUMBER", "XAT_NEXT_VERSION", "XAT_PREV_VERSION",
    "XAT_PAGES_IN_FILE", "XAT_BYTES_IN_FILE", "XAT_DATE_CREATED",
    "XAT_LAST_READ_DATE", "XAT_LAST_WRITE_DATE", "ALL_XAT_KEYS", "XAT_EXTENSION",
    # Parity
    "strip_parity", "set_parity", "is_text_type",
    # Wildcard matching
    "wildmatch",
    # Filesystem
    "NdfsFileSystem",
]
