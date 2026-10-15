/**
 * NDFS access control: determines permission level and checks operations.
 *
 * 3-tier hierarchy:
 *   Own    - user owns the file
 *   Friend - user is in owner's friend list
 *   Public - everyone else
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { FileAccessType, FileOperationType } from './types.js';
import { ObjectEntry } from './object-entry.js';
import { UserEntry } from './user-entry.js';
import { UserFriend } from './user-friend.js';
import { AccessPermissions, PERM_READ, PERM_WRITE, PERM_APPEND, PERM_EXECUTE, PERM_DELETE } from './access-permissions.js';

/**
 * Determine the access level for a user relative to a file.
 */
export function getAccessLevel(
  file: ObjectEntry,
  user: UserEntry,
  owner: UserEntry,
): FileAccessType {
  if (user.userIndex === owner.userIndex) return FileAccessType.Own;
  if (owner.isFriend(user.userIndex)) return FileAccessType.Friend;
  return FileAccessType.Public;
}

/**
 * Get friend-specific permissions (if user is in owner's friend list).
 */
export function getFriendPermissions(
  user: UserEntry,
  owner: UserEntry,
): UserFriend | null {
  return owner.getFriend(user.userIndex);
}

/**
 * Map a FileOperationType to the corresponding permission bit.
 */
function operationToPermBit(operation: FileOperationType): number {
  switch (operation) {
    case FileOperationType.Read:
      return PERM_READ;
    case FileOperationType.Write:
      return PERM_WRITE;
    case FileOperationType.Append:
      return PERM_APPEND;
    case FileOperationType.Execute:
      return PERM_EXECUTE;
    case FileOperationType.Delete:
      return PERM_DELETE;
    case FileOperationType.List:
      return PERM_READ; // List uses read permission
  }
}

/**
 * Check if a user can perform an operation on a file.
 * Friend-specific permissions override file-level friend permissions.
 */
export function checkAccess(
  file: ObjectEntry,
  user: UserEntry,
  owner: UserEntry,
  operation: FileOperationType,
): boolean {
  const accessLevel = getAccessLevel(file, user, owner);

  // Owner always checks own permissions
  if (accessLevel === FileAccessType.Own) {
    const perms = new AccessPermissions(file.accessBits);
    return perms.hasPermission(FileAccessType.Own, operationToPermBit(operation));
  }

  // Friend: check friend-specific permissions first
  if (accessLevel === FileAccessType.Friend) {
    const friendEntry = getFriendPermissions(user, owner);
    if (friendEntry && friendEntry.entryUsed) {
      // Friend-specific permissions override file-level
      switch (operation) {
        case FileOperationType.Read:
          return friendEntry.readAccess;
        case FileOperationType.Write:
        case FileOperationType.Delete:
          return friendEntry.writeAccess;
        case FileOperationType.Append:
          return friendEntry.appendAccess;
        case FileOperationType.Execute:
          return friendEntry.commonAccess;
        case FileOperationType.List:
          return friendEntry.directoryAccess;
      }
    }
    // Fall through to file-level friend permissions
    const perms = new AccessPermissions(file.accessBits);
    return perms.hasPermission(FileAccessType.Friend, operationToPermBit(operation));
  }

  // Public
  const perms = new AccessPermissions(file.accessBits);
  return perms.hasPermission(FileAccessType.Public, operationToPermBit(operation));
}
