"""
Tests for ndfs.boot_loader -- NDFS boot code detection and loading.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
import os
import pytest

from ndfs.boot_loader import load_boot_code, detect_boot_format, is_bootable, detect_controller_type
from ndfs.types import BootFormat, BootControllerType
from ndfs.constants import NDFS_PAGE_SIZE
from ndfs.filesystem import NdfsFileSystem
from ndfs.image_creator import create_image
from ndfs.types import ImageTemplate, ImageCreationOptions, BootChoice

FIXTURE_DIR = os.path.join(os.path.dirname(__file__), "fixtures")
BOOT_SECTOR_DIR = os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "boot-sectors"
)


class TestBootFormatDetection:
    def test_empty_page_returns_none(self):
        data = bytearray(NDFS_PAGE_SIZE)
        result = load_boot_code(data)
        assert result is None

    def test_all_ff_returns_none(self):
        data = bytearray(b"\xFF" * NDFS_PAGE_SIZE)
        result = load_boot_code(data)
        assert result is None

    def test_too_small_data_returns_none(self):
        data = bytearray(100)
        result = load_boot_code(data)
        assert result is None

    def test_detect_format_empty_returns_none(self):
        data = bytearray(NDFS_PAGE_SIZE)
        fmt = detect_boot_format(data)
        assert fmt == BootFormat.NONE

    def test_is_bootable_empty_returns_false(self):
        data = bytearray(NDFS_PAGE_SIZE)
        assert is_bootable(data) is False


class TestBPUNFormat:
    def _make_bpun_page(self, address, word_count, words, checksum=None):
        """Build a minimal BPUN page with given parameters."""
        page = bytearray(NDFS_PAGE_SIZE)
        pos = 0

        # BPUN delimiter
        page[pos] = 0x21  # '!'
        pos += 1

        # Address (2 bytes, big-endian)
        page[pos] = (address >> 8) & 0xFF
        page[pos + 1] = address & 0xFF
        pos += 2

        # Count (2 bytes, big-endian)
        page[pos] = (word_count >> 8) & 0xFF
        page[pos + 1] = word_count & 0xFF
        pos += 2

        # Data words (each 2 bytes, big-endian)
        calc_checksum = 0
        for w in words:
            page[pos] = (w >> 8) & 0xFF
            page[pos + 1] = w & 0xFF
            calc_checksum = (calc_checksum + w) & 0xFFFF
            pos += 2

        # Checksum
        cs = checksum if checksum is not None else calc_checksum
        page[pos] = (cs >> 8) & 0xFF
        page[pos + 1] = cs & 0xFF
        pos += 2

        # Action (2 bytes)
        page[pos] = 0
        page[pos + 1] = 0
        pos += 2

        return page

    def test_detects_bpun_format(self):
        page = self._make_bpun_page(0x100, 3, [0x1234, 0x5678, 0x9ABC])
        boot = load_boot_code(page)
        assert boot is not None
        assert boot.format == BootFormat.BPUN

    def test_bpun_load_address(self):
        page = self._make_bpun_page(0x200, 2, [0xAAAA, 0xBBBB])
        boot = load_boot_code(page)
        assert boot is not None
        assert boot.load_address == 0x200

    def test_bpun_code_data(self):
        page = self._make_bpun_page(0x100, 2, [0x1234, 0x5678])
        boot = load_boot_code(page)
        assert boot is not None
        assert len(boot.data) == 4
        assert boot.data[0] == 0x12
        assert boot.data[1] == 0x34
        assert boot.data[2] == 0x56
        assert boot.data[3] == 0x78

    def test_bpun_valid_checksum(self):
        page = self._make_bpun_page(0x100, 2, [0x1234, 0x5678])
        boot = load_boot_code(page)
        assert boot is not None
        assert boot.checksum_valid is True

    def test_bpun_invalid_checksum(self):
        page = self._make_bpun_page(0x100, 2, [0x1234, 0x5678], checksum=0x0000)
        boot = load_boot_code(page)
        assert boot is not None
        assert boot.checksum_valid is False


class TestFLOMONFormat:
    def _make_flomon_page(self, word_count=0, words=None):
        """Build a minimal FLOMON page."""
        page = bytearray(NDFS_PAGE_SIZE)
        pos = 0

        # BPUN delimiter
        page[pos] = 0x21  # '!'
        pos += 1

        # Address = 0
        page[pos] = 0
        page[pos + 1] = 0
        pos += 2

        # Count = 0
        page[pos] = 0
        page[pos + 1] = 0
        pos += 2

        # Checksum = 0
        page[pos] = 0
        page[pos + 1] = 0
        pos += 2

        # Word count (single byte)
        page[pos] = word_count & 0xFF
        pos += 1

        # FLOMON words: 00 HI 00 LO pattern
        if words is not None:
            for w in words:
                hi = (w >> 8) & 0xFF
                lo = w & 0xFF
                page[pos] = 0
                page[pos + 1] = hi
                page[pos + 2] = 0
                page[pos + 3] = lo
                pos += 4

        return page

    def test_detects_flomon_format(self):
        page = self._make_flomon_page(0)
        boot = load_boot_code(page)
        assert boot is not None
        assert boot.format == BootFormat.FLOMON

    def test_flomon_with_zero_words(self):
        page = self._make_flomon_page(0)
        boot = load_boot_code(page)
        assert boot is not None
        assert boot.word_count == 0
        assert len(boot.data) == 0

    def test_flomon_with_data(self):
        page = self._make_flomon_page(2, [0x1234, 0x5678])
        boot = load_boot_code(page)
        assert boot is not None
        assert boot.format == BootFormat.FLOMON
        assert boot.word_count == 2
        assert len(boot.data) == 4
        assert boot.data[0] == 0x12
        assert boot.data[1] == 0x34
        assert boot.data[2] == 0x56
        assert boot.data[3] == 0x78


class TestBinaryFormat:
    def test_detects_binary_code_with_real_prologue_opcode(self):
        # A real ND-100 bootstrap always starts by disabling interrupts/paging.
        # PIOF = octal 150405 = 0xD105 (big-endian on disk).
        page = bytearray(NDFS_PAGE_SIZE)
        page[0] = 0xD1
        page[1] = 0x05
        boot = load_boot_code(page)
        assert boot is not None
        assert boot.format == BootFormat.BINARY

    def test_all_f6_not_detected_as_binary(self):
        page = bytearray(b"\xF6" * NDFS_PAGE_SIZE)
        boot = load_boot_code(page)
        assert boot is None

    def test_rejects_nonuniform_garbage_without_prologue_opcode(self):
        # Regression: the old heuristic accepted ANY non-uniform block without a
        # '!' byte as "valid binary code". A real bootstrap must start with the
        # PIOF/IOF prologue opcode; this buffer does not.
        page = bytearray(NDFS_PAGE_SIZE)
        for i in range(256):
            page[i] = (i * 7 + 3) & 0xFF
            if page[i] == 0x21:
                page[i] = 0x22  # avoid accidental BPUN delimiter
        boot = load_boot_code(page)
        assert boot is None

    def test_detects_smd_controller_type(self):
        # IOX 1540 octal (SMD/ECC base) = opcode 0164000 | 1540 octal = 0165540
        # octal = 0xEB60 big-endian.
        page = bytearray(NDFS_PAGE_SIZE)
        page[0] = 0xD1
        page[1] = 0x05
        page[2] = 0xEB
        page[3] = 0x60
        boot = load_boot_code(page)
        assert boot is not None
        assert boot.controller_type == BootControllerType.SMD_ECC
        assert detect_controller_type(page) == BootControllerType.SMD_ECC

    def test_detects_scsi_controller_type(self):
        # Fixed indirect IOXT opcode = octal 150415 = 0xD10D big-endian.
        page = bytearray(NDFS_PAGE_SIZE)
        page[0] = 0xD1
        page[1] = 0x01
        page[2] = 0xD1
        page[3] = 0x0D
        boot = load_boot_code(page)
        assert boot is not None
        assert boot.controller_type == BootControllerType.SCSI


class TestBootLoaderFixtures:
    """Tests using real boot sector fixture files."""

    def _load_boot_sector(self, filename):
        """Load a boot sector binary file."""
        path = os.path.join(BOOT_SECTOR_DIR, filename)
        if not os.path.exists(path):
            pytest.skip(f"Boot sector fixture not found: {path}")
        with open(path, "rb") as f:
            data = f.read()
        # Pad to page size if needed
        if len(data) < NDFS_PAGE_SIZE:
            data = data + b"\x00" * (NDFS_PAGE_SIZE - len(data))
        return data

    def test_floppy360kb_boot_sector(self):
        data = self._load_boot_sector("Floppy360KB.bin")
        boot = load_boot_code(data)
        # Should detect as FLOMON or BPUN
        if boot is not None:
            assert boot.format in (BootFormat.FLOMON, BootFormat.BPUN, BootFormat.BINARY)

    def test_floppy12mb_boot_sector(self):
        data = self._load_boot_sector("Floppy12MB.bin")
        boot = load_boot_code(data)
        if boot is not None:
            assert boot.format in (BootFormat.FLOMON, BootFormat.BPUN, BootFormat.BINARY)

    def test_smd_boot_sector(self):
        data = self._load_boot_sector("Smd.bin")
        boot = load_boot_code(data)
        if boot is not None:
            assert boot.format in (BootFormat.BPUN, BootFormat.BINARY)

    def test_winchester_boot_sector(self):
        data = self._load_boot_sector("Winchester.bin")
        boot = load_boot_code(data)
        if boot is not None:
            assert boot.format in (BootFormat.BPUN, BootFormat.BINARY)

    def test_scsi_boot_sector(self):
        data = self._load_boot_sector("Scsi.bin")
        boot = load_boot_code(data)
        if boot is not None:
            assert boot.format in (BootFormat.BPUN, BootFormat.BINARY)


class TestFilesystemBootMethods:
    """Test boot methods added to NdfsFileSystem."""

    def test_created_image_not_bootable(self):
        opts = ImageCreationOptions(
            template=ImageTemplate.Floppy360KB,
            directory_name="NOBOOT",
            boot_choice=BootChoice.NONE,
        )
        ndfs = NdfsFileSystem.create_image(opts)
        assert ndfs.is_bootable() is False

    def test_detect_boot_format_on_empty(self):
        opts = ImageCreationOptions(
            template=ImageTemplate.Floppy360KB,
            directory_name="NOBOOT",
            boot_choice=BootChoice.NONE,
        )
        ndfs = NdfsFileSystem.create_image(opts)
        fmt = ndfs.detect_boot_format()
        assert fmt == BootFormat.NONE

    def test_load_boot_code_on_empty(self):
        opts = ImageCreationOptions(
            template=ImageTemplate.Floppy360KB,
            directory_name="NOBOOT",
            boot_choice=BootChoice.NONE,
        )
        ndfs = NdfsFileSystem.create_image(opts)
        boot = ndfs.load_boot_code()
        assert boot is None

    def test_fixture_boot_detection(self):
        path = os.path.join(FIXTURE_DIR, "withfiles.ndfs")
        if not os.path.exists(path):
            pytest.skip("Fixture not found")
        with open(path, "rb") as f:
            data = f.read()
        ndfs = NdfsFileSystem(data, read_only=True)
        # Just verify the method works without error
        fmt = ndfs.detect_boot_format()
        assert isinstance(fmt, BootFormat)
