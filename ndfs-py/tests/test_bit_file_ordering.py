"""Bit-file bit ordering, checked against sources OUTSIDE this codebase.

WHY THIS FILE EXISTS
====================
Until 2026-08-02 every port here addressed the bit file as "byte N/8, bit N%8".
SINTRAN actually addresses it as a 16-bit WORD: page N is bit N%16 of word N/16. On a
big-endian image those two differ by a byte swap inside every word.

The bug survived a full test suite for months. Understanding why is the point of this
file:

  1. Every existing test was a ROUND TRIP - written with convention X, read back with
     convention X. That passes for any self-consistent X, including a wrong one. A
     symmetric error is invisible to symmetric tests.
  2. The one check made against real data was a POPCOUNT comparison, and popcount is
     invariant under byte-swapping. It could not fail for this bug even in principle,
     yet it is what the docs cited as "VERIFIED".
  3. The tests were written from the same incorrect note as the code, so they asserted
     the bug as though it were the specification - ``bytes([0xFF, 0x00])  # first 8
     used`` in test_bit_file.py said exactly that.

So every test below is deliberately ASYMMETRIC: it compares this code against something
this project did not produce - ND's own manual, SINTRAN's own allocator, or a pack real
SINTRAN wrote. Nothing here round-trips our own output.

AUTHORITY
=========
SINTRAN III System Supervisor, ND-30.003.007 EN, appendix F.2 "Bit-File"::

    PAGE = BLOCK*400B + WORD*20B + BIT

20B is 16 decimal, so one 16-bit word maps 16 pages; and because the formula ADDS the
bit number, page x sits at bit 0 - the least significant bit. The Norwegian edition
ND-30.003.7 NO carries the identical formula. SINTRAN's own allocator agrees: TPAGF at
51043B forms the word index with ``SHA ZIN SHR 4``, i.e. page/16, not page/8.
"""
from __future__ import annotations

import math
import os

import pytest

from ndfs.bit_file import BitFile


# --------------------------------------------------------------------------
# 1. The manual's own worked example
# --------------------------------------------------------------------------


def test_manual_worked_example_word_313_octal():
    """Reproduce the bit pattern printed in ND-30.003.007 appendix F.2.

    The manual shows a single bit-file word holding 313B and labels which of the 16
    pages it covers are Free and which are Used. 313B = 0b0000_0000_1100_1011, so bits
    0, 1, 3, 6 and 7 are set.

    This vector comes from ND, not from us. Under the old byte convention the same two
    bytes decode to a completely different set of pages, so this test fails loudly if
    the ordering is ever swapped back.
    """
    word = 0o313
    assert word == 0b0000000011001011

    # Big-endian on disk: high byte first.
    raw = bytes([(word >> 8) & 0xFF, word & 0xFF])

    bf = BitFile()
    bf.initialize(16)
    bf.load_bitmap(raw)

    expected_used = {0, 1, 3, 6, 7}
    for page in range(16):
        assert bf.is_block_used(page) is (page in expected_used), (
            f"page {page}: manual says "
            f"{'USED' if page in expected_used else 'FREE'} for word 313B"
        )


