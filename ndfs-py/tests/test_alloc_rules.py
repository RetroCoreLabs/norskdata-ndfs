"""
Tests for SINTRAN's page allocation rules and image layout (docs/NDFS-ALLOCATION-RULES.md,
docs/CREATE-DIRECTORY-PLACEMENT.md).

The expected numbers come from real SINTRAN M runs in the nd100x emulator: bit file address,
first single page of 12 disk formats, contiguous stacks, index block placement and quota.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs.bit_file import BitFile
from ndfs.constants import NDFS_PAGE_SIZE
from ndfs.filesystem import NdfsFileSystem
from ndfs.image_creator import create_image, options_for_disk_type
from ndfs.types import ImageTemplate, ImageCreationOptions, BootChoice
from ndfs.disk_types import DiskFamily, DISK_TYPES, disk_type_count, find_disk_type, get_disk_type
from ndfs.boot_blobs import boot_blob, SCSI_LAST_PAGE


def make_bf(cap, ba):
    """A bit file for a disk of `cap` pages with the bit file at `ba`: page 0 and the bit file pages used."""
    bf = BitFile()
    bf.initialize(cap)
    bf.alloc_ceiling = cap
    bf.bit_address = ba
    bf.mark_block_used(0)
    for i in range((cap + 16383) // 16384):
        bf.mark_block_used(ba + i)
    return bf


def take_hint(bf):
    p = bf.find_free_hint()
    bf.mark_block_used(p)
    return p


def take_block(bf):
    p = bf.find_free_data_block()
    bf.mark_block_used(p)
    return p


class TestHintRule:
    """RSBPG: first free page going down from (BA - 1) OR 15."""

    def test_smd75_four_pages(self):
        bf = make_bf(36945, 18468)
        assert [take_hint(bf) for _ in range(4)] == [18479, 18478, 18477, 18476]

    def test_bit_file_in_home_word(self):
        # BA 70191 ends in binary 1111: the bit file holds the top of the home word.
        bf = make_bf(140391, 70191)
        assert [take_hint(bf) for _ in range(4)] == [70190, 70189, 70188, 70187]

    def test_winchester14(self):
        bf = make_bf(6912, 3456)
        assert [take_hint(bf) for _ in range(4)] == [3455, 3454, 3453, 3452]

    def test_spiral_when_home_word_full(self):
        # Read in the SINTRAN code, not measured: word+1 lowest bit first, then word-1 highest bit first.
        bf = make_bf(36945, 18468)
        for p in range(18464, 18480):
            bf.mark_block_used(p)
        assert take_hint(bf) == 18480
        assert take_hint(bf) == 18481
        for p in range(18482, 18496):
            bf.mark_block_used(p)
        assert take_hint(bf) == 18463

    def test_limits(self):
        bf = make_bf(100, 50)
        for p in range(7, 100):
            if not bf.is_block_used(p):
                bf.mark_block_used(p)
        assert bf.find_free_hint() == -1
        bf.mark_block_free(99)
        assert bf.find_free_hint() == 99

    def test_falls_back_without_bit_address(self):
        bf = make_bf(1000, 500)
        bf.bit_address = 0
        top = bf.find_first_free_block()
        assert bf.find_free_hint() == top
        assert bf.find_free_data_block() == top


class TestBlockRule:
    """RSPAG: lowest free page of a 256 page block, then block+1, block-1, ..."""

    COLD_START = [
        # capacity, bit file address, first page given on a cold start (measured with SINTRAN M)
        (18486, 9243, 16384),     # DISC-38MB
        (34765, 17379, 32768),    # DISC-70MB
        (36945, 18468, 32768),    # DISC-75MB
        (69530, 34758, 65536),    # DISC-140MB-F
        (140391, 70191, 131072),  # DISC-288MB
        (220584, 110286, 212992), # DISC-450MB
        (22032, 11016, 16384),    # DISC-45MB
        (36396, 18198, 32768),    # DISC-74MB
        (6912, 3456, 3328),       # DISC-14MB
        (10728, 5364, 5120),      # DISC-21MB
        (11016, 5508, 5376),      # DISC-23MB
        (13648, 6822, 6656),      # DISC-28MB
    ]

    @pytest.mark.parametrize("cap,ba,first", COLD_START)
    def test_cold_start_first_page(self, cap, ba, first):
        bf = make_bf(cap, ba)
        assert take_block(bf) == first
        assert take_block(bf) == first + 1   # the next call stays in the block

    def test_spiral(self):
        bf = make_bf(36945, 18468)
        for p in range(32768, 33024):
            bf.mark_block_used(p)               # cold start block full
        assert take_block(bf) == 33024          # block + 1
        for p in range(33025, 33280):
            bf.mark_block_used(p)               # block + 1 full
        assert take_block(bf) == 33280          # the cursor moved to block + 1; its next block up
        bf._cursor_block = 128                  # back to the first block: up is full, so down
        assert take_block(bf) == 32512

    def test_cursor_follows_last_page(self):
        bf = make_bf(36945, 18468)
        assert take_block(bf) == 32768
        bf._cursor_block = 72                   # 18432..18687, the block of BA + 1
        assert take_block(bf) == 18432
        for p in range(18433, 18468):
            bf.mark_block_used(p)
        assert take_block(bf) == 18471          # lowest free page of the block


def new_smd75():
    return NdfsFileSystem(create_image(ImageCreationOptions(
        template=ImageTemplate.Smd75MB, directory_name="ALLOC")))


def used_of(ndfs, name):
    return next(u.pages_used for u in ndfs.get_users() if u.user_name == name)


def entry_of(ndfs, name):
    return next(e for e in ndfs.get_object_entries() if e.object_name == name.split(":")[0])


class TestFileSystemPlacement:
    def test_contiguous_stack_from_top(self):
        ndfs = new_smd75()
        ndfs.create_contiguous_file("SYSTEM/C1:DATA", 10)
        ndfs.create_contiguous_file("SYSTEM/C2:DATA", 20)
        assert entry_of(ndfs, "C1:DATA").file_pointer.block_id == 36935
        assert entry_of(ndfs, "C2:DATA").file_pointer.block_id == 36915

    def test_single_page_file_uses_block_rule(self):
        ndfs = new_smd75()
        ndfs.create_contiguous_file("SYSTEM/N1:DATA", 1)
        ndfs.create_contiguous_file("SYSTEM/N2:DATA", 1)
        assert entry_of(ndfs, "N1:DATA").file_pointer.block_id == 32768
        assert entry_of(ndfs, "N2:DATA").file_pointer.block_id == 32769

    def test_indexed_file_placement(self):
        ndfs = new_smd75()
        ndfs.create_contiguous_file("SYSTEM/N1:DATA", 1)
        ndfs.create_contiguous_file("SYSTEM/N2:DATA", 1)
        ndfs.write_file("SYSTEM/N3:DATA", bytes([0x5A]) * (3 * NDFS_PAGE_SIZE))
        assert entry_of(ndfs, "N3:DATA").file_pointer.block_id == 18475   # index block, hint rule
        assert ndfs.get_file_blocks("SYSTEM/N3:DATA") == [32770, 32771, 32772]

    def test_quota_charges_index_and_object_pages(self):
        ndfs = new_smd75()
        assert used_of(ndfs, "SYSTEM") == 1                   # its object data page
        ndfs.write_file("SYSTEM/Q1:DATA", bytes([0x11]) * (3 * NDFS_PAGE_SIZE))
        assert used_of(ndfs, "SYSTEM") == 1 + 3 + 1           # + 3 data + 1 index
        assert ndfs.add_user("ALPHA", 50)
        assert used_of(ndfs, "ALPHA") == 0
        ndfs.write_file("ALPHA/A1:DATA", bytes([0x22]) * NDFS_PAGE_SIZE)
        assert used_of(ndfs, "ALPHA") == 1 + 1 + 1            # data + index + object page
        ndfs.delete_file("ALPHA/A1:DATA")
        assert used_of(ndfs, "ALPHA") == 1                    # the object page stays charged
        ndfs.delete_file("SYSTEM/Q1:DATA")
        assert used_of(ndfs, "SYSTEM") == 1

    def test_quota_large_file_sub_index(self):
        ndfs = new_smd75()
        data = bytes((i * 7 + 1) & 0xFF for i in range(600 * NDFS_PAGE_SIZE))
        ndfs.write_file("SYSTEM/BIGF:DATA", data)
        assert used_of(ndfs, "SYSTEM") == 1 + 600 + 3         # top block + 2 sub-index blocks
        assert ndfs.verify_integrity() is True
        assert ndfs.read_file("SYSTEM/BIGF:DATA") == data

    def test_bit_file_sized_from_capacity(self):
        # Device 153576 pages, directory 140391 (DISC-288MB-F): 9 bit file pages, not the 10 that the
        # device length would give. The page after the bit file stays untouched.
        opts = ImageCreationOptions(template=ImageTemplate.Custom, directory_name="BIGDEV",
                                    custom_pages=140391, device_pages=153576, include_extended_info=True)
        image = create_image(opts)
        assert len(image) == 153576 * NDFS_PAGE_SIZE
        ndfs = NdfsFileSystem(image)
        after = ndfs.get_master_block().bit_file_pointer.block_id + 9
        ndfs.write_file("SYSTEM/BIG:DATA", bytes([1, 2, 3, 4, 5, 6, 7, 8]))
        buf = ndfs.to_buffer()
        assert not any(buf[after * NDFS_PAGE_SIZE:(after + 1) * NDFS_PAGE_SIZE])
        assert ndfs.verify_integrity() is True
        assert ndfs.read_file("SYSTEM/BIG:DATA") == bytes([1, 2, 3, 4, 5, 6, 7, 8])


def ptr(buf, off):
    return int.from_bytes(buf[0x7E0 + off:0x7E0 + off + 4], "big") & 0x3FFFFFFF


def raw_bit(buf, bit_file, page):
    return (buf[bit_file * NDFS_PAGE_SIZE + ((page >> 3) ^ 1)] >> (page & 7)) & 1


class TestCreatedLayout:
    def make(self, template, answer=0, boot=BootChoice.NONE):
        return bytes(create_image(ImageCreationOptions(
            template=template, directory_name="RULES", bit_file_answer=answer, boot_choice=boot)))

    def test_scsi_126mb(self):
        img = self.make(ImageTemplate.Scsi126MB)
        assert len(img) == 64656 * NDFS_PAGE_SIZE
        assert (ptr(img, 0x10), ptr(img, 0x14), ptr(img, 0x18)) == (30506, 30508, 30510)
        ndfs = NdfsFileSystem(img)
        assert ndfs.get_master_block().ext_pages_available == 61036

    def test_scsi_last_page_written(self):
        img = self.make(ImageTemplate.Scsi126MB)
        assert img[64655 * NDFS_PAGE_SIZE:] == SCSI_LAST_PAGE

    def test_bit_file_answers_smd(self):
        img = self.make(ImageTemplate.Smd75MB, -1)     # end of disc: capacity - 3 bit file pages
        assert (ptr(img, 0x18), ptr(img, 0x14), ptr(img, 0x10)) == (36942, 36940, 36938)
        img = self.make(ImageTemplate.Smd75MB, 16384)  # a typed address
        assert (ptr(img, 0x18), ptr(img, 0x14), ptr(img, 0x10)) == (16384, 16382, 16380)

    def test_default_pointers_per_template(self):
        expected = {
            ImageTemplate.Floppy360KB: (76, 78, 72),
            ImageTemplate.Floppy12MB: (316, 318, 306),
            ImageTemplate.Smd75MB: (18476, 18478, 18468),
            ImageTemplate.Winchester74MB: (18204, 18206, 18198),
            ImageTemplate.Scsi126MB: (30506, 30508, 30510),
        }
        for tmpl, (obj, usr, bit) in expected.items():
            img = self.make(tmpl)
            assert (ptr(img, 0x10), ptr(img, 0x14), ptr(img, 0x18)) == (obj, usr, bit), tmpl

    def test_unreserved_pages_formula(self):
        # capacity - 1 - bitmap pages - 2 - 1 - the SYSTEM quota; SMD 75MB: 36945-1-3-2-1-18472 = 18466
        img = self.make(ImageTemplate.Smd75MB)
        assert int.from_bytes(img[0x7E0 + 0x1C:0x7E0 + 0x20], "big") == 18466

    def test_pages_above_capacity_marked_used(self):
        img = self.make(ImageTemplate.Smd75MB)
        ba = ptr(img, 0x18)
        assert raw_bit(img, ba, 36945) == 1
        assert raw_bit(img, ba, 37119) == 1
        assert raw_bit(img, ba, 37120) == 0
        assert raw_bit(img, ba, 0) == 1
        assert raw_bit(img, ba, 1) == 0 and raw_bit(img, ba, 6) == 0

    def test_all_bit_file_pages_marked_used(self):
        img = self.make(ImageTemplate.Scsi126MB)
        ba = ptr(img, 0x18)
        for i in range(4):
            assert raw_bit(img, ba, ba + i) == 1

    def test_floppy_has_no_extended_info(self):
        img = self.make(ImageTemplate.Floppy12MB)
        assert not any(img[0x7D0:0x7E0])

    def test_boot_code_written_and_override(self):
        img = self.make(ImageTemplate.Smd75MB, 0, BootChoice.AUTO)
        assert img[:2000] == boot_blob("smd")
        assert img[0] == 0xD1 and img[1] == 0x05      # PIOF, the first word of every hard disk boot area
        img = self.make(ImageTemplate.Smd75MB, 0, BootChoice.NONE)
        assert not any(img[:2000])
        own = bytes([1, 2, 3, 4, 5, 6, 7, 8])
        img = bytes(create_image(ImageCreationOptions(
            template=ImageTemplate.Floppy360KB, directory_name="OWNBOOT", boot_data=own)))
        assert img[:8] == own and img[8] == 0


class TestDiskTypes:
    def test_table(self):
        assert disk_type_count() == 40
        assert find_disk_type("DISC-75MB").pages == 36945
        assert find_disk_type("disc-74mb").pages == 36396          # case-insensitive
        assert find_disk_type("DISC-450MB-F").pages == 220584
        assert find_disk_type("DISC-14MB").pages == 6912
        assert find_disk_type("DISC-90MB").pages == 7389
        assert find_disk_type("DISC-SCSI-1") is None
        assert find_disk_type("DISC-75MB-1") is None
        assert find_disk_type("") is None
        assert find_disk_type("DISC-74MB").family == DiskFamily.WINCHESTER and find_disk_type("DISC-74MB").tested
        assert find_disk_type("DISC-75MB").family == DiskFamily.SMD
        assert not find_disk_type("DISC-30MB").tested
        assert find_disk_type("DISC-10MB").pages == 0              # valid name, size not known
        assert get_disk_type(0) is not None and get_disk_type(40) is None
        for t in DISK_TYPES:
            assert find_disk_type(t.name) is t
            assert t.pages == 0 or t.pages >= 32

    def test_layout_pages(self):
        assert find_disk_type("DISC-288MB-R").layout_pages == 140733
        assert find_disk_type("DISC-288MB-F").layout_pages == 153576
        assert find_disk_type("DISC-450MB-F").layout_pages == 230364
        assert find_disk_type("DISC-450MB-N").layout_pages == 223820
        assert find_disk_type("DISC-75MB").layout_pages == 37035
        assert find_disk_type("DISC-30MB").layout_pages == 0
        assert find_disk_type("DISC-140MB-F").layout_pages == 0

    def test_create_from_disk_type(self):
        opts = options_for_disk_type("DISC-14MB", "DISC14")
        img = create_image(opts)
        assert len(img) == 6912 * NDFS_PAGE_SIZE
        ndfs = NdfsFileSystem(img)
        assert ndfs.get_master_block().ext_pages_available == 6912

    def test_create_288_uses_full_layout_length(self):
        img = create_image(options_for_disk_type("DISC-288MB-F", "BIG"))
        assert len(img) == 153576 * NDFS_PAGE_SIZE
        ndfs = NdfsFileSystem(img)
        mb = ndfs.get_master_block()
        assert (mb.object_file_pointer.block_id, mb.user_file_pointer.block_id,
                mb.bit_file_pointer.block_id) == (70187, 70189, 70191)
        assert mb.ext_pages_available == 140391

    def test_unknown_or_unsized_names_are_refused(self):
        with pytest.raises(ValueError):
            options_for_disk_type("DISC-NOPE")
        with pytest.raises(ValueError):
            options_for_disk_type("DISC-10MB")

    def test_device_pages_smaller_than_capacity_is_ignored(self):
        img = create_image(ImageCreationOptions(template=ImageTemplate.Custom, custom_pages=6912,
                                                device_pages=100, include_extended_info=True))
        assert len(img) == 6912 * NDFS_PAGE_SIZE


class TestDefaultFileAccess:
    def test_system_and_extra_users_get_0x04ff(self):
        from ndfs.types import UserCreationInfo
        ndfs = NdfsFileSystem(create_image(ImageCreationOptions(
            template=ImageTemplate.Custom, custom_pages=200,
            users=[UserCreationInfo(name="ALICE", reserved_pages=50)])))
        assert ndfs.get_user(0).default_file_access == 0x04FF
        alice = next(u for u in ndfs.get_users() if u.user_name == "ALICE")
        assert alice.default_file_access == 0x04FF
