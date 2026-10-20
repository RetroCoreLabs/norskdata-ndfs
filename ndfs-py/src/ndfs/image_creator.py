"""
NDFS image creator: creates new empty NDFS disk images.

The layout follows what real SINTRAN M writes with @CREATE-DIRECTORY, measured in the nd100x
emulator (docs/CREATE-DIRECTORY-ORACLE.md, docs/CREATE-DIRECTORY-PLACEMENT.md):

* the bit file goes at the address of SINTRAN's prompt "OCTAL BIT FILE ADDRESS (-1 END OF DISC)":
  0 gives 9 * floor(floor(capacity / 2) / 9), -1 gives capacity minus the bit file pages;
* the bit file is ceil(capacity / 16384) pages and marks page 0, itself, and the pages from the capacity
  up to the next multiple of 256 (not past the end of the device);
* the user data page, user index block, object data page and object index block are the first four
  free pages going down from ``(BA - 1) OR 15`` (routine RSBPG);
* hard disks get an Extended Info Block with the declared capacity, floppies none.

Templates: Floppy360KB, Floppy12MB, Smd75MB, Winchester74MB, Scsi126MB and Custom. Any SINTRAN disk type
name can be made with :func:`options_for_disk_type`.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Optional

from ndfs.constants import (
    NDFS_PAGE_SIZE,
    NDFS_NAME_MAX,
    MASTER_BLOCK_OFFSET,
    EXTENDED_INFO_OFFSET,
    USER_ENTRY_FLAG,
)
from ndfs.endian import write_uint16_be, write_uint32_be
from ndfs.ndfs_name import write_ndfs_name
from ndfs.bit_file import BitFile
from ndfs.block_pointer import BlockPointer
from ndfs.master_block import MasterBlock
from ndfs.types import PointerType, ImageTemplate, ImageCreationOptions, BootChoice
from ndfs.boot_blobs import boot_blob, SCSI_LAST_PAGE
from ndfs.disk_types import DiskFamily, find_disk_type


@dataclass
class TemplateSpec:
    """What a template fixes. Everything else (where the bit file, the user file and the object file
    go, the bitmap, the Unreserved Pages count) is worked out from these numbers."""
    capacity: int = 0            #: declared directory size in pages (Pages Available)
    device_pages: int = 0        #: length of the image in pages; never less than capacity
    ext_valid: bool = False      #: hard disk: write the Extended Info Block (floppies have none)
    boot: BootChoice = BootChoice.NONE   #: boot area written when the caller says AUTO
    scsi_last_page: bool = False #: copy the real SCSI last page (SINTRAN needs it)


_TEMPLATES = {
    ImageTemplate.Floppy360KB: TemplateSpec(154, 154, False, BootChoice.FLOPPY360, False),
    ImageTemplate.Floppy12MB: TemplateSpec(616, 616, False, BootChoice.FLOPPY616, False),
    # capacity 36945 is DISC-75MB in SINTRAN's name table; 38400 pages is the file size of the real SMD0.IMG.
    ImageTemplate.Smd75MB: TemplateSpec(36945, 38400, True, BootChoice.SMD, False),
    # Real Micropolis 1325 image: file 36864 pages, declared capacity 36396 (DISC-74MB).
    ImageTemplate.Winchester74MB: TemplateSpec(36396, 36864, True, BootChoice.WINCHESTER, False),
    # Real SCSI image: SINTRAN accepts a SCSI disk only with exactly this length and the real last page.
    ImageTemplate.Scsi126MB: TemplateSpec(61036, 64656, True, BootChoice.SCSI, True),
}


def _get_template_spec(template: ImageTemplate) -> TemplateSpec:
    spec = _TEMPLATES.get(template)
    if spec is None:
        raise ValueError(f"No pre-defined spec for template: {template}")
    return spec


def _create_custom_spec(options: ImageCreationOptions) -> TemplateSpec:
    if options.custom_pages is None or options.custom_pages < 32:
        raise ValueError("custom_pages must be specified (>= 32) for Custom template")
    pages = options.custom_pages
    boot = (BootChoice.FLOPPY360 if pages == 154
            else BootChoice.FLOPPY616 if pages == 616 else BootChoice.NONE)
    device = pages
    if options.device_pages is not None and options.device_pages > device:
        device = options.device_pages
    # Floppy sized images carry no Extended Info Block.
    return TemplateSpec(pages, device, pages > 1000, boot, False)


def options_for_disk_type(name: str, directory_name: Optional[str] = None) -> ImageCreationOptions:
    """Image options for a SINTRAN disk type name such as ``DISC-288MB-R``.

    The image has the directory size SINTRAN gives the type and the full disk layout length where that is
    known (the emulator's disk controllers pick their geometry from the file length). The boot area is the
    one of the controller family. Raises ValueError for an unknown name or one whose size is not known.
    """
    t = find_disk_type(name)
    if t is None:
        raise ValueError(f"Unknown SINTRAN disk type: {name}")
    if t.pages == 0:
        raise ValueError(f"{t.name} is a valid SINTRAN disk type name but its size is not known")
    boot = (BootChoice.SMD if t.family == DiskFamily.SMD
            else BootChoice.WINCHESTER if t.family == DiskFamily.WINCHESTER else BootChoice.AUTO)
    return ImageCreationOptions(
        template=ImageTemplate.Custom,
        directory_name=directory_name,
        custom_pages=t.pages,
        device_pages=t.layout_pages if t.layout_pages > t.pages else None,
        include_extended_info=t.pages > 1000,
        boot_choice=boot,
    )


def _mark_page_used(bitmap: bytearray, page: int) -> None:
    """Delegates the addressing to :meth:`BitFile._bit_position` (one place to be wrong)."""
    byte_index, bit_index = BitFile._bit_position(page)
    bitmap[byte_index] |= 1 << bit_index


def _page_is_used(bitmap: bytearray, page: int) -> bool:
    byte_index, bit_index = BitFile._bit_position(page)
    return bool(bitmap[byte_index] & (1 << bit_index))


def _alloc_near_hint(bitmap: bytearray, capacity: int, hint: int) -> int:
    """RSBPG rule (see BitFile.find_free_near): the first free page going down from
    ``hint OR 15``, then word+1 up, word-1 down, ... Marks the page used. Returns -1 if none."""
    word = hint >> 4
    nwords = (capacity + 15) >> 4

    def ok(p: int) -> bool:
        return p < capacity and not _page_is_used(bitmap, p)

    for bit in range(15, -1, -1):
        p = (word << 4) + bit
        if ok(p):
            _mark_page_used(bitmap, p)
            return p
    for step in range(1, nwords + 1):
        w = word + step
        if w < nwords:
            for bit in range(16):
                p = (w << 4) + bit
                if ok(p):
                    _mark_page_used(bitmap, p)
                    return p
        if word >= step:
            w = word - step
            for bit in range(15, -1, -1):
                p = (w << 4) + bit
                if ok(p):
                    _mark_page_used(bitmap, p)
                    return p
    return -1


def _write_extended_info(disk_image: bytearray, ndfs_pages: int, system_number: int, flag_word: int) -> None:
    """Write Extended Info block (bytes 2000-2015)."""
    ext = EXTENDED_INFO_OFFSET
    # Kernel-correct checksum: a 16-bit ADDITIVE sum of the seven words following the checksum slot.
    pages_lo = ndfs_pages & 0xFFFF
    pages_hi = (ndfs_pages >> 16) & 0xFFFF
    checksum = MasterBlock.compute_ext_checksum(0, 0, 0, flag_word, system_number, pages_hi, pages_lo)
    write_uint16_be(disk_image, ext, checksum)
    write_uint16_be(disk_image, ext + 2, 0)
    write_uint16_be(disk_image, ext + 4, 0)
    write_uint16_be(disk_image, ext + 6, 0)
    write_uint16_be(disk_image, ext + 8, flag_word)
    write_uint16_be(disk_image, ext + 10, system_number)
    write_uint32_be(disk_image, ext + 12, ndfs_pages)


def _write_user_entry(disk_image: bytearray, offset: int, user_index: int, name: str,
                      reserved_pages: int, used_pages: int = 0) -> None:
    """Write a single user entry at the given offset."""
    disk_image[offset] = USER_ENTRY_FLAG  # 0x81 valid user
    write_ndfs_name(disk_image, offset + 2, name, NDFS_NAME_MAX)
    write_uint32_be(disk_image, offset + 28, reserved_pages)
    write_uint32_be(disk_image, offset + 32, used_pages)
    disk_image[offset + 37] = user_index & 0xFF
    write_uint16_be(disk_image, offset + 40, 0x04FF)  # default file access (offset 40; 38-39 is an unused gap)


def create_image(options: ImageCreationOptions) -> bytearray:
    """Create a new NDFS disk image in memory.

    Args:
        options: Image creation options specifying template and parameters.

    Returns:
        A bytearray containing the complete disk image.
    """
    if options.template == ImageTemplate.Custom:
        spec = _create_custom_spec(options)
    else:
        spec = _get_template_spec(options.template)
    cap = spec.capacity
    dev = spec.device_pages

    # The bit file: one bit per page, 16384 bits per page, sized by the declared capacity.
    bitmap_pages = (cap + 16383) // 16384

    # Bit file address, the answer to SINTRAN's prompt "OCTAL BIT FILE ADDRESS (-1 END OF DISC)".
    answer = options.bit_file_answer
    if answer == 0:
        ba = 9 * ((cap // 2) // 9)
    elif answer == -1:
        ba = cap - bitmap_pages
    elif answer > 0:
        ba = answer
    else:
        raise ValueError("bit_file_answer must be 0, -1 or a page address")
    if ba < 8 or ba + bitmap_pages > cap:
        raise ValueError("bit file address outside the disk")

    # Pages left for the user quota: capacity - page 0 - the bit file - the user file (index block +
    # data page) - the object file index block. Unreserved Pages is this minus the sum of the user quotas.
    if cap < 40 + bitmap_pages:
        raise ValueError("disk too small")
    avail = cap - 1 - bitmap_pages - 2 - 1
    quota = cap // 2 if cap > 1000 else min(avail, 500)

    disk_image = bytearray(dev * NDFS_PAGE_SIZE)

    # Bitmap, as @CREATE-DIRECTORY leaves it, then the four pages the first user and the first file take.
    bm = bytearray(bitmap_pages * NDFS_PAGE_SIZE)
    _mark_page_used(bm, 0)
    for i in range(bitmap_pages):
        _mark_page_used(bm, ba + i)
    if spec.ext_valid and dev > cap:
        # When the device is longer than the capacity SINTRAN marks the pages from the capacity up to the
        # next multiple of 256 as used (not past the end of the device).
        end = min(((cap + 255) // 256) * 256, dev)
        for i in range(cap, end):
            _mark_page_used(bm, i)
    user_data = _alloc_near_hint(bm, cap, ba - 1)
    user_index = _alloc_near_hint(bm, cap, ba - 1)
    obj_data = _alloc_near_hint(bm, cap, ba - 1)
    obj_index = _alloc_near_hint(bm, cap, ba - 1)
    if min(user_data, user_index, obj_data, obj_index) < 0:
        raise ValueError("no space for the directory files")

    # Boot area: bytes 0..1999 of page 0.
    boot = options.boot_choice if options.boot_choice != BootChoice.AUTO else spec.boot
    if options.boot_data:
        n = min(len(options.boot_data), 2000)
        disk_image[0:n] = options.boot_data[:n]
    elif boot not in (BootChoice.NONE, BootChoice.AUTO):
        disk_image[0:2000] = boot_blob(boot.value)

    # Page 0: master block (and Extended Info Block on hard disks).
    off = MASTER_BLOCK_OFFSET
    dir_name = options.directory_name if options.directory_name else "EMPTY-DISK"
    write_ndfs_name(disk_image, off, dir_name, NDFS_NAME_MAX)
    BlockPointer(obj_index, PointerType.Indexed).to_bytes(disk_image, off + 0x10)
    BlockPointer(user_index, PointerType.Indexed).to_bytes(disk_image, off + 0x14)
    BlockPointer(ba, PointerType.Contiguous).to_bytes(disk_image, off + 0x18)
    write_uint32_be(disk_image, off + 0x1C, avail - quota)

    write_ext = options.include_extended_info or (spec.ext_valid and options.include_extended_info is not False)
    if write_ext:
        system_number = options.system_number if options.system_number is not None else 0
        flag_word = options.flag_word if options.flag_word is not None else 0
        _write_extended_info(disk_image, cap, system_number, flag_word)

    # User file: index block -> data page with the SYSTEM entry. SYSTEM is charged the object data page.
    BlockPointer(user_data, PointerType.Contiguous).to_bytes(disk_image, user_index * NDFS_PAGE_SIZE)
    usr_data_off = user_data * NDFS_PAGE_SIZE
    _write_user_entry(disk_image, usr_data_off, 0, "SYSTEM", quota, 1)
    if options.users:
        for i, info in enumerate(options.users):
            _write_user_entry(disk_image, usr_data_off + (i + 1) * 64, i + 1, info.name, info.reserved_pages)

    # Object file: index block -> data page (no files yet, all zero).
    BlockPointer(obj_data, PointerType.Contiguous).to_bytes(disk_image, obj_index * NDFS_PAGE_SIZE)

    # Bit file.
    disk_image[ba * NDFS_PAGE_SIZE:ba * NDFS_PAGE_SIZE + len(bm)] = bm

    # SCSI: the last page the real disks carry.
    if spec.scsi_last_page:
        disk_image[(dev - 1) * NDFS_PAGE_SIZE:dev * NDFS_PAGE_SIZE] = SCSI_LAST_PAGE

    return disk_image
