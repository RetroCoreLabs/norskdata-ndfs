"""SINTRAN III CPU number and CPU type on a system pack: read / change / verify.

A SINTRAN pack carries its CPU number twice (SYSNO and FCPUN) and its CPU type
once (HWINF+2) in the resident data page. That page is stored on disk in two
segments: the image copy (segment named S3IDPIT, used at a warm start) and the
save copy (S3SDPIT, used at a cold start). Nothing here is hard-coded:

* the word addresses come from the pack's own ``(SYSTEM)SYMBOL-1-LIST:SYMB`` and
  ``SYMBOL-2-LIST:SYMB`` (FCPUN moves from build to build);
* the segment numbers come from the RT-loader "DEFINED SEGMENT NAME" records
  (type 25B) in ``(SYSTEM)RTFIL:DATA`` or their copies inside SEGFIL0;
* the disk pages and the segment file come from the segment table in SEGFIL0.

Ported behaviour-for-behaviour from ``ndfs-c/src/sintran_identity.c``. The
measurements and the derivation are in NDInsight, chapter
``SINTRAN/OS/28-CPU-NUMBER-AND-TYPE-ON-DISK.md``.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING, Iterator, List, Optional, Tuple, Union

from ndfs.master_block import MasterBlock
from ndfs.parity import set_parity, strip_parity
from ndfs.sintran import find_segment_table_page

if TYPE_CHECKING:
    from ndfs.filesystem import NdfsFileSystem

#: Segment names of the two disk copies of the resident data page.
SEGNAME_IMAGE_DPIT = "S3IDPIT"
SEGNAME_SAVE_DPIT = "S3SDPIT"

#: RT-loader record type of a "DEFINED SEGMENT NAME" record (octal 25).
RTFIL_TYPE_SEGMENT_NAME = 0o25

#: A segment name is at most 8 SIXBIT characters (three 16-bit words).
SEGNAME_MAX = 8

_PAGE_BYTES = 2048
_SEG_ENTRY_BYTES = 16
_NAME_RECORD_BYTES = 16

#: Anything the readers accept as a byte buffer.
_Buf = Union[bytes, bytearray, memoryview]

# INIBU per version, as sintran.py. Only used to name the version letter;
# nothing is addressed through it.
_INIBU_TO_VERSION = {0o67172: "K", 0o74123: "L", 0o102327: "M"}


class CpuIdentityError(LookupError):
    """A pack does not hold what the reader or writer needs (no symbol list, no
    name records, a copy whose file does not hold the word, ...). The message
    says which piece is missing."""


@dataclass
class SintranSymbols:
    """Word addresses read from the pack's symbol lists; -1 when absent."""

    sysno: int = -1
    hwinf: int = -1
    revle: int = -1
    fcpun: int = -1
    cpn33: int = -1   #: 33CPN: the CPU number the generation was asked for
    cpu33: int = -1   #: 33CPU: the CPU type the generation was asked for
    inibu: int = -1


@dataclass
class DpitCopy:
    """One disk copy of the resident data page and the identity words in it."""

    name: str
    role: str                       #: "image" or "save"
    segment_number: int = -1        #: from the name records; -1 when unresolved
    present: bool = False           #: table entry has pages and covers SYSNO
    logad: int = 0
    segle: int = 0
    madr: int = 0
    flag: int = 0
    segment_file: int = 0           #: (flag >> 13) & 7
    file_path: str = ""             #: NDFS file holding the segment file, "" when unknown
    file_found: bool = False
    sysno_offset: int = -1          #: byte offsets inside file_path; -1 = not reachable
    fcpun_offset: int = -1
    type_offset: int = -1
    revle_offset: int = -1
    sysno: int = -1
    fcpun: int = -1
    cpu_type: int = -1
    revle: int = -1


@dataclass
class CpuIdentity:
    """Everything the reader found. Filled as far as the pack allows, so a
    refusal still reports what was there."""

    version: str = "?"
    sym: SintranSymbols = field(default_factory=SintranSymbols)
    symbol1_path: str = ""
    symbol2_path: str = ""
    names_path: str = ""            #: where the segment numbers came from
    cpn33_value_offset: int = -1    #: byte offset of the 33CPN digits in symbol2_path
    cpu33_value_offset: int = -1
    cpn33_value_width: int = 0
    cpu33_value_width: int = 0
    symbol2_has_parity: bool = False
    segment_table_page: int = -1
    copies: List[DpitCopy] = field(default_factory=lambda: [
        DpitCopy(SEGNAME_IMAGE_DPIT, "image"),
        DpitCopy(SEGNAME_SAVE_DPIT, "save"),
    ])
    dir_ext_valid: bool = False
    dir_entered: bool = False
    dir_system_number: int = 0


