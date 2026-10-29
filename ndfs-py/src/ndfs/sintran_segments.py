"""SINTRAN III segments on a system pack: list, export, import.

A segment is a run of pages on one of up to five segment files. The segment
table (inside SEGFIL0) gives each segment its LOGAD, SEGLE, MADR and FLAG, and
bits 13-15 of FLAG name the segment file. Where a segment file begins on the
pack comes from the kernel's own tables in the resident data page (DBLST,
ABLPAGE, MASSNO, read from the S3IDPIT copy), with one rule the kernel applies
at every start-up: segment file 4 starts at disk page 1 (OPPSTART:751). Names
come from the RT-loader DEFINED SEGMENT NAME records.

Ported behaviour-for-behaviour from ``ndfs-c/src/sintran_segments.c``. The model
is in NDInsight chapters SINTRAN/OS/11 and 22, the measurements in chapter 28.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Dict, List, Optional, Union

from ndfs.parity import strip_parity
from ndfs.sintran import find_segment_table_page
from ndfs.sintran_identity import (
    SEGNAME_IMAGE_DPIT,
    CpuIdentityError,
    _name_records,
    find_symbol,
    find_system_file,
    segment_word_offset,
)

if TYPE_CHECKING:
    from ndfs.filesystem import NdfsFileSystem

#: SINTRAN knows segment files 0..4.
SEGFILE_COUNT = 5

_PAGE_BYTES = 2048
_SEG_ENTRY_BYTES = 16
_NAME_RECORD_BYTES = 16
_INIBU_TO_VERSION = {0o67172: "K", 0o74123: "L", 0o102327: "M"}
_Buf = Union[bytes, bytearray, memoryview]


@dataclass
class SegFile:
    """One of the five segment files as the kernel tables describe it."""

    defined: bool = False
    dblst: int = 0                  #: first disk block (DBLST), 32-bit
    blocks_per_page: int = 0        #: ABLPAGE
    massno: int = 0                 #: MASSNO: which disk unit
    on_this_disk: bool = True
    base_page: int = -1             #: first disk page on this pack
    source: str = "unknown"         #: where base_page came from
    file_path: str = ""             #: NDFS file starting at base_page, for labelling


@dataclass
class Segment:
    """One segment table entry plus its name and its place on the pack."""

    number: int
    name: str = ""
    segli: int = 0
    prese: int = 0
    logad: int = 0
    segle: int = 0
    madr: int = 0
    flag: int = 0
    sgsta: int = 0
    bpagl: int = 0
    segfile: int = 0                #: (flag >> 13) & 7
    ok5: bool = False               #: flag bit 0 (5OK): loaded in memory; says nothing about disk
    memory_only: bool = False       #: SEGLE 0: no pages on disk (S3DPIT, S3RPIT ...)
    disk_page: int = -1             #: first disk page, -1 when not on a known local file


@dataclass
class SegmentList:
    """Result of :func:`list_segments`."""

    version: str = "?"
    segment_table_page: int = -1
    segfil_path: str = ""
    names_path: str = ""
    have_symbols: bool = False
    blst_sym: int = -1
    ablpa_sym: int = -1
    massn_sym: int = -1
    segfiles: List[SegFile] = field(default_factory=lambda: [SegFile() for _ in range(SEGFILE_COUNT)])
    segments: List[Segment] = field(default_factory=list)


def _word_be(b: _Buf, o: int) -> int:
    return (b[o] << 8) | b[o + 1]


# -- Names -----------------------------------------------------------------

def collect_segment_names(buf: _Buf) -> Dict[int, str]:
    """Every DEFINED SEGMENT NAME record in `buf`: {segment_number: name}. The
    first record for a number wins."""
    names: Dict[int, str] = {}
    if len(buf) < _NAME_RECORD_BYTES:
        return names
    for segno, name in _name_records(buf):
        if segno not in names:
            names[segno] = name
    return names


# -- Symbols ---------------------------------------------------------------

def _read_symbol(fs: "NdfsFileSystem", path: str, name: str) -> Optional[int]:
    hit = find_symbol(strip_parity(fs.read_file(path)), name)
    return None if hit is None else hit[0]


# -- Segment table ---------------------------------------------------------

def _read_entry(seg: _Buf, page: int, n: int) -> Segment:
    o = page * _PAGE_BYTES + n * _SEG_ENTRY_BYTES
    s = Segment(number=n)
    s.segli = _word_be(seg, o + 0)
    s.prese = _word_be(seg, o + 2)
    s.logad = _word_be(seg, o + 4)
    s.segle = _word_be(seg, o + 6) & 0x3FF
    s.madr = _word_be(seg, o + 8)
    s.flag = _word_be(seg, o + 10)
    s.sgsta = _word_be(seg, o + 12)
    s.bpagl = _word_be(seg, o + 14)
    s.segfile = (s.flag >> 13) & 7
    s.ok5 = (s.flag & 1) != 0
    # No pages: S3DPIT, S3RPIT ... MADR 0 is a real offset (S3IMAGE, S3SAVE).
    s.memory_only = s.segle == 0
    s.disk_page = -1
    return s


def _label_file_at_page(fs: "NdfsFileSystem", page: int) -> str:
    """The NDFS file whose first disk page is `page`, for labelling only."""
    if page < 0:
        return ""
    for obj in fs.get_object_entries():
        if obj.pages_in_file == 0:
            continue
        t = (obj.type or "").rstrip("' ")
        candidate = f"{obj.user_name}/{obj.object_name}:{t}"
        try:
            blocks = fs.get_file_blocks(candidate)
        except (FileNotFoundError, OSError):
            continue
        if blocks and blocks[0] == page:
            return candidate
    return ""


def _read_segfile_tables(fs: "NdfsFileSystem", seg: bytes, out: SegmentList,
                         idpit: Optional[Segment]) -> None:
    """Segment-file bases from the kernel tables inside the S3IDPIT copy."""
    for sf in out.segfiles:
        sf.defined = False
        sf.base_page = -1
        sf.source = "unknown"
        sf.file_path = ""
        sf.on_this_disk = True

    # Where the DPIT image copy is inside SEGFIL0 (segment file 0 by construction:
    # the table and the DPIT image are both in SEGFIL0:DATA).
    dpit_ok = (idpit is not None and not idpit.memory_only and out.have_symbols
               and (idpit.madr + idpit.segle) * _PAGE_BYTES <= len(seg))

    if dpit_ok:
        assert idpit is not None
        ablpage0 = 0
        massno0 = 0
        for n in range(SEGFILE_COUNT):
            sf = out.segfiles[n]
            bl = segment_word_offset(idpit.logad, idpit.segle, idpit.madr, out.blst_sym + 2 * n)
            ab = segment_word_offset(idpit.logad, idpit.segle, idpit.madr, out.ablpa_sym + n)
            mn = segment_word_offset(idpit.logad, idpit.segle, idpit.madr, out.massn_sym + n)
            if bl < 0 or ab < 0 or mn < 0:
                continue
            sf.dblst = (_word_be(seg, bl) << 16) | _word_be(seg, bl + 2)
            sf.blocks_per_page = _word_be(seg, ab)
            sf.massno = _word_be(seg, mn)
            if n == 0:
                ablpage0, massno0 = sf.blocks_per_page, sf.massno
            if sf.blocks_per_page == 0:
                sf.blocks_per_page = ablpage0
            sf.on_this_disk = sf.massno == 0 or sf.massno == massno0
            if sf.dblst != 0 and sf.blocks_per_page > 0:
                sf.defined = True
                sf.base_page = sf.dblst // sf.blocks_per_page
                sf.source = "DBLST / ABLPAGE in S3IDPIT"
            elif n == 4 and ablpage0 > 0:
                # Set at every start-up, not stored: BLST(11B) := ABLPAGE(0) + XXSWTYPE
                # (PH-P2-OPPSTART.NPL:751), i.e. one page when XXSWTYPE is 0. Measured on
                # the K packs: the save copy at MADR 27 is disk page 28.
                sf.defined = True
                sf.blocks_per_page = ablpage0
                sf.base_page = 1
                sf.source = "start-up rule, ABLPAGE(0) blocks (OPPSTART:751)"

    # Without the tables, segment file 0 is where SEGFIL0:DATA begins.
    if not out.segfiles[0].defined:
        blocks = fs.get_file_blocks(out.segfil_path)
        if blocks:
            out.segfiles[0].defined = True
            out.segfiles[0].base_page = blocks[0]
            out.segfiles[0].blocks_per_page = 0
            out.segfiles[0].source = "first page of SEGFIL0:DATA (no symbol list)"

    for sf in out.segfiles:
        if sf.defined and sf.on_this_disk:
            sf.file_path = _label_file_at_page(fs, sf.base_page)


# -- List ------------------------------------------------------------------

def list_segments(fs: "NdfsFileSystem") -> SegmentList:
    """Every segment the table uses or the name records name, with the segment
    files and each segment's first disk page. Raises CpuIdentityError when the
    pack has no SEGFIL0 or no segment table."""
    out = SegmentList()

    # Symbols: INIBU for the version letter, BLST / ABLPA / MASSN for the segment files.
    path = find_system_file(fs, "SYMBOL-1-LIST", "SYMB")
    if path is not None:
        inibu = _read_symbol(fs, path, "INIBU")
        if inibu is not None:
            out.version = _INIBU_TO_VERSION.get(inibu, "?")
        blst = _read_symbol(fs, path, "BLST")
        ablpa = _read_symbol(fs, path, "ABLPA")
        massn = _read_symbol(fs, path, "MASSN")
        if blst is not None and ablpa is not None and massn is not None:
            out.have_symbols = True
            out.blst_sym, out.ablpa_sym, out.massn_sym = blst, ablpa, massn

    # SEGFIL0 and its segment table.
    segfil = find_system_file(fs, "SEGFIL0", "DATA")
    if segfil is None:
        segfil = find_system_file(fs, "SEGFIL", "DATA")
    if segfil is None:
        raise CpuIdentityError("no SEGFIL0:DATA")
    out.segfil_path = segfil
    seg = fs.read_file(segfil)
    out.segment_table_page = find_segment_table_page(seg)
    if out.segment_table_page < 0:
        raise CpuIdentityError("no segment table found in SEGFIL0")

    # Names: RTFIL:DATA first, then the loader table copies inside SEGFIL0.
    names: Dict[int, str] = {}
    path = find_system_file(fs, "RTFIL", "DATA")
    if path is not None:
        names = collect_segment_names(fs.read_file(path))
        if names:
            out.names_path = path
    if not out.names_path:
        names = collect_segment_names(seg)
        if names:
            out.names_path = "SEGFIL0 (loader table copy)"

    # Every entry that is used or named.
    idpit: Optional[Segment] = None
    for n in range(1, 256):
        s = _read_entry(seg, out.segment_table_page, n)
        if s.segle == 0 and s.madr == 0 and s.logad == 0 and n not in names:
            continue
        s.name = names.get(n, "")
        if s.name == SEGNAME_IMAGE_DPIT:
            idpit = s
        out.segments.append(s)

    _read_segfile_tables(fs, seg, out, idpit)

    # Disk page of every segment with pages on a known, local segment file. The 5OK bit
    # is NOT a condition: it means "loaded in memory" (SEGIN sets it at run time), and a
    # segment the RT-loader has just written has it clear while its pages are on disk
    # (measured 2026-10-29: segment 101B on a fresh segment file 1, FLAG 020000).
    for s in out.segments:
        sf = out.segfiles[s.segfile]
        if s.memory_only or s.segle == 0:
            continue
        if not sf.defined or not sf.on_this_disk:
            continue
        s.disk_page = sf.base_page + s.madr
    return out


# -- Lookup ----------------------------------------------------------------

def find_segment(seglist: SegmentList, name_or_number: str) -> int:
    """Index into ``seglist.segments`` of the segment named, or numbered (octal
    with a trailing B, decimal without), case-insensitive; -1 when absent."""
    if not name_or_number:
        return -1
    text = name_or_number
    base = 10
    if text[-1] in ("B", "b"):
        base = 8
        text = text[:-1]
    allowed = "01234567" if base == 8 else "0123456789"
    if text and all(ch in allowed for ch in text):
        v = int(text, base)
        for i, s in enumerate(seglist.segments):
            if s.number == v:
                return i
        return -1
    want = name_or_number.upper()
    for i, s in enumerate(seglist.segments):
        if s.name.upper() == want and s.name:
            return i
    return -1


# -- Export / import -------------------------------------------------------

def _check_place(fs: "NdfsFileSystem", seglist: SegmentList, index: int) -> Segment:
    if index < 0 or index >= len(seglist.segments):
        raise ValueError("segment index out of range")
    s = seglist.segments[index]
    if s.memory_only or s.segle == 0 or s.disk_page < 0:
        raise CpuIdentityError(f"segment {s.number} has no disk copy on this pack")
    if s.disk_page + s.segle > fs.total_pages:
        raise IndexError(f"segment {s.number} runs past the end of the image")
    return s


def export_segment(fs: "NdfsFileSystem", seglist: SegmentList, index: int) -> bytes:
    """The segment's pages as they are on the pack (SEGLE * 2048 bytes)."""
    s = _check_place(fs, seglist, index)
    out = bytearray()
    for p in range(s.segle):
        out += fs.read_disk_page(s.disk_page + p)
    return bytes(out)


def import_segment(fs: "NdfsFileSystem", seglist: SegmentList, index: int, data: _Buf) -> None:
    """Write `data` over the segment's pages. Up to SEGLE pages; a last partial
    page keeps its remaining bytes. Raises IndexError when `data` is longer than
    the segment."""
    s = _check_place(fs, seglist, index)
    size = len(data)
    if size > s.segle * _PAGE_BYTES:
        raise IndexError("data is longer than the segment")
    done = 0
    while done < size:
        pg = s.disk_page + done // _PAGE_BYTES
        chunk = min(size - done, _PAGE_BYTES)
        if chunk < _PAGE_BYTES:
            page = bytearray(fs.read_disk_page(pg))
        else:
            page = bytearray(_PAGE_BYTES)
        page[:chunk] = data[done:done + chunk]
        fs.write_disk_page(pg, page)
        done += chunk
