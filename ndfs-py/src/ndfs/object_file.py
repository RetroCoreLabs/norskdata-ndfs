"""
NDFS object file: manages the collection of file (object) entries.

Can be indexed (single index block with up to 512 pointers to data pages)
or sub-indexed (sub-index block with 512 pointers to index blocks).

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
HackerCorp Labs - https://github.com/HackerCorpLabs
"""
from __future__ import annotations

from typing import Callable, Dict, List, Optional, Union

from ndfs.constants import (
    NDFS_PAGE_SIZE,
    ENTRIES_PER_PAGE,
    ENTRY_SIZE,
    FILES_PER_OBJECT_BLOCK,
    MAX_OBJECT_BLOCKS,
    MAX_OBJECT_FILE_POINTERS,
    MAX_VERIFIED_MULTIBLOCK_USER,
    PAGES_PER_INDEX_BLOCK,
    PAGES_PER_OBJECT_BLOCK,
    USERS_PER_INDEX_BLOCK,
)
from ndfs.block_pointer import BlockPointer
from ndfs.object_entry import ObjectEntry, compute_file_number
from ndfs.types import PointerType

_BufType = Union[bytes, bytearray, memoryview]


class ObjectFile:
    """Manages the collection of NDFS file (object) entries."""

    __slots__ = ("index_pointer", "_entries", "_next_index", "unreadable_pages")

    def __init__(self) -> None:
        self.index_pointer: Optional[BlockPointer] = None
        self._entries: Dict[int, ObjectEntry] = {}
        self._next_index: int = 0
        # Object-file pages that could not be read on the last load. Greater
        # than zero means the listing is what survived on a damaged floppy,
        # not the whole file list - each lost page took up to 32 entries.
        self.unreadable_pages: int = 0

    def get_objects(self) -> List[ObjectEntry]:
        """Get all object entries."""
        result: List[ObjectEntry] = []
        for v in self._entries.values():
            result.append(v)
        return result

    def get_object(self, index: int) -> Optional[ObjectEntry]:
        """Get an object entry by index."""
        return self._entries.get(index, None)

    @staticmethod
    def _normalize_type(file_type: Optional[str]) -> str:
        """Uppercase a file type and drop the padding SINTRAN leaves on it.

        Types read off a pack can carry a trailing apostrophe and spaces
        (the same reason other call sites do rstrip("' ")), so compare on a
        normalized form rather than raw bytes.
        """
        return (file_type or "").upper().strip().rstrip("'").strip()

    def find_object(
        self,
        object_name: str,
        user_name: str,
        file_type: Optional[str] = None,
    ) -> Optional[ObjectEntry]:
        """Find an object by name and user, and by type when one is given.

        A SINTRAN file is identified by NAME:TYPE, and two files may share a
        name while differing only in type -- a real pack carries both
        (TCP-IP)TCP-IP-LO-D02:MODE and :LIST, and (TCP-IP)SKP-C00 exists as
        :DEFS, :IMPT and :INTL at once.

        Matching on the name alone made write_file treat those as one file:
        writing AAA:MODE after AAA:LIST overwrote the LIST entry's data while
        leaving its type as LIST, so the MODE file vanished and the LIST file
        silently held the wrong bytes. Measured 2026-08-19 while installing
        the COSMOS TCP/IP kit -- TCP-IP-LO-D02:MODE and TCP-START-D02:MODE
        were both lost that way.

        file_type=None keeps the old name-only behaviour, for callers that
        genuinely do not know the type.
        """
        name_upper = object_name.upper()
        user_upper = user_name.upper()
        type_upper = self._normalize_type(file_type) if file_type else None
        for entry in self._entries.values():
            if entry.object_name.upper() != name_upper:
                continue
            if entry.user_name.upper() != user_upper:
                continue
            if type_upper is not None and self._normalize_type(entry.type) != type_upper:
                continue
            return entry
        return None

    def add_object(self, entry: ObjectEntry) -> None:
        """Add or update an object entry."""
        self._entries[entry.object_index] = entry
        if entry.object_index >= self._next_index:
            self._next_index = entry.object_index + 1

    def remove_object(self, index: int) -> bool:
        """Remove an object entry."""
        if index in self._entries:
            del self._entries[index]
            return True
        return False

    def get_user_objects(self, user_name_or_index: Union[str, int]) -> List[ObjectEntry]:
        """Get objects belonging to a user (by name or index)."""
        result: List[ObjectEntry] = []
        for entry in self._entries.values():
            if isinstance(user_name_or_index, str):
                if entry.user_name.upper() == user_name_or_index.upper():
                    result.append(entry)
            else:
                if entry.user_index == user_name_or_index:
                    result.append(entry)
        return result

    def get_next_available_index(self) -> int:
        """Get next available object index."""
        for i in range(self._next_index + 1):
            if i not in self._entries:
                return i
        return self._next_index

    @staticmethod
    def physical_slot(user_index: int, block: int, slot_in_block: int) -> int:
        """Physical object-file position of a slot in a user's *block*-th object block.

        Kept for compatibility: it assumes the block-th block lives in the group
        ``home + block``, which is where an UNOBSTRUCTED overflow block lands. Use
        :meth:`physical_slot_in_group` when the group is known, because SINTRAN skips
        groups whose slot is already spoken for.
        """
        return ObjectFile.physical_slot_in_group(
            user_index, (user_index // USERS_PER_INDEX_BLOCK) + block, slot_in_block
        )

    @staticmethod
    def physical_slot_in_group(user_index: int, group: int, slot_in_block: int) -> int:
        """Physical object-file position of ``slot_in_block`` in a given index-block group.

        A user always occupies slot ``user % 64`` of a group, eight pages wide::

            page     = group*512 + (user % 64)*8 + slot_in_block // 32
            physical = page*32 + slot_in_block % 32

        The user's HOME group is ``user // 64``; that pairing reduces to the flat
        ``page = user*8`` for a first block. Overflow blocks use the same slot in a
        higher group.
        """
        page = (group * PAGES_PER_INDEX_BLOCK
                + (user_index % USERS_PER_INDEX_BLOCK) * PAGES_PER_OBJECT_BLOCK
                + slot_in_block // ENTRIES_PER_PAGE)
        return page * ENTRIES_PER_PAGE + (slot_in_block % ENTRIES_PER_PAGE)

    def groups_used_by(self, user_index: int) -> List[int]:
        """The index-block groups this user has entries in, ascending."""
        groups = set()
        for entry in self._entries.values():
            if entry.user_index != user_index:
                continue
            page = entry.object_index // ENTRIES_PER_PAGE
            groups.add(page // PAGES_PER_INDEX_BLOCK)
        return sorted(groups)

    def find_free_user_slot(
        self,
        user_index: int,
        max_object_blocks: int = 1,
        allocated_object_blocks: int = 1,
        group_available=None,
    ) -> int:
        """Find the next free object slot for a user, across all their object blocks.

        A user area holds 256 files per object block. Every one of a user's blocks sits
        at slot ``user % 64`` of some index-block group; the HOME group ``user // 64``
        holds block 0, and overflow blocks take the same slot in a HIGHER group.

        **SINTRAN skips a group whose slot is already spoken for.** Measured live
        (2026-08-02): user 8 overflowing past 256 files did not take group 1 - user 72
        lives there - it took the next group whose slot 8 was free. See
        ``_assign_file_numbers`` for the full experiment.

        Args:
            user_index: The owning user.
            max_object_blocks: MXOBL, 1..16.
            allocated_object_blocks: ACOBL, 1..MXOBL.
            group_available: Optional predicate ``(group) -> bool`` used only when a NEW
                block is needed. The file-system layer supplies it so this class does not
                need to know about the user file. Default: any group is acceptable, which
                reproduces the old unobstructed behaviour.

        Returns:
            The physical slot, or -1 when every permitted block is full or no group can
            be found for the next one.
        """
        max_blocks = max(1, min(MAX_OBJECT_BLOCKS, max_object_blocks))
        allocated = max(1, min(max_blocks, allocated_object_blocks))

        # Blocks the user already occupies, plus the HOME group - which is block 0 and
        # must be searched even when the user has no entries yet. Look for a hole; holes
        # are normal, since deleting a file leaves one and SINTRAN never compacts.
        home = user_index // USERS_PER_INDEX_BLOCK
        for group in sorted(set(self.groups_used_by(user_index)) | {home}):
            for slot_in_block in range(FILES_PER_OBJECT_BLOCK):
                physical = self.physical_slot_in_group(user_index, group, slot_in_block)
                if physical not in self._entries:
                    return physical

        # Every allocated block is full. Open the next one if the user's maximum allows,
        # searching upward from home for a group whose slot is free AND acceptable.
        if allocated >= max_blocks:
            return -1

        used = set(self.groups_used_by(user_index)) | {home}
        for group in range(home, MAX_OBJECT_FILE_POINTERS):
            if group in used:
                continue
            if group_available is not None and not group_available(group):
                continue
            first = self.physical_slot_in_group(user_index, group, 0)
            if first not in self._entries:
                return first

        return -1

    def get_total_pages_used(self) -> int:
        """Get total pages used by all objects."""
        total = 0
        for e in self._entries.values():
            total += e.pages_in_file
        return total

    def clear(self) -> None:
        """Clear all entries."""
        self._entries.clear()
        self._next_index = 0

    def load_from_pages(
        self,
        pointer: BlockPointer,
        read_page: Callable[[int], _BufType],
    ) -> None:
        """Load entries from an indexed or sub-indexed structure.

        Args:
            pointer: The object file's block pointer (from master block).
            read_page: Callback to read a page by block ID.
        """
        self._entries.clear()
        self.unreadable_pages = 0
        self.index_pointer = pointer
        global_object_index = 0

        if pointer.type == PointerType.Indexed:
            # Single index block -> up to 512 data page pointers
            index_page = read_page(pointer.block_id)
            self._load_objects_from_index_block(index_page, read_page, global_object_index)
        elif pointer.type == PointerType.SubIndexed:
            # Sub-index block -> up to 512 index block pointers
            sub_index_page = read_page(pointer.block_id)
            for i in range(MAX_OBJECT_FILE_POINTERS):
                index_ptr = BlockPointer.from_bytes(sub_index_page, i * 4)
                if not index_ptr.is_valid():
                    continue
                # A page of a damaged floppy cannot be read at all, and one
                # lost index page must not cost the entries the other pages
                # still name.
                try:
                    index_page = read_page(index_ptr.block_id)
                except Exception:
                    self.unreadable_pages += 1
                    continue
                global_object_index = self._load_objects_from_index_block(
                    index_page, read_page, global_object_index
                )

        # File numbers can only be assigned once every entry is loaded - see
        # _assign_file_numbers for why they are not a pure function of position.
        self._assign_file_numbers()

    def _assign_file_numbers(self) -> None:
        """Assign each entry the number SINTRAN reports, e.g. the 307 in "FILE 307".

        The number is ``logical_block * 256 + slot_in_block``, where **logical_block is
        the ORDINAL RANK of the entry's index-block group among the groups that user
        occupies**, ascending - NOT the physical group number.

        That distinction is invisible until a user's overflow block gets relocated, and
        SINTRAN relocates them freely. Measured on a live SINTRAN III K pack
        (2026-08-02): user 8 with 300 files held block 0 at group 0 and its overflow
        block at group 2. Creating files for user 136 - whose HOME is group 2 slot 8 -
        moved user 8's overflow to group 3; then creating files for user 200, whose home
        is group 3 slot 8, moved it again to group 4. All 300 files survived every move,
        and SINTRAN reported them throughout as FILE 0..299.

        A user's blocks always sit at the same slot ``user % 64``; only the group varies.
        The home group ``user // 64`` holds block 0, and overflow blocks take the same
        slot in a higher group, yielding it to the rightful home owner on demand.

        This is why the old ``block = page // 512`` formula was wrong: for user 8 above it
        produced file numbers 0..255 and then 1024..1067, where SINTRAN says 256..299. It
        looked right on the earlier BIGMAN pack only because nothing had displaced that
        pack's overflow block, so the physical group happened to equal the ordinal.
        """
        by_user: Dict[int, List[ObjectEntry]] = {}
        for entry in self._entries.values():
            by_user.setdefault(entry.user_index, []).append(entry)

        for user_index, entries in by_user.items():
            groups = sorted({
                (e.object_index // ENTRIES_PER_PAGE) // PAGES_PER_INDEX_BLOCK
                for e in entries
            })
            rank = {g: i for i, g in enumerate(groups)}

            for entry in entries:
                page = entry.object_index // ENTRIES_PER_PAGE
                group = page // PAGES_PER_INDEX_BLOCK
                page_in_group = page % PAGES_PER_INDEX_BLOCK
                # The slot WITHIN a group is (user % 64), not the raw user index -
                # 64 users' slots fill one 512-page group.
                slot_base = (user_index % USERS_PER_INDEX_BLOCK) * PAGES_PER_OBJECT_BLOCK
                offset_pages = page_in_group - slot_base
                if offset_pages < 0 or offset_pages >= PAGES_PER_OBJECT_BLOCK:
                    # Not inside this user's slot - report -1 rather than a plausible
                    # wrong number, so a mismatch surfaces.
                    entry.file_number = -1
                    continue
                slot = (offset_pages * ENTRIES_PER_PAGE
                        + entry.object_index % ENTRIES_PER_PAGE)
                entry.file_number = rank[group] * FILES_PER_OBJECT_BLOCK + slot

    def _load_objects_from_index_block(
        self,
        index_page: _BufType,
        read_page: Callable[[int], _BufType],
        start_index: int,
    ) -> int:
        """Load objects from a single index block."""
        object_index = start_index

        for i in range(MAX_OBJECT_FILE_POINTERS):
            ptr = BlockPointer.from_bytes(index_page, i * 4)
            if not ptr.is_valid():
                object_index += ENTRIES_PER_PAGE
                continue

            # Same again one level down: an unreadable data page loses its own
            # 32 entries and nothing else. The slots are still counted, so
            # every entry after it keeps the object index it has on the media.
            try:
                data_page = read_page(ptr.block_id)
            except Exception:
                self.unreadable_pages += 1
                object_index += ENTRIES_PER_PAGE
                continue
            for j in range(ENTRIES_PER_PAGE):
                entry = ObjectEntry.from_bytes(data_page, j * ENTRY_SIZE)
                if entry is not None:
                    entry.object_index = object_index + j
                    # The number SINTRAN shows is NOT the physical position:
                    # a user's files can span several object blocks, each a
                    # whole index block apart. compute_file_number undoes that.
                    entry.file_number = compute_file_number(
                        entry.object_index, entry.user_index
                    )
                    self._entries[entry.object_index] = entry
                    if entry.object_index >= self._next_index:
                        self._next_index = entry.object_index + 1
            object_index += ENTRIES_PER_PAGE

        return object_index

    def to_data_pages(self) -> Dict[int, bytearray]:
        """Serialize all object entries into page-aligned buffers.

        Returns a dict mapping page_index to page data.
        """
        page_map: Dict[int, bytearray] = {}

        for entry in self._entries.values():
            page_index = entry.object_index // ENTRIES_PER_PAGE
            if page_index not in page_map:
                page_map[page_index] = bytearray(NDFS_PAGE_SIZE)
            page = page_map[page_index]
            slot_in_page = entry.object_index % ENTRIES_PER_PAGE
            entry.to_bytes(page, slot_in_page * ENTRY_SIZE)

        return page_map

    def to_data_page(self, page_index: int) -> bytearray:
        """Serialize the single object-file data page `page_index`, zero-filled.

        Zero-fill clears any slot freed by a deletion so a removed file does
        not reappear when the image is reloaded.
        """
        page = bytearray(NDFS_PAGE_SIZE)
        for entry in self._entries.values():
            if entry.object_index // ENTRIES_PER_PAGE == page_index:
                slot_in_page = entry.object_index % ENTRIES_PER_PAGE
                entry.to_bytes(page, slot_in_page * ENTRY_SIZE)
        return page