# -- Pure helpers ----------------------------------------------------------

def _word_be(b: _Buf, o: int) -> int:
    return (b[o] << 8) | b[o + 1]


def segment_word_offset(logad: int, segle: int, madr: int, addr: int) -> int:
    """Byte offset of word address `addr` inside the segment file, or -1 when the
    segment does not cover it. The logical base is the low six bits of LOGAD in
    pages: byte = MADR*2048 + 2*(addr - base)."""
    base = (logad & 0o77) * 1024
    if addr < base or addr >= base + segle * 1024:
        return -1
    return madr * _PAGE_BYTES + (addr - base) * 2


def find_symbol(text: _Buf, name: str) -> Optional[Tuple[int, int, int]]:
    """Find ``NAME=OOOOOO`` at the start of a line (leading blanks allowed) in a
    parity-stripped symbol list. Returns (value, offset_of_first_digit, digit_count)
    or None. The name must be followed by '=' so "CPN" never matches "33CPN"."""
    if not name:
        return None
    nb = name.encode("ascii")
    nlen = len(nb)
    n = len(text)
    i = 0
    while i < n:
        p = i
        while p < n and text[p] in (0x20, 0x09):
            p += 1
        if p + nlen < n and bytes(text[p:p + nlen]) == nb and text[p + nlen] == 0x3D:
            d = p + nlen + 1
            value = 0
            width = 0
            while d + width < n and 0x30 <= text[d + width] <= 0x37:
                value = value * 8 + (text[d + width] - 0x30)
                width += 1
            if width > 0:
                return value, d, width
        while i < n and text[i] not in (0x0A, 0x0D):
            i += 1
        while i < n and text[i] in (0x0A, 0x0D):
            i += 1
    return None


def decode_packed_name(w0: int, w1: int, w2: int) -> Optional[str]:
    """Decode an RT-loader packed segment name: SIXBIT characters, right-justified
    base 64 across three words. None when the words are not a name."""
    n = (w0 << 32) | (w1 << 16) | w2
    if n == 0:
        return None
    chars: List[str] = []
    while n != 0:
        c6 = n & 0x3F
        n >>= 6
        if len(chars) >= SEGNAME_MAX:
            return None
        # SIXBIT: 1..26 are A..Z (ASCII & 0x3F), 0x30..0x39 are the digits.
        if 1 <= c6 <= 26:
            chars.append(chr(ord("A") + c6 - 1))
        elif 0x30 <= c6 <= 0x39:
            chars.append(chr(c6))
        else:
            return None
    chars.reverse()
    return "".join(chars)


def _name_records(buf: _Buf) -> Iterator[Tuple[int, str]]:
    """Yield (segment_number, name) for every well-formed DEFINED SEGMENT NAME
    record in `buf`, scanning on word boundaries."""
    size = len(buf)
    o = 0
    while o + _NAME_RECORD_BYTES <= size:
        if (_word_be(buf, o + 10) == RTFIL_TYPE_SEGMENT_NAME
                and _word_be(buf, o + 8) == 0 and _word_be(buf, o + 14) == 0):
            segno = _word_be(buf, o + 6)
            if 0 < segno <= 0o377 and segno == _word_be(buf, o + 12):
                name = decode_packed_name(_word_be(buf, o), _word_be(buf, o + 2), _word_be(buf, o + 4))
                if name is not None:
                    yield segno, name
        o += 2


def find_segment_by_name(buf: _Buf, name: str) -> int:
    """Segment number the first DEFINED SEGMENT NAME record for `name` gives, or -1."""
    if len(buf) < _NAME_RECORD_BYTES:
        return -1
    for segno, decoded in _name_records(buf):
        if decoded == name:
            return segno
    return -1


def _trim_type(t: str) -> str:
    return t.rstrip("' ")


def find_system_file(fs: "NdfsFileSystem", name: str, want_type: str) -> Optional[str]:
    """"USER/NAME:TYPE" of the first object owned by SYSTEM with this name and
    type (case-insensitive), or None."""
    for obj in fs.get_object_entries():
        if obj.user_name.upper() != "SYSTEM":
            continue
        if obj.object_name.upper() != name.upper():
            continue
        t = _trim_type(obj.type or "")
        if t.upper() != want_type.upper():
            continue
        return f"{obj.user_name}/{obj.object_name}:{t}"
    return None


