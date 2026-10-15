"""
NDFS access control: determines permission level and checks operations.

3-tier hierarchy:
  Own    - user owns the file
  Friend - user is in owner's friend list
  Public - everyone else

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from typing import Optional

from ndfs.types import FileAccessType, FileOperationType
from ndfs.object_entry import ObjectEntry
from ndfs.user_entry import UserEntry
from ndfs.user_friend import UserFriend
from ndfs.access_permissions import (
    AccessPermissions,
    PERM_READ,
    PERM_WRITE,
    PERM_APPEND,
    PERM_EXECUTE,
    PERM_DELETE,
)


def get_access_level(
    file: ObjectEntry,
    user: UserEntry,
    owner: UserEntry,
) -> FileAccessType:
    """Determine the access level for a user relative to a file."""
    if user.user_index == owner.user_index:
        return FileAccessType.Own
    if owner.is_friend(user.user_index):
        return FileAccessType.Friend
    return FileAccessType.Public


def get_friend_permissions(
    user: UserEntry,
    owner: UserEntry,
) -> Optional[UserFriend]:
    """Get friend-specific permissions (if user is in owner's friend list)."""
    return owner.get_friend(user.user_index)


def _operation_to_perm_bit(operation: FileOperationType) -> int:
    """Map a FileOperationType to the corresponding permission bit."""
    if operation == FileOperationType.Read:
        return PERM_READ
    elif operation == FileOperationType.Write:
        return PERM_WRITE
    elif operation == FileOperationType.Append:
        return PERM_APPEND
    elif operation == FileOperationType.Execute:
        return PERM_EXECUTE
    elif operation == FileOperationType.Delete:
        return PERM_DELETE
    elif operation == FileOperationType.List:
        return PERM_READ  # List uses read permission
    return PERM_READ


def check_access(
    file: ObjectEntry,
    user: UserEntry,
    owner: UserEntry,
    operation: FileOperationType,
) -> bool:
    """Check if a user can perform an operation on a file.

    Friend-specific permissions override file-level friend permissions.
    """
    access_level = get_access_level(file, user, owner)

    # Owner always checks own permissions
    if access_level == FileAccessType.Own:
        perms = AccessPermissions(file.access_bits)
        return perms.has_permission(FileAccessType.Own, _operation_to_perm_bit(operation))

    # Friend: check friend-specific permissions first
    if access_level == FileAccessType.Friend:
        friend_entry = get_friend_permissions(user, owner)
        if friend_entry is not None and friend_entry.entry_used:
            # Friend-specific permissions override file-level
            if operation == FileOperationType.Read:
                return friend_entry.read_access
            elif operation == FileOperationType.Write or operation == FileOperationType.Delete:
                return friend_entry.write_access
            elif operation == FileOperationType.Append:
                return friend_entry.append_access
            elif operation == FileOperationType.Execute:
                return friend_entry.common_access
            elif operation == FileOperationType.List:
                return friend_entry.directory_access
        # Fall through to file-level friend permissions
        perms = AccessPermissions(file.access_bits)
        return perms.has_permission(FileAccessType.Friend, _operation_to_perm_bit(operation))

    # Public
    perms = AccessPermissions(file.access_bits)
    return perms.has_permission(FileAccessType.Public, _operation_to_perm_bit(operation))
