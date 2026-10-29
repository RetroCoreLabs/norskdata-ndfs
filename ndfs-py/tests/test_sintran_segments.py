"""Tests for the SINTRAN segment lister / exporter / importer (ndfs.sintran_segments).

Mirrors ndfs-c/tests/test_sintran_segments.c. A synthetic pack is built with the image
creator: a contiguous SEGFIL0:DATA with a self-describing segment table, a DPIT image
copy whose kernel tables DBLST / ABLPAGE / MASSNO point at the real disk pages of
SEGFIL0 and of a second contiguous segment file SEGFIL1:DATA, an RTFIL:DATA with the
name records, and a symbol list with BLST / ABLPA / MASSN / INIBU. Shapes follow the K
and L packs measured on 2026-10-29.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from typing import Tuple

import pytest
from test_sintran_identity import put_name_record, put_seg_entry, put_word

from ndfs import sintran_segments as ss
from ndfs.filesystem import NdfsFileSystem
from ndfs.parity import set_parity
from ndfs.sintran_identity import CpuIdentityError
from ndfs.types import ImageCreationOptions, ImageTemplate

PAGE = 2048
TABLE_PAGE = 5
SEGFIL0_PAGES = 64
SEGFIL1_PAGES = 16

# The L07 symbol values.
BLST_SYM = 0o4023
ABLPA_SYM = 0o4461
MASSN_SYM = 0o4441

SEG_IDPIT = 0o44
SEG_SDPIT = 0o53
SEG_FS = 0o06        # a code segment on segment file 0
SEG_USER = 0o101     # a user segment on segment file 1
SEG_MEMORY = 0o23    # S3DPIT, memory only
SEG_SAVE4 = 0o54     # a save segment on segment file 4 (absolute)
SEG_IMAGE = 0o02     # S3IMAGE: file 0, MADR 0, real pages

IDPIT_MADR, SDPIT_MADR, FS_MADR, USER_MADR, SAVE4_MADR = 20, 40, 10, 2, 30
DPIT_LOGAD = 0o000102    # base page 2 = 4000B


def _contiguous(blocks) -> bool:
    return all(blocks[i] == blocks[i - 1] + 1 for i in range(1, len(blocks)))


def build_pack(with_symbols: bool) -> Tuple[NdfsFileSystem, int, int]:
    """Build the pack; returns (fs, first disk page of SEGFIL0, first page of SEGFIL1)."""
    fs = NdfsFileSystem.create_image(ImageCreationOptions(
        template=ImageTemplate.Smd75MB, directory_name="PACK-ONE",
        include_extended_info=True, system_number=100, flag_word=0x8000))

    # Both segment files contiguous, as SINTRAN requires (@ALLOCATE-FILE).
    fs.create_contiguous_file("SYSTEM/SEGFIL0:DATA", SEGFIL0_PAGES)
    fs.create_contiguous_file("SYSTEM/SEGFIL1:DATA", SEGFIL1_PAGES)
    blocks0 = fs.get_file_blocks("SYSTEM/SEGFIL0:DATA")
    assert _contiguous(blocks0)
    sf0 = blocks0[0]
    sf1 = fs.get_file_blocks("SYSTEM/SEGFIL1:DATA")[0]

    seg = bytearray(SEGFIL0_PAGES * PAGE)
    seg1 = bytearray(SEGFIL1_PAGES * PAGE)

    # Segment table: self-describing entry 027 (S3ISGT on the real packs), filler,
    # then the real entries; entry 2 is S3IMAGE.
    put_seg_entry(seg, TABLE_PAGE, 0o27, 0, 1, TABLE_PAGE, 1, 0xE000)
    special = {SEG_IDPIT, SEG_SDPIT, SEG_FS, SEG_MEMORY, SEG_SAVE4, SEG_IMAGE, 0o27}
    for n in range(3, 61):
        if n not in special:
            put_seg_entry(seg, TABLE_PAGE, n, 0, 1, 1, 1, 0xE000)
    put_seg_entry(seg, TABLE_PAGE, SEG_FS, 0o000026, 4, FS_MADR, 0o000111, 0o162000)
    put_seg_entry(seg, TABLE_PAGE, SEG_MEMORY, DPIT_LOGAD, 0, 0, 0o000111, 0o162000)
    put_seg_entry(seg, TABLE_PAGE, SEG_IMAGE, 0o000000, 2, 0, 0o000111, 0o162000)
    put_seg_entry(seg, TABLE_PAGE, SEG_IDPIT, DPIT_LOGAD, 4, IDPIT_MADR, 0o000111, 0o162000)
    put_seg_entry(seg, TABLE_PAGE, SEG_SDPIT, DPIT_LOGAD, 4, SDPIT_MADR, 0o000111, 0o162000)
    put_seg_entry(seg, TABLE_PAGE, SEG_SAVE4, 0o000030, 2, SAVE4_MADR, 0o100111, 0o162000)
    put_seg_entry(seg, TABLE_PAGE, SEG_USER, 0o000060, 3, USER_MADR, 0o020111, 0o162000)

    # Recognisable content: every page of a segment carries its number and page index.
    for p in range(4):
        put_word(seg, (FS_MADR + p) * PAGE, 0x5300 | p)
    for p in range(2):
        put_word(seg, p * PAGE, 0x0200 | p)              # S3IMAGE at MADR 0
    for p in range(4):
        put_word(seg, (IDPIT_MADR + p) * PAGE, 0x4400 | p)
    for p in range(4):
        put_word(seg, (SDPIT_MADR + p) * PAGE, 0x5300 | p)
    for p in range(3):
        put_word(seg1, (USER_MADR + p) * PAGE, 0xA100 | p)

    # The kernel tables in the DPIT image copy (base 4000B): DBLST in blocks, 2 per page.
    base = IDPIT_MADR * PAGE
    dblst0, dblst1 = sf0 * 2, sf1 * 2
    put_word(seg, base + (BLST_SYM + 0 - 0o4000) * 2, dblst0 >> 16)
    put_word(seg, base + (BLST_SYM + 1 - 0o4000) * 2, dblst0 & 0xFFFF)
    put_word(seg, base + (BLST_SYM + 2 - 0o4000) * 2, dblst1 >> 16)
    put_word(seg, base + (BLST_SYM + 3 - 0o4000) * 2, dblst1 & 0xFFFF)
    # segment file 2: defined but on another disk unit
    put_word(seg, base + (BLST_SYM + 5 - 0o4000) * 2, 2000)
    put_word(seg, base + (ABLPA_SYM + 0 - 0o4000) * 2, 2)
    put_word(seg, base + (ABLPA_SYM + 1 - 0o4000) * 2, 2)
    put_word(seg, base + (MASSN_SYM + 0 - 0o4000) * 2, 0o1100)
    put_word(seg, base + (MASSN_SYM + 1 - 0o4000) * 2, 0o1100)
    put_word(seg, base + (MASSN_SYM + 2 - 0o4000) * 2, 0o1500)
    put_word(seg, base + (0o4051 - 0o4000) * 2, 100)      # SYSNO, for show

    fs.write_file("SYSTEM/SEGFIL0:DATA", seg)
    fs.write_file("SYSTEM/SEGFIL1:DATA", seg1)

    # Segment file 4 is absolute: disk page 1 + MADR. Page 31 and 32 of a fresh 75 MB pack
    # are free, so writing them raw harms nothing.
    page = bytearray(PAGE)
    put_word(page, 0, 0x5400)
    fs.write_disk_page(1 + SAVE4_MADR, page)
    put_word(page, 0, 0x5401)
    fs.write_disk_page(2 + SAVE4_MADR, page)

    rtfil = bytearray(PAGE)
    put_name_record(rtfil, 0 * 16, "S3IMAGE", SEG_IMAGE)
    put_name_record(rtfil, 1 * 16, "S3FS", SEG_FS)
    put_name_record(rtfil, 2 * 16, "S3DPIT", SEG_MEMORY)
    put_name_record(rtfil, 3 * 16, "S3IDPIT", SEG_IDPIT)
    put_name_record(rtfil, 4 * 16, "S3SDPIT", SEG_SDPIT)
    put_name_record(rtfil, 5 * 16, "S3SPRMA", SEG_SAVE4)
    put_name_record(rtfil, 6 * 16, "USERSEG", SEG_USER)
    fs.write_file("SYSTEM/RTFIL:DATA", rtfil)

    if with_symbols:
        text = ("BLST=004023\r\nDBLST=004023\r\nMASSN=004441\r\nMASSU=004451\r\nABLPA=004461\r\n"
                "SYSNO=004051\r\nINIBU=074123\r\n")
        fs.write_file("SYSTEM/SYMBOL-1-LIST:SYMB", set_parity(text.encode("ascii")))
    return fs, sf0, sf1


def _word(data: bytes, o: int) -> int:
    return (data[o] << 8) | data[o + 1]


# -- Names -----------------------------------------------------------------

def test_collect_names():
    buf = bytearray(128)
    put_name_record(buf, 0, "S3FS", 0o06)
    put_name_record(buf, 16, "S3SDPIT", 0o53)
    put_name_record(buf, 32, "S3SDPIT", 0o54)       # a second record for a name: first wins
    names = ss.collect_segment_names(buf)
    assert len(names) == 3
    assert names[0o06] == "S3FS"
    assert names[0o53] == "S3SDPIT"
    assert names[0o54] == "S3SDPIT"
    assert 1 not in names


# -- Raw page API ----------------------------------------------------------

def test_raw_page_api():
    fs = NdfsFileSystem.create_image(
        ImageCreationOptions(template=ImageTemplate.Floppy12MB, directory_name="FLOPPY"))
    total = fs.total_pages
    assert total > 100                               # the floppy template, whatever its exact size
    page = bytes([0x5A]) * PAGE
    fs.write_disk_page(100, page)
    assert fs.read_disk_page(100) == page
    with pytest.raises(IndexError):
        fs.read_disk_page(total)
    with pytest.raises(IndexError):
        fs.write_disk_page(total, page)
    with pytest.raises(ValueError):
        fs.write_disk_page(100, b"short")


# -- List ------------------------------------------------------------------

def test_list_segments():
    fs, sf0, sf1 = build_pack(True)
    lst = ss.list_segments(fs)
    assert lst.version == "L"
    assert lst.segment_table_page == TABLE_PAGE
    assert lst.have_symbols
    assert lst.names_path == "SYSTEM/RTFIL:DATA"

    # Segment files from the kernel tables.
    assert lst.segfiles[0].defined
    assert lst.segfiles[0].base_page == sf0
    assert lst.segfiles[0].file_path == "SYSTEM/SEGFIL0:DATA"
    assert lst.segfiles[1].defined
    assert lst.segfiles[1].base_page == sf1
    assert lst.segfiles[1].file_path == "SYSTEM/SEGFIL1:DATA"
    assert lst.segfiles[2].defined
    assert not lst.segfiles[2].on_this_disk
    assert not lst.segfiles[3].defined
    assert lst.segfiles[4].defined
    assert lst.segfiles[4].base_page == 1

    # The segments.
    i = ss.find_segment(lst, "S3FS")
    assert i >= 0
    assert lst.segments[i].number == SEG_FS
    assert lst.segments[i].disk_page == sf0 + 10
    assert lst.segments[i].segle == 4
    assert lst.segments[i].segfile == 0
    i = ss.find_segment(lst, "userseg")                 # case-insensitive
    assert i >= 0
    assert lst.segments[i].segfile == 1
    assert lst.segments[i].disk_page == sf1 + 2
    i = ss.find_segment(lst, "S3DPIT")
    assert i >= 0
    assert lst.segments[i].memory_only
    assert lst.segments[i].disk_page == -1
    i = ss.find_segment(lst, "S3IMAGE")                 # MADR 0 is the first page of file 0
    assert i >= 0
    assert not lst.segments[i].memory_only
    assert lst.segments[i].disk_page == sf0
    i = ss.find_segment(lst, "S3SPRMA")
    assert i >= 0
    assert lst.segments[i].segfile == 4
    assert lst.segments[i].disk_page == 31
    # By number: octal with B, decimal without.
    assert ss.find_segment(lst, "S3SDPIT") == ss.find_segment(lst, "53B")
    assert ss.find_segment(lst, "S3SDPIT") == ss.find_segment(lst, "43")
    assert ss.find_segment(lst, "NOSUCH") == -1
    assert ss.find_segment(lst, "60B") >= 0             # filler entry 060B: listed, unnamed


def test_list_without_symbols_falls_back_to_segfil0_file():
    fs, sf0, _sf1 = build_pack(False)
    lst = ss.list_segments(fs)
    assert not lst.have_symbols
    assert lst.version == "?"
    assert lst.segfiles[0].defined
    assert lst.segfiles[0].base_page == sf0
    assert not lst.segfiles[1].defined
    assert not lst.segfiles[4].defined
    i = ss.find_segment(lst, "S3FS")
    assert lst.segments[i].disk_page == sf0 + 10
    i = ss.find_segment(lst, "USERSEG")
    assert lst.segments[i].disk_page == -1           # segment file 1 unknown


# -- Export / import -------------------------------------------------------

def test_export_matches_file_bytes():
    fs, _sf0, _sf1 = build_pack(True)
    lst = ss.list_segments(fs)

    # S3FS: 4 pages at SEGFIL0 page 10 - must equal the file's bytes.
    i = ss.find_segment(lst, "S3FS")
    data = ss.export_segment(fs, lst, i)
    assert len(data) == 4 * PAGE
    file = fs.read_file("SYSTEM/SEGFIL0:DATA")
    assert data == file[10 * PAGE:14 * PAGE]
    assert _word(data, 2 * PAGE) == 0x5302

    # USERSEG on segment file 1.
    i = ss.find_segment(lst, "USERSEG")
    data = ss.export_segment(fs, lst, i)
    assert len(data) == 3 * PAGE
    assert _word(data, PAGE) == 0xA101

    # The absolute save segment on file 4.
    i = ss.find_segment(lst, "S3SPRMA")
    data = ss.export_segment(fs, lst, i)
    assert _word(data, 0) == 0x5400
    assert _word(data, PAGE) == 0x5401

    # S3IMAGE at MADR 0: the first pages of SEGFIL0.
    i = ss.find_segment(lst, "S3IMAGE")
    data = ss.export_segment(fs, lst, i)
    assert _word(data, PAGE) == 0x0201

    # Memory-only: refused.
    i = ss.find_segment(lst, "S3DPIT")
    with pytest.raises(CpuIdentityError):
        ss.export_segment(fs, lst, i)


def test_import_round_trip_and_refusals():
    fs, _sf0, _sf1 = build_pack(True)
    lst = ss.list_segments(fs)
    i = ss.find_segment(lst, "S3FS")

    # Modify the export and write it back; the file view must show it.
    data = bytearray(ss.export_segment(fs, lst, i))
    data[100] = 0xDE
    data[101] = 0xAD
    data[3 * PAGE + 7] = 0x77
    ss.import_segment(fs, lst, i, data)
    assert ss.export_segment(fs, lst, i) == bytes(data)
    back = fs.read_file("SYSTEM/SEGFIL0:DATA")
    assert back[10 * PAGE + 100] == 0xDE
    assert back[13 * PAGE + 7] == 0x77

    # A shorter import keeps the tail of the last page.
    data[0] = 0x11
    data[1] = 0x22
    ss.import_segment(fs, lst, i, data[:10])
    back = ss.export_segment(fs, lst, i)
    assert back[0] == 0x11
    assert back[100] == 0xDE                         # untouched tail of page 0

    # Too long: refused, nothing written.
    big = bytes([0xEE]) * (5 * PAGE)
    with pytest.raises(IndexError):
        ss.import_segment(fs, lst, i, big)
    assert ss.export_segment(fs, lst, i)[0] == 0x11

    # Memory-only: refused.
    i = ss.find_segment(lst, "S3DPIT")
    with pytest.raises(CpuIdentityError):
        ss.import_segment(fs, lst, i, big[:10])