def _find_segfil(fs: "NdfsFileSystem") -> Optional[str]:
    """SEGFIL0:DATA, or the older name SEGFIL:DATA."""
    path = find_system_file(fs, "SEGFIL0", "DATA")
    if path is None:
        path = find_system_file(fs, "SEGFIL", "DATA")
    return path


# -- Symbol lists ----------------------------------------------------------

def _read_symlist(fs: "NdfsFileSystem", path: str) -> Tuple[bytes, bool]:
    """(parity-stripped text, had_parity)."""
    raw = fs.read_file(path)
    has_parity = any(b & 0x80 for b in raw)
    return strip_parity(raw), has_parity


def _sym_or_minus_one(a: bytes, b: bytes, name: str) -> int:
    hit = find_symbol(a, name)
    if hit is None:
        hit = find_symbol(b, name)
    return -1 if hit is None else hit[0]


# -- Segment names and table ----------------------------------------------

def _seg_entry(seg: _Buf, page: int, n: int) -> Tuple[int, int, int, int]:
    o = page * _PAGE_BYTES + n * _SEG_ENTRY_BYTES
    return (_word_be(seg, o + 4), _word_be(seg, o + 6) & 0x3FF,
            _word_be(seg, o + 8), _word_be(seg, o + 10))


def _resolve_segment_names(fs: "NdfsFileSystem", seg: bytes, out: CpuIdentity) -> None:
    """Both DPIT names to segment numbers: RTFIL:DATA first (small), then the
    copies of the loader table inside SEGFIL0."""
    for c in out.copies:
        c.segment_number = -1
    out.names_path = ""

    path = find_system_file(fs, "RTFIL", "DATA")
    if path is not None:
        rt = fs.read_file(path)
        for c in out.copies:
            c.segment_number = find_segment_by_name(rt, c.name)
        if all(c.segment_number > 0 for c in out.copies):
            out.names_path = path
            return

    # Fall back to the S3RTFIL segment pages inside SEGFIL0.
    for c in out.copies:
        if c.segment_number <= 0:
            c.segment_number = find_segment_by_name(seg, c.name)
    if all(c.segment_number > 0 for c in out.copies):
        out.names_path = "SEGFIL0 (loader table copy)"


def _segment_file_path(fs: "NdfsFileSystem", segment_file: int, segfil_path: str) -> Optional[str]:
    """The file a segment file number names. 0 is SEGFIL0; on K the save copy sits
    on segment file 4, which the K release information places in SINTRAN:DATA.
    Other numbers are reported and never written."""
    if segment_file == 0:
        return segfil_path
    if segment_file == 4:
        return find_system_file(fs, "SINTRAN", "DATA")
    return None


def _offset_in_file(offset: int, file_size: int) -> int:
    """A word offset is usable only when the file really holds it; otherwise -1.
    The K pack's SINTRAN:DATA is 60 pages while S3SDPIT is listed as 35 pages from
    page 27, so the segment's tail is past the file end but every identity word
    is inside it. The check is therefore per word, not per segment."""
    if offset < 0 or offset + 2 > file_size:
        return -1
    return offset


def _read_copy_words(fs: "NdfsFileSystem", sym: SintranSymbols, c: DpitCopy) -> None:
    """Read the four identity words of one copy out of its file."""
    c.sysno = c.fcpun = c.cpu_type = c.revle = -1
    c.sysno_offset = segment_word_offset(c.logad, c.segle, c.madr, sym.sysno)
    c.fcpun_offset = segment_word_offset(c.logad, c.segle, c.madr, sym.fcpun)
    c.type_offset = (segment_word_offset(c.logad, c.segle, c.madr, sym.hwinf + 2)
                     if sym.hwinf >= 0 else -1)
    c.revle_offset = (segment_word_offset(c.logad, c.segle, c.madr, sym.revle)
                      if sym.revle >= 0 else -1)
    c.file_found = False
    if not c.file_path:
        return
    try:
        data = fs.read_file(c.file_path)
    except (FileNotFoundError, OSError):
        return
    size = len(data)
    # The file must at least reach the copy's first page; each word is then
    # checked against the file length on its own.
    if size > c.madr * _PAGE_BYTES:
        c.file_found = True
        c.sysno_offset = _offset_in_file(c.sysno_offset, size)
        c.fcpun_offset = _offset_in_file(c.fcpun_offset, size)
        c.type_offset = _offset_in_file(c.type_offset, size)
        c.revle_offset = _offset_in_file(c.revle_offset, size)
        if c.sysno_offset >= 0:
            c.sysno = _word_be(data, c.sysno_offset)
        if c.fcpun_offset >= 0:
            c.fcpun = _word_be(data, c.fcpun_offset)
        if c.type_offset >= 0:
            c.cpu_type = _word_be(data, c.type_offset)
        if c.revle_offset >= 0:
            c.revle = _word_be(data, c.revle_offset)


