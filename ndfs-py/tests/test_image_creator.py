"""
Tests for ndfs.image_creator -- NDFS image creation.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import pytest

from ndfs.filesystem import NdfsFileSystem
from ndfs.image_creator import create_image, TemplateSpec
from ndfs.types import ImageTemplate, ImageCreationOptions, UserCreationInfo
from ndfs.constants import NDFS_PAGE_SIZE


class TestFloppy360KB:
    def test_creates_correct_size(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy360KB, directory_name="FLOPPY360")
        data = create_image(opts)
        assert len(data) == 154 * NDFS_PAGE_SIZE

    def test_opens_as_valid_ndfs(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy360KB, directory_name="FLOPPY360")
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        assert ndfs.get_directory_name() == "FLOPPY360"

    def test_has_system_user(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy360KB)
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        users = ndfs.get_users()
        assert len(users) == 1
        assert users[0].user_name == "SYSTEM"

    def test_master_block_pointers(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy360KB)
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        mb = ndfs.get_master_block()
        assert mb.object_file_pointer.block_id == 149
        assert mb.user_file_pointer.block_id == 151
        assert mb.bit_file_pointer.block_id == 153


class TestFloppy12MB:
    def test_creates_correct_size(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy12MB, directory_name="FLOPPY12")
        data = create_image(opts)
        assert len(data) == 616 * NDFS_PAGE_SIZE

    def test_opens_as_valid_ndfs(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy12MB, directory_name="FLOPPY12")
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        assert ndfs.get_directory_name() == "FLOPPY12"

    def test_master_block_pointers(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy12MB)
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        mb = ndfs.get_master_block()
        assert mb.object_file_pointer.block_id == 611
        assert mb.user_file_pointer.block_id == 613
        assert mb.bit_file_pointer.block_id == 615


class TestSmd75MB:
    def test_creates_correct_size(self):
        opts = ImageCreationOptions(template=ImageTemplate.Smd75MB, directory_name="SMD75")
        data = create_image(opts)
        assert len(data) == 38400 * NDFS_PAGE_SIZE

    def test_opens_as_valid_ndfs(self):
        opts = ImageCreationOptions(template=ImageTemplate.Smd75MB, directory_name="SMD75")
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        assert ndfs.get_directory_name() == "SMD75"

    def test_master_block_pointers(self):
        opts = ImageCreationOptions(template=ImageTemplate.Smd75MB)
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        mb = ndfs.get_master_block()
        assert mb.object_file_pointer.block_id == 18684
        assert mb.user_file_pointer.block_id == 18686
        # ALBIT 137526B: floor(36945/2)=18472 rounded DOWN to a multiple of 9 = 18468 --
        # the true PACK-ONE bit_file_ptr. This asserted 18472 (plain pages/2), off by 4.
        assert mb.bit_file_pointer.block_id == 18468


class TestWinchester74MB:
    def test_creates_correct_size(self):
        opts = ImageCreationOptions(template=ImageTemplate.Winchester74MB, directory_name="WIN74")
        data = create_image(opts)
        # Real Winchester drive = a Micropolis 1325 (5.25" ST-506/MFM), measured across 7
        # real images: device 36864 pages = exactly 72.0 MiB, capacity 36396, spare 468.
        # This asserted 36360 pages -- SMALLER than the declared 36396-page capacity, so the
        # created image's last 36 pages did not exist at all.
        assert len(data) == 36864 * NDFS_PAGE_SIZE

    def test_opens_as_valid_ndfs(self):
        opts = ImageCreationOptions(template=ImageTemplate.Winchester74MB, directory_name="WIN74")
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        assert ndfs.get_directory_name() == "WIN74"

    def test_master_block_pointers(self):
        opts = ImageCreationOptions(template=ImageTemplate.Winchester74MB)
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        mb = ndfs.get_master_block()
        # Measured from the real Micropolis 1325 images.
        assert mb.object_file_pointer.block_id == 18428
        assert mb.user_file_pointer.block_id == 18430
        assert mb.bit_file_pointer.block_id == 18198  # 9*floor(floor(36396/2)/9)


class TestCustomTemplate:
    def test_creates_custom_small_image(self):
        opts = ImageCreationOptions(
            template=ImageTemplate.Custom,
            directory_name="CUSTOM",
            custom_pages=100,
        )
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        assert ndfs.get_directory_name() == "CUSTOM"
        assert len(data) == 100 * NDFS_PAGE_SIZE

    def test_creates_custom_large_image(self):
        opts = ImageCreationOptions(
            template=ImageTemplate.Custom,
            directory_name="BIGCUSTOM",
            custom_pages=2000,
        )
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        assert ndfs.get_directory_name() == "BIGCUSTOM"

    def test_rejects_too_small_custom(self):
        opts = ImageCreationOptions(
            template=ImageTemplate.Custom,
            custom_pages=5,
        )
        with pytest.raises(ValueError):
            create_image(opts)


class TestBitmapAllocation:
    def test_system_blocks_marked_used(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy360KB, directory_name="TEST")
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        # Block 0 (master) should be used
        assert ndfs.is_block_used(0) is True
        # Object file block
        assert ndfs.is_block_used(149) is True
        # User file block
        assert ndfs.is_block_used(151) is True
        # Bit file block
        assert ndfs.is_block_used(153) is True

    def test_free_blocks_available(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy360KB, directory_name="TEST")
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        free = ndfs.get_free_pages()
        assert free > 100  # Most of 154 pages should be free


class TestSystemUser:
    def test_system_user_created(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy360KB, directory_name="TEST")
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        users = ndfs.get_users()
        assert len(users) >= 1
        system_user = None
        for i in range(len(users)):
            if users[i].user_name == "SYSTEM":
                system_user = users[i]
                break
        assert system_user is not None
        assert system_user.user_index == 0

    def test_system_user_has_reserved_pages(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy360KB, directory_name="TEST")
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        users = ndfs.get_users()
        system_user = users[0]
        assert system_user.pages_reserved > 0


class TestAdditionalUsers:
    def test_creates_with_extra_users(self):
        opts = ImageCreationOptions(
            template=ImageTemplate.Floppy12MB,
            directory_name="MULTI",
            users=[
                UserCreationInfo(name="ALICE", reserved_pages=100),
                UserCreationInfo(name="BOB", reserved_pages=200),
            ],
        )
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        users = ndfs.get_users()
        assert len(users) == 3  # SYSTEM + ALICE + BOB

        names = []
        for i in range(len(users)):
            names.append(users[i].user_name)
        assert "SYSTEM" in names
        assert "ALICE" in names
        assert "BOB" in names


class TestCreateImageClassMethod:
    def test_filesystem_create_image(self):
        opts = ImageCreationOptions(
            template=ImageTemplate.Floppy360KB,
            directory_name="CLASSMETHOD",
        )
        ndfs = NdfsFileSystem.create_image(opts)
        assert ndfs.get_directory_name() == "CLASSMETHOD"
        assert len(ndfs.get_users()) >= 1

    def test_write_file_after_creation(self):
        opts = ImageCreationOptions(
            template=ImageTemplate.Floppy12MB,
            directory_name="WRITABLE",
        )
        ndfs = NdfsFileSystem.create_image(opts)
        ndfs.write_file("SYSTEM/HELLO:DATA", b"Hello from new image!")
        content = ndfs.read_file("SYSTEM/HELLO:DATA")
        assert content == b"Hello from new image!"


class TestDefaultDirectoryName:
    def test_uses_empty_disk_when_no_name(self):
        opts = ImageCreationOptions(template=ImageTemplate.Floppy360KB)
        data = create_image(opts)
        ndfs = NdfsFileSystem(data)
        assert ndfs.get_directory_name() == "EMPTY-DISK"
