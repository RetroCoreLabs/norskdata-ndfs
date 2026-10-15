/**
 * SINTRAN III initial-command buffer: locate / parse / encode / repair.
 *
 * The initial commands are the list SINTRAN runs first at every restart
 * (@LIST-INITIAL-COMMANDS). The buffer has no file form: it lives at the kernel
 * symbol INIBU inside the SINTRAN command segment, whose disk image is stored in
 * the (SYSTEM)SEGFIL0:DATA object file, and is located WITHOUT byte-searching
 * from the INIBU symbol value plus the segment table read off SEGFIL0 itself.
 *
 * Ported behaviour-for-behaviour from ndfs-c/src/sintran.c and the RetroFS.NDFS
 * C# implementation, both live-verified on real K03/L07/M06 packs. See
 * docs/SINTRAN-INITIAL-COMMANDS-SPEC.md for the full derivation.
 *
 * SPDX-License-Identifier: MIT
 * Copyright (c) 1985-2026 Ronny Hansen
 * RetroCore Labs - https://github.com/RetroCoreLabs
 */

/** Kernel symbol INIBU per SINTRAN version (octal in the symbol lists). */
export const INIBU: Readonly<Record<string, number>> = {
  K: 0o67172,
  L: 0o74123,
  M: 0o102327,
};

/* Region layout (identical in K03/L07/M06):
 *   INIBU+0..+129  command text  (260 bytes)
 *   INIBU+130      length cell    (byte count of the text)
 *   INIBU+131      INCOM          (executable code - never written)               */
export const CAP_BYTES = 262;
export const LEN_CELL_BYTES = 260;
export const MAX_TEXT_BYTES = 254; // NEXIN handler literal 252 -> (252+2) & ~1

const SEG_ENTRY_BYTES = 16;
const PAGE_BYTES = 2048;
const CMD_END = 0x27; // "'" terminates a command; alone = end of buffer
const VERSION_ORDER = ['L', 'M', 'K'];

/** Decoded initial-command buffer and where it was found in SEGFIL0. */
export interface InitialCommands {
  version: string;
  segmentNumber: number;
  madr: number;
  byteOffset: number;
  segmentTablePage: number;
  textLength: number;
  lengthCellOffset: number;
  commands: string[];
}

/** One INIBU-containing segment candidate (diagnostic). */
export interface InitCmdCandidate {
  version: string;
  segmentNumber: number;
  madr: number;
  byteOffset: number;
  lengthCell: number;
  lengthPlausible: boolean;
  consistent: boolean;
  commands: string[] | null;
}

function wordBE(seg: Uint8Array, o: number): number {
  return (seg[o] << 8) | seg[o + 1];
}

interface SegEntry {
  logad: number;
  segle: number;
  madr: number;
  flag: number;
  sgsta: number;
}

function segEntry(seg: Uint8Array, page: number, n: number): SegEntry {
  const o = page * PAGE_BYTES + n * SEG_ENTRY_BYTES;
  return {
    logad: wordBE(seg, o + 4),
    segle: wordBE(seg, o + 6) & 0x3ff,
    madr: wordBE(seg, o + 8),
    flag: wordBE(seg, o + 10),
    sgsta: wordBE(seg, o + 12),
  };
}

/** Locate the self-describing segment-table page; -1 if not found. */
export function findSegmentTablePage(seg: Uint8Array): number {
  const pages = Math.floor(seg.length / PAGE_BYTES);
  let best = -1;
  let bestGood = -1;
  for (let p = 0; p < pages; p++) {
    const base = p * PAGE_BYTES;
    let zero = true;
    for (let i = 0; i < SEG_ENTRY_BYTES; i++) {
      if (seg[base + i] !== 0) {
        zero = false;
        break;
      }
    }
    if (!zero) continue;

    let good = 0;
    let selfRef = false;
    for (let n = 1; n < 128; n++) {
      const e = segEntry(seg, p, n);
      if (e.segle === 0 && e.madr === 0 && e.logad === 0) continue;
      if (e.segle > 0 && e.segle < 1024 && e.madr > 0 && (e.flag & 1) !== 0 && (e.sgsta & 0xe000) !== 0) {
        good++;
        if (e.madr === p && e.segle <= 32) selfRef = true;
      }
    }
    if (good >= 40 && selfRef && good > bestGood) {
      best = p;
      bestGood = good;
    }
  }
  return best;
}