# -- Read ------------------------------------------------------------------

def read_cpu_identity(fs: "NdfsFileSystem") -> CpuIdentity:
    """Read the CPU number and type from both DPIT copies, the generation symbols
    and the directory stamp. Raises CpuIdentityError when the pack has no symbol
    lists, no SYSNO/FCPUN symbol, no segment table or no name records; the
    partially filled result travels on the exception as ``.identity``."""
    out = CpuIdentity()

    # Directory stamp first: it needs no SINTRAN at all.
    mb = fs.get_master_block()
    out.dir_ext_valid = mb.ext_valid
    out.dir_entered = (mb.ext_flag_word & MasterBlock.EXT_FLAG_ENTERED) != 0
    out.dir_system_number = mb.ext_last_system_number

    # The pack's own symbol lists.
    p1 = find_system_file(fs, "SYMBOL-1-LIST", "SYMB")
    p2 = find_system_file(fs, "SYMBOL-2-LIST", "SYMB")
    if p1 is None or p2 is None:
        _refuse(out, "not a SINTRAN system pack: no SYMBOL-1-LIST / SYMBOL-2-LIST under SYSTEM")
    assert p1 is not None and p2 is not None
    out.symbol1_path, out.symbol2_path = p1, p2
    s1, _ = _read_symlist(fs, p1)
    s2, out.symbol2_has_parity = _read_symlist(fs, p2)

    out.sym.sysno = _sym_or_minus_one(s1, s2, "SYSNO")
    out.sym.hwinf = _sym_or_minus_one(s1, s2, "HWINF")
    out.sym.revle = _sym_or_minus_one(s1, s2, "REVLE")
    out.sym.fcpun = _sym_or_minus_one(s2, s1, "FCPUN")   # K keeps it in list 2
    out.sym.inibu = _sym_or_minus_one(s1, s2, "INIBU")
    hit = find_symbol(s2, "33CPN")
    if hit is not None:
        out.sym.cpn33, out.cpn33_value_offset, out.cpn33_value_width = hit
    hit = find_symbol(s2, "33CPU")
    if hit is not None:
        out.sym.cpu33, out.cpu33_value_offset, out.cpu33_value_width = hit

    out.version = _INIBU_TO_VERSION.get(out.sym.inibu, "?")

    # Without SYSNO and FCPUN nothing can be addressed safely.
    if out.sym.sysno < 0 or out.sym.fcpun < 0:
        _refuse(out, "SYSNO or FCPUN missing from the symbol lists")

    # The segment table inside SEGFIL0.
    segfil_path = _find_segfil(fs)
    if segfil_path is None:
        _refuse(out, "no SEGFIL0:DATA")
    assert segfil_path is not None
    seg = fs.read_file(segfil_path)
    out.segment_table_page = find_segment_table_page(seg)
    if out.segment_table_page < 0:
        _refuse(out, "no segment table found in SEGFIL0")

    # The segment numbers, by name, from the pack's RT-loader records.
    _resolve_segment_names(fs, seg, out)
    if any(c.segment_number <= 0 for c in out.copies):
        _refuse(out, "DEFINED SEGMENT NAME records for S3IDPIT / S3SDPIT not found")

    for c in out.copies:
        logad, segle, madr, flag = _seg_entry(seg, out.segment_table_page, c.segment_number)
        c.logad, c.segle, c.madr, c.flag = logad, segle, madr, flag
        c.segment_file = (flag >> 13) & 7
        c.file_path = ""
        # Usable: a length and a range that covers SYSNO. The logical base comes from
        # the entry itself, not from a constant. The 5OK bit is not required: it means
        # "loaded in memory" (SEGIN sets it at run time), not "has pages on disk".
        c.present = segle > 0 and segment_word_offset(logad, segle, madr, out.sym.sysno) >= 0
        if not c.present:
            continue
        path = _segment_file_path(fs, c.segment_file, segfil_path)
        if path is None:
            continue
        c.file_path = path
        _read_copy_words(fs, out.sym, c)
    return out


def _refuse(partial: CpuIdentity, why: str) -> None:
    err = CpuIdentityError(why)
    err.identity = partial  # type: ignore[attr-defined]
    raise err


# -- Write -----------------------------------------------------------------

