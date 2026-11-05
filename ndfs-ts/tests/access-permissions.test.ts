import { describe, it, expect } from 'vitest';
import {
  AccessPermissions,
  PERM_READ,
  PERM_WRITE,
  PERM_APPEND,
  PERM_EXECUTE,
  PERM_DELETE,
  parseAccessRights,
  applyAccessTriple,
} from '../src/access-permissions.js';
import { FileAccessType } from '../src/types.js';

describe('AccessPermissions', () => {
  describe('default', () => {
    it('creates 0x4FF default permissions', () => {
      const p = AccessPermissions.default();
      expect(p.rawBits).toBe(0x4ff);
    });
  });

  describe('fromTiers', () => {
    it('encodes separate tier values', () => {
      // Own=all(0x1f), Friend=RW(0x03), Public=R(0x01)
      const p = AccessPermissions.fromTiers(0x1f, 0x03, 0x01);
      expect(p.canRead(FileAccessType.Own)).toBe(true);
      expect(p.canDelete(FileAccessType.Own)).toBe(true);
      expect(p.canRead(FileAccessType.Friend)).toBe(true);
      expect(p.canWrite(FileAccessType.Friend)).toBe(true);
      expect(p.canAppend(FileAccessType.Friend)).toBe(false);
      expect(p.canRead(FileAccessType.Public)).toBe(true);
      expect(p.canWrite(FileAccessType.Public)).toBe(false);
    });
  });

  describe('permission checks', () => {
    it('checks individual permissions', () => {
      const p = new AccessPermissions(0);
      p.setPermission(FileAccessType.Own, PERM_READ, true);
      p.setPermission(FileAccessType.Own, PERM_WRITE, true);
      expect(p.canRead(FileAccessType.Own)).toBe(true);
      expect(p.canWrite(FileAccessType.Own)).toBe(true);
      expect(p.canDelete(FileAccessType.Own)).toBe(false);
    });
  });

  describe('getPermissionString', () => {
    it('formats all permissions', () => {
      const p = AccessPermissions.fromTiers(0x1f, 0, 0);
      expect(p.getPermissionString(FileAccessType.Own)).toBe('DXAWR');
    });

    it('formats no permissions', () => {
      const p = new AccessPermissions(0);
      expect(p.getPermissionString(FileAccessType.Own)).toBe('-----');
    });
  });

  describe('ownerOnly', () => {
    it('gives full access to owner, none to others', () => {
      const p = AccessPermissions.ownerOnly();
      expect(p.canRead(FileAccessType.Own)).toBe(true);
      expect(p.canDelete(FileAccessType.Own)).toBe(true);
      expect(p.canRead(FileAccessType.Friend)).toBe(false);
      expect(p.canRead(FileAccessType.Public)).toBe(false);
    });
  });
});

// SINTRAN's access fields: one field, then the PUBLIC,FRIEND,OWN triple
// (@SET-FILE-ACCESS, SINTRAN III Reference Manual ND-60.128.5).
describe('SINTRAN access form', () => {
  it('parses one field', () => {
    expect(parseAccessRights('N')).toBe(0);
    expect(parseAccessRights('DCAWR')).toBe(0x1f);
    expect(parseAccessRights('ra')).toBe(0x05);
    for (const bad of ['', 'NR', 'X']) expect(() => parseAccessRights(bad)).toThrow();
  });

  it('applies the PUBLIC,FRIEND,OWN triple', () => {
    // @SET-FILE-ACCESS F-1:DATA N,N,RWACD -> owner only.
    expect(applyAccessTriple(0x07ff, 'N,N,RWACD')).toBe(0x001f);
    // The generated default (rule 2 of @SET-DEFAULT-FILE-ACCESS) = 0x04FF.
    expect(applyAccessTriple(0, 'R,RWA,RWACD')).toBe(0x04ff);
    // Empty fields keep their tier.
    expect(applyAccessTriple(0x04ff, ',,N')).toBe(0x04e0);
    expect(applyAccessTriple(0x04ff, 'N')).toBe(0x00ff);
    expect(applyAccessTriple(0x04ff, '')).toBe(0x04ff);
    expect(applyAccessTriple(0, 'R, RWA, RWACD')).toBe(0x04ff);
    for (const bad of ['R,R,R,R', 'R,NW,R']) expect(() => applyAccessTriple(0, bad)).toThrow();
  });
});
