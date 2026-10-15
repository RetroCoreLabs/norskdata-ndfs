"""Tests for the SINTRAN initial-command buffer (ndfs.sintran + fs methods).

The pure parse/encode cases mirror ndfs-c/tests/test_sintran.c and the RetroFS.NDFS
C# tests, whose expected values were captured on live K/L/M systems. The locate
test builds a synthetic self-describing segment table; the patch test exercises the
in-place write path end-to-end on a created image.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs import sintran
from ndfs.filesystem import NdfsFileSystem
from ndfs.types import ImageTemplate, ImageCreationOptions


def _fixture(spec: str) -> bytes:
    """'|' marks a NUL pad byte; other chars are literal; padded to 64 bytes."""
    out = bytearray(64)
    for i, ch in enumerate(spec[:64]):
        out[i] = 0x00 if ch == "|" else ord(ch)
    return bytes(out)


THREE = ["ENTER-DIR,,DI-75-1,0", "ENTER-DIR,,DI-74-1,0", "SET-AVAIL"]


# -- Parser --------------------------------------------------------------

def test_parse_two_commands_with_pad():
    assert sintran.parse_buffer(_fixture("AB'|CD'|'"), 0, 64) == ["AB", "CD"]


def test_parse_even_needs_no_pad():
    assert sintran.parse_buffer(_fixture("SET-AVAIL'CC HELLO'|'"), 0, 64) == ["SET-AVAIL", "CC HELLO"]


def test_parse_lone_quote_empty():
    assert sintran.parse_buffer(_fixture("'"), 0, 64) == []


def test_parse_parity_stripped():
    assert sintran.parse_buffer(bytes([0xC1, 0xC2, 0x27, 0x00, 0x27]), 0, 5) == ["AB"]


def test_parse_nonprintable_rejected():
    assert sintran.parse_buffer(bytes([0x01, 0x02, 0x27, 0x27]), 0, 4) is None


# -- Encoder -------------------------------------------------------------

def test_encode_three_textlen54():
    _, tl = sintran.encode_buffer(THREE)
    assert tl == 54


def test_encode_plus_offline_textlen74():
    _, tl = sintran.encode_buffer(THREE + ["CC OFFLINE WRITE OK"])
    assert tl == 74


def test_encode_uppercases():
    b, _ = sintran.encode_buffer(["cc hi"])
    assert b[:5] == b"CC HI"


def test_encode_254_ok_256_rejected():
    _, tl = sintran.encode_buffer(["C" * 253])   # 253 + quote = 254, even
    assert tl == 254
    with pytest.raises(ValueError):
        sintran.encode_buffer(["C" * 255])       # 256


def test_encode_embedded_quote_rejected():
    with pytest.raises(ValueError):
        sintran.encode_buffer(["BAD'CMD"])


def test_encode_parse_roundtrip():
    cmds = THREE + ["CC OFFLINE WRITE OK"]
    enc, tl = sintran.encode_buffer(cmds)
    assert sintran.parse_buffer(enc, 0, tl) == cmds


# -- Self-consistency (the false-positive guard) -------------------------

def test_reencode_matches_rejects_coincidental():
    assert sintran.reencode_matches(["B,"], 2) is False   # re-encodes to 4
    assert sintran.reencode_matches(THREE, 54) is True


# -- Full locate against a synthetic self-describing segment table -------

def _put_word(b: bytearray, off: int, val: int) -> None:
    b[off] = (val >> 8) & 0xFF
    b[off + 1] = val & 0xFF


def _put_seg_entry(b, page, n, logad, segle, madr, flag, sgsta):
    o = page * 2048 + n * 16
    _put_word(b, o + 4, logad)
    _put_word(b, o + 6, segle)
    _put_word(b, o + 8, madr)
    _put_word(b, o + 10, flag)
    _put_word(b, o + 12, sgsta)


def test_locate_synthetic():
    PAGES, TABLE_PAGE, BUF_MADR = 64, 5, 20
    inibu_l = 0o74123
    seg_base = 12 * 1024
    seg = bytearray(PAGES * 2048)

    _put_seg_entry(seg, TABLE_PAGE, 1, 12, 52, BUF_MADR, 1, 0xE000)  # command segment
    _put_seg_entry(seg, TABLE_PAGE, 2, 0, 1, TABLE_PAGE, 1, 0xE000)  # self-describing
    for n in range(3, 41):
        _put_seg_entry(seg, TABLE_PAGE, n, 0, 1, 1, 1, 0xE000)       # plausible filler

    byte_offset = BUF_MADR * 2048 + (inibu_l - seg_base) * 2
    enc, tl = sintran.encode_buffer(THREE)
    seg[byte_offset:byte_offset + len(enc)] = enc
    _put_word(seg, byte_offset + sintran.LEN_CELL_BYTES, tl)

    assert sintran.find_segment_table_page(seg) == TABLE_PAGE
    loc = sintran.locate(seg)
    assert loc is not None
    assert loc.version == "L"
    assert loc.byte_offset == byte_offset
    assert loc.text_length == 54
    assert loc.commands == THREE


# -- In-place patch primitive (the write path), cross-page ---------------

def _make_fs(pages=200):
    return NdfsFileSystem.create_image(ImageCreationOptions(
        template=ImageTemplate.Custom, directory_name="PATCHT", custom_pages=pages))


def test_patch_file_region_crosses_page():
    fs = _make_fs()
    fs.write_file("SYSTEM/BIG:DATA", b"A" * 3000)

    fs.patch_file_region("SYSTEM/BIG:DATA", 2000, b"B" * 100)  # straddles page boundary

    back = fs.read_file("SYSTEM/BIG:DATA")
    assert len(back) == 3000
    assert back[:2000] == b"A" * 2000
    assert back[2000:2100] == b"B" * 100
    assert back[2100:] == b"A" * 900

    # A region past the file's allocated pages is rejected.
    with pytest.raises(IndexError):
        fs.patch_file_region("SYSTEM/BIG:DATA", 4050, b"B" * 100)
