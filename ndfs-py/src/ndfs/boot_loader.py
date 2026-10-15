"""
NDFS boot loader: detects and loads boot code from NDFS block 0.

Supports BPUN, FLOMON, and raw binary formats.

NDFS disks may contain bootable code in the first page (block 0, 2048 bytes).
Formats:

1. BPUN Format (http://www.ndwiki.org/wiki/BPUN_File_Format):
   - ASCII preamble with start/boot addresses (octal, CR-terminated)
   - '!' delimiter
   - Binary section: Address(2) + Count(2) + Data + Checksum(2) + Action(2)
   - All multi-byte values are big-endian

2. FLOMON Format (Floppy Monitor - simplified BPUN):
   - Same preamble as BPUN
   - After '!' delimiter, if Address=0, Count=0, Checksum=0:
     Next byte is word count (single byte, NOT a word)
     Data follows as: 00 HI 00 LO (4 bytes per word)

3. Raw Binary:
   - Direct machine code starting at offset 0

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from typing import Optional, Union

from ndfs.constants import NDFS_PAGE_SIZE
from ndfs.types import BootFormat, BootControllerType, BootCode

_BufType = Union[bytes, bytearray, memoryview]

BPUN_DELIMITER = 0x21  # '!'

# ND-100 CPU opcodes (16-bit, big-endian on disk). A real bootstrap always begins by
# disabling interrupts (and usually paging) before touching hardware - these are the
# ONLY two words a genuine boot sector can start with. Verified against the assembler
# source (norsk_data/nd100-as/instr.h: PIOF=0150405, IOF=0150401) and cross-checked
# against nd100x/asm/DISK-IMAGE-INVENTORY.md, which uses the same two values to
# classify real disk images.
_OPCODE_PIOF = 0xD105  # octal 150405 - disable interrupts + paging
_OPCODE_IOF = 0xD101   # octal 150401 - disable interrupts only

# Literal IOX instruction word = base opcode 0164000 (octal) OR'd with an 11-bit device
# address in bits 10-0 (norsk_data/nd100-as/instr.h: OPC("iox",A_IOX,0164000)). IOXT
# (device address taken from the T register at runtime - used by SCSI/NCR-5386) is the
# single fixed word 0150415 octal; no device address is visible in the instruction
# stream for it.
_IOX_OPCODE_BASE = 0xE800  # octal 164000
_IOX_OPCODE_MASK = 0xF800  # top 5 bits select the IOX instruction class
_IOX_DEVICE_MASK = 0x07FF  # low 11 bits carry the literal device address
_IOXT_OPCODE = 0xD10D      # octal 150415 (SCSI/NCR-5386, indirect)

# Hard-disk controller IOX device bases (octal, converted to decimal), each covering an
# 8-word register window. Source: nd100-dis/DEVICE-BASES.md (thumbwheel-selectable base
# addresses extracted from the RetroCore NDBus controller drivers).
_SMD_ECC_BASES = (0x360, 0x368, 0x160, 0x168)  # 1540, 1550, 540, 550 octal
_WINCHESTER_BASES = (0x140, 0x148)             # 500, 510 octal
_FLOPPY_BASES = (0x370, 0x378)                 # 1560, 1570 octal


def _is_prologue_opcode(word: int) -> bool:
    """Checks whether a 16-bit word is the genuine ND-100 bootstrap prologue."""
    return word == _OPCODE_PIOF or word == _OPCODE_IOF


def _is_in_device_window(address: int, bases: tuple) -> bool:
    """Checks whether a device address falls within any of the given controllers'
    8-word register windows (base .. base+7)."""
    for base in bases:
        if base <= address <= base + 7:
            return True
    return False


def _detect_controller_type(data: _BufType) -> BootControllerType:
    """Scans block 0 for a literal IOX (SMD/ECC, Winchester, Floppy) or indirect IOXT
    (SCSI/NCR-5386) instruction to classify which hard-disk controller the bootstrap
    targets. Only meaningful once _is_prologue_opcode has already confirmed the block
    is a genuine bootstrap."""
    word_count = len(data) // 2

    for i in range(word_count):
        word = (data[i * 2] << 8) | data[i * 2 + 1]

        if word == _IOXT_OPCODE:
            return BootControllerType.SCSI

        if (word & _IOX_OPCODE_MASK) == _IOX_OPCODE_BASE:
            device_address = word & _IOX_DEVICE_MASK

            if _is_in_device_window(device_address, _SMD_ECC_BASES):
                return BootControllerType.SMD_ECC
            if _is_in_device_window(device_address, _WINCHESTER_BASES):
                return BootControllerType.WINCHESTER
            if _is_in_device_window(device_address, _FLOPPY_BASES):
                return BootControllerType.FLOPPY

    return BootControllerType.UNKNOWN


def _try_load_raw_binary(data: _BufType) -> Optional[BootCode]:
    """Attempts to load a raw hard-disk bootstrap (SMD/ECC, Winchester, SCSI) from
    block 0. Unlike BPUN/FLOMON, raw bootstraps carry no ASCII preamble or delimiter,
    so the only reliable signature is the CPU opcode itself: real boot code always
    starts with PIOF or IOF. Anything else - including data that merely "looks
    non-uniform" - is not bootable."""
    if len(data) < 2:
        return None

    word0 = (data[0] << 8) | data[1]
    if not _is_prologue_opcode(word0):
        return None

    boot = BootCode()
    boot.format = BootFormat.BINARY
    boot.controller_type = _detect_controller_type(data)
    boot.start_address = 0
    boot.load_address = 0
    boot.data = bytes(data[:NDFS_PAGE_SIZE])
    boot.checksum_valid = False
    return boot


