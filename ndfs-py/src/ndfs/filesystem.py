"""
NdfsFileSystem: main class for reading and writing NDFS disk images.

Operates on a bytearray buffer representing the entire disk image.
Supports contiguous, indexed, and sub-indexed file allocation,
sparse files, user management, and quota tracking.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
HackerCorp Labs - https://github.com/HackerCorpLabs
"""
from __future__ import annotations

import math
from typing import Dict, List, NamedTuple, Optional, Sequence, Tuple, Union

from ndfs.constants import (
    NDFS_PAGE_SIZE,
    ENTRIES_PER_PAGE,
    ENTRY_SIZE,
    FILES_PER_OBJECT_BLOCK,
    MAX_OBJECT_BLOCKS,
    MAX_OBJECT_FILE_POINTERS,
    MAX_USER_FILE_POINTERS,
    PAGES_PER_INDEX_BLOCK,
    PAGES_PER_OBJECT_BLOCK,
    USERS_PER_INDEX_BLOCK,
    FIRST_ALLOCATABLE_BLOCK,
    MAX_USERS,
    NDFS_NAME_MAX,
    NDFS_TYPE_MAX,
)
from ndfs.endian import read_uint32_be, write_uint32_be
from ndfs.errors import ObjectBlockCollisionError, ObjectBlockLimitError
from ndfs.block_pointer import BlockPointer
from ndfs.master_block import MasterBlock
from ndfs.bit_file import BitFile
from ndfs.user_file import UserFile
from ndfs.object_file import ObjectFile
from ndfs.user_entry import UserEntry
from ndfs.user_friend import UserFriend
from ndfs.object_entry import (
    ObjectEntry,
    ACCESS_DEFAULT,
    FT_INDEXED,
    FT_CONTIGUOUS,
    compute_file_number,
)
from ndfs.types import PointerType, FileEntry, ImageCreationOptions, BootFormat, BootControllerType, BootCode
from ndfs.xat import object_entry_to_xat, xat_to_object_entry, XAT_HOLES
from ndfs import sintran

_BufType = Union[bytes, bytearray, memoryview]


class FriendInfo(NamedTuple):
    """One entry returned by NdfsFileSystem.list_friends."""
    index: int       # friend's user index
    name: str        # friend's user name, or "" if no such user
    bits: int        # raw 16-bit friend entry
    perms: str       # "RWACD" / "-----"


