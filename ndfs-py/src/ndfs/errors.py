"""
NDFS error types that callers need to tell apart.

The one distinction that matters to a user interface is between a limit the
operator can lift and a limit nobody can lift:

  * SOFT - the user has filled every object block ALLOCATED to them and every
    block their maximum ALLOWS, but that maximum is still below the SINTRAN
    ceiling of 16. Granting more blocks fixes it. On a real machine that is
    ``@GIVE-OBJECT-BLOCKS <user>,<count>``; in this library it is
    ``NdfsFileSystem.give_object_blocks(user, count)``.

  * HARD - the user already has all 16 object blocks, i.e. 4096 files. There is
    nothing to grant. The only remedy is deleting files or using another user
    area.

A copy or import loop that stops on ``ObjectBlockLimitError`` can therefore
print the right advice by looking at :attr:`ObjectBlockLimitError.is_hard_limit`
instead of matching on message text.

Both derive from ``OSError`` (``IOError``) because that is what the file-system
layer raised for a full object table before object blocks were understood, and
existing callers catch it.

SPDX-License-Identifier: MIT
Copyright (c) 1985-2026 Ronny Hansen
RetroCore Labs - https://github.com/RetroCoreLabs
"""
from __future__ import annotations

from typing import Optional


class NdfsError(OSError):
    """Base class for NDFS errors that carry structured detail."""


class ObjectBlockLimitError(NdfsError):
    """A user cannot take another file because their object blocks are full.

    Attributes:
        user_name: The user area that filled up.
        max_object_blocks: The user's MXOBL, 1..16.
        allocated_object_blocks: The user's ACOBL, 1..MXOBL.
        is_hard_limit: True when MXOBL is already at the SINTRAN maximum of 16,
            so no grant can help. False when more blocks can still be given.
    """

    __slots__ = ("user_name", "max_object_blocks", "allocated_object_blocks", "is_hard_limit")

    def __init__(
        self,
        user_name: str,
        max_object_blocks: int,
        allocated_object_blocks: int,
        is_hard_limit: bool,
        message: Optional[str] = None,
    ) -> None:
        if message is None:
            message = _build_message(
                user_name, max_object_blocks, allocated_object_blocks, is_hard_limit
            )
        super().__init__(message)
        self.user_name = user_name
        self.max_object_blocks = max_object_blocks
        self.allocated_object_blocks = allocated_object_blocks
        self.is_hard_limit = is_hard_limit


class ObjectBlockCollisionError(NdfsError):
    """Growing a user into another object block would land on another user's pages.

    A user's object block *n* occupies exactly the pages this library uses for
    user ``n*64 + U``'s FIRST block, so the two placements cannot coexist. See
    ``docs/NDFS-OBJECT-BLOCKS-SPEC.md`` section 6.
    """

    __slots__ = ("user_name", "block", "colliding_user_index")

    def __init__(self, user_name: str, block: int, colliding_user_index: int) -> None:
        super().__init__(
            f"Cannot give user {user_name} object block {block}: those pages already "
            f"belong to user index {colliding_user_index}. A user's second object block "
            "and a high user's first block occupy the same place. "
            "See docs/NDFS-OBJECT-BLOCKS-SPEC.md."
        )
        self.user_name = user_name
        self.block = block
        self.colliding_user_index = colliding_user_index


def _build_message(
    user_name: str,
    max_object_blocks: int,
    allocated_object_blocks: int,
    is_hard_limit: bool,
) -> str:
    """Compose the operator-facing text for a full object table."""
    used_files = max_object_blocks * 256
    if is_hard_limit:
        # MXOBL is 16. Nothing to grant - say so plainly rather than sending the
        # operator off to a command that will refuse.
        return (
            f"User {user_name} has reached the SINTRAN maximum of {used_files} files "
            f"({max_object_blocks} object blocks). This is a hard limit: no further "
            "object blocks can be granted. Delete files or use another user area."
        )
    return (
        f"User {user_name} is full at {used_files} files "
        f"({allocated_object_blocks} of {max_object_blocks} object blocks allocated, "
        f"{max_object_blocks} allowed). Grant more with "
        f"give_object_blocks('{user_name}', n) here, or @GIVE-OBJECT-BLOCKS "
        f"{user_name},n on the machine. The ceiling is 16 blocks = 4096 files."
    )