function isPrintable(s: string): boolean {
  if (s.length === 0) return false;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    if (c < 0x20 || c > 0x7e) return false;
  }
  return true;
}

/**
 * Parse a buffer region: text + 0x27, word-aligned; a lone 0x27 ends it.
 * Returns the command list (possibly empty), or null when the region does not
 * decode to a plausible buffer (non-printable payload).
 */
export function parseBuffer(seg: Uint8Array, off: number, capBytes: number): string[] | null {
  const cmds: string[] = [];
  let i = off;
  const end = Math.min(off + capBytes, seg.length);
  while (i < end) {
    if (seg[i] === 0x00) {
      i++;
      continue;
    }
    if (seg[i] === CMD_END) break;
    let s = '';
    while (i < end && seg[i] !== CMD_END) {
      s += String.fromCharCode(seg[i] & 0x7f);
      i++;
    }
    i++; // consume the "'"
    if (i < end && seg[i] === 0x00 && ((i - off) & 1) !== 0) i++;
    if (!isPrintable(s)) return null;
    cmds.push(s);
  }
  return cmds;
}

/**
 * Encode a command list into buffer bytes. Returns { bytes, textLen } where
 * textLen is the length-cell value (text bytes EXCLUDING the terminator).
 * Throws on an empty/embedded-quote/non-printable command or an over-long text.
 */
export function encodeBuffer(
  commands: readonly string[],
  maxTextBytes: number = MAX_TEXT_BYTES,
): { bytes: Uint8Array; textLen: number } {
  const limit = maxTextBytes > 0 ? maxTextBytes : MAX_TEXT_BYTES;
  const out: number[] = [];
  for (const cmd of commands) {
    const c = (cmd ?? '').toUpperCase();
    if (!isPrintable(c) || c.indexOf("'") !== -1) {
      throw new Error(`invalid command: ${cmd}`);
    }
    for (let j = 0; j < c.length; j++) out.push(c.charCodeAt(j) & 0x7f);
    out.push(CMD_END);
    if ((out.length & 1) !== 0) out.push(0x00);
  }
  const textLen = out.length;
  if (textLen > limit) {
    throw new Error(`buffer overflow: text is ${textLen} bytes, max ${limit}`);
  }
  out.push(CMD_END);
  if ((out.length & 1) !== 0) out.push(0x00);
  return { bytes: Uint8Array.from(out), textLen };
}

/**
 * True when re-encoding `commands` reproduces exactly `storedTextLen` bytes, i.e.
 * a self-consistent round trip (a REAL buffer) rather than a coincidental parse.
 */
export function reencodeMatches(commands: readonly string[], storedTextLen: number): boolean {
  try {
    return encodeBuffer(commands).textLen === storedTextLen;
  } catch {
    return false;
  }
}

/** Build the 262-byte region image (text area + length cell). */
export function buildRegionImage(encoded: Uint8Array, textLen: number): Uint8Array {
  if (encoded.length > LEN_CELL_BYTES) {
    throw new Error(`encoded buffer is ${encoded.length} bytes, exceeds text area ${LEN_CELL_BYTES}`);
  }
  const region = new Uint8Array(CAP_BYTES);
  region.set(encoded, 0);
  region[LEN_CELL_BYTES] = (textLen >> 8) & 0xff;
  region[LEN_CELL_BYTES + 1] = textLen & 0xff;
  return region;
}

/**
 * Locate and decode the initial-command buffer inside a SEGFIL0 buffer. Requires
 * SELF-CONSISTENCY (stored length == re-encoded length), so a garbage region
 * whose length cell coincidentally reads small is not mistaken for a buffer.
 * Returns null when no consistent buffer is found.
 */