class NdfsFileSystem:
    """Main class for reading and writing NDFS disk images.

    Args:
        data: The raw disk image bytes. A trailing partial page (size not a
            whole multiple of 2048) is dropped and the image opens read-only.
        read_only: If True, write operations will raise an error.
    """

    def __init__(
        self,
        data: Union[bytes, bytearray, memoryview],
        read_only: bool = False,
    ) -> None:
        # Always work on a mutable copy
        self._data: bytearray = bytearray(data)
        self._read_only: bool = read_only

        if len(self._data) < NDFS_PAGE_SIZE:
            raise ValueError("Image too small: must be at least one NDFS page (2048 bytes)")

        # Tolerate images whose byte length is not a whole multiple of the page
        # size (a common dump artefact): work on whole pages only and drop the
        # trailing partial page, exactly as the reference ndfs does. Such images
        # are forced read-only so writing whole pages cannot lose the dropped tail.
        self._unaligned: bool = len(self._data) % NDFS_PAGE_SIZE != 0
        pages = len(self._data) // NDFS_PAGE_SIZE
        if self._unaligned:
            self._data = self._data[: pages * NDFS_PAGE_SIZE]
            self._read_only = True

        # Parse master block
        page0 = self._read_page(0)
        self._master_block: MasterBlock = MasterBlock.from_bytes(page0)
        if not self._master_block.is_valid():
            raise ValueError("Invalid NDFS master block")
        self._master_block.image_size = pages

        self._bit_file: BitFile = BitFile()
        self._user_file: UserFile = UserFile()
        self._object_file: ObjectFile = ObjectFile()

        # Load filesystem structures
        self._load_structures()

    # -- Factory methods ---------------------------------------------------

    @classmethod
    def create_image(cls, options: ImageCreationOptions) -> NdfsFileSystem:
        """Create a new NDFS disk image from options.

        Args:
            options: Image creation options specifying template and parameters.

        Returns:
            A new NdfsFileSystem instance backed by the created image.
        """
        from ndfs.image_creator import create_image as _create_image
        data = _create_image(options)
        return cls(data, read_only=False)

    # -- Lifecycle ---------------------------------------------------------

    def to_buffer(self) -> bytearray:
        """Export the current image as a new bytearray."""
        return bytearray(self._data)

    # -- Read operations ---------------------------------------------------

    def get_master_block(self) -> MasterBlock:
        """Get the parsed master block."""
        return self._master_block

    def get_directory_name(self) -> str:
        """Get the volume/directory name."""
        return self._master_block.directory_name

    def list_directory(self, path: str = "") -> List[FileEntry]:
        """List directory contents.

        - path="" or "/": lists users as directories.
        - path="USERNAME": lists that user's files.
        """
        normalized = path.strip("/")
        entries: List[FileEntry] = []

        if normalized == "":
            # Root: list users as directories
            users = self._user_file.get_users()
            for i in range(len(users)):
                u = users[i]
                entries.append(FileEntry(
                    name=u.user_name,
                    type="",
                    full_name=u.user_name,
                    user_name=u.user_name,
                    size=0,
                    pages=0,
                    is_directory=True,
                    last_modified=None,
                ))
        else:
            parts = normalized.split("/")
            if len(parts) > 1:
                raise ValueError("NDFS does not support subdirectories")
            user_name = parts[0]
            objects = self._object_file.get_user_objects(user_name)
            for i in range(len(objects)):
                obj = objects[i]
                full_name = f"{obj.object_name}:{obj.type}" if obj.type else obj.object_name
                entries.append(FileEntry(
                    name=obj.object_name,
                    type=obj.type,
                    full_name=full_name,
                    user_name=obj.user_name,
                    size=obj.bytes_in_file,
                    pages=obj.pages_in_file,
                    is_directory=False,
                    last_modified=None,
                ))
        return entries

    def read_file(self, path: str, parity: str = "none") -> bytes:
        """Read a file's contents.

        Args:
            path: "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"
            parity: Parity handling -- "none" (default, raw bytes),
                "strip" (clear bit 7, for reading ND text as ASCII).
        """
        from ndfs.parity import strip_parity as _strip

        obj = self._find_object(path)
        if obj is None:
            raise FileNotFoundError(f"File not found: {path}")
        data = bytes(self._read_object_data(obj))
        if parity == "strip":
            data = _strip(data)
        return data

    def get_metadata(self, path: str) -> Optional[FileEntry]:
        """Get file metadata, or None if not found."""
        obj = self._find_object(path)
        if obj is None:
            return None
        full_name = f"{obj.object_name}:{obj.type}" if obj.type else obj.object_name
        return FileEntry(
            name=obj.object_name,
            type=obj.type,
            full_name=full_name,
            user_name=obj.user_name,
            size=obj.bytes_in_file,
            pages=obj.pages_in_file,
            is_directory=False,
            last_modified=None,
        )

    def file_exists(self, path: str) -> bool:
        """Check if a file exists."""
        return self._find_object(path) is not None

    # -- Write operations --------------------------------------------------

    def write_file(
        self,
        path: str,
        file_data: Union[bytes, bytearray, memoryview],
        parity: str = "none",
        holes: Optional[Sequence[int]] = None,
        access_bits: Optional[int] = None,
    ) -> None:
        """Write (create or overwrite) a file.

        Every logical page is allocated a real disk block unless it is named in
        `holes`. An all-zero page is NOT a hole: a hole is a property of the
        file, so it has to be stated. Inferring one from the data produces a
        file SINTRAN cannot read -- READ-BI and READ-PROGFILE fail on a hole
        with "NO SUCH PAGE / ERROR IN ACCESSING INPUT FILE" -- see
        `_write_data_page_to_index`.

        Args:
            path: "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"
            file_data: The raw bytes to write.
            parity: Parity handling -- "none" (default, write raw bytes),
                "set" (apply ND-100 even parity before writing, for text files).
            holes: Logical page indices to leave unallocated, or None for a
                fully allocated file. The only legitimate source is the holes
                recorded from an original -- the XAT sidecar's "ndfs.holes".
            access_bits: 15-bit access word to stamp on a NEWLY CREATED file,
                or None to inherit the owning user's default file access.

                Layout (offset 26 of the object entry, uint16 BE):

                    bits 14-10  PUBLIC   5 bits
                    bits  9-5   FRIEND   5 bits
                    bits  4-0   OWN      5 bits

                and inside each 5-bit tier:

                    bit 4 Delete | bit 3 Execute/Common | bit 2 Append
                    bit 1 Write  | bit 0 Read

                PASS THIS WHEN BUILDING DISTRIBUTION MEDIA. A user created by a
                real SINTRAN carries default_file_access 0x03FF -- owner and
                friends everything, PUBLIC NOTHING -- so files inherited from it
                are invisible to every other user, SYSTEM included, and
                @LIST-FILES from SYSTEM prints nothing at all while the pack
                itself is perfectly valid. Measured on a real ND product floppy
                (210580B01-XX-01D): every file on it carries 0x07FF, which is
                the same owner/friend rights plus PUBLIC READ. Adding one file
                with 0x03FF beside them made SINTRAN list the original twelve
                and silently skip the thirteenth; changing that single field to
                0x07FF made it appear.
        """
        holes = frozenset(holes) if holes else None
        from ndfs.parity import set_parity as _set

        self._ensure_writable()

        if parity == "set":
            file_data = _set(bytes(file_data))

        user_name, object_name, file_type = self._parse_path(path)
        if not object_name:
            raise ValueError("Invalid path: filename required")

        # Find user
        user = self._resolve_user(user_name)
        if user is None:
            raise ValueError(f"User not found: {user_name or '(default)'}")

        # Calculate required pages for quota purposes. Quota (pages_used)
        # tracks only REAL disk consumption: sparse (all-zero) pages allocate
        # no block (see docs\NDFS-FORMAT.md, "No disk space is allocated for
        # sparse holes") and the index/sub-index structural pages are
        # filesystem overhead, not user data -- see
        # _count_real_data_pages_in_buffer / _count_real_data_pages_for_object.
        total_required = self._count_real_data_pages_in_buffer(file_data, holes)

        # Check for existing file. The TYPE is part of a SINTRAN file's
        # identity, so it has to be passed: without it AAA:MODE and AAA:LIST
        # resolve to the same entry and the second write silently overwrites
        # the first's data while keeping the first's type.
        existing = self._object_file.find_object(object_name, user.user_name, file_type)

        # Determine additional pages needed
        additional_needed = total_required
        if existing is not None and (existing.file_type_flags & FT_CONTIGUOUS):
            # A contiguous file already owns every page it will ever have, so a write can
            # never need more. Skipping the quota step matters: the check below would
            # otherwise expand the user's reservation for pages that are never allocated,
            # and it would do so before _update_existing_contiguous_file rejects an
            # oversized write.
            additional_needed = 0
        elif existing is not None:
            existing_total = self._count_real_data_pages_for_object(existing)
            additional_needed = total_required - existing_total if total_required > existing_total else 0

        # Check and expand quota if needed
        if additional_needed > 0:
            available_to_user = user.pages_reserved - user.pages_used
            if available_to_user < additional_needed:
                expansion = additional_needed - available_to_user
                free_on_disk = self._bit_file.get_free_pages()
                if free_on_disk < expansion:
                    raise IOError(
                        f"Insufficient disk space: need {expansion} pages, "
                        f"only {free_on_disk} available"
                    )
                user.pages_reserved += expansion

        if existing is not None:
            self._update_existing_file(existing, user, file_data, holes)
        else:
            self._create_new_file(object_name, file_type, user, file_data, holes, access_bits)
        # _update_existing_file / _create_new_file perform their own surgical
        # metadata writes (object page, owner user page, bitmap).

    def delete_file(self, path: str) -> None:
        """Delete a file."""
        self._ensure_writable()

        obj = self._find_object(path)
        if obj is None:
            raise FileNotFoundError(f"File not found: {path}")

        # Count real (non-sparse) data pages BEFORE freeing -- _free_file_blocks
        # only marks bitmap entries free, it doesn't zero the index/data page
        # bytes, but we compute this first regardless so the refund reflects
        # the file's actual on-disk footprint rather than any structural
        # overhead. See _count_real_data_pages_for_object.
        real_pages_freed = self._count_real_data_pages_for_object(obj)

        # Free blocks
        if obj.file_pointer is not None and obj.file_pointer.block_id > 0:
            self._free_file_blocks(obj)

        # Update user pages used: refund only the real data pages that were
        # charged at write time -- never the index/sub-index structural
        # block(s), which were never charged against the user's quota.
        user = self._user_file.get_user(obj.user_index)
        if user is not None:
            user.pages_used = (
                user.pages_used - real_pages_freed if user.pages_used >= real_pages_freed else 0
            )

        # Surgical writes: capture index/owner before removal, then rewrite the
        # freed object's page (zero-fill clears the slot), the owner's user
        # page, and the allocation bitmap.
        freed_index = obj.object_index
        owner = obj.user_index
        self._object_file.remove_object(obj.object_index)
        self._write_object_page(freed_index)
        self._write_user_page(owner)
        self._write_bit_file()
        self._persist_master_block()

    def rename(self, old_path: str, new_path: str) -> None:
        """Rename a file."""
        self._ensure_writable()

        obj = self._find_object(old_path)
        if obj is None:
            raise FileNotFoundError(f"File not found: {old_path}")

        _, object_name, file_type = self._parse_path(new_path)
        if not object_name:
            raise ValueError("Invalid new path")

        obj.object_name = object_name.upper()[:NDFS_NAME_MAX]
        obj.type = file_type.upper()[:NDFS_TYPE_MAX]
        # Rename touches only this file's object entry.
        self._write_object_page(obj.object_index)

    # -- User management ---------------------------------------------------

    def get_users(self) -> List[UserEntry]:
        """Get all users."""
        return self._user_file.get_users()

    def get_user(self, index: int) -> Optional[UserEntry]:
        """Get a user by index."""
        return self._user_file.get_user(index)

    def add_user(self, name: str, reserved_pages: int) -> bool:
        """Add a new user."""
        self._ensure_writable()

        if self._user_file.find_user(name) is not None:
            return False  # already exists
        idx = self._user_file.get_next_available_index()
        if idx < 0:
            return False  # no slots

        user = UserEntry()
        user.set_name(name)
        user.user_index = idx
        user.pages_reserved = reserved_pages
        self._user_file.add_user(user)
        # Ensure the UserFile data page for this slot is allocated on disk
        # before writing to it -- a fresh/small image may only have its
        # first user-file page pre-allocated, and without this the write
        # below silently no-ops for user_index >= ENTRIES_PER_PAGE, leaving
        # the new user in memory only (it vanishes on the next remount).
        self._ensure_user_dir_page(user.user_index)
        # Add-user touches only the UserFile page holding this new user.
        self._write_user_page(user.user_index)
        # _ensure_user_dir_page may have allocated a fresh bitmap page;
        # persist the bitmap and the master block's cached free-page count.
        self._write_bit_file()
        self._persist_master_block()
        return True

    def remove_user(self, index: int) -> bool:
        """Remove a user (only if they have no files)."""
        self._ensure_writable()

        files = self._object_file.get_user_objects(index)
        if len(files) > 0:
            return False  # user has files

        ok = self._user_file.remove_user(index)
        if ok:
            # Removed user's UserFile page (slot zeroed by rebuild).
            self._write_user_page(index)
        return ok

    def update_user_quota(self, index: int, new_pages: int) -> bool:
        """Update a user's page quota."""
        self._ensure_writable()
        ok = self._user_file.update_user_quota(index, new_pages)
        if ok:
            self._write_user_page(index)
        return ok

    def clear_user_password(self, index_or_name: Union[int, str]) -> bool:
        """Clear a user's password (set to 0)."""
        self._ensure_writable()

        user: Optional[UserEntry] = None
        if isinstance(index_or_name, int):
            user = self._user_file.get_user(index_or_name)
        else:
            user = self._user_file.find_user(index_or_name)
        if user is None:
            return False

        user.password = 0
        self._write_user_page(user.user_index)
        return True

    # -- Friends -----------------------------------------------------------

    def _resolve_user_ref(self, ref: Union[int, str]) -> Optional[UserEntry]:
        """Resolve a user reference (name or index) to a UserEntry, or None.
        A pure-digit string or int is treated as a user index."""
        if isinstance(ref, int):
            return self._user_file.get_user(ref)
        if ref.isdigit():
            v = int(ref)
            if 0 <= v <= 255:
                return self._user_file.get_user(v)
            return None
        return self._user_file.find_user(ref)

    def _resolve_friend_index(self, ref: Union[int, str]) -> int:
        """Resolve a friend reference to a user index. A numeric ref is taken
        literally (need not be a named user); a name must resolve to a user.
        Raises ValueError/KeyError on a bad/unknown reference."""
        if isinstance(ref, int):
            if not (0 <= ref <= 255):
                raise ValueError(f"User index out of range: {ref}")
            return ref
        if ref.isdigit():
            v = int(ref)
            if not (0 <= v <= 255):
                raise ValueError(f"User index out of range: {ref}")
            return v
        u = self._user_file.find_user(ref)
        if u is None:
            raise KeyError(f"No such user: {ref}")
        return u.user_index

    def list_friends(self, user_ref: Union[int, str]) -> List["FriendInfo"]:
        """List a user's friends. Raises KeyError if the user does not exist."""
        owner = self._resolve_user_ref(user_ref)
        if owner is None:
            raise KeyError(f"No such user: {user_ref}")
        out: List[FriendInfo] = []
        for f in owner.friends:
            if not f.entry_used:
                continue
            idx = f.friend_user_index
            fu = self._user_file.get_user(idx)
            out.append(FriendInfo(
                index=idx,
                name=fu.user_name if fu is not None else "",
                bits=f.bits,
                perms=f.get_permission_string(),
            ))
        return out

    def add_friend(self, user_ref: Union[int, str], friend_ref: Union[int, str],
                   perms: Optional[str] = "RWA") -> None:
        """Add a friend to a user with the given permission letters (default
        'RWA'). Persists the owner's user page. Raises KeyError (owner/friend
        unknown), ValueError (already a friend, list full, or bad letters)."""
        self._ensure_writable()
        owner = self._resolve_user_ref(user_ref)
        if owner is None:
            raise KeyError(f"No such user: {user_ref}")
        friend_index = self._resolve_friend_index(friend_ref)
        perm_bits = UserFriend.parse_permissions(perms if perms else "RWA")
        if owner.is_friend(friend_index):
            raise ValueError(f"Already a friend: {friend_ref}")
        if not owner.add_friend(friend_index, perm_bits):
            raise ValueError("Friend list is full (max 8)")
        self._write_user_page(owner.user_index)

    def remove_friend(self, user_ref: Union[int, str],
                      friend_ref: Union[int, str]) -> None:
        """Remove a friend from a user. Persists the owner's page. Raises
        KeyError if the owner does not exist or the friend is not present."""
        self._ensure_writable()
        owner = self._resolve_user_ref(user_ref)
        if owner is None:
            raise KeyError(f"No such user: {user_ref}")
        friend_index = self._resolve_friend_index(friend_ref)
        if not owner.remove_friend(friend_index):
            raise KeyError(f"Not a friend: {friend_ref}")
        self._write_user_page(owner.user_index)

    # -- Bitmap queries ----------------------------------------------------

    def is_block_used(self, block_id: int) -> bool:
        return self._bit_file.is_block_used(block_id)

    @property
    def unaligned(self) -> bool:
        """True if the image size was not a whole multiple of the page size.

        The trailing partial page was dropped and the filesystem forced
        read-only. False for cleanly page-aligned images.
        """
        return self._unaligned

    def get_free_pages(self) -> int:
        return self._bit_file.get_free_pages()

    def get_used_pages(self) -> int:
        return self._bit_file.calc_used_pages()

    # -- Boot loader -------------------------------------------------------

    def detect_boot_format(self) -> BootFormat:
        """Detect the boot format of this disk image."""
        from ndfs.boot_loader import detect_boot_format as _detect
        page0 = self._read_page(0)
        return _detect(page0)

    def load_boot_code(self) -> Optional[BootCode]:
        """Load boot code from page 0."""
        from ndfs.boot_loader import load_boot_code as _load
        page0 = self._read_page(0)
        return _load(page0)

    def is_bootable(self) -> bool:
        """Check if this disk image has valid boot code."""
        from ndfs.boot_loader import is_bootable as _is_bootable
        page0 = self._read_page(0)
        return _is_bootable(page0)

    def detect_boot_controller_type(self) -> BootControllerType:
        """Detect the hard-disk controller family (SMD/ECC, Winchester, SCSI)
        targeted by a raw-binary bootstrap. Returns BootControllerType.UNKNOWN
        for BPUN/FLOMON (floppy) boots or non-bootable disks."""
        from ndfs.boot_loader import detect_controller_type as _detect
        page0 = self._read_page(0)
        return _detect(page0)

    # -- Low-level access --------------------------------------------------

    def get_object_entries(self) -> List[ObjectEntry]:
        return self._object_file.get_objects()

    def get_object_entry(
        self, name: str, user_name: str, file_type: Optional[str] = None
    ) -> Optional[ObjectEntry]:
        """Look up one object entry. Pass file_type to distinguish files that
        share a name, e.g. TCP-IP-LO-D02:MODE from TCP-IP-LO-D02:LIST."""
        return self._object_file.find_object(name, user_name, file_type)

    # -- Diagnostics -------------------------------------------------------

    def verify_integrity(self) -> bool:
        """Basic integrity verification."""
        if not self._master_block.is_valid():
            return False

        # Check all file blocks are marked in bitmap
        objects = self._object_file.get_objects()
        for i in range(len(objects)):
            obj = objects[i]
            if obj.file_pointer is None or not obj.file_pointer.is_valid():
                continue
            # Check index block
            if not self._bit_file.is_block_used(obj.file_pointer.block_id):
                return False
        return True

    def generate_report(self) -> str:
        """Generate a text report about the filesystem."""
        total_pages = len(self._data) // NDFS_PAGE_SIZE
        used_pages = self._bit_file.calc_used_pages()
        free_pages = self._bit_file.get_free_pages()
        users = self._user_file.get_users()
        objects = self._object_file.get_objects()

        lines: List[str] = []
        lines.append("NDFS Filesystem Report")
        lines.append("======================")
        lines.append(f"Volume: {self._master_block.directory_name}")
        lines.append(f"Total pages: {total_pages}")
        lines.append(f"Used pages: {used_pages}")
        lines.append(f"Free pages: {free_pages}")
        lines.append(f"Users: {len(users)}")
        lines.append(f"Files: {len(objects)}")
        lines.append("")

        lines.append("Users:")
        for i in range(len(users)):
            u = users[i]
            lines.append(
                f"  [{u.user_index}] {u.user_name} - "
                f"Reserved: {u.pages_reserved}, Used: {u.pages_used}"
            )

        lines.append("")
        lines.append("Files:")
        for i in range(len(objects)):
            o = objects[i]
            lines.append(
                f"  {o.user_name}/{o.object_name}:{o.type} - "
                f"{o.bytes_in_file} bytes ({o.pages_in_file} pages)"
            )

        return "\n".join(lines) + "\n"

    # -- XAT (Extended Attribute) support --------------------------------

    def get_file_properties(self, path: str) -> Optional[dict]:
        """Get XAT properties for a file, or None if not found.

        Args:
            path: "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"
        """
        obj = self._find_object(path)
        if obj is None:
            return None
        return object_entry_to_xat(obj, self._find_holes(path))

    def read_file_with_properties(self, path: str) -> Tuple[bytes, dict]:
        """Read a file's data along with its XAT properties.

        Args:
            path: "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"

        Returns:
            Tuple of (data_bytes, xat_properties_dict).
        """
        obj = self._find_object(path)
        if obj is None:
            raise FileNotFoundError(f"File not found: {path}")
        data = bytes(self._read_object_data(obj))
        properties = object_entry_to_xat(obj, self._find_holes(path))
        return data, properties

    def _find_holes(self, path: str) -> List[int]:
        """Return the ascending logical page indices of a file's sparse holes.

        Empty for a solid file. Contiguous files are solid by construction, so
        this is only ever non-empty for Indexed and SubIndexed ones.
        """
        return [i for i, block in enumerate(self.get_file_blocks(path)) if block == 0]

    def write_file_with_properties(
        self,
        path: str,
        data: Union[bytes, bytearray, memoryview],
        properties: dict,
    ) -> None:
        """Write a file and apply XAT properties to restore metadata.

        The file is written first, then the XAT properties are applied
        to the resulting object entry.

        This is the one path that may legitimately create sparse holes: the
        sidecar's "ndfs.holes" records where the ORIGINAL file's holes were, so
        restoring them reproduces the original rather than inventing holes from
        whatever happens to be zero (see `write_file`). A sidecar without the
        key leaves the file fully allocated -- the key is optional precisely
        because a writer that never looked cannot claim the file is solid.

        Args:
            path: "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE"
            data: The raw bytes to write.
            properties: XAT properties dict to apply after writing.
        """
        holes = properties.get(XAT_HOLES) if properties else None
        if holes is not None and not isinstance(holes, (list, tuple)):
            holes = None
        self.write_file(path, data, holes=holes)

        # Now find the written entry and apply status-related XAT fields
        obj = self._find_object(path)
        if obj is not None:
            if "ndfs.access_bits" in properties and isinstance(properties["ndfs.access_bits"], int):
                obj.access_bits = properties["ndfs.access_bits"]
            if "ndfs.file_type" in properties and isinstance(properties["ndfs.file_type"], int):
                obj.file_type = properties["ndfs.file_type"]
            if "ndfs.date_created" in properties and isinstance(properties["ndfs.date_created"], int):
                obj.date_created = properties["ndfs.date_created"]
            if "ndfs.last_read_date" in properties and isinstance(properties["ndfs.last_read_date"], int):
                obj.last_date_read = properties["ndfs.last_read_date"]
            if "ndfs.last_write_date" in properties and isinstance(properties["ndfs.last_write_date"], int):
                obj.last_date_written = properties["ndfs.last_write_date"]
            # Properties change only this file's object entry.
            self._write_object_page(obj.object_index)

    # ==================================================================
    #  Private implementation
    # ==================================================================

    # -- SINTRAN initial commands -----------------------------------------

    def get_file_blocks(self, path: str) -> List[int]:
        """Return the ordered data-block IDs of a file (0 marks a sparse hole).

        The list is indexed by LOGICAL page, so a hole keeps its own position and
        the pages after it are not shifted.

        FIXED 2026-07-30. This used to walk the index for ``pages_in_file`` slots.
        That is wrong for a sparse file: ``pages_in_file`` counts the pages really
        allocated, while the index is as long as the file's logical extent, holes
        included. Measured on ``(SYSTEM)S3-CONFIG-E01:PROG`` in
        ``BIGDISK0-K-103.IMG``: ``pages_in_file`` is 89, the last used index slot
        is 98, and slots 54 to 63 are holes. Walking only 89 slots stopped at 88,
        so it dropped the last 10 real pages AND reported the 10 holes, returning
        79 real blocks instead of 89 -- truncated, and every page past slot 53 at
        the wrong offset.

        The logical extent comes from ``bytes_in_file`` rather than the page
        count. For that file 202752 / 2048 is exactly 99, which matches. The main
        read paths in the C library and in RetroFS already bound themselves this
        way; only these block-listing helpers did not.

        Confirmed independently on real hardware: transferring that file from one
        ND-100 to another sends 89 messages carrying page numbers 0..53 then
        64..98 -- one gap, exactly the holes above.
        """
        obj = self._find_object(path)
        if obj is None:
            raise FileNotFoundError(f"File not found: {path}")
        blocks: List[int] = []
        fp = obj.file_pointer
        if fp is None or fp.block_id == 0:
            return blocks

        # Logical extent, holes included. Contiguous files have no holes, so their
        # own page count is the honest bound and is kept.
        logical_pages = (obj.bytes_in_file + NDFS_PAGE_SIZE - 1) // NDFS_PAGE_SIZE

        if fp.type == PointerType.Contiguous:
            for i in range(obj.pages_in_file):
                blocks.append(fp.block_id + i)
        elif fp.type == PointerType.Indexed:
            ib = self._read_page(fp.block_id)
            for i in range(min(logical_pages, MAX_OBJECT_FILE_POINTERS)):
                blocks.append(BlockPointer.from_bytes(ib, i * 4).block_id)
        elif fp.type == PointerType.SubIndexed:
            sib = self._read_page(fp.block_id)
            remaining = logical_pages
            for si in range(MAX_OBJECT_FILE_POINTERS):
                if remaining <= 0:
                    break
                ip = BlockPointer.from_bytes(sib, si * 4)
                if ip.block_id == 0:
                    break
                ib = self._read_page(ip.block_id)
                for j in range(MAX_OBJECT_FILE_POINTERS):
                    if remaining <= 0:
                        break
                    blocks.append(BlockPointer.from_bytes(ib, j * 4).block_id)
                    remaining -= 1
        return blocks

    def patch_file_region(self, path: str, file_offset: int,
                          data: Union[bytes, bytearray, memoryview]) -> None:
        """Overwrite ``data`` at file-relative ``file_offset`` IN PLACE, touching
        only the page(s) that overlap. The file's allocation never changes, so
        this is safe on a large contiguous system file (e.g. SEGFIL0)."""
        self._ensure_writable()
        if not data:
            return
        blocks = self.get_file_blocks(path)
        end = file_offset + len(data)
        start_page = file_offset // NDFS_PAGE_SIZE
        end_page = (end - 1) // NDFS_PAGE_SIZE
        if end_page >= len(blocks):
            raise IndexError("patch region extends past the file")
        for pg in range(start_page, end_page + 1):
            if blocks[pg] == 0:
                raise IOError("patch region overlaps a sparse hole")
            page = bytearray(self._read_page(blocks[pg]))
            page_start = pg * NDFS_PAGE_SIZE
            ov_start = max(file_offset, page_start)
            ov_end = min(end, page_start + NDFS_PAGE_SIZE)
            page[ov_start - page_start:ov_end - page_start] = \
                data[ov_start - file_offset:ov_end - file_offset]
            self._write_page(blocks[pg], page)

    def _find_segfil_path(self) -> Optional[str]:
        """Path (USER/NAME:TYPE) of the SEGFIL0/SEGFIL object, or None."""
        for obj in self.get_object_entries():
            if obj.object_name.upper() in ("SEGFIL0", "SEGFIL"):
                t = (obj.type or "").rstrip("' ")
                if t:
                    return f"{obj.user_name}/{obj.object_name}:{t}"
                return f"{obj.user_name}/{obj.object_name}"
        return None

    def read_initial_commands(self) -> Optional[sintran.InitialCommands]:
        """Locate and decode the SINTRAN initial-command buffer, or None."""
        path = self._find_segfil_path()
        if path is None:
            return None
        seg = self.read_file(path)
        return sintran.locate(seg)

    def write_initial_commands(self, commands: Sequence[str]) -> bool:
        """Replace the initial-command buffer and write it back to SEGFIL0 in
        place. Returns False when the pack has no locatable buffer."""
        self._ensure_writable()
        path = self._find_segfil_path()
        if path is None:
            return False
        loc = sintran.locate(self.read_file(path))
        if loc is None:
            return False
        encoded, text_len = sintran.encode_buffer(commands)
        region = sintran.build_region_image(encoded, text_len)
        self.patch_file_region(path, loc.byte_offset, region)
        return True

    def repair_initial_commands(self, commands: Sequence[str]) -> Optional[Tuple[int, int]]:
        """Repair a header-corrupted buffer the normal locator can no longer see,
        by matching the surviving command text. Returns (byte_offset, score) or
        None when no unambiguous target is found."""
        self._ensure_writable()
        path = self._find_segfil_path()
        if path is None:
            return None
        target = sintran.locate_repair_target(self.read_file(path))
        if target is None:
            return None
        byte_offset, score = target
        encoded, text_len = sintran.encode_buffer(commands)
        region = sintran.build_region_image(encoded, text_len)
        self.patch_file_region(path, byte_offset, region)
        return byte_offset, score

    def diagnose_initial_commands(self) -> List[sintran.InitCmdCandidate]:
        """Enumerate every INIBU-containing segment candidate (diagnostic)."""
        path = self._find_segfil_path()
        if path is None:
            return []
        return sintran.enumerate_candidates(self.read_file(path))

    def _read_page(self, block_id: int) -> memoryview:
        """Read a page from the image buffer."""
        offset = block_id * NDFS_PAGE_SIZE
        if offset + NDFS_PAGE_SIZE > len(self._data):
            raise IndexError(f"Block {block_id} out of range")
        return memoryview(self._data)[offset:offset + NDFS_PAGE_SIZE]

    def _write_page(self, block_id: int, page_data: Union[bytes, bytearray, memoryview]) -> None:
        """Write a page to the image buffer."""
        offset = block_id * NDFS_PAGE_SIZE
        if offset + NDFS_PAGE_SIZE > len(self._data):
            raise IndexError(f"Block {block_id} out of range")
        self._data[offset:offset + NDFS_PAGE_SIZE] = page_data[:NDFS_PAGE_SIZE]

    def _load_structures(self) -> None:
        """Load user file, object file, and bit file from the image."""
        mb = self._master_block

        # Load user file. The object file does not depend on it, so a user file
        # that cannot be read must not take the file listing down with it: a
        # floppy whose user index page is damaged still has an intact object
        # file naming every file on the disk, and reading a damaged page raises
        # out of _read_page(). Without user names the objects keep their user
        # index and nothing else is lost.
        user_map: Dict[int, str] = {}
        if mb.user_file_pointer is not None and mb.user_file_pointer.is_valid():
            try:
                index_page = self._read_page(mb.user_file_pointer.block_id)
                self._user_file.load_from_pages(index_page, lambda bid: self._read_page(bid))

                users = self._user_file.get_users()
                for i in range(len(users)):
                    user_map[users[i].user_index] = users[i].user_name
            except Exception:
                # damaged user file: carry on with no user names
                pass

        # Load object file
        if mb.object_file_pointer is not None and mb.object_file_pointer.is_valid():
            self._object_file.load_from_pages(
                mb.object_file_pointer,
                lambda bid: self._read_page(bid),
            )

            # Resolve user names on objects
            objects = self._object_file.get_objects()
            for i in range(len(objects)):
                name = user_map.get(objects[i].user_index)
                if name is not None:
                    objects[i].user_name = name

        # Load bit file
        if mb.bit_file_pointer is not None and mb.bit_file_pointer.is_valid():
            total_pages = len(self._data) // NDFS_PAGE_SIZE
            self._bit_file.initialize(total_pages)

            # Bound the ALLOCATION window by the directory's declared capacity
            # (ext_pages_available, words 1756B-1757B) when the extended-info block is
            # valid. SINTRAN's downward allocator never hands out a page above capacity-1;
            # the gap between capacity and the physical device is the drive's bad-sector
            # spare region, which stays free-but-unreachable.
            #
            # The `<= total_pages` clamp is load-bearing, not defensive noise: the real
            # Winchester WD0.img declares a capacity of 36396 pages in a file only 36360
            # pages long. Without the clamp the allocator would start at page 36395, which
            # does not exist in the file.
            #
            # Falls back to the whole device when there is no valid extended info (FLOMON
            # floppies never have one).
            if (mb.ext_valid
                    and 0 < mb.ext_pages_available <= total_pages):
                self._bit_file.alloc_ceiling = mb.ext_pages_available

            # Determine bitmap size in pages.
            #
            # Rounded up to a whole number of 16-bit WORDS, because that is how the bit
            # file is addressed (see BitFile._bit_position). On a device whose page count
            # is not a multiple of 16, the final word is only half covered by
            # ceil(pages/8) bytes, and the pages living in its high byte would be cut off
            # - a 616-page floppy loses pages 608..615 exactly this way.
            bitmap_bytes = (math.ceil(total_pages / 8) + 1) & ~1
            bitmap_pages = math.ceil(bitmap_bytes / NDFS_PAGE_SIZE)

            # Read contiguous bitmap pages
            bitmap_data = bytearray(bitmap_pages * NDFS_PAGE_SIZE)
            for i in range(bitmap_pages):
                page = self._read_page(mb.bit_file_pointer.block_id + i)
                bitmap_data[i * NDFS_PAGE_SIZE:(i + 1) * NDFS_PAGE_SIZE] = page
            self._bit_file.load_bitmap(bitmap_data[:bitmap_bytes])

    def _structural_pages_for(self, data_pages: int) -> int:
        """Return the number of *structural* (non-data) pages a file's block
        pointer tree costs, for quota/pages_used accounting.

        - Plain Indexed layout (<=512 data pages): 1 page (the single index
          block).
        - SubIndexed layout (>512 data pages): one group index block per
          512-page group, plus one top-level sub-index block pointing at
          those group index blocks. E.g. 1000 pages -> ceil(1000/512) = 2
          group index blocks + 1 sub-index block = 3 structural pages.
        """
        if data_pages <= MAX_OBJECT_FILE_POINTERS:
            return 1
        num_index_blocks = math.ceil(data_pages / MAX_OBJECT_FILE_POINTERS)
        return num_index_blocks + 1  # + top-level sub-index block

    def _count_real_data_pages_in_buffer(self, data: _BufType,
                                         holes: Optional[frozenset] = None) -> int:
        """Count how many 2048-byte pages of *data* are NOT sparse holes --
        i.e. how many pages `_write_data_page_to_index` will actually
        allocate a real disk block for.

        Per docs\\NDFS-FORMAT.md, "No disk space is allocated for sparse
        holes"; user.pages_used quota accounting must reflect only this real
        count, never the file's full logical page count (which would count
        holes as if they consumed space) and never the index/sub-index
        structural pages (filesystem overhead, not user data -- see
        `_count_real_data_pages_for_object`).

        Uses the exact same all-zero-page test as
        `_write_data_page_to_index` (a short trailing slice, i.e. one that
        does not fill a whole page, can never be treated as a sparse hole --
        matching that method's own `len(page_slice) == NDFS_PAGE_SIZE` guard)
        so the count always agrees with what allocation will actually do.
        """
        total_pages = math.ceil(len(data) / NDFS_PAGE_SIZE)
        if total_pages == 0:
            total_pages = 1  # empty file still occupies (and charges for) one page

        real_pages = 0
        for p in range(total_pages):
            offset = p * NDFS_PAGE_SIZE
            end = min(offset + NDFS_PAGE_SIZE, len(data))
            page_slice = data[offset:end]

            # Mirrors _write_data_page_to_index exactly: a page is a hole only
            # when it was DECLARED one. Its contents never decide it.
            if not (holes and p in holes):
                real_pages += 1

        return real_pages

    def _count_real_data_pages_for_object(self, obj: ObjectEntry) -> int:
        """Count how many of *obj*'s data pages are backed by a real disk
        block (BlockID > 0), by walking its Indexed/SubIndexed block-pointer
        tree. Never counts the index/sub-index block(s) themselves -- those
        are filesystem overhead and were never charged against
        user.pages_used (see `_count_real_data_pages_in_buffer`).

        Used for the delete-time refund and the pre-write quota check against
        an existing file, where the buffer itself is not available -- only
        the on-disk block-pointer structure is.
        """
        if obj.file_pointer is None or obj.file_pointer.block_id == 0:
            return 0

        count = 0
        if obj.file_pointer.type == PointerType.Indexed:
            index_page = self._read_page(obj.file_pointer.block_id)
            for i in range(MAX_OBJECT_FILE_POINTERS):
                ptr = BlockPointer.from_bytes(index_page, i * 4)
                if ptr.block_id > 0:
                    count += 1
        elif obj.file_pointer.type == PointerType.Contiguous:
            # A Contiguous run has no sparse holes -- every page in it is a
            # real allocated block.
            count = obj.pages_in_file
        elif obj.file_pointer.type == PointerType.SubIndexed:
            sub_index_page = self._read_page(obj.file_pointer.block_id)
            for si in range(MAX_OBJECT_FILE_POINTERS):
                index_ptr = BlockPointer.from_bytes(sub_index_page, si * 4)
                if not index_ptr.is_valid():
                    continue
                index_page = self._read_page(index_ptr.block_id)
                for i in range(MAX_OBJECT_FILE_POINTERS):
                    data_ptr = BlockPointer.from_bytes(index_page, i * 4)
                    if data_ptr.block_id > 0:
                        count += 1
        return count

    def _write_data_page_to_index(
        self,
        file_data: Union[bytes, bytearray, memoryview],
        data_page_index: int,
        index_page: bytearray,
        slot_in_index: int,
        holes: Optional[frozenset] = None,
    ) -> None:
        """Write one data page and record its pointer in `index_page` at
        `slot_in_index`. Shared by both the plain Indexed allocation loop and
        each group's index block under a SubIndexed layout, so hole handling is
        identical in both layouts.

        A page becomes a sparse hole ONLY when `holes` names it. The page's
        contents never decide it.

        It used to: any page that was entirely zero and filled a whole page was
        silently made a hole. That was wrong in a way this library could not
        see, because reading such a file back returns the same bytes either way.
        SINTRAN is not so forgiving -- READ-BI and READ-PROGFILE stop at a hole
        with

            NO SUCH PAGE
            ERROR IN ACCESSING INPUT FILE

        so a :BPUN or :PROG copied onto a pack that way is a file the machine
        refuses to load. Measured on a SINTRAN III VSX-K pack 2026-08-18: the
        (TCP-IP)TCP-SER-B0..B3-B05:BPUN PIOC bank images arrived with 0/48/62/53
        invented holes, and banks 1-3 all failed with NO SUCH PAGE while bank 0
        (which had no all-zero page) loaded. The same files on genuine ND media
        have no holes at all -- a 128 KB bank dump is fully allocated even where
        the bank is empty.

        Holes are still supported; they just have to be declared, which is what
        the XAT sidecar's "ndfs.holes" records them for.
        """
        page_offset = data_page_index * NDFS_PAGE_SIZE
        page_end = min(page_offset + NDFS_PAGE_SIZE, len(file_data))
        page_slice = file_data[page_offset:page_end]

        if holes and data_page_index in holes:
            # Sparse hole: write block_id=0 to index (no disk block allocated)
            write_uint32_be(index_page, slot_in_index * 4, 0)
        else:
            # Allocate data block
            data_block_id = self._bit_file.find_first_free_block()
            if data_block_id < 0:
                raise IOError("No free blocks for data")
            self._bit_file.mark_block_used(data_block_id)

            # Write data to page
            data_page = bytearray(NDFS_PAGE_SIZE)
            data_page[:len(page_slice)] = page_slice
            self._write_page(data_block_id, data_page)

            # Write pointer to index block
            data_ptr = BlockPointer(data_block_id, PointerType.Contiguous)
            data_ptr.to_bytes(index_page, slot_in_index * 4)

    def _allocate_and_write_data(
        self, file_data: Union[bytes, bytearray, memoryview], data_pages: int,
        holes: Optional[frozenset] = None
    ) -> Tuple[int, "PointerType", int]:
        """Allocate blocks and write file data to disk.

        Returns (top_block_id, pointer_type, structural_pages_used):
        - top_block_id: block id of the top-level index or sub-index block
          (this becomes the object entry's file_pointer.block_id).
        - pointer_type: PointerType.Indexed or PointerType.SubIndexed,
          depending on whether the file fits in a single 512-pointer index
          block.
        - structural_pages_used: number of non-data pages consumed (1 for
          Indexed; numIndexBlocks+1 for SubIndexed) -- matches
          `_structural_pages_for(data_pages)` and is used by callers for
          pages_used accounting.

        Files up to MAX_OBJECT_FILE_POINTERS (512) data pages use a single
        Indexed index block (as before). Larger files (up to
        512*512 = 262144 data pages, ~512MB) use a SubIndexed layout: a
        top-level sub-index block holding up to 512 pointers to *group*
        index blocks, each of which holds up to 512 data-block pointers --
        i.e. exactly the Indexed layout, repeated per 512-page group.
        """
        use_sub_indexed = data_pages > MAX_OBJECT_FILE_POINTERS

        if not use_sub_indexed:
            # Simple indexed allocation (unchanged from before).
            index_block_id = self._bit_file.find_first_free_block()
            if index_block_id < 0:
                raise IOError("No free blocks for index block")
            self._bit_file.mark_block_used(index_block_id)

            index_page = bytearray(NDFS_PAGE_SIZE)
            for i in range(data_pages):
                self._write_data_page_to_index(file_data, i, index_page, i, holes)

            self._write_page(index_block_id, index_page)
            return index_block_id, PointerType.Indexed, 1

        # SubIndexed allocation: sub-index block -> group index blocks -> data blocks.
        if data_pages > MAX_OBJECT_FILE_POINTERS * MAX_OBJECT_FILE_POINTERS:
            raise IOError(
                "File too large for a sub-indexed object file "
                f"(>{MAX_OBJECT_FILE_POINTERS * MAX_OBJECT_FILE_POINTERS} pages)"
            )

        sub_index_block_id = self._bit_file.find_first_free_block()
        if sub_index_block_id < 0:
            raise IOError("No free blocks for sub-index block")
        self._bit_file.mark_block_used(sub_index_block_id)

        sub_index_page = bytearray(NDFS_PAGE_SIZE)
        # Each group covers up to 512 data pages; the last group may be partial.
        num_index_blocks = math.ceil(data_pages / MAX_OBJECT_FILE_POINTERS)
        structural_pages_used = 1  # count the sub-index block itself

        for si in range(num_index_blocks):
            idx_block_id = self._bit_file.find_first_free_block()
            if idx_block_id < 0:
                raise IOError("No free blocks for index block")
            self._bit_file.mark_block_used(idx_block_id)
            structural_pages_used += 1

            # Record this group's index block in the top-level sub-index block.
            idx_ptr = BlockPointer(idx_block_id, PointerType.Contiguous)
            idx_ptr.to_bytes(sub_index_page, si * 4)

            # Build this group's own index block, covering data pages
            # [start_page, end_page) of the overall file.
            index_page = bytearray(NDFS_PAGE_SIZE)
            start_page = si * MAX_OBJECT_FILE_POINTERS
            end_page = min(start_page + MAX_OBJECT_FILE_POINTERS, data_pages)

            for i in range(start_page, end_page):
                self._write_data_page_to_index(file_data, i, index_page, i - start_page, holes)

            self._write_page(idx_block_id, index_page)

        self._write_page(sub_index_block_id, sub_index_page)
        return sub_index_block_id, PointerType.SubIndexed, structural_pages_used

    def _read_object_data(self, obj: ObjectEntry) -> bytearray:
        """Read all data bytes for an object entry."""
        if obj.file_pointer is None or obj.file_pointer.block_id == 0:
            return bytearray(0)

        result = bytearray(obj.bytes_in_file)
        bytes_read = 0

        if obj.file_pointer.type == PointerType.Contiguous:
            # Sequential pages starting at block_id
            for i in range(obj.pages_in_file):
                if bytes_read >= obj.bytes_in_file:
                    break
                page = self._read_page(obj.file_pointer.block_id + i)
                to_copy = min(NDFS_PAGE_SIZE, obj.bytes_in_file - bytes_read)
                result[bytes_read:bytes_read + to_copy] = page[:to_copy]
                bytes_read += to_copy
        elif obj.file_pointer.type == PointerType.Indexed:
            bytes_read = self._read_indexed_data(obj.file_pointer.block_id, obj, result)
        elif obj.file_pointer.type == PointerType.SubIndexed:
            bytes_read = self._read_sub_indexed_data(obj.file_pointer.block_id, obj, result)

        return result

    def _read_indexed_data(
        self, index_block_id: int, obj: ObjectEntry, result: bytearray
    ) -> int:
        """Read data from an indexed file structure."""
        index_page = self._read_page(index_block_id)
        bytes_read = 0

        for i in range(MAX_OBJECT_FILE_POINTERS):
            if bytes_read >= obj.bytes_in_file:
                break
            ptr = BlockPointer.from_bytes(index_page, i * 4)

            if ptr.block_id == 0:
                # Sparse hole: result is already zero-initialized
                to_copy = min(NDFS_PAGE_SIZE, obj.bytes_in_file - bytes_read)
                bytes_read += to_copy
            else:
                page = self._read_page(ptr.block_id)
                to_copy = min(NDFS_PAGE_SIZE, obj.bytes_in_file - bytes_read)
                result[bytes_read:bytes_read + to_copy] = page[:to_copy]
                bytes_read += to_copy
        return bytes_read

    def _read_sub_indexed_data(
        self, sub_index_block_id: int, obj: ObjectEntry, result: bytearray
    ) -> int:
        """Read data from a sub-indexed file structure."""
        sub_index_page = self._read_page(sub_index_block_id)
        bytes_read = 0

        for si in range(MAX_OBJECT_FILE_POINTERS):
            if bytes_read >= obj.bytes_in_file:
                break
            index_ptr = BlockPointer.from_bytes(sub_index_page, si * 4)
            if not index_ptr.is_valid():
                continue

            index_page = self._read_page(index_ptr.block_id)
            for i in range(MAX_OBJECT_FILE_POINTERS):
                if bytes_read >= obj.bytes_in_file:
                    break
                data_ptr = BlockPointer.from_bytes(index_page, i * 4)
                if data_ptr.block_id == 0:
                    to_copy = min(NDFS_PAGE_SIZE, obj.bytes_in_file - bytes_read)
                    bytes_read += to_copy
                else:
                    page = self._read_page(data_ptr.block_id)
                    to_copy = min(NDFS_PAGE_SIZE, obj.bytes_in_file - bytes_read)
                    result[bytes_read:bytes_read + to_copy] = page[:to_copy]
                    bytes_read += to_copy
        return bytes_read

    def _allocate_user_object_slot(self, user) -> int:
        """Pick a free object slot for *user*, opening a new object block if needed.

        A user's blocks all sit at slot ``user % 64`` of an index-block group. Block 0 is
        in the HOME group ``user // 64``; further blocks take the same slot in a HIGHER
        group, skipping any group whose slot belongs to a user that already exists.

        That skipping is what SINTRAN does, measured live on 2026-08-02: user 8 growing
        past 256 files did NOT take group 1 (user 72 lives there) - it moved on. SINTRAN
        goes further still and RELOCATES an overflow block when the rightful home owner
        later needs the group; this library instead declines to place a block in another
        user's home in the first place, which never produces a layout needing relocation.

        Raises IOError only when the user is genuinely at their ceiling, or when no group
        can be found for another block.
        """
        def group_available(group: int) -> bool:
            # The user whose HOME this group/slot is. Placing an overflow block there
            # would be taken back by SINTRAN the moment that user creates a file, so
            # avoid it if that user exists at all.
            home_owner = group * USERS_PER_INDEX_BLOCK + (user.user_index % USERS_PER_INDEX_BLOCK)
            if home_owner == user.user_index:
                return True
            if home_owner >= MAX_USERS:
                return True
            return self._user_file.get_user(home_owner) is None

        slot = self._object_file.find_free_user_slot(
            user.user_index,
            user.max_object_blocks,
            user.allocated_object_blocks,
            group_available,
        )
        if slot < 0:
            raise ObjectBlockLimitError(
                user.user_name,
                user.max_object_blocks,
                user.allocated_object_blocks,
                is_hard_limit=user.max_object_blocks >= MAX_OBJECT_BLOCKS,
            )

        # Count the distinct groups this user now occupies: that IS the allocated block
        # count, and byte 47's low nibble records it.
        groups = set(self._object_file.groups_used_by(user.user_index))
        page = slot // ENTRIES_PER_PAGE
        groups.add(page // PAGES_PER_INDEX_BLOCK)
        user.allocated_object_blocks = max(1, min(user.max_object_blocks, len(groups)))

        return slot

    # ------------------------------------------------------------------
    # Object block administration - the @GIVE-OBJECT-BLOCKS equivalent
    # ------------------------------------------------------------------

    def get_object_block_info(self, user_name_or_index):
        """Report a user's object-block position: what they hold and what they may hold.

        This is the query a user interface needs in order to say anything useful
        about "why can I not add another file". It returns a plain dict:

            user_name                the user area
            user_index               its index in the user file
            allocated_object_blocks  ACOBL - blocks in use, 1..max
            max_object_blocks        MXOBL - blocks permitted, 1..16
            files_in_use             entries actually present for this user
            max_files                MXOBL * 256, what @USER-STATISTICS prints
                                     as MAXIMUM NUMBER OF FILES
            allocated_files          ACOBL * 256, capacity without growing
            can_grant                True while MXOBL < 16
            grantable_blocks         16 - MXOBL, how many give_object_blocks may add

        Blocks are allocated on demand, so ``allocated_object_blocks`` climbs by
        itself as files are created; only ``max_object_blocks`` needs an operator.

        Args:
            user_name_or_index: User name (case-insensitive) or user index.

        Returns:
            The dict described above.

        Raises:
            ValueError: If no such user exists.
        """
        user = self._require_user(user_name_or_index)
        files_in_use = len(self._object_file.get_user_objects(user.user_index))
        return {
            "user_name": user.user_name,
            "user_index": user.user_index,
            "allocated_object_blocks": user.allocated_object_blocks,
            "max_object_blocks": user.max_object_blocks,
            "files_in_use": files_in_use,
            "max_files": user.max_object_blocks * FILES_PER_OBJECT_BLOCK,
            "allocated_files": user.allocated_object_blocks * FILES_PER_OBJECT_BLOCK,
            "can_grant": user.max_object_blocks < MAX_OBJECT_BLOCKS,
            "grantable_blocks": MAX_OBJECT_BLOCKS - user.max_object_blocks,
        }

    def give_object_blocks(self, user_name_or_index, count: int = 1) -> int:
        """Grant *count* additional object blocks to a user, raising their file ceiling.

        This is the library equivalent of the SINTRAN operator command::

            @GIVE-OBJECT-BLOCKS (<directory>:)<user>,<count>

        which likewise ADDS to the maximum rather than setting it. Each block is
        256 files, so granting 3 blocks to a default user takes their ceiling
        from 256 to 1024 - exactly what ``@USER-STATISTICS`` then reports as
        MAXIMUM NUMBER OF FILES. VERIFIED on a live SINTRAN III K pack: user
        BIGMAN went from 256 to 1024 after ``@GIVE-OBJECT-BLOCKS BIGMAN,3``, and
        user-entry byte 47 read back 0x31 = MXOBL 4.

        What this does NOT do, deliberately:

          * It does not allocate the blocks. Only the MAXIMUM moves. SINTRAN
            allocates a block the first time a file needs it, and so does this
            library - see ``_allocate_user_object_slot``. A user granted 4 blocks
            who holds 300 files still has ACOBL 2.
          * It does not reserve disk pages. Object blocks limit the number of
            FILES, not their size; page quota is separate
            (``pages_reserved``/``@GIVE-USER-SPACE``).
          * It does not convert an Indexed object file to SubIndexed. A second
            object block lives at page 512 and up, which an Indexed object file
            cannot address at all; growth into it fails later, when a file
            actually needs the block. See docs/NDFS-OBJECT-BLOCKS-SPEC.md
            section 3.

        Args:
            user_name_or_index: User name (case-insensitive) or user index.
            count: Blocks to ADD, 1 or more. Defaults to 1.

        Returns:
            The user's new maximum object block count, 2..16.

        Raises:
            ValueError: If no such user exists, if *count* is below 1, or if the
                grant would push the maximum past the SINTRAN limit of 16 blocks.
                The limit case reports how many blocks are still grantable so the
                caller can retry with a figure that fits.
        """
        self._ensure_writable()
        if count < 1:
            raise ValueError(f"count must be 1 or more, got {count}")

        user = self._require_user(user_name_or_index)
        new_max = user.max_object_blocks + count
        if new_max > MAX_OBJECT_BLOCKS:
            grantable = MAX_OBJECT_BLOCKS - user.max_object_blocks
            raise ValueError(
                f"Cannot give user {user.user_name} {count} more object block(s): they "
                f"have {user.max_object_blocks} and SINTRAN allows at most "
                f"{MAX_OBJECT_BLOCKS} ({MAX_OBJECT_BLOCKS * FILES_PER_OBJECT_BLOCK} "
                f"files). At most {grantable} more can be granted."
            )

        user.max_object_blocks = new_max
        self._write_user_page(user.user_index)
        return new_max

    def take_object_blocks(self, user_name_or_index, count: int = 1) -> int:
        """Lower a user's object-block maximum by *count*.

        **This has no SINTRAN equivalent.** No command to remove object blocks is
        documented in the manuals or was found while carving the kernel, and that
        is consistent with the structure: file numbers are stable identifiers
        derived from the block a file sits in, so dropping a block would orphan
        every file in it. This exists only so a tool can undo its own grant on an
        image it is building, and it refuses anything that would strand a file.

        The maximum can never fall below the number of blocks actually allocated,
        nor below 1.

        Args:
            user_name_or_index: User name (case-insensitive) or user index.
            count: Blocks to remove, 1 or more. Defaults to 1.

        Returns:
            The user's new maximum object block count.

        Raises:
            ValueError: If no such user exists, if *count* is below 1, or if the
                reduction would drop the maximum below the allocated count.
        """
        self._ensure_writable()
        if count < 1:
            raise ValueError(f"count must be 1 or more, got {count}")

        user = self._require_user(user_name_or_index)
        new_max = user.max_object_blocks - count
        if new_max < user.allocated_object_blocks:
            raise ValueError(
                f"Cannot take {count} object block(s) from user {user.user_name}: "
                f"{user.allocated_object_blocks} of {user.max_object_blocks} are "
                "allocated and hold files. Lowering the maximum below the allocated "
                "count would orphan them."
            )
        if new_max < 1:
            raise ValueError("A user must keep at least one object block")

        user.max_object_blocks = new_max
        self._write_user_page(user.user_index)
        return new_max

    def _require_user(self, user_name_or_index) -> UserEntry:
        """Look a user up by name or index, raising ValueError when absent.

        Thin wrapper over the existing ``_resolve_user_ref`` (which returns None
        for a miss) so the object-block calls can fail loudly instead of each
        repeating the same None check.
        """
        user = self._resolve_user_ref(user_name_or_index)
        if user is None:
            raise ValueError(f"User not found: {user_name_or_index}")
        return user

    def _ensure_object_dir_page(self, object_index: int) -> None:
        """Ensure the object-file directory page holding *object_index* exists,
        allocating and linking it on demand (each user's region grows as
        needed). Page index in the object-file index block is object_index/32;
        for user U that maps to index-pointer slots U*8..U*8+7.

        If the object file is still a plain Indexed structure and growth
        requires a page_idx past MAX_OBJECT_FILE_POINTERS (512 -- i.e. past
        user #63's slots), it is converted to SubIndexed in place first (see
        the one-time-conversion block below) before falling through to the
        SubIndexed handling for the actual requested page.
        """
        mb = self._master_block
        if mb.object_file_pointer is None or not mb.object_file_pointer.is_valid():
            return
        page_idx = object_index // ENTRIES_PER_PAGE

        def alloc_page() -> int:
            blk = self._bit_file.find_first_free_block()
            if blk < 0:
                raise IOError("No free blocks for object directory page")
            self._bit_file.mark_block_used(blk)
            self._write_page(blk, bytearray(NDFS_PAGE_SIZE))
            return blk

        if mb.object_file_pointer.type == PointerType.Indexed and page_idx >= MAX_OBJECT_FILE_POINTERS:
            # The object file (volume-wide directory listing every file across
            # every user) has outgrown a single Indexed index block: 512
            # directory-page slots is only 512*32 = 16384 objects, i.e. 64
            # users at 8 reserved index-pointer slots each. Perform a
            # one-time conversion to SubIndexed: allocate a fresh sub-index
            # block, install the EXISTING Indexed block as its slot-0 entry
            # (no data migration -- that block's directory-page pointers for
            # page_idx 0..511 stay exactly where they are), then repoint the
            # master block's object_file_pointer at the new sub-index block
            # as SubIndexed and persist it. This mirrors the fix already
            # applied to the C# NdfsFileSystem.EnsureObjectDirPage.
            old_index_block = mb.object_file_pointer.block_id
            new_sub_index_block = alloc_page()
            sub_page = bytearray(NDFS_PAGE_SIZE)
            BlockPointer(old_index_block, PointerType.Indexed).to_bytes(sub_page, 0)
            self._write_page(new_sub_index_block, sub_page)
            mb.object_file_pointer = BlockPointer(new_sub_index_block, PointerType.SubIndexed)
            self._persist_master_block()

        if mb.object_file_pointer.type == PointerType.Indexed:
            index_page = bytearray(self._read_page(mb.object_file_pointer.block_id))
            ptr = BlockPointer.from_bytes(index_page, page_idx * 4)
            if ptr.is_valid():
                return
            blk = alloc_page()
            BlockPointer(blk, PointerType.Contiguous).to_bytes(index_page, page_idx * 4)
            self._write_page(mb.object_file_pointer.block_id, index_page)
        elif mb.object_file_pointer.type == PointerType.SubIndexed:
            sub_idx = page_idx // MAX_OBJECT_FILE_POINTERS
            inner_idx = page_idx % MAX_OBJECT_FILE_POINTERS
            sub_page = bytearray(self._read_page(mb.object_file_pointer.block_id))
            sub_ptr = BlockPointer.from_bytes(sub_page, sub_idx * 4)
            if not sub_ptr.is_valid():
                ib = alloc_page()
                BlockPointer(ib, PointerType.Contiguous).to_bytes(sub_page, sub_idx * 4)
                self._write_page(mb.object_file_pointer.block_id, sub_page)
                sub_ptr = BlockPointer(ib, PointerType.Contiguous)
            inner_page = bytearray(self._read_page(sub_ptr.block_id))
            ptr = BlockPointer.from_bytes(inner_page, inner_idx * 4)
            if ptr.is_valid():
                return
            blk = alloc_page()
            BlockPointer(blk, PointerType.Contiguous).to_bytes(inner_page, inner_idx * 4)
            self._write_page(sub_ptr.block_id, inner_page)

    def _ensure_user_dir_page(self, user_index: int) -> None:
        """Ensure the UserFile data page holding *user_index* exists,
        allocating and linking it on demand.

        The UserFile is always a plain Indexed structure (max 256 users
        fits in the MAX_USER_FILE_POINTERS-slot index block -- see
        docs\\NDFS-FORMAT.md's "User File | Indexed (01)" entry), so unlike
        `_ensure_object_dir_page` there is no SubIndexed case to handle
        here: this mirrors only that method's single-level-index branch.
        """
        mb = self._master_block
        if mb.user_file_pointer is None or not mb.user_file_pointer.is_valid():
            return
        page_idx = user_index // ENTRIES_PER_PAGE
        if page_idx >= MAX_USER_FILE_POINTERS:
            return

        index_page = bytearray(self._read_page(mb.user_file_pointer.block_id))
        ptr = BlockPointer.from_bytes(index_page, page_idx * 4)
        if ptr.is_valid():
            return

        blk = self._bit_file.find_first_free_block()
        if blk < 0:
            raise IOError("No free blocks for user directory page")
        self._bit_file.mark_block_used(blk)
        self._write_page(blk, bytearray(NDFS_PAGE_SIZE))
        BlockPointer(blk, PointerType.Contiguous).to_bytes(index_page, page_idx * 4)
        self._write_page(mb.user_file_pointer.block_id, index_page)

    def _create_new_file(
        self,
        object_name: str,
        file_type: str,
        user: UserEntry,
        file_data: Union[bytes, bytearray, memoryview],
        holes: Optional[frozenset] = None,
        access_bits: Optional[int] = None,
    ) -> None:
        """Create a new file on the filesystem."""
        data_pages = math.ceil(len(file_data) / NDFS_PAGE_SIZE)
        if data_pages == 0:
            data_pages = 1
        # A single indexed block holds 512 data-page pointers; larger files
        # (up to 512*512 = 262144 pages, ~512MB) use a SubIndexed layout --
        # see _allocate_and_write_data. Files beyond that ceiling still raise
        # (a real hardware limit: a sub-index block can only address 512
        # group index blocks).
        if data_pages > MAX_OBJECT_FILE_POINTERS * MAX_OBJECT_FILE_POINTERS:
            raise IOError(
                "File too large for a sub-indexed object file "
                f"(>{MAX_OBJECT_FILE_POINTERS * MAX_OBJECT_FILE_POINTERS} pages)"
            )

        # Choose the object slot inside the OWNING USER's region (SINTRAN
        # partitions the object file: user U owns slots U*256..U*256+255 and
        # the object-index high byte is the owner) and ensure its directory
        # page exists, before allocating file data.
        obj_index = self._allocate_user_object_slot(user)
        self._ensure_object_dir_page(obj_index)

        # Allocate index/sub-index block(s) and write data (sparse-aware).
        # Picks Indexed (<=512 pages) or SubIndexed (>512 pages) automatically.
        top_block_id, top_pointer_type, structural_pages = self._allocate_and_write_data(
            file_data, data_pages, holes
        )

        # Create object entry
        entry = ObjectEntry()
        entry.object_index = obj_index
        # The user-visible number is not the physical position once a user owns
        # more than one object block - compute it here too, or a freshly created
        # entry reads 0 until the pack is reloaded.
        entry.file_number = compute_file_number(obj_index, user.user_index)
        entry.object_name = object_name.upper()[:NDFS_NAME_MAX]
        entry.type = file_type.upper()[:NDFS_TYPE_MAX]
        entry.user_index = user.user_index
        entry.user_name = user.user_name
        # pages_in_file counts the pages ACTUALLY ALLOCATED, not the logical extent.
        #
        # Corrected 2026-07-30 to match SINTRAN. On a real pack, (SYSTEM)S3-CONFIG-E01:PROG
        # records 89 while its index spans 99 slots with 10 holes, and the machine itself
        # reports "No of pages in file: 89" while transferring page numbers 0..53 then
        # 64..98. Writing the logical extent here charged the user quota for holes that
        # occupy no disk, and produced packs that disagreed with SINTRAN-written ones.
        #
        # The logical extent is not lost - it is ceil(bytes_in_file / NDFS_PAGE_SIZE), and
        # get_file_blocks uses exactly that to walk the index.
        entry.pages_in_file = self._count_real_data_pages_in_buffer(file_data, holes)
        entry.bytes_in_file = len(file_data) if len(file_data) > 0 else 1
        entry.file_pointer = BlockPointer(top_block_id, top_pointer_type)
        # A new file inherits the OWNING USER's default file access, which is
        # what SINTRAN does (@SET-DEFAULT-FILE-ACCESS sets the value copied onto
        # every file the user then creates). It used to be a flat ACCESS_DEFAULT
        # (0x03FF: owner and friends everything, PUBLIC nothing), which ignored
        # the user entry and left the file unreadable by anyone else -- SYSTEM
        # included, since SYSTEM is neither owner nor friend of an ordinary
        # user. That bites at once in practice: the RT-LOADER runs as SYSTEM, so
        # a :BPUN copied into a user area could not be read by the very thing
        # meant to load it. A brand-new user gets 0x04FF, which grants PUBLIC
        # READ. Falls back to ACCESS_DEFAULT only when the user entry carries no
        # default at all.
        #
        # file_type_flags: SINTRAN uses the same FT_INDEXED flag for both
        # Indexed and SubIndexed layouts -- the actual layout is carried by the
        # BlockPointer's own type field, not by a separate flag bit.
        # An EXPLICIT access_bits always wins: inheriting is right for a file
        # created on a live machine, but wrong for distribution media, where the
        # owning user's default may grant PUBLIC nothing and make every file
        # invisible to SYSTEM. See write_file's docstring for the measurement.
        if access_bits is not None:
            entry.access_bits = access_bits & 0x7FFF
        else:
            entry.access_bits = (user.default_file_access & 0x7FFF) \
                if user.default_file_access else ACCESS_DEFAULT
        entry.file_type_flags = FT_INDEXED
        # object_index already encodes [user|fileEntry]; the object-index word
        # and the self-referential version chain all equal it.
        entry.disk_object_index = obj_index
        entry.next_version = obj_index
        entry.prev_version = obj_index
        self._object_file.add_object(entry)

        # Update user pages used: only the REAL (non-sparse) data pages --
        # never the index/sub-index structural pages, which are filesystem
        # overhead, not user data. See _count_real_data_pages_in_buffer.
        user.pages_used += self._count_real_data_pages_in_buffer(file_data, holes)

        # Surgical writes: new object's page, owner's user page, bitmap.
        self._write_object_page(entry.object_index)
        self._write_user_page(user.user_index)
        self._write_bit_file()
        self._persist_master_block()

    def create_contiguous_file(self, path: str, pages: int) -> None:
        """Create an empty contiguous file occupying a fixed run of `pages` pages.

        This is SINTRAN's ``@CREATE-FILE name pages`` -- the form that produces a file the
        machine reports as ``CONTINUOUS FILE``. Every page is allocated up front and the
        pages are consecutive on the pack, but the file itself is created empty.

        The reservation is fixed for the life of the file. A later write of more than
        `pages` pages raises instead of growing the file or converting it to an indexed
        one, because the pages following the run belong to other files and cannot be
        claimed -- SINTRAN cannot grow a continuous file either.

        Args:
            path: "USERNAME/FILENAME:TYPE" or "FILENAME:TYPE".
            pages: Number of pages to reserve. Must be at least 1.

        Raises:
            ValueError: The path carries no file name, `pages` is below 1, or the named
                user does not exist.
            FileExistsError: A file of that name already exists for the user.
            IOError: No free run of `pages` consecutive pages exists, or the user's
                object table is full.
        """
        self._ensure_writable()

        if pages < 1:
            raise ValueError("A contiguous file needs at least one page")

        user_name, object_name, file_type = self._parse_path(path)
        if not object_name:
            raise ValueError("Invalid path: filename required")

        user = self._resolve_user(user_name)
        if user is None:
            raise ValueError(f"User not found: {user_name or '(default)'}")

        # Same as write_file: NAME:TYPE identifies the file, so a contiguous
        # FOO:DATA must not collide with an existing FOO:PROG.
        if self._object_file.find_object(object_name, user.user_name, file_type) is not None:
            raise FileExistsError(f"File already exists: {path}")

        # Every page of a contiguous file is real disk, so the whole run is charged to the
        # user's quota at once -- there are no holes to discount, unlike an indexed file.
        available_to_user = user.pages_reserved - user.pages_used
        if available_to_user < pages:
            expansion = pages - available_to_user
            free_on_disk = self._bit_file.get_free_pages()
            if free_on_disk < expansion:
                raise IOError(
                    f"Insufficient disk space: need {expansion} more pages for the quota, "
                    f"only {free_on_disk} available"
                )
            user.pages_reserved += expansion

        obj_index = self._allocate_user_object_slot(user)
        self._ensure_object_dir_page(obj_index)

        # The defining property: one run, no index block. find_free_block_range is the
        # only allocator that can promise consecutive blocks -- the sparse allocator picks
        # each page independently and would scatter them.
        start_block = self._bit_file.find_free_block_range(pages)
        if start_block < 0:
            raise IOError(f"No contiguous run of {pages} free pages is available on this pack")

        self._bit_file.allocate_blocks(start_block, pages)

        # Zero the run. A freshly created file must not expose whatever a deleted file left
        # behind on those pages.
        empty_page = bytes(NDFS_PAGE_SIZE)
        for i in range(pages):
            self._write_page(start_block + i, empty_page)

        entry = ObjectEntry()
        entry.object_index = obj_index
        # The user-visible number is not the physical position once a user owns
        # more than one object block - compute it here too, or a freshly created
        # entry reads 0 until the pack is reloaded.
        entry.file_number = compute_file_number(obj_index, user.user_index)
        entry.object_name = object_name.upper()[:NDFS_NAME_MAX]
        entry.type = file_type.upper()[:NDFS_TYPE_MAX]
        entry.user_index = user.user_index
        entry.user_name = user.user_name

        # Unlike an indexed file, allocated and logical extent are the same here, and both
        # equal the reservation -- there are no holes in a contiguous run.
        entry.pages_in_file = pages

        # Created empty: the pages exist, but no byte of them is file content yet.
        entry.bytes_in_file = 0

        # Points straight at the first data page. There is no index block to walk: page N
        # of the file is simply block (file_pointer.block_id + N).
        entry.file_pointer = BlockPointer(start_block, PointerType.Contiguous)
        entry.access_bits = ACCESS_DEFAULT

        # Contiguous, not indexed -- this is the flag SINTRAN prints as "CONTINUOUS FILE".
        entry.file_type_flags = FT_CONTIGUOUS
        entry.disk_object_index = obj_index
        entry.next_version = obj_index
        entry.prev_version = obj_index
        self._object_file.add_object(entry)

        user.pages_used += pages

        self._write_object_page(entry.object_index)
        self._write_user_page(user.user_index)
        self._write_bit_file()
        self._persist_master_block()

    def _update_existing_contiguous_file(
        self,
        existing: ObjectEntry,
        file_data: Union[bytes, bytearray, memoryview],
    ) -> None:
        """Write into an existing contiguous file, in place and within its reservation.

        The run is never reallocated, so unlike the indexed path this neither frees nor
        claims blocks, and the user's page count does not move: the pages were charged at
        creation and stay charged until the file is deleted.

        Raises:
            IOError: `file_data` needs more pages than the file reserved, or the file has
                no allocated run.
        """
        reserved_pages = existing.pages_in_file
        needed_pages = math.ceil(len(file_data) / NDFS_PAGE_SIZE)

        if needed_pages > reserved_pages:
            raise IOError(
                f"Cannot write {len(file_data)} bytes ({needed_pages} pages) to contiguous "
                f"file '{existing.object_name}': it reserved {reserved_pages} pages and a "
                "contiguous file cannot grow."
            )

        if existing.file_pointer is None or existing.file_pointer.block_id == 0:
            raise IOError(f"Contiguous file '{existing.object_name}' has no allocated run")

        start_block = existing.file_pointer.block_id
        data = bytes(file_data)

        # Write the run straight through. Every page of the reservation is rewritten, not
        # just the pages the data covers, so bytes left over from a previous longer write
        # cannot reappear past the new end of file.
        for i in range(reserved_pages):
            offset = i * NDFS_PAGE_SIZE
            chunk = data[offset:offset + NDFS_PAGE_SIZE]
            if len(chunk) < NDFS_PAGE_SIZE:
                chunk = chunk + bytes(NDFS_PAGE_SIZE - len(chunk))
            self._write_page(start_block + i, chunk)

        existing.bytes_in_file = len(data)

        # pages_in_file stays at the reservation. For a contiguous file the pages are
        # allocated whether or not the data reaches them, so counting only the written
        # pages would under-report the disk this file actually holds.
        self._write_object_page(existing.object_index)

    def _update_existing_file(
        self,
        existing: ObjectEntry,
        user: UserEntry,
        file_data: Union[bytes, bytearray, memoryview],
        holes: Optional[frozenset] = None,
    ) -> None:
        """Update an existing file with new data."""
        # A contiguous file keeps the run it was created with. It is never converted to an
        # indexed file behind the caller's back, and it never grows: SINTRAN cannot grow a
        # CONTINUOUS file either, because the pages after the run belong to someone else.
        if existing.file_type_flags & FT_CONTIGUOUS:
            self._update_existing_contiguous_file(existing, file_data)
            return

        # Count the REAL (non-sparse) data pages the old file was charged for
        # BEFORE freeing its blocks (structural index/sub-index blocks are
        # never part of this count -- they were never charged against the
        # user's quota; see _count_real_data_pages_for_object).
        old_real_pages = self._count_real_data_pages_for_object(existing)

        # Free old blocks (data blocks, plus index/sub-index structural
        # block(s) -- _free_file_blocks already handles both Indexed and
        # SubIndexed layouts via obj.file_pointer.type).
        self._free_file_blocks(existing)

        # Subtract old usage
        user.pages_used = user.pages_used - old_real_pages if user.pages_used >= old_real_pages else 0

        # Create new allocation. Files up to 512 pages use a single Indexed
        # index block; larger files (up to 262144 pages) use a SubIndexed
        # layout -- _allocate_and_write_data raises past that hard ceiling.
        data_pages = math.ceil(len(file_data) / NDFS_PAGE_SIZE)
        if data_pages == 0:
            data_pages = 1
        if data_pages > MAX_OBJECT_FILE_POINTERS * MAX_OBJECT_FILE_POINTERS:
            raise IOError(
                "File too large for a sub-indexed object file "
                f"(>{MAX_OBJECT_FILE_POINTERS * MAX_OBJECT_FILE_POINTERS} pages)"
            )

        top_block_id, top_pointer_type, structural_pages = self._allocate_and_write_data(
            file_data, data_pages, holes
        )

        # Update existing entry
        # Allocated count, not the logical extent - see the note at the create site.
        existing.pages_in_file = self._count_real_data_pages_in_buffer(file_data, holes)
        existing.bytes_in_file = len(file_data) if len(file_data) > 0 else 1
        existing.file_pointer = BlockPointer(top_block_id, top_pointer_type)

        # Update user pages used: only the REAL (non-sparse) data pages of the
        # new content -- never the index/sub-index structural pages. See
        # _count_real_data_pages_in_buffer.
        user.pages_used += self._count_real_data_pages_in_buffer(file_data, holes)

        # Surgical writes: the file's object page, owner's user page, bitmap.
        self._write_object_page(existing.object_index)
        self._write_user_page(user.user_index)
        self._write_bit_file()
        self._persist_master_block()

    def _free_file_blocks(self, obj: ObjectEntry) -> None:
        """Free all blocks associated with a file."""
        if obj.file_pointer is None or obj.file_pointer.block_id == 0:
            return

        if obj.file_pointer.type == PointerType.Indexed:
            index_page = self._read_page(obj.file_pointer.block_id)
            for i in range(MAX_OBJECT_FILE_POINTERS):
                ptr = BlockPointer.from_bytes(index_page, i * 4)
                if ptr.block_id > 0:
                    self._bit_file.mark_block_free(ptr.block_id)
            # Free the index block itself
            self._bit_file.mark_block_free(obj.file_pointer.block_id)
        elif obj.file_pointer.type == PointerType.Contiguous:
            self._bit_file.free_blocks(obj.file_pointer.block_id, obj.pages_in_file)
        elif obj.file_pointer.type == PointerType.SubIndexed:
            sub_index_page = self._read_page(obj.file_pointer.block_id)
            for si in range(MAX_OBJECT_FILE_POINTERS):
                index_ptr = BlockPointer.from_bytes(sub_index_page, si * 4)
                if not index_ptr.is_valid():
                    continue
                index_page = self._read_page(index_ptr.block_id)
                for i in range(MAX_OBJECT_FILE_POINTERS):
                    data_ptr = BlockPointer.from_bytes(index_page, i * 4)
                    if data_ptr.block_id > 0:
                        self._bit_file.mark_block_free(data_ptr.block_id)
                self._bit_file.mark_block_free(index_ptr.block_id)
            self._bit_file.mark_block_free(obj.file_pointer.block_id)

    # NDFS writes are immediate and surgical (matching RetroCommander/RetroCore):
    # a mutation rewrites ONLY the block(s) it touched, never the whole
    # filesystem. Each helper rebuilds a single 2048-byte page from the
    # in-memory model; rebuilding zero-filled also clears freed slots so a
    # deleted file/user does not reappear on reload.

    def _write_bit_file(self) -> None:
        """Write the BitFile allocation bitmap (small, contiguous)."""
        mb = self._master_block
        if mb.bit_file_pointer is None or not mb.bit_file_pointer.is_valid():
            return
        pages = self._bit_file.to_page_buffers()
        for i in range(len(pages)):
            self._write_page(mb.bit_file_pointer.block_id + i, pages[i])

    def _persist_master_block(self) -> None:
        """Refresh the master block's cached "Unreserved Pages" field (page 0,
        offset MASTER_BLOCK_OFFSET+0x1C) from the live bitmap and write it
        back to disk.

        Real SINTRAN reads this cached count to report free disk space
        without rescanning the bitmap, so it must be kept in sync with the
        actual bitmap state after every allocation-affecting mutation.
        Callers must invoke this after `_write_bit_file()` succeeds (i.e.
        after the bitmap mutation that changed free-page count), never on
        error/early-return paths.

        `MasterBlock.write_to_bytes` only patches its own 32-byte region
        (page 0, MASTER_BLOCK_OFFSET..MASTER_BLOCK_OFFSET+32); it does not
        touch the separate Extended Info Block checksum at offset 0x07D0,
        so that unrelated structure is left byte-identical.
        """
        self._master_block.unreserved_pages = self._bit_file.get_free_pages()
        page0 = bytearray(self._read_page(0))
        self._master_block.write_to_bytes(page0)
        self._write_page(0, page0)

    def _write_user_page(self, user_index: int) -> None:
        """Write only the UserFile data page holding `user_index`."""
        mb = self._master_block
        if mb.user_file_pointer is None or not mb.user_file_pointer.is_valid():
            return
        page_index = user_index // ENTRIES_PER_PAGE
        if page_index >= MAX_USER_FILE_POINTERS:
            return
        index_page = self._read_page(mb.user_file_pointer.block_id)
        ptr = BlockPointer.from_bytes(index_page, page_index * 4)
        if not ptr.is_valid():
            return
        self._write_page(ptr.block_id, self._user_file.to_data_page(page_index))

    def _object_page_block(self, page_index: int) -> Optional[int]:
        """Resolve the on-disk data block backing ObjectFile page `page_index`."""
        mb = self._master_block
        if mb.object_file_pointer is None or not mb.object_file_pointer.is_valid():
            return None
        if mb.object_file_pointer.type == PointerType.Indexed:
            if page_index >= MAX_OBJECT_FILE_POINTERS:
                return None
            index_page = self._read_page(mb.object_file_pointer.block_id)
            ptr = BlockPointer.from_bytes(index_page, page_index * 4)
            return ptr.block_id if ptr.is_valid() else None
        if mb.object_file_pointer.type == PointerType.SubIndexed:
            sub_idx = page_index // MAX_OBJECT_FILE_POINTERS
            inner_idx = page_index % MAX_OBJECT_FILE_POINTERS
            sub_index_page = self._read_page(mb.object_file_pointer.block_id)
            sub_ptr = BlockPointer.from_bytes(sub_index_page, sub_idx * 4)
            if not sub_ptr.is_valid():
                return None
            inner_index_page = self._read_page(sub_ptr.block_id)
            data_ptr = BlockPointer.from_bytes(inner_index_page, inner_idx * 4)
            return data_ptr.block_id if data_ptr.is_valid() else None
        return None

    def _write_object_page(self, object_index: int) -> None:
        """Write only the ObjectFile data page holding `object_index`.

        Rebuilt zero-filled, which clears any slot freed by a delete.
        """
        page_index = object_index // ENTRIES_PER_PAGE
        data_block = self._object_page_block(page_index)
        if data_block is None:
            return
        self._write_page(data_block, self._object_file.to_data_page(page_index))

    def _find_object(self, path: str) -> Optional[ObjectEntry]:
        """Find an object entry by path."""
        user_name, object_name, file_type = self._parse_path(path)
        if file_type:
            search_name = f"{object_name}:{file_type}"
        else:
            search_name = object_name

        objects = self._object_file.get_objects()
        for i in range(len(objects)):
            o = objects[i]
            if user_name and o.user_name.upper() != user_name.upper():
                continue

            full_name = f"{o.object_name}:{o.type}" if o.type else o.object_name
            if full_name.upper() == search_name.upper():
                return o
            # Also try matching object name only (without type)
            if not file_type and o.object_name.upper() == object_name.upper():
                return o
        return None

    def _parse_path(self, path: str) -> Tuple[str, str, str]:
        """Parse a path into (user_name, object_name, file_type)."""
        normalized = path.strip("/")
        parts = normalized.split("/")

        user_name = ""
        file_name_part = ""

        if len(parts) >= 2:
            user_name = parts[0]
            file_name_part = "/".join(parts[1:])
        else:
            file_name_part = parts[0]

        # Split filename into name and type (separator is : or last .)
        object_name = file_name_part
        file_type = ""

        colon_idx = file_name_part.find(":")
        if colon_idx >= 0:
            object_name = file_name_part[:colon_idx]
            file_type = file_name_part[colon_idx + 1:]
        else:
            dot_idx = file_name_part.rfind(".")
            if dot_idx >= 0:
                object_name = file_name_part[:dot_idx]
                file_type = file_name_part[dot_idx + 1:]

        return user_name, object_name, file_type

    def _resolve_user(self, user_name: str) -> Optional[UserEntry]:
        """Resolve a user by name, or return the first user if no name given."""
        if user_name:
            return self._user_file.find_user(user_name)
        # Default to first user
        users = self._user_file.get_users()
        return users[0] if len(users) > 0 else None

    def _ensure_writable(self) -> None:
        """Raise if the filesystem is read-only."""
        if self._read_only:
            raise IOError("Filesystem is read-only")
