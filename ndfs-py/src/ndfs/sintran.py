"""SINTRAN III initial-command buffer: locate / parse / encode / repair.

The initial commands are the list SINTRAN runs first at every restart
(``@LIST-INITIAL-COMMANDS``). The buffer has no file form: it lives at the
kernel symbol INIBU inside the SINTRAN command segment, whose disk image is
stored in the ``(SYSTEM)SEGFIL0:DATA`` object file, and is located WITHOUT
byte-searching from the INIBU symbol value plus the segment table read off
SEGFIL0 itself.

Ported behaviour-for-behaviour from ``ndfs-c/src/sintran.c`` and the RetroFS.NDFS
C# implementation, both live-verified on real K03/L07/M06 packs. See
``docs/SINTRAN-INITIAL-COMMANDS-SPEC.md`` for the full derivation.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Sequence, Tuple

# Kernel symbol INIBU per SINTRAN version (octal in the symbol lists):
# K03 = 067172, L07 = 074123, M06 = 102327.
INIBU = {"K": 0o67172, "L": 0o74123, "M": 0o102327}

# Region layout (identical in K03/L07/M06):
#   INIBU+0..+129  command text  (260 bytes)
#   INIBU+130      length cell    (byte count of the text)
#   INIBU+131      INCOM          (executable code - never written)
CAP_WORDS = 131
CAP_BYTES = 262
LEN_CELL_BYTES = 260
MAX_TEXT_BYTES = 254  # NEXIN handler literal 252 -> (252+2) & ~1

_SEG_ENTRY_BYTES = 16
_PAGE_BYTES = 2048
_CMD_END = 0x27  # "'" terminates a command; alone = end of buffer
_VERSION_ORDER = ("L", "M", "K")


@dataclass
class InitialCommands:
    """Decoded initial-command buffer and where it was found in SEGFIL0."""

    version: str
    segment_number: int
    madr: int
    byte_offset: int
    segment_table_page: int
    text_length: int
    length_cell_offset: int
    commands: List[str] = field(default_factory=list)


@dataclass
class InitCmdCandidate:
    """One INIBU-containing segment candidate (diagnostic)."""

    version: str
    segment_number: int
    madr: int
    byte_offset: int
    length_cell: int
    length_plausible: bool
    consistent: bool
    commands: Optional[List[str]]


def _word_be(seg, o: int) -> int:
    return (seg[o] << 8) | seg[o + 1]


def _seg_entry(seg, page: int, n: int):
    o = page * _PAGE_BYTES + n * _SEG_ENTRY_BYTES
    return {
        "logad": _word_be(seg, o + 4),
        "segle": _word_be(seg, o + 6) & 0x3FF,
        "madr": _word_be(seg, o + 8),
        "flag": _word_be(seg, o + 10),
        "sgsta": _word_be(seg, o + 12),
    }


def find_segment_table_page(seg) -> int:
    """Locate the self-describing segment-table page; -1 if not found."""
    pages = len(seg) // _PAGE_BYTES
    best, best_good = -1, -1
    for p in range(pages):
        base = p * _PAGE_BYTES
        if any(seg[base + i] != 0 for i in range(_SEG_ENTRY_BYTES)):
            continue  # entry 0 must be all-zero
        good, self_ref = 0, False
        for n in range(1, 128):
            e = _seg_entry(seg, p, n)
            if e["segle"] == 0 and e["madr"] == 0 and e["logad"] == 0:
                continue
            if (0 < e["segle"] < 1024 and e["madr"] > 0
                    and (e["flag"] & 1) and (e["sgsta"] & 0xE000)):
                good += 1
                if e["madr"] == p and e["segle"] <= 32:
                    self_ref = True
        if good >= 40 and self_ref and good > best_good:
            best, best_good = p, good
    return best


def _is_printable(s: str) -> bool:
    return len(s) > 0 and all(0x20 <= ord(c) <= 0x7E for c in s)


def parse_buffer(seg, off: int, cap_bytes: int) -> Optional[List[str]]:
    """Parse a buffer region: text + 0x27, word-aligned; a lone 0x27 ends it.

    Returns the command list (possibly empty), or ``None`` when the region does
    not decode to a plausible buffer (non-printable payload).
    """
    cmds: List[str] = []
    i = off
    end = min(off + cap_bytes, len(seg))
    while i < end:
        if seg[i] == 0x00:
            i += 1
            continue  # word-alignment padding
        if seg[i] == _CMD_END:
            break  # empty command = terminator
        start = i
        while i < end and seg[i] != _CMD_END:
            i += 1
        s = "".join(chr(seg[k] & 0x7F) for k in range(start, i))
        i += 1  # consume the "'"
        if i < end and seg[i] == 0x00 and ((i - off) & 1):
            i += 1
        if not _is_printable(s):
            return None
        cmds.append(s)
    return cmds


def encode_buffer(commands: Sequence[str],
                  max_text_bytes: int = MAX_TEXT_BYTES) -> Tuple[bytes, int]:
    """Encode a command list into buffer bytes.

    Returns ``(bytes, text_len)`` where ``text_len`` is the length-cell value
    (text bytes EXCLUDING the terminator). Commands are upper-cased; a command
    that is empty, contains a ``'`` (0x27), or a non-printable character raises
    ``ValueError``, and an over-long text (> ``max_text_bytes``) raises
    ``ValueError``.
    """
    limit = max_text_bytes if max_text_bytes > 0 else MAX_TEXT_BYTES
    out = bytearray()
    for cmd in commands:
        c = (cmd or "").upper()
        if not _is_printable(c) or "'" in c:
            raise ValueError(f"invalid command: {cmd!r}")
        out.extend(ord(ch) & 0x7F for ch in c)
        out.append(_CMD_END)
        if len(out) & 1:
            out.append(0x00)  # word-align
    text_len = len(out)  # the length-cell value
    if text_len > limit:
        raise ValueError(f"buffer overflow: text is {text_len} bytes, max {limit}")
    out.append(_CMD_END)  # buffer terminator
    if len(out) & 1:
        out.append(0x00)
    return bytes(out), text_len


def reencode_matches(commands: Sequence[str], stored_text_len: int) -> bool:
    """True when re-encoding ``commands`` reproduces exactly ``stored_text_len``
    bytes, i.e. a self-consistent round trip (a REAL buffer) rather than a
    coincidental parse of unrelated bytes.
    """
    try:
        _, text_len = encode_buffer(commands)
    except ValueError:
        return False
    return text_len == stored_text_len


def build_region_image(encoded: bytes, text_len: int) -> bytes:
    """Build the 262-byte region image (text area + length cell)."""
    if len(encoded) > LEN_CELL_BYTES:
        raise ValueError(f"encoded buffer is {len(encoded)} bytes, exceeds text area {LEN_CELL_BYTES}")
    region = bytearray(CAP_BYTES)
    region[: len(encoded)] = encoded
    region[LEN_CELL_BYTES] = (text_len >> 8) & 0xFF
    region[LEN_CELL_BYTES + 1] = text_len & 0xFF
    return bytes(region)


def locate(seg, prefer_version: Optional[str] = None) -> Optional[InitialCommands]:
    """Locate and decode the initial-command buffer inside a SEGFIL0 buffer.

    Requires SELF-CONSISTENCY (stored length == re-encoded length) so a garbage
    region whose length cell coincidentally reads small is not mistaken for a
    buffer. Returns ``None`` when no consistent buffer is found.
    """
    st_page = find_segment_table_page(seg)
    if st_page < 0:
        return None

    order: List[str] = []
    if prefer_version:
        pv = prefer_version.upper()
        if pv in INIBU:
            order.append(pv)
    for v in _VERSION_ORDER:
        if v not in order:
            order.append(v)

    for letter in order:
        inibu = INIBU.get(letter)
        if inibu is None:
            continue
        for n in range(1, 128):
            e = _seg_entry(seg, st_page, n)
            if e["madr"] == 0 or e["segle"] == 0:
                continue
            seg_base = (e["logad"] & 0x3F) * 1024
            if not (seg_base <= inibu < seg_base + e["segle"] * 1024):
                continue
            off = inibu - seg_base
            byte_offset = e["madr"] * _PAGE_BYTES + off * 2
            if byte_offset + CAP_BYTES > len(seg):
                continue
            text_len = _word_be(seg, byte_offset + LEN_CELL_BYTES)
            if text_len == 0 or text_len > LEN_CELL_BYTES:
                continue
            cmds = parse_buffer(seg, byte_offset, text_len)
            if cmds and reencode_matches(cmds, text_len):
                return InitialCommands(
                    version=letter,
                    segment_number=n,
                    madr=e["madr"],
                    byte_offset=byte_offset,
                    segment_table_page=st_page,
                    text_length=text_len,
                    length_cell_offset=byte_offset + LEN_CELL_BYTES,
                    commands=cmds,
                )
    return None


_REPAIR_KEYWORDS = (
    "DIRECTORY", "DISC-", "SET-AVAIL", "APPEND-BATCH", "LOAD-MODE",
    "HENT-MODE", "CONNECT", "ENTER-DIR", "SYSTEM-OUTPUT",
)


def locate_repair_target(seg) -> Optional[Tuple[int, int]]:
    """Find the write target for repairing a buffer whose header was clobbered.

    Scores every INIBU-containing segment by surviving command keywords and
    requires a single clear winner (score >= 2 and strictly above the runner-up).
    Returns ``(byte_offset, score)`` or ``None`` when no unambiguous target.
    """
    st_page = find_segment_table_page(seg)
    if st_page < 0:
        return None
    best_offset, best_score, second_score = -1, 0, 0
    for letter in _VERSION_ORDER:
        inibu = INIBU[letter]
        for n in range(1, 128):
            e = _seg_entry(seg, st_page, n)
            if e["madr"] == 0 or e["segle"] == 0:
                continue
            seg_base = (e["logad"] & 0x3F) * 1024
            if not (seg_base <= inibu < seg_base + e["segle"] * 1024):
                continue
            off = inibu - seg_base
            byte_offset = e["madr"] * _PAGE_BYTES + off * 2
            if byte_offset + CAP_BYTES > len(seg):
                continue
            text = "".join(
                chr(seg[byte_offset + i] & 0x7F)
                if 0x20 <= (seg[byte_offset + i] & 0x7F) < 0x7F else " "
                for i in range(CAP_BYTES)
            )
            score = sum(1 for kw in _REPAIR_KEYWORDS if kw in text)
            if score > best_score:
                second_score, best_score, best_offset = best_score, score, byte_offset
            elif score > second_score:
                second_score = score
    if best_offset < 0 or best_score < 2 or best_score == second_score:
        return None
    return best_offset, best_score


def enumerate_candidates(seg) -> List[InitCmdCandidate]:
    """Enumerate EVERY INIBU-containing segment candidate (diagnostic)."""
    result: List[InitCmdCandidate] = []
    st_page = find_segment_table_page(seg)
    if st_page < 0:
        return result
    for letter in _VERSION_ORDER:
        inibu = INIBU[letter]
        for n in range(1, 128):
            e = _seg_entry(seg, st_page, n)
            if e["madr"] == 0 or e["segle"] == 0:
                continue
            seg_base = (e["logad"] & 0x3F) * 1024
            if not (seg_base <= inibu < seg_base + e["segle"] * 1024):
                continue
            off = inibu - seg_base
            byte_offset = e["madr"] * _PAGE_BYTES + off * 2
            if byte_offset + CAP_BYTES > len(seg):
                continue
            text_len = _word_be(seg, byte_offset + LEN_CELL_BYTES)
            plausible = 0 < text_len <= LEN_CELL_BYTES
            cmds = parse_buffer(seg, byte_offset, text_len) if plausible else None
            consistent = bool(cmds) and reencode_matches(cmds, text_len)
            result.append(InitCmdCandidate(
                version=letter,
                segment_number=n,
                madr=e["madr"],
                byte_offset=byte_offset,
                length_cell=text_len,
                length_plausible=plausible,
                consistent=consistent,
                commands=cmds,
            ))
    return result