def test_manual_formula_page_equals_block_times_400_plus_word_times_20_plus_bit():
    """PAGE = BLOCK*400B + WORD*20B + BIT, checked directly.

    400B = 256 pages per bit-file block, 20B = 16 pages per word. Setting exactly one
    bit must make exactly the page the formula names read as used, and no other.
    """
    pages_per_block = 0o400   # 256
    pages_per_word = 0o20     # 16
    assert pages_per_block == 256
    assert pages_per_word == 16

    for block, word, bit in [(0, 0, 0), (0, 0, 15), (0, 1, 0), (0, 5, 9),
                             (1, 0, 0), (1, 15, 15), (3, 7, 4)]:
        page = block * pages_per_block + word * pages_per_word + bit

        bf = BitFile()
        bf.initialize(4096)
        bf.mark_block_used(page)

        raw = bf.get_bitmap_data()
        # Which 16-bit big-endian word did the bit actually land in?
        word_index = block * (pages_per_block // pages_per_word) + word
        stored = (raw[word_index * 2] << 8) | raw[word_index * 2 + 1]

        assert stored == (1 << bit), (
            f"page {page} (block {block}, word {word}, bit {bit}) must set bit {bit} "
            f"of word {word_index}; got 0x{stored:04X}"
        )
        # And nothing else anywhere.
        assert bf.calc_used_pages() == 1


# --------------------------------------------------------------------------
# 2. The byte-swap, asserted explicitly
# --------------------------------------------------------------------------


def test_page_zero_lives_in_the_low_byte_of_the_first_word():
    """The single assertion that most directly contradicts the old convention.

    A 16-bit big-endian word is stored high byte first, and page 0 is bit 0 - the LEAST
    significant bit - so page 0 lands in byte 1, not byte 0. The old "byte N/8" scheme
    put it in byte 0. Nothing else in the suite pins this down so bluntly.
    """
    bf = BitFile()
    bf.initialize(64)
    bf.mark_block_used(0)

    raw = bf.get_bitmap_data()
    assert raw[0] == 0x00, "page 0 must NOT be in the high byte"
    assert raw[1] == 0x01, "page 0 is bit 0 of the low byte"


def test_page_eight_lives_in_the_high_byte():
    """The mirror of the above: page 8 crosses into the high byte of the same word."""
    bf = BitFile()
    bf.initialize(64)
    bf.mark_block_used(8)

    raw = bf.get_bitmap_data()
    assert raw[0] == 0x01, "page 8 is bit 8 of word 0, i.e. bit 0 of the HIGH byte"
    assert raw[1] == 0x00


def test_first_sixteen_pages_fill_low_byte_then_high_byte():
    """Marking pages 0..15 in order fills byte 1 before byte 0.

    Under the old convention it filled byte 0 then byte 1. This walks the whole word so
    a partial regression cannot slip through.
    """
    bf = BitFile()
    bf.initialize(64)

    for page in range(8):
        bf.mark_block_used(page)
    assert bf.get_bitmap_data()[0] == 0x00, "high byte still untouched after 8 pages"
    assert bf.get_bitmap_data()[1] == 0xFF, "low byte full after 8 pages"

    for page in range(8, 16):
        bf.mark_block_used(page)
    assert bf.get_bitmap_data()[0] == 0xFF
    assert bf.get_bitmap_data()[1] == 0xFF


def test_the_old_byte_convention_is_actively_rejected():
    """Guard against a silent revert.

    Computes what the discredited "byte N/8, bit N%8" scheme would produce and asserts
    we do NOT produce it. If someone reintroduces the old arithmetic, this fails even if
    every round-trip test still passes - which is exactly the hole the original bug went
    through.
    """
    bf = BitFile()
    bf.initialize(64)
    for page in (0, 3, 17, 40):
        bf.mark_block_used(page)

    actual = bytes(bf.get_bitmap_data())

    old_scheme = bytearray(len(actual))
    for page in (0, 3, 17, 40):
        old_scheme[page >> 3] |= 1 << (page & 7)

    assert actual != bytes(old_scheme), (
        "bitmap matches the old byte-per-8-pages convention, which SINTRAN does not use"
    )


# --------------------------------------------------------------------------
# 3. Sizing: devices whose page count is not a multiple of 16
# --------------------------------------------------------------------------


@pytest.mark.parametrize("total_pages", [1, 8, 15, 16, 17, 100, 616, 4095, 38400])
def test_every_page_is_addressable(total_pages):
    """The last page must be reachable, whatever the device size.

    A 616-page ND floppy is the real case that exposed this: ceil(616/8) = 77 bytes
    covers only 616 pages if you address by byte, but word addressing needs the 78th
    byte to exist or pages 608..615 fall off the end. The bitmap is therefore rounded up
    to a whole 16-bit word.
    """
    bf = BitFile()
    bf.initialize(total_pages)

    assert len(bf.get_bitmap_data()) % 2 == 0, "bitmap must be a whole number of words"
    assert len(bf.get_bitmap_data()) >= math.ceil(total_pages / 8)

    last = total_pages - 1
    bf.mark_block_used(last)
    assert bf.is_block_used(last) is True, f"page {last} of {total_pages} unreachable"
    assert bf.calc_used_pages() == 1


def test_every_page_round_trips_individually():
    """Mark, verify, clear, verify - for every page of an awkwardly sized device.

    Not a round trip of the ENCODING (which is what hid the bug), but a check that the
    mark/clear pair is self-inverse and that no two pages alias onto the same bit.
    """
    total = 616
    bf = BitFile()
    bf.initialize(total)

    for page in range(total):
        bf.mark_block_used(page)
        assert bf.is_block_used(page) is True
        assert bf.calc_used_pages() == 1, f"page {page} set more than one bit"
        bf.mark_block_free(page)
        assert bf.is_block_used(page) is False
        assert bf.calc_used_pages() == 0


def test_pages_do_not_alias():
    """Distinct pages must occupy distinct bits across a whole word boundary region."""
    bf = BitFile()
    bf.initialize(256)
    for page in range(256):
        bf.mark_block_used(page)
    assert bf.calc_used_pages() == 256
    assert all(b == 0xFF for b in bf.get_bitmap_data()[:32])


# --------------------------------------------------------------------------
# 4. Against packs that real SINTRAN wrote
# --------------------------------------------------------------------------

# These are the strongest tests available, because the ground truth was produced by
# SINTRAN itself. They are skipped when the media are not present rather than failing,
# since the images live outside this repository.
#
# Set NDFS_REAL_IMAGES to the image paths, separated by os.pathsep (";" on Windows,
# ":" elsewhere). The three packs used when this test was written were
# BIGDISK0-L.IMG, 210319H02-XX-01D.img and Nd-210523I01-XX-01D.img. When the variable
# is unset the list holds one empty entry, so the tests report SKIPPED, not "no tests".
REAL_IMAGES = [p for p in os.environ.get("NDFS_REAL_IMAGES", "").split(os.pathsep) if p] or [""]


@pytest.mark.parametrize("image_path", REAL_IMAGES)
def test_no_page_holding_file_data_is_marked_free(image_path):
    """On a real pack, every page holding file data must be marked used.

    This is the invariant that finally settled the question, and it is one no
    self-consistent-but-wrong convention can satisfy: a page containing a real file's
    bytes cannot also be free. Under the old byte convention these three media reported
    32, 4 and 3 such pages respectively - pages the allocator would happily have handed
    out, overwriting live files.

    Tolerance is zero, deliberately.
    """
    if not os.path.exists(image_path):
        pytest.skip(f"real SINTRAN media not present (set NDFS_REAL_IMAGES): {image_path!r}")

    from ndfs.filesystem import NdfsFileSystem

    with open(image_path, "rb") as fh:
        data = fh.read()

    fs = NdfsFileSystem(bytearray(data), read_only=True)
    bit_file = fs._bit_file

    data_blocks = set()
    for entry in fs.get_object_entries():
        path = f"{entry.user_name}/{entry.object_name}:{entry.type}"
        try:
            for block in fs.get_file_blocks(path):
                if block:
                    data_blocks.add(block)
        except Exception:
            # A file we cannot walk tells us nothing about the bitmap; skip it rather
            # than letting an unrelated defect masquerade as a bit-order failure.
            continue

    assert data_blocks, "expected at least one file with allocated pages"

    free_but_occupied = sorted(b for b in data_blocks if not bit_file.is_block_used(b))
    assert free_but_occupied == [], (
        f"{len(free_but_occupied)} page(s) hold file data yet are marked FREE: "
        f"{free_but_occupied[:16]}"
    )


@pytest.mark.parametrize("image_path", REAL_IMAGES)
def test_structural_pages_are_marked_used(image_path):
    """The pack's own metadata pages must be marked used on a real pack."""
    if not os.path.exists(image_path):
        pytest.skip(f"real SINTRAN media not present (set NDFS_REAL_IMAGES): {image_path!r}")

    from ndfs.filesystem import NdfsFileSystem

    with open(image_path, "rb") as fh:
        fs = NdfsFileSystem(bytearray(fh.read()), read_only=True)

    mb = fs._master_block
    for name, pointer in [
        ("object file", mb.object_file_pointer),
        ("user file", mb.user_file_pointer),
        ("bit file", mb.bit_file_pointer),
    ]:
        assert fs._bit_file.is_block_used(pointer.block_id) is True, (
            f"the {name} root page {pointer.block_id} must be marked used"
        )

    assert fs._bit_file.is_block_used(0) is True, "the master block must be marked used"