def _patch_word(fs: "NdfsFileSystem", path: str, offset: int, value: int) -> None:
    if offset < 0:
        raise CpuIdentityError(f"{path}: word not reachable")
    fs.patch_file_region(path, offset, bytes([(value >> 8) & 0xFF, value & 0xFF]))


def _patch_symbol_value(fs: "NdfsFileSystem", path: str, offset: int, width: int,
                        parity: bool, value: int) -> None:
    """Rewrite the octal digits of a ``NAME=OOOOOO`` symbol in place, same width,
    with the list's own parity convention."""
    if offset < 0 or width <= 0 or width > 15:
        raise CpuIdentityError(f"{path}: symbol value not reachable")
    # The value must fit the digits that are there; 6 digits hold any 16-bit word.
    if width < 6 and value >= (1 << (3 * width)):
        raise ValueError(f"{value} does not fit {width} octal digits")
    digits = bytearray(width)
    v = value
    for i in range(width - 1, -1, -1):
        digits[i] = 0x30 + (v & 7)
        v >>= 3
    out = set_parity(bytes(digits)) if parity else bytes(digits)
    fs.patch_file_region(path, offset, out)


def write_cpu_identity(fs: "NdfsFileSystem", cpu_number: int, cpu_type: Optional[int] = None,
                       keep_stamp: bool = False) -> CpuIdentity:
    """Set the CPU number (SYSNO and FCPUN) in both DPIT copies, the CPU type
    (HWINF+2) when given, rewrite 33CPN / 33CPU in SYMBOL-2-LIST so a later ND
    patch run keeps the values, clear the directory's "entered by system N" stamp
    unless `keep_stamp`, then read everything back and compare.

    Nothing is written unless every target word is reachable. Raises ValueError
    for a value outside 0..65535, CpuIdentityError when a target is missing, and
    OSError when the read-back does not match what was written."""
    if cpu_number < 0 or cpu_number > 0xFFFF:
        raise ValueError("cpu_number must be 0..65535")
    if cpu_type is not None and (cpu_type < 0 or cpu_type > 0xFFFF):
        raise ValueError("cpu_type must be 0..65535")

    ident = read_cpu_identity(fs)

    # Everything must be addressable before the first byte is written.
    for c in ident.copies:
        if not c.present or not c.file_found:
            raise CpuIdentityError(f"{c.name} ({c.role} copy) has no reachable disk copy")
        if c.sysno_offset < 0 or c.fcpun_offset < 0:
            raise CpuIdentityError(f"{c.name}: SYSNO or FCPUN is outside {c.file_path}")
        if cpu_type is not None and c.type_offset < 0:
            raise CpuIdentityError(f"{c.name}: HWINF+2 is outside {c.file_path}")

    for c in ident.copies:
        _patch_word(fs, c.file_path, c.sysno_offset, cpu_number)
        _patch_word(fs, c.file_path, c.fcpun_offset, cpu_number)
        if cpu_type is not None:
            _patch_word(fs, c.file_path, c.type_offset, cpu_type)

    # The generation symbols, so a later ND patch run keeps the new values.
    if ident.cpn33_value_offset >= 0:
        _patch_symbol_value(fs, ident.symbol2_path, ident.cpn33_value_offset,
                            ident.cpn33_value_width, ident.symbol2_has_parity, cpu_number)
    if cpu_type is not None and ident.cpu33_value_offset >= 0:
        _patch_symbol_value(fs, ident.symbol2_path, ident.cpu33_value_offset,
                            ident.cpu33_value_width, ident.symbol2_has_parity, cpu_type)

    if not keep_stamp and (ident.dir_entered or ident.dir_system_number != 0):
        fs.release_directory()

    # Read back and compare.
    after = read_cpu_identity(fs)
    for c in after.copies:
        if c.sysno != cpu_number or c.fcpun != cpu_number:
            raise OSError(f"read-back of {c.name} does not show CPU number {cpu_number}")
        if cpu_type is not None and c.cpu_type != cpu_type:
            raise OSError(f"read-back of {c.name} does not show CPU type {cpu_type}")
    if after.cpn33_value_offset >= 0 and after.sym.cpn33 != cpu_number:
        raise OSError("read-back of 33CPN does not show the new CPU number")
    if cpu_type is not None and after.cpu33_value_offset >= 0 and after.sym.cpu33 != cpu_type:
        raise OSError("read-back of 33CPU does not show the new CPU type")
    if not keep_stamp and (after.dir_entered or after.dir_system_number != 0):
        raise OSError("the directory stamp is still set after the release")
    return after
