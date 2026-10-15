"""
NDFS bit file (allocation bitmap): one bit per page, 0=free, 1=used.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

import math
from typing import List, Optional, Union

from ndfs.constants import NDFS_PAGE_SIZE, FIRST_ALLOCATABLE_BLOCK
from ndfs.block_pointer import BlockPointer

_BufType = Union[bytes, bytearray, memoryview]


class BitFile:
    """NDFS allocation bitmap manager."""

    __slots__ = ("index_pointer", "total_pages", "alloc_ceiling", "_bitmap",
                 "_scan_cursor", "_used_count")

    def __init__(self) -> None:
        self.index_pointer: Optional[BlockPointer] = None
        #: Pages the bitmap covers -- the PHYSICAL DEVICE size, not the declared capacity.
        self.total_pages: int = 0
        #: Where the descending free-block search left off. None = start from the
        #: top. Cleared whenever a block is freed, so it can only ever skip a
        #: prefix that was genuinely used. Pure optimisation, no format meaning.
        self._scan_cursor: Optional[int] = None
        #: Running count of used pages, or None when it has to be rebuilt from
        #: the bitmap. Keeps calc_used_pages() O(1) -- see its docstring.
        self._used_count: Optional[int] = None
        #: Highest allocatable page + 1 -- the descending allocator's ceiling.
        #:
        #: SINTRAN bounds the allocatable window by the directory's DECLARED CAPACITY
        #: (ext_pages_available, words 1756B-1757B), not by the device size. On PACK-ONE
        #: the capacity is 36945, so the highest allocatable page is 36944; pages
        #: 36945..38399 are a deliberate free-but-unreachable gap (the drive's bad-sector
        #: spare region) -- they stay 0 in the bitmap yet are never handed out.
        #:
        #: Defaults to total_pages when the capacity is unknown (FLOMON floppies carry no
        #: valid extended-info block). Always clamped to total_pages: the real Winchester
        #: WD0.img declares a capacity 36 pages LARGER than the file, and allocating there
        #: would run off the end.
        self.alloc_ceiling: int = 0
        self._bitmap: Optional[bytearray] = None

    def initialize(self, total_pages: int) -> None:
        """Initialize a new empty bitmap for the given total page count.

        ``total_pages`` is the PHYSICAL DEVICE size -- SINTRAN sizes the bitmap to the
        device (e.g. 38400 on a 75MB SMD pack), not to the declared capacity.
        """
        self.total_pages = total_pages
        self._scan_cursor = None  # new bitmap: any cached search position is stale
        self._used_count = None   # and the used-page total must be rebuilt
        # Until the caller supplies the declared capacity, the whole device is allocatable.
        self.alloc_ceiling = total_pages
        # Rounded up to a whole number of 16-bit WORDS. The bitmap is addressed by word
        # (see _bit_position), so an odd byte count would leave the last word half
        # present and put the final few pages past the end of the buffer.
        bitmap_bytes = (math.ceil(total_pages / 8) + 1) & ~1
        self._bitmap = bytearray(bitmap_bytes)

    def load_bitmap(self, data: _BufType) -> None:
        """Load bitmap data from raw bytes.

        Padded to a whole number of 16-bit words for the same reason as
        :meth:`initialize`: word addressing needs the high byte of the final word to
        exist, even when the source slice stops on an odd boundary.
        """
        self._bitmap = bytearray(data)
        self._scan_cursor = None  # new bitmap: any cached search position is stale
        self._used_count = None   # and the used-page total must be rebuilt
        if len(self._bitmap) & 1:
            # Should not happen - the caller slices whole words - but a half word here
            # would make the last few pages unreadable rather than merely unallocatable.
            self._bitmap.append(0)

    @staticmethod
    def _bit_position(block_id: int):
        """Byte index and bit index of *block_id* in the on-disk bitmap.

        Page N lives in 16-bit WORD N/16, at bit N%16 counting from the LSB.

        AUTHORITY: SINTRAN III System Supervisor, ND-30.003.007 EN, appendix F.2
        "Bit-File"::

            PAGE = BLOCK*400B + WORD*20B + BIT

        20B is 16 decimal, so ONE 16-BIT WORD MAPS 16 PAGES - not one byte mapping 8.
        And because the formula ADDS the bit number, page x sits at bit 0, the least
        significant bit. The Norwegian edition (ND-30.003.7 NO) carries the identical
        formula, and SINTRAN's own allocator agrees: TPAGF at 51043B forms the word
        index with ``SHA ZIN SHR 4``, i.e. page/16.

        The image is big-endian, so the word at index w occupies bytes w*2 (high) and
        w*2+1 (low). Page N therefore lands in the OTHER byte of the pair from where a
        naive "byte N/8, bit N%8" scheme would put it, which reduces to flipping the
        low bit of the byte index.

        WHY THIS MATTERS: this library previously used the naive byte scheme. Wrong in
        both reader and writer, it was self-consistent and every round-trip test
        passed - but on a pack written by real SINTRAN it reads the allocation map with
        every byte pair swapped. Measured on three genuine ND media (a 78 MB pack and
        two ND distribution floppies), the naive scheme reports 32, 4 and 3 pages that
        demonstrably hold file data as FREE; this scheme reports none. Handing those
        pages out would have overwritten live files.
        """
        return (block_id >> 3) ^ 1, block_id & 7

    def is_block_used(self, block_id: int) -> bool:
        """Check if a block is marked as used."""
        if self._bitmap is None or block_id >= self.total_pages:
            return False
        byte_index, bit_index = self._bit_position(block_id)
        return (self._bitmap[byte_index] & (1 << bit_index)) != 0

    def mark_block_used(self, block_id: int) -> None:
        """Mark a block as used."""
        if self._bitmap is None or block_id >= self.total_pages:
            raise IndexError(f"Block ID {block_id} out of range")
        byte_index, bit_index = self._bit_position(block_id)
        if not (self._bitmap[byte_index] & (1 << bit_index)) and self._used_count is not None:
            self._used_count += 1
        self._bitmap[byte_index] |= 1 << bit_index

    def mark_block_free(self, block_id: int) -> None:
        """Mark a block as free."""
        # A freed block may sit ABOVE the descending search cursor, so drop the
        # cursor: without this, find_first_free_block would never look back up
        # and the freed page would be lost until the next full scan.
        self._scan_cursor = None
        if self._bitmap is None or block_id >= self.total_pages:
            raise IndexError(f"Block ID {block_id} out of range")
        byte_index, bit_index = self._bit_position(block_id)
        if (self._bitmap[byte_index] & (1 << bit_index)) and self._used_count is not None:
            self._used_count -= 1
        self._bitmap[byte_index] &= ~(1 << bit_index)

    def calc_used_pages(self) -> int:
        """Count total used pages.

        Served from a running total kept by mark_block_used/mark_block_free, so
        this is O(1) rather than a walk of every page in the volume. The count
        is recomputed from scratch whenever the bitmap itself is replaced
        (initialize/load_bitmap), which are the only ways it can change behind
        this class's back.

        Why it matters: write_file consults get_free_pages() before expanding a
        user's reservation, so this used to walk all 38400 pages of an SMD pack
        on every file written. That went unnoticed while an all-zero file needed
        no pages at all and skipped the quota branch entirely; once every page
        became real (see filesystem._write_data_page_to_index) a test that
        creates a few thousand small files turned into tens of millions of
        bit tests and the Python suite stopped finishing.
        """
        if self._bitmap is None:
            return 0
        if self._used_count is None:
            self._used_count = self._recount_used()
        return self._used_count

    def _recount_used(self) -> int:
        """Walk the bitmap and count used pages. Only for (re)building the cache."""
        count = 0
        for i in range(self.total_pages):
            if self.is_block_used(i):
                count += 1
        return count

    def get_free_pages(self) -> int:
        """Get number of free pages."""
        return self.total_pages - self.calc_used_pages()

    def _top_scan_index(self) -> int:
        """Highest page the allocator may consider (inclusive), or -1 if no usable window.

        Bounded by :attr:`alloc_ceiling` -- the DECLARED CAPACITY, not the device size --
        and clamped to the bitmap so a stale ceiling can never index past it.
        """
        ceiling = self.alloc_ceiling
        if ceiling <= 0 or ceiling > self.total_pages:
            ceiling = self.total_pages
        if ceiling <= FIRST_ALLOCATABLE_BLOCK:
            return -1
        return ceiling - 1

    def find_first_free_block(self) -> int:
        """Find a single free block, scanning HIGH -> LOW.

        KERNEL-CORRECTED: SINTRAN allocates from the TOP of the volume downward. The
        scanner TESTP (51355B) only ever DECREMENTS its bitmap word index (``AAX -1`` at
        51372B and 51401B) -- it never increments -- bounded below by the block-7 floor.
        This matches the @CREATE-FILE rule "contiguous files are positioned in the highest
        page addresses". The old upward scan produced the opposite layout to real SINTRAN.

        Blocks 0-6 are system-reserved and are never handed out.

        Returns the block ID or -1 if no free block exists.
        """
        # Resume where the last successful search stopped instead of rescanning
        # the whole volume from the top. Everything above `_scan_cursor` was
        # already used when we passed it, and `mark_block_free` clears the
        # cursor, so this returns exactly the same block the full scan would --
        # it just does not walk the used prefix again.
        #
        # This matters now that an all-zero page is a real page rather than an
        # inferred hole (see filesystem._write_data_page_to_index). Writing a
        # 1000-page file used to allocate a handful of blocks; it now allocates
        # 1000, and a fresh top-down scan per block made that O(pages x volume)
        # -- on a 38400-page pack the Python test suite went from 11 s to over
        # 10 minutes before this cursor was added.
        start = self._top_scan_index() if self._scan_cursor is None else self._scan_cursor
        if start > self._top_scan_index():
            start = self._top_scan_index()
        for i in range(start, FIRST_ALLOCATABLE_BLOCK - 1, -1):
            if not self.is_block_used(i):
                self._scan_cursor = i
                return i
        # Nothing free at or below the cursor. If the cursor had narrowed the
        # window, retry once over the whole range before declaring the pack
        # full, so a stale cursor can never invent an out-of-space error.
        if self._scan_cursor is not None:
            self._scan_cursor = None
            for i in range(self._top_scan_index(), FIRST_ALLOCATABLE_BLOCK - 1, -1):
                if not self.is_block_used(i):
                    self._scan_cursor = i
                    return i
        return -1

    def find_free_block_range(self, blocks_needed: int) -> int:
        """Find a contiguous run of free blocks, scanning HIGH -> LOW.

        The run lands in the highest available addresses (SINTRAN's downward range
        reserve, RSPAG 51120B -> TESTP). Returns the LOWEST block of the run (its start),
        or -1 if no run is found.
        """
        if blocks_needed == 0 or blocks_needed > self.total_pages:
            return -1

        top = self._top_scan_index()
        if top < FIRST_ALLOCATABLE_BLOCK:
            return -1

        consecutive_free = 0
        for i in range(top, FIRST_ALLOCATABLE_BLOCK - 1, -1):
            if not self.is_block_used(i):
                consecutive_free += 1
                if consecutive_free >= blocks_needed:
                    return i  # i is the lowest block of the run
            else:
                consecutive_free = 0
        return -1

    def allocate_blocks(self, start_block: int, count: int) -> bool:
        """Allocate a range of blocks (mark as used).

        Blocks 0-6 cannot be allocated. Returns False if any block is already used.
        """
        if start_block < FIRST_ALLOCATABLE_BLOCK:
            return False
        if start_block + count > self.total_pages:
            return False

        # Check all blocks are free
        for i in range(start_block, start_block + count):
            if self.is_block_used(i):
                return False

        # Mark all blocks as used
        for i in range(start_block, start_block + count):
            self.mark_block_used(i)
        return True

    def free_blocks(self, start_block: int, count: int) -> None:
        """Free a range of blocks."""
        for i in range(start_block, min(start_block + count, self.total_pages)):
            self.mark_block_free(i)

    def get_bitmap_data(self) -> bytes:
        """Get a copy of the raw bitmap data."""
        if self._bitmap is None:
            return b""
        return bytes(self._bitmap)

    def to_page_buffers(self) -> List[bytearray]:
        """Write bitmap data into page-aligned buffers for disk writing.

        Returns a list of page buffers to write starting at the pointer's block ID.
        """
        if self._bitmap is None:
            return []

        pages_needed = math.ceil(len(self._bitmap) / NDFS_PAGE_SIZE)
        pages: List[bytearray] = []

        for i in range(pages_needed):
            page = bytearray(NDFS_PAGE_SIZE)
            src_offset = i * NDFS_PAGE_SIZE
            bytes_to_copy = min(NDFS_PAGE_SIZE, len(self._bitmap) - src_offset)
            if bytes_to_copy > 0:
                page[0:bytes_to_copy] = self._bitmap[src_offset:src_offset + bytes_to_copy]
            pages.append(page)
        return pages
