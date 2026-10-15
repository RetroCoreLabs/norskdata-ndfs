"""
Tests for the sparse-hole map in XAT sidecars.

A sparse file has pages that were never allocated. Nothing in the sidecar said
WHERE they were: pages_in_file and bytes_in_file together reveal THAT a file is
sparse, but not the positions, so a file copied out to a host and back returned
with a different layout. XAT_HOLES records them.

The behaviour these tests pin was measured on real hardware, not invented: the
file (SYSTEM)S3-CONFIG-E01:PROG on a live pack has 89 real pages spread over a
99-page extent with holes at 54..63, and transferring it between two ND-100s
sends 89 messages carrying page numbers 0..53 then 64..98.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import json

from ndfs.constants import NDFS_PAGE_SIZE
from ndfs.filesystem import NdfsFileSystem
from ndfs.object_entry import ObjectEntry
from ndfs.xat import (
    ALL_XAT_KEYS,
    OPTIONAL_XAT_KEYS,
    XAT_BYTES_IN_FILE,
    XAT_HOLES,
    XAT_PAGES_IN_FILE,
    deserialize_xat,
    object_entry_to_xat,
    serialize_xat,
)


def _sparse_content(page_count: int, filled_pages) -> bytes:
    """Build file content of ``page_count`` pages, non-zero only where asked."""
    data = bytearray(page_count * NDFS_PAGE_SIZE)
    for page in filled_pages:
        start = page * NDFS_PAGE_SIZE
        for i in range(start, start + NDFS_PAGE_SIZE):
            data[i] = 0xA5
    return bytes(data)


def _holes(page_count: int, filled_pages) -> list:
    """The hole list for content built by :func:`_sparse_content`.

    Holes are DECLARED, never inferred from zeros -- writing a hole where the
    original merely had zeros produces a file SINTRAN cannot READ-BI, so the
    writer no longer guesses. These tests state the same holes the old
    inference would have produced, which is what makes them still test holes.
    """
    return [p for p in range(page_count) if p not in set(filled_pages)]


class TestXatHoles:
    """The hole map is recorded, and says where the holes actually are."""

    def test_solid_file_reports_an_empty_hole_list(self, test_image):
        fs = NdfsFileSystem(test_image)
        fs.write_file("SYSTEM/SOLID:DATA", b"x" * (2 * NDFS_PAGE_SIZE))

        props = fs.get_file_properties("SYSTEM/SOLID:DATA")

        # Empty means "checked, and there are none" - not the same as absent.
        assert props[XAT_HOLES] == []

    def test_sparse_file_reports_the_hole_positions(self, test_image):
        fs = NdfsFileSystem(test_image)
        # 6 pages, real data only on the first and last, so pages 1..4 hole out.
        fs.write_file("SYSTEM/SPARSE:DATA", _sparse_content(6, [0, 5]), holes=_holes(6, [0, 5]))

        props = fs.get_file_properties("SYSTEM/SPARSE:DATA")

        assert props[XAT_HOLES] == [1, 2, 3, 4]

    def test_hole_list_is_ascending_and_within_the_extent(self, test_image):
        fs = NdfsFileSystem(test_image)
        fs.write_file("SYSTEM/GAPPY:DATA", _sparse_content(8, [0, 3, 7]), holes=_holes(8, [0, 3, 7]))

        props = fs.get_file_properties("SYSTEM/GAPPY:DATA")
        holes = props[XAT_HOLES]

        logical_pages = (props[XAT_BYTES_IN_FILE] + NDFS_PAGE_SIZE - 1) // NDFS_PAGE_SIZE
        assert holes == sorted(holes)
        assert len(set(holes)) == len(holes)
        for page in holes:
            assert 0 <= page < logical_pages

    def test_holes_match_the_zero_entries_in_the_index(self, test_image):
        """The hole list must be exactly the zero slots of the file's index.

        This is the ground truth the sidecar is copied from, so it is what the
        test asserts. See test_pages_in_file_convention_differs_from_sintran for
        why the page count cannot be used for this check.
        """
        fs = NdfsFileSystem(test_image)
        fs.write_file("SYSTEM/CHECK:DATA", _sparse_content(6, [0, 5]), holes=_holes(6, [0, 5]))

        props = fs.get_file_properties("SYSTEM/CHECK:DATA")
        blocks = fs.get_file_blocks("SYSTEM/CHECK:DATA")

        expected = [i for i, block in enumerate(blocks) if block == 0]
        assert props[XAT_HOLES] == expected

    def test_pages_in_file_counts_allocated_pages_like_sintran(self, test_image):
        """pages_in_file is the ALLOCATED count, not the logical extent.

        This library used to write the logical extent, holes included, so a
        6-page file with 4 holes recorded 6 rather than 2. That charged the user
        quota for space no disk holds, and produced packs that disagreed with
        SINTRAN-written ones. Corrected 2026-07-30.

        The reference is a real pack: (SYSTEM)S3-CONFIG-E01:PROG in
        BIGDISK0-K-103.IMG records pages_in_file 89 while its index spans 99
        slots with 10 holes. SINTRAN itself agrees, reporting "No of pages in
        file: 89" while transferring page numbers 0..53 then 64..98.

        The logical extent is not lost - it remains
        ceil(bytes_in_file / NDFS_PAGE_SIZE), which is what get_file_blocks uses
        to walk the index. This test pins BOTH so the two cannot silently become
        the same number again.
        """
        fs = NdfsFileSystem(test_image)
        fs.write_file("SYSTEM/CONV:DATA", _sparse_content(6, [0, 5]), holes=_holes(6, [0, 5]))

        props = fs.get_file_properties("SYSTEM/CONV:DATA")
        blocks = fs.get_file_blocks("SYSTEM/CONV:DATA")

        logical_pages = (props[XAT_BYTES_IN_FILE] + NDFS_PAGE_SIZE - 1) // NDFS_PAGE_SIZE
        real_pages = sum(1 for block in blocks if block != 0)

        assert logical_pages == 6
        assert real_pages == 2

        # The field follows the allocated count, matching SINTRAN.
        assert props[XAT_PAGES_IN_FILE] == real_pages

        # And the two really are different here, so the assertion above is not vacuous.
        assert props[XAT_PAGES_IN_FILE] != logical_pages

        # The holes account for exactly the difference.
        assert len(props[XAT_HOLES]) == logical_pages - real_pages

    def test_quota_is_not_charged_for_holes(self, test_image):
        """The user's pages_used must reflect real disk consumption only.

        This is the reason the convention matters. A sparse file that reserved
        quota for its holes would let a pack run out of quota while disk space
        remained.
        """
        fs = NdfsFileSystem(test_image)

        before = fs.get_user(0).pages_used
        fs.write_file("SYSTEM/QUOTA:DATA", _sparse_content(6, [0, 5]), holes=_holes(6, [0, 5]))
        after = fs.get_user(0).pages_used

        charged = after - before
        blocks = fs.get_file_blocks("SYSTEM/QUOTA:DATA")
        real_pages = sum(1 for block in blocks if block != 0)

        # Two real data pages, plus whatever structural index pages the layout needed.
        # The point is that the four holes are NOT charged.
        assert charged >= real_pages
        assert charged < 6

    def test_read_with_properties_carries_the_holes_too(self, test_image):
        fs = NdfsFileSystem(test_image)
        fs.write_file("SYSTEM/BOTH:DATA", _sparse_content(6, [0, 5]), holes=_holes(6, [0, 5]))

        data, props = fs.read_file_with_properties("SYSTEM/BOTH:DATA")

        assert props[XAT_HOLES] == [1, 2, 3, 4]
        # And the holes still read back as zeros, so the data is unaffected.
        assert data[NDFS_PAGE_SIZE:2 * NDFS_PAGE_SIZE] == bytes(NDFS_PAGE_SIZE)


class TestXatHolesSerialization:
    """The hole map survives the JSON round trip."""

    def test_holes_survive_serialize_and_deserialize(self, test_image):
        fs = NdfsFileSystem(test_image)
        fs.write_file("SYSTEM/RTRIP:DATA", _sparse_content(6, [0, 5]), holes=_holes(6, [0, 5]))

        props = fs.get_file_properties("SYSTEM/RTRIP:DATA")
        restored = deserialize_xat(serialize_xat(props))

        assert restored[XAT_HOLES] == props[XAT_HOLES]

    def test_holes_are_a_json_array_of_numbers(self, test_image):
        """The on-disk form must be a flat array, because that is what the C and
        TypeScript ports emit. The C library stores runs internally but expands
        them here, so all four sidecars are byte-comparable."""
        fs = NdfsFileSystem(test_image)
        fs.write_file("SYSTEM/JSONF:DATA", _sparse_content(6, [0, 5]), holes=_holes(6, [0, 5]))

        raw = json.loads(serialize_xat(fs.get_file_properties("SYSTEM/JSONF:DATA")))

        assert raw["ndfs.holes"] == [1, 2, 3, 4]
        for value in raw["ndfs.holes"]:
            assert isinstance(value, int)

    def test_holes_is_an_optional_key_not_a_mandatory_one(self):
        """ALL_XAT_KEYS is the set every sidecar carries; holes is conditional.

        Keeping it out matters: several existing tests assert that every key in
        ALL_XAT_KEYS is present after serializing a bare object entry, and an
        entry alone cannot know where the holes are.
        """
        assert XAT_HOLES not in ALL_XAT_KEYS
        assert XAT_HOLES in OPTIONAL_XAT_KEYS

    def test_key_is_omitted_when_the_holes_were_not_determined(self):
        """An object entry on its own cannot know where the holes are - they live
        in the file's index. Omitting the key says "not checked", which is a
        different claim from an empty list."""
        entry = ObjectEntry()
        entry.object_name = "LONE"
        entry.type = "DATA"

        props = object_entry_to_xat(entry)

        assert XAT_HOLES not in props

    def test_applying_a_sidecar_does_not_fail_on_the_holes_key(self, test_image):
        """The holes are recorded, not restored. Applying a sidecar that carries
        them must simply ignore the key rather than error."""
        fs = NdfsFileSystem(test_image)
        fs.write_file("SYSTEM/APPLY:DATA", _sparse_content(6, [0, 5]), holes=_holes(6, [0, 5]))
        props = fs.get_file_properties("SYSTEM/APPLY:DATA")

        entry = ObjectEntry()
        from ndfs.xat import xat_to_object_entry

        xat_to_object_entry(props, entry)

        assert entry.object_name == "APPLY"
