"""Tests for the SINTRAN CPU number / CPU type reader and writer (ndfs.sintran_identity).

Mirrors ndfs-c/tests/test_sintran_identity.c case for case. A synthetic system pack is
built with the image creator: both symbol lists (with even parity, as the real packs
store them), an RTFIL:DATA with the RT-loader "DEFINED SEGMENT NAME" records, a
SEGFIL0:DATA holding a self-describing segment table and the two DPIT copies, and for
the K shape a SINTRAN:DATA that holds the save copy (segment file 4). The segment
numbers in the fixture are deliberately NOT the real 44B/53B in one case, so a test
proves the lookup goes by name. Shapes and addresses follow the real packs measured on
2026-10-29 (NDInsight chapter SINTRAN/OS/28, sections 4 to 6; chapter 22 section 6.5
for the records).

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from dataclasses import dataclass

import pytest

from ndfs import sintran_identity as si
from ndfs.filesystem import NdfsFileSystem
from ndfs.parity import set_parity, strip_parity
from ndfs.types import ImageCreationOptions, ImageTemplate

PAGE = 2048

# Fixture shape: logical base 4000B (LOGAD low bits = page 2, as on every real pack),
# 35 pages per copy as on the K pack so the K FCPUN at 105034B (page 32 of the copy)
# is inside. SEGFIL0 is 80 pages: table at page 5, image copy at page 20.
# L shape: image copy at SEGFIL0 page 20, save at page 44 (fits 44+35 = 79).
# K shape: save copy at page 10 of SINTRAN:DATA (segment file 4).
TABLE_PAGE = 5
IMAGE_MADR = 20
SAVE_MADR_L = 44
SAVE_MADR_K = 10
SEGLE = 35
LOGAD = 0o000102
SEGFIL_PAGES = 80

SYSNO = 0o4051
HWINF = 0o4052
REVLE = 0o4057
FCPUN_L = 0o4414
FCPUN_K = 0o105034


@dataclass
class Shape:
    k_shape: bool               # save copy on segment file 4 in SINTRAN:DATA
    with_fcpun_symbol: bool     # False: SYMBOL-2-LIST lacks FCPUN
    image_segno: int            # segment numbers the name records give
    save_segno: int
    sintran_data_pages: int     # K only; too short = refuse
    with_name_records: bool     # False: no RTFIL:DATA and no records in SEGFIL0


def l_shape() -> Shape:
    return Shape(False, True, 0o44, 0o53, 0, True)


def k_shape() -> Shape:
    return Shape(True, True, 0o44, 0o53, 48, True)


def put_word(b: bytearray, off: int, val: int) -> None:
    b[off] = (val >> 8) & 0xFF
    b[off + 1] = val & 0xFF


def put_seg_entry(b: bytearray, page: int, n: int, logad: int, segle: int, madr: int,
                  flag: int, sgsta: int) -> None:
    o = page * PAGE + n * 16
    put_word(b, o + 4, logad)
    put_word(b, o + 6, segle)
    put_word(b, o + 8, madr)
    put_word(b, o + 10, flag)
    put_word(b, o + 12, sgsta)


def put_name_record(b: bytearray, off: int, name: str, segno: int) -> None:
    """Pack a name the way the RT-loader does: SIXBIT, right-justified base 64."""
    n = 0
    for ch in name:
        n = (n << 6) | (ord(ch) & 0x3F)
    put_word(b, off + 0, (n >> 32) & 0xFFFF)
    put_word(b, off + 2, (n >> 16) & 0xFFFF)
    put_word(b, off + 4, n & 0xFFFF)
    put_word(b, off + 6, segno)
    put_word(b, off + 8, 0)
    put_word(b, off + 10, 0o25)
    put_word(b, off + 12, segno)
    put_word(b, off + 14, 0)


def put_dpit_words(buf: bytearray, base: int, fcpun_addr: int, sysno: int, cpu_type: int,
                   revle: int, fcpun: int) -> None:
    """Write one identity word set into a DPIT copy that starts at byte `base`."""
    put_word(buf, base + (SYSNO - 0o4000) * 2, sysno)
    put_word(buf, base + (HWINF + 2 - 0o4000) * 2, cpu_type)
    put_word(buf, base + (REVLE - 0o4000) * 2, revle)
    put_word(buf, base + (fcpun_addr - 0o4000) * 2, fcpun)


def write_text_file(fs: NdfsFileSystem, path: str, text: str, parity: bool = True) -> None:
    data = text.encode("ascii")
    fs.write_file(path, set_parity(data) if parity else data)


SYM1_K = "SYSNO=004051\r\nHWINF=004052\r\nREVLE=004057\r\nINIBU=067172\r\n"
SYM2_K = "FCPUN=105034\r\n33CPU=000156\r\n33CPN=000144\r\nSYSNO=004051\r\n"
SYM2_K_NOFCPUN = "33CPU=000156\r\n33CPN=000144\r\nSYSNO=004051\r\n"
SYM1_L = "FCPUN=004414\r\nREVLE=004057\r\nSYSNO=004051\r\nHWINF=004052\r\nINIBU=074123\r\n"
SYM2_L = "GCPUN=061434\r\n33CPU=000000\r\n33CPN=000000\r\n"


def build_pack(sh: Shape) -> NdfsFileSystem:
    opts = ImageCreationOptions(
        template=ImageTemplate.Smd75MB,
        directory_name="PACK-ONE",
        include_extended_info=True,
        system_number=100,
        flag_word=0x8000,       # "entered by system 100", as every real pack here
    )
    fs = NdfsFileSystem.create_image(opts)
    fcpun_addr = FCPUN_K if sh.k_shape else FCPUN_L

    # Segment table: entry 2 self-describing, filler so the page has >= 40 good entries,
    # then the two DPIT copies at the numbers the name records will give.
    seg = bytearray(SEGFIL_PAGES * PAGE)
    put_seg_entry(seg, TABLE_PAGE, 2, 0, 1, TABLE_PAGE, 1, 0xE000)
    for n in range(3, 61):
        if n != sh.image_segno and n != sh.save_segno:
            put_seg_entry(seg, TABLE_PAGE, n, 0, 1, 1, 1, 0xE000)
    put_seg_entry(seg, TABLE_PAGE, sh.image_segno, LOGAD, SEGLE, IMAGE_MADR, 0o000111, 0o162000)
    put_seg_entry(seg, TABLE_PAGE, sh.save_segno, LOGAD, SEGLE,
                  SAVE_MADR_K if sh.k_shape else SAVE_MADR_L,
                  0o100111 if sh.k_shape else 0o000111, 0o162000)

    put_dpit_words(seg, IMAGE_MADR * PAGE, fcpun_addr, 21056, 9883, 0o10200, 21056)
    if not sh.k_shape:
        put_dpit_words(seg, SAVE_MADR_L * PAGE, fcpun_addr, 21056, 9883, 0o10200, 21056)
    fs.write_file("SYSTEM/SEGFIL0:DATA", seg)

    if sh.k_shape:
        sd = bytearray(sh.sintran_data_pages * PAGE)
        if len(sd) >= (SAVE_MADR_K + SEGLE) * PAGE:
            put_dpit_words(sd, SAVE_MADR_K * PAGE, fcpun_addr, 21056, 9883, 0o10200, 21056)
        fs.write_file("SYSTEM/SINTRAN:DATA", sd)

    if sh.with_name_records:
        # RTFIL:DATA as on the real packs: records at page 0 and 1, other names around.
        rtfil = bytearray(PAGE * 2)
        put_name_record(rtfil, 0 * 16, "S3IMAGE", 0o02)
        put_name_record(rtfil, 1 * 16, "S3RTFIL", 0o10)
        put_name_record(rtfil, 2 * 16, "S3DPIT", 0o23)
        put_name_record(rtfil, 3 * 16, "S3ISGT", 0o27)
        put_name_record(rtfil, PAGE + 0 * 16, "S3PATCH", 0o43)
        put_name_record(rtfil, PAGE + 1 * 16, "S3IDPIT", sh.image_segno)
        put_name_record(rtfil, PAGE + 2 * 16, "S3SDPIT", sh.save_segno)
        fs.write_file("SYSTEM/RTFIL:DATA", rtfil)

    write_text_file(fs, "SYSTEM/SYMBOL-1-LIST:SYMB", SYM1_K if sh.k_shape else SYM1_L)
    if sh.k_shape:
        sym2 = SYM2_K if sh.with_fcpun_symbol else SYM2_K_NOFCPUN
    else:
        sym2 = SYM2_L
    write_text_file(fs, "SYSTEM/SYMBOL-2-LIST:SYMB", sym2)
    return fs


# -- Pure helpers ----------------------------------------------------------

def test_find_symbol():
    txt = b"SYSNO=004051\r\n   FCPUN=004414\r\n33CPN=000000\r\nX=7\n"
    assert si.find_symbol(txt, "SYSNO") == (0o4051, 6, 6)
    hit = si.find_symbol(txt, "FCPUN")
    assert hit is not None and hit[0] == 0o4414
    hit = si.find_symbol(txt, "33CPN")
    assert hit is not None and hit[0] == 0
    # A name that is only the tail of another ("CPN" inside "33CPN") must not match.
    assert si.find_symbol(txt, "CPN") is None
    assert si.find_symbol(txt, "HWINF") is None


def test_decode_packed_name():
    # The two worked examples of NDInsight chapter 22 section 6.5.
    assert si.decode_packed_name(0x0004, 0xF325, 0x31D4) == "S3ISGT"
    assert si.decode_packed_name(0x013C, 0xD250, 0x10C3) == "S3RTACC"
    assert si.decode_packed_name(0, 0, 0) is None
    # A code outside A-Z / 0-9 (0x3F) is not a name.
    assert si.decode_packed_name(0, 0, 0x003F) is None


def test_find_segment_by_name():
    buf = bytearray(256)
    put_name_record(buf, 16, "S3IDPIT", 0o44)
    put_name_record(buf, 48, "S3SDPIT", 0o53)
    # A record with the wrong type must be ignored even with the right name.
    put_name_record(buf, 80, "S3DPIT", 0o23)
    put_word(buf, 80 + 10, 0o24)
    assert si.find_segment_by_name(buf, "S3IDPIT") == 0o44
    assert si.find_segment_by_name(buf, "S3SDPIT") == 0o53
    assert si.find_segment_by_name(buf, "S3DPIT") == -1
    assert si.find_segment_by_name(buf, "S3FS") == -1


def test_segment_word_offset():
    # L pack, S3SDPIT: MADR 64, SEGLE 45, LOGAD 000102 -> SYSNO at 64*2048 + 41*2 = 131154.
    assert si.segment_word_offset(0o000102, 45, 64, 0o4051) == 131154
    assert si.segment_word_offset(0o000102, 45, 64, 0o4054) == 131160
    # Below the base or past the end: -1.
    assert si.segment_word_offset(0o000102, 45, 64, 0o3777) == -1
    assert si.segment_word_offset(0o000102, 45, 64, 0o4000 + 45 * 1024) == -1
    # A segment based elsewhere does not contain 4051B.
    assert si.segment_word_offset(0o000114, 45, 64, 0o4051) == -1


# -- Read ------------------------------------------------------------------

def test_read_l_shape():
    fs = build_pack(l_shape())
    ident = si.read_cpu_identity(fs)
    assert ident.version == "L"
    assert ident.sym.sysno == 0o4051
    assert ident.sym.fcpun == 0o4414
    assert ident.sym.cpn33 == 0
    assert ident.segment_table_page == TABLE_PAGE
    assert ident.names_path == "SYSTEM/RTFIL:DATA"
    assert ident.copies[0].segment_number == 0o44
    assert ident.copies[1].segment_number == 0o53
    assert ident.copies[0].present and ident.copies[0].file_found
    assert ident.copies[1].present and ident.copies[1].file_found
    assert ident.copies[1].segment_file == 0
    assert ident.copies[1].file_path == "SYSTEM/SEGFIL0:DATA"
    assert ident.copies[0].sysno == 21056
    assert ident.copies[0].fcpun == 21056
    assert ident.copies[0].cpu_type == 9883
    assert ident.copies[0].revle == 0o10200
    assert ident.copies[1].sysno == 21056
    assert ident.dir_ext_valid
    assert ident.dir_entered
    assert ident.dir_system_number == 100


def test_read_k_shape_save_copy_in_sintran_data():
    fs = build_pack(k_shape())
    ident = si.read_cpu_identity(fs)
    assert ident.version == "K"
    assert ident.sym.fcpun == 0o105034
    assert ident.sym.cpn33 == 0o144
    assert ident.sym.cpu33 == 0o156
    assert ident.copies[1].segment_file == 4
    assert ident.copies[1].file_path == "SYSTEM/SINTRAN:DATA"
    assert ident.copies[1].file_found
    assert ident.copies[1].sysno == 21056
    assert ident.copies[1].fcpun == 21056
    assert ident.copies[1].cpu_type == 9883
    assert ident.copies[0].fcpun == 21056


def test_read_segments_found_by_name_not_number():
    sh = l_shape()
    sh.image_segno = 0o45     # not the real 44B/53B: only the name records say where
    sh.save_segno = 0o54
    fs = build_pack(sh)
    ident = si.read_cpu_identity(fs)
    assert ident.copies[0].segment_number == 0o45
    assert ident.copies[1].segment_number == 0o54
    assert ident.copies[0].sysno == 21056
    assert ident.copies[1].sysno == 21056


def test_read_names_from_segfil_when_rtfil_missing():
    sh = l_shape()
    sh.with_name_records = False
    fs = build_pack(sh)
    # No RTFIL:DATA and no records anywhere: refuse.
    with pytest.raises(si.CpuIdentityError) as ei:
        si.read_cpu_identity(fs)
    assert ei.value.identity.copies[0].segment_number == -1
    # Put the loader table copy inside SEGFIL0 (page 60, as the S3RTFIL segment does).
    rec = bytearray(2 * 16)
    put_name_record(rec, 0, "S3IDPIT", 0o44)
    put_name_record(rec, 16, "S3SDPIT", 0o53)
    fs.patch_file_region("SYSTEM/SEGFIL0:DATA", 60 * PAGE, rec)
    ident = si.read_cpu_identity(fs)
    assert ident.copies[0].segment_number == 0o44
    assert ident.copies[1].sysno == 21056
    assert ident.names_path == "SEGFIL0 (loader table copy)"


def test_read_refuses_without_fcpun_symbol():
    sh = k_shape()
    sh.with_fcpun_symbol = False
    fs = build_pack(sh)
    with pytest.raises(si.CpuIdentityError) as ei:
        si.read_cpu_identity(fs)
    partial = ei.value.identity
    assert partial.sym.sysno == 0o4051    # what was found is still reported
    assert partial.sym.fcpun == -1


def test_read_not_a_system_pack():
    fs = NdfsFileSystem.create_image(
        ImageCreationOptions(template=ImageTemplate.Floppy12MB, directory_name="FLOPPY"))
    with pytest.raises(si.CpuIdentityError):
        si.read_cpu_identity(fs)


# -- Write -----------------------------------------------------------------

def test_write_l_shape_both_copies_symbols_and_stamp():
    fs = build_pack(l_shape())
    after = si.write_cpu_identity(fs, 21238, 9883, keep_stamp=False)
    assert after.copies[0].sysno == 21238
    assert after.copies[0].fcpun == 21238
    assert after.copies[1].sysno == 21238
    assert after.copies[1].fcpun == 21238
    assert after.copies[1].cpu_type == 9883
    assert after.copies[1].revle == 0o10200          # untouched
    assert after.sym.cpn33 == 21238
    assert after.sym.cpu33 == 9883
    assert not after.dir_entered
    assert after.dir_system_number == 0
    assert after.dir_ext_valid                       # checksum rewritten

    # The symbol list kept its line shape, "33CPN=051366", and the new digits carry
    # even parity like the rest of the file ('1' = 0x31 has three 1-bits, so 0xB1).
    sym2 = fs.read_file("SYSTEM/SYMBOL-2-LIST:SYMB")
    plain = strip_parity(sym2)
    at = plain.find(b"33CPN=051366")
    assert at >= 0
    assert sym2[at + 6:at + 12] == set_parity(b"051366")
    assert b"33CPU=023233\r\n" in plain

    # The reopened image agrees: the stamp really went to disk with a valid checksum.
    reopened = NdfsFileSystem(fs.to_buffer(), read_only=True)
    mb = reopened.get_master_block()
    assert mb.ext_valid and not mb.is_directory_entered and mb.ext_last_system_number == 0


def test_write_type_optional_and_keep_stamp():
    fs = build_pack(l_shape())
    after = si.write_cpu_identity(fs, 7, None, keep_stamp=True)
    assert after.copies[0].sysno == 7
    assert after.copies[0].cpu_type == 9883          # kept
    assert after.sym.cpu33 == 0                      # kept
    assert after.dir_entered                         # kept
    assert after.dir_system_number == 100


def test_write_k_shape_writes_sintran_data():
    fs = build_pack(k_shape())
    after = si.write_cpu_identity(fs, 21238, None, keep_stamp=False)
    assert after.copies[0].sysno == 21238
    assert after.copies[0].fcpun == 21238
    assert after.copies[1].sysno == 21238
    assert after.copies[1].fcpun == 21238
    assert after.copies[1].cpu_type == 9883
    assert after.sym.cpn33 == 21238
    assert after.sym.cpu33 == 0o156                  # type not given: kept
    # The save copy really went into SINTRAN:DATA page 10.
    sd = fs.read_file("SYSTEM/SINTRAN:DATA")
    o = SAVE_MADR_K * PAGE + (SYSNO - 0o4000) * 2
    assert (sd[o] << 8) | sd[o + 1] == 21238


def test_write_refuses_when_save_file_too_short():
    sh = k_shape()
    sh.sintran_data_pages = 16     # SYSNO (page 10) is inside, FCPUN (page 10 + 32) is not
    fs = build_pack(sh)
    ident = si.read_cpu_identity(fs)
    assert ident.copies[1].present
    assert ident.copies[1].file_found
    assert ident.copies[1].sysno_offset >= 0
    assert ident.copies[1].fcpun_offset == -1
    assert ident.copies[1].fcpun == -1
    with pytest.raises(si.CpuIdentityError):
        si.write_cpu_identity(fs, 21238, None, keep_stamp=False)
    ident = si.read_cpu_identity(fs)
    assert ident.copies[0].sysno == 21056            # nothing changed
    assert ident.dir_entered


def test_write_rejects_bad_values():
    fs = build_pack(l_shape())
    with pytest.raises(ValueError):
        si.write_cpu_identity(fs, -1)
    with pytest.raises(ValueError):
        si.write_cpu_identity(fs, 65536)
    with pytest.raises(ValueError):
        si.write_cpu_identity(fs, 1, 65536)
    # Nothing was written.
    assert si.read_cpu_identity(fs).copies[0].sysno == 21056