def _try_parse_bpun_binary_section(
    data: _BufType,
    pos: int,
) -> Optional[BootCode]:
    """Try to parse BPUN binary section starting after a '!' delimiter.

    Returns a BootCode or None if invalid.
    """
    if pos + 4 > len(data):
        return None

    # Address (2 bytes, big-endian)
    address = (data[pos] << 8) | data[pos + 1]
    pos += 2

    # Count (2 bytes, big-endian) -- word count
    count = (data[pos] << 8) | data[pos + 1]
    pos += 2

    # Validate: count * 2 bytes + checksum(2) + action(2) must fit
    data_bytes = count * 2
    total_needed = pos + data_bytes + 4
    if total_needed > NDFS_PAGE_SIZE:
        return None

    # Read code data and calculate checksum
    code_data = bytearray(data_bytes)
    checksum = 0
    for i in range(0, data_bytes, 2):
        if pos + 1 >= len(data):
            return None
        hi = data[pos]
        lo = data[pos + 1]
        code_data[i] = hi
        code_data[i + 1] = lo
        word = (hi << 8) | lo
        checksum = (checksum + word) & 0xFFFF
        pos += 2

    # Read file checksum (2 bytes, big-endian)
    if pos + 1 >= len(data):
        return None
    file_checksum = (data[pos] << 8) | data[pos + 1]
    pos += 2

    # Check for FLOMON format (address=0, count=0, checksum=0)
    if address == 0 and count == 0 and file_checksum == 0:
        return _load_flomon_format(data, pos)

    # Standard BPUN format
    boot = BootCode()
    boot.format = BootFormat.BPUN
    boot.load_address = address
    boot.word_count = count
    boot.data = bytes(code_data)
    boot.checksum_valid = (file_checksum == checksum)
    boot.start_address = 0
    boot.boot_address = 0

    return boot


def _load_flomon_format(data: _BufType, pos: int) -> BootCode:
    """Load FLOMON format (special BPUN variant for floppy boot)."""
    boot = BootCode()
    boot.format = BootFormat.FLOMON
    boot.load_address = 0

    if pos >= len(data):
        boot.data = b""
        boot.word_count = 0
        return boot

    # Next byte is word count (single byte)
    word_count = data[pos]
    pos += 1
    boot.word_count = word_count

    # FLOMON: each word stored as 4 bytes: 00 HI 00 LO
    code_data = bytearray(word_count * 2)
    code_index = 0

    for _ in range(word_count):
        if pos + 3 >= len(data):
            break

        pad1 = data[pos]
        if pad1 != 0:
            break
        hi = data[pos + 1]
        pad2 = data[pos + 2]
        if pad2 != 0:
            break
        lo = data[pos + 3]
        pos += 4

        code_data[code_index] = hi
        code_data[code_index + 1] = lo
        code_index += 2

    boot.data = bytes(code_data[:code_index])
    boot.checksum_valid = True  # FLOMON has no checksum to validate
    return boot


def load_boot_code(data: _BufType) -> Optional[BootCode]:
    """Load boot code from raw page 0 bytes.

    Args:
        data: First page (at least 2048 bytes) from NDFS disk.

    Returns:
        BootCode with format and data, or None if no valid boot code found.
    """
    if len(data) < NDFS_PAGE_SIZE:
        return None

    # Find all '!' delimiters and try each one
    delimiter_positions = []
    for i in range(min(len(data), NDFS_PAGE_SIZE)):
        if data[i] == BPUN_DELIMITER:
            delimiter_positions.append(i)

    # Try each delimiter position
    for delim_pos in delimiter_positions:
        result = _try_parse_bpun_binary_section(data, delim_pos + 1)
        if result is not None:
            return result

    # Check for raw binary hard-disk bootstrap (SMD/ECC, Winchester, SCSI - not BPUN/FLOMON)
    return _try_load_raw_binary(data)


def detect_boot_format(data: _BufType) -> BootFormat:
    """Detect boot format without loading full code.

    Args:
        data: First page (at least 2048 bytes) from NDFS disk.

    Returns:
        The detected BootFormat.
    """
    boot = load_boot_code(data)
    if boot is None:
        return BootFormat.NONE
    return boot.format


def is_bootable(data: _BufType) -> bool:
    """Check if NDFS disk has valid boot code.

    Args:
        data: First page (at least 2048 bytes) from NDFS disk.

    Returns:
        True if valid boot code exists.
    """
    return detect_boot_format(data) != BootFormat.NONE


def detect_controller_type(data: _BufType) -> BootControllerType:
    """Detect the hard-disk controller family (SMD/ECC, Winchester, SCSI) targeted by
    a raw-binary bootstrap. Returns BootControllerType.UNKNOWN for BPUN/FLOMON
    (floppy) boots or non-bootable disks.

    Args:
        data: First page (at least 2048 bytes) from NDFS disk.

    Returns:
        The detected BootControllerType.
    """
    boot = load_boot_code(data)
    if boot is None:
        return BootControllerType.UNKNOWN
    return boot.controller_type
