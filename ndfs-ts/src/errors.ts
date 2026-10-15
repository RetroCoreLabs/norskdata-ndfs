/**
 * NDFS error types that callers need to tell apart.
 *
 * The one distinction that matters to a user interface is between a limit an
 * operator can lift and a limit nobody can lift:
 *
 *  - SOFT: the user has filled every object block their maximum ALLOWS, but that
 *    maximum is still below the SINTRAN ceiling of 16. Granting more blocks
 *    fixes it - `@GIVE-OBJECT-BLOCKS` on a real machine, or
 *    {@link NdfsFileSystem.giveObjectBlocks} here.
 *  - HARD: the user already has all 16 object blocks, i.e. 4096 files. There is
 *    nothing to grant; the only remedy is deleting files or another user area.
 *
 * A copy or import loop that stops on {@link ObjectBlockLimitError} can print the
 * right advice by reading {@link ObjectBlockLimitError.isHardLimit} instead of
 * matching on message text.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

import { FILES_PER_OBJECT_BLOCK, MAX_OBJECT_BLOCKS } from './constants.js';

/** Base class for NDFS errors that carry structured detail. */
export class NdfsError extends Error {}

/**
 * A user cannot take another file because their object blocks are full.
 */
export class ObjectBlockLimitError extends NdfsError {
  /** The user area that filled up. */
  readonly userName: string;

  /** MXOBL - object blocks permitted, 1..16. */
  readonly maxObjectBlocks: number;

  /** ACOBL - object blocks actually allocated, 1..{@link maxObjectBlocks}. */
  readonly allocatedObjectBlocks: number;

  /**
   * True when MXOBL is already at the SINTRAN maximum of 16, so no grant can
   * help.
   */
  readonly isHardLimit: boolean;

  constructor(
    userName: string,
    maxObjectBlocks: number,
    allocatedObjectBlocks: number,
    isHardLimit: boolean,
  ) {
    super(
      buildMessage(userName, maxObjectBlocks, allocatedObjectBlocks, isHardLimit),
    );
    this.name = 'ObjectBlockLimitError';
    this.userName = userName;
    this.maxObjectBlocks = maxObjectBlocks;
    this.allocatedObjectBlocks = allocatedObjectBlocks;
    this.isHardLimit = isHardLimit;
  }
}

/**
 * Growing a user into another object block would land on another user's pages.
 *
 * A user's object block `n` occupies exactly the pages this library uses for
 * user `n*64 + U`'s FIRST block, so the two placements cannot coexist. See
 * `docs/NDFS-OBJECT-BLOCKS-SPEC.md` section 6.
 */
export class ObjectBlockCollisionError extends NdfsError {
  /** The user that could not be grown. */
  readonly userName: string;

  /** The zero-based object block being grown into. */
  readonly block: number;

  /** The index of the user whose first object block already owns those pages. */
  readonly collidingUserIndex: number;

  constructor(userName: string, block: number, collidingUserIndex: number) {
    super(
      `Cannot give user ${userName} object block ${block}: those pages already ` +
        `belong to user index ${collidingUserIndex}. A user's second object block ` +
        `and a high user's first block occupy the same place. ` +
        `See docs/NDFS-OBJECT-BLOCKS-SPEC.md.`,
    );
    this.name = 'ObjectBlockCollisionError';
    this.userName = userName;
    this.block = block;
    this.collidingUserIndex = collidingUserIndex;
  }
}

/** Composes the operator-facing text for a full object table. */
function buildMessage(
  userName: string,
  maxObjectBlocks: number,
  allocatedObjectBlocks: number,
  isHardLimit: boolean,
): string {
  const maxFiles = maxObjectBlocks * FILES_PER_OBJECT_BLOCK;

  if (isHardLimit) {
    // MXOBL is 16. Say so plainly rather than sending the operator off to a
    // command that will only refuse.
    return (
      `User ${userName} has reached the SINTRAN maximum of ${maxFiles} files ` +
      `(${maxObjectBlocks} object blocks). This is a hard limit: no further ` +
      `object blocks can be granted. Delete files or use another user area.`
    );
  }

  return (
    `User ${userName} is full at ${maxFiles} files ` +
    `(${allocatedObjectBlocks} of ${maxObjectBlocks} object blocks allocated, ` +
    `${maxObjectBlocks} allowed). Grant more with ` +
    `giveObjectBlocks('${userName}', n) here, or @GIVE-OBJECT-BLOCKS ` +
    `${userName},n on the machine. The ceiling is ${MAX_OBJECT_BLOCKS} blocks = ` +
    `${MAX_OBJECT_BLOCKS * FILES_PER_OBJECT_BLOCK} files.`
  );
}