export function locate(seg: Uint8Array, preferVersion?: string): InitialCommands | null {
  const stPage = findSegmentTablePage(seg);
  if (stPage < 0) return null;

  const order: string[] = [];
  if (preferVersion) {
    const pv = preferVersion.toUpperCase();
    if (pv in INIBU) order.push(pv);
  }
  for (const v of VERSION_ORDER) if (!order.includes(v)) order.push(v);

  for (const letter of order) {
    const inibu = INIBU[letter];
    if (inibu === undefined) continue;
    for (let n = 1; n < 128; n++) {
      const e = segEntry(seg, stPage, n);
      if (e.madr === 0 || e.segle === 0) continue;
      const segBase = (e.logad & 0x3f) * 1024;
      if (inibu < segBase || inibu >= segBase + e.segle * 1024) continue;
      const off = inibu - segBase;
      const byteOffset = e.madr * PAGE_BYTES + off * 2;
      if (byteOffset + CAP_BYTES > seg.length) continue;
      const textLen = wordBE(seg, byteOffset + LEN_CELL_BYTES);
      if (textLen === 0 || textLen > LEN_CELL_BYTES) continue;
      const cmds = parseBuffer(seg, byteOffset, textLen);
      if (cmds && cmds.length > 0 && reencodeMatches(cmds, textLen)) {
        return {
          version: letter,
          segmentNumber: n,
          madr: e.madr,
          byteOffset,
          segmentTablePage: stPage,
          textLength: textLen,
          lengthCellOffset: byteOffset + LEN_CELL_BYTES,
          commands: cmds,
        };
      }
    }
  }
  return null;
}

const REPAIR_KEYWORDS = [
  'DIRECTORY',
  'DISC-',
  'SET-AVAIL',
  'APPEND-BATCH',
  'LOAD-MODE',
  'HENT-MODE',
  'CONNECT',
  'ENTER-DIR',
  'SYSTEM-OUTPUT',
];

/**
 * Find the write target for repairing a buffer whose header was clobbered, by
 * scoring surviving command keywords. Requires a single clear winner (score >= 2
 * and strictly above the runner-up). Returns { byteOffset, score } or null.
 */
export function locateRepairTarget(seg: Uint8Array): { byteOffset: number; score: number } | null {
  const stPage = findSegmentTablePage(seg);
  if (stPage < 0) return null;
  let bestOffset = -1;
  let bestScore = 0;
  let secondScore = 0;
  for (const letter of VERSION_ORDER) {
    const inibu = INIBU[letter];
    for (let n = 1; n < 128; n++) {
      const e = segEntry(seg, stPage, n);
      if (e.madr === 0 || e.segle === 0) continue;
      const segBase = (e.logad & 0x3f) * 1024;
      if (inibu < segBase || inibu >= segBase + e.segle * 1024) continue;
      const off = inibu - segBase;
      const byteOffset = e.madr * PAGE_BYTES + off * 2;
      if (byteOffset + CAP_BYTES > seg.length) continue;
      let text = '';
      for (let i = 0; i < CAP_BYTES; i++) {
        const c = seg[byteOffset + i] & 0x7f;
        text += c >= 0x20 && c < 0x7f ? String.fromCharCode(c) : ' ';
      }
      let score = 0;
      for (const kw of REPAIR_KEYWORDS) if (text.indexOf(kw) !== -1) score++;
      if (score > bestScore) {
        secondScore = bestScore;
        bestScore = score;
        bestOffset = byteOffset;
      } else if (score > secondScore) {
        secondScore = score;
      }
    }
  }
  if (bestOffset < 0 || bestScore < 2 || bestScore === secondScore) return null;
  return { byteOffset: bestOffset, score: bestScore };
}

/** Enumerate EVERY INIBU-containing segment candidate (diagnostic). */
export function enumerateCandidates(seg: Uint8Array): InitCmdCandidate[] {
  const result: InitCmdCandidate[] = [];
  const stPage = findSegmentTablePage(seg);
  if (stPage < 0) return result;
  for (const letter of VERSION_ORDER) {
    const inibu = INIBU[letter];
    for (let n = 1; n < 128; n++) {
      const e = segEntry(seg, stPage, n);
      if (e.madr === 0 || e.segle === 0) continue;
      const segBase = (e.logad & 0x3f) * 1024;
      if (inibu < segBase || inibu >= segBase + e.segle * 1024) continue;
      const off = inibu - segBase;
      const byteOffset = e.madr * PAGE_BYTES + off * 2;
      if (byteOffset + CAP_BYTES > seg.length) continue;
      const textLen = wordBE(seg, byteOffset + LEN_CELL_BYTES);
      const plausible = textLen > 0 && textLen <= LEN_CELL_BYTES;
      const cmds = plausible ? parseBuffer(seg, byteOffset, textLen) : null;
      const consistent = !!cmds && cmds.length > 0 && reencodeMatches(cmds, textLen);
      result.push({
        version: letter,
        segmentNumber: n,
        madr: e.madr,
        byteOffset,
        lengthCell: textLen,
        lengthPlausible: plausible,
        consistent,
        commands: cmds,
      });
    }
  }
  return result;
}
