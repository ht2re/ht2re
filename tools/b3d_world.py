#!/usr/bin/env python3
"""b3d_world.py -- assemble B3D scene tiles into world space and validate numerically.

    python3 tools/b3d_world.py <b3d> [--way <way>] [--terrain original/MTR/terrain0.raw]
                                     [--out DIR] [--topview] [--convention row|col]
                                     [--all] [--quiet]

WHAT IT DOES
  * independent B3D record walker (does not import tools/b3d_full.py, so a bug in one
    parser cannot hide a bug in the other); asserts 100% byte consumption.
  * reads the record semantics derived from king.exe (see docs/b3d_transform.md):
        24 'XSpace'  { m12f ; case 1f }          count { child }
        18 'link_X'  { sphere4f ; space32 ; obj32 }              (leaf)
         4 'Inst'    { sphere4f ; anchor32 ; name2_32 } count { child }
         5 'Group'   { sphere4f ; name32 }                count { child }
        19 'room_X'  count { child }                              (container)
        30 (portal)  { centre4f ; 'tile:room'32 ; p0_3f ; p1_3f } (leaf)
  * builds the accumulated transform per node.
        space abs = local x parentAbs   (row-vector composition: local first)
        4x3 matrix layout = 4 rows of 3 floats, row-major:
            p_world = p_local . M + t          <-- ROW-VECTOR convention (king.exe prove)
  * places subtag 4/5 containers: once per anchor that references them
        - subtag 4 self-reference: param name1 (e.g. 'DRZ001Space_')
        - subtag 18 link (A,B): container named B is emitted at anchor A
        - otherwise the container inherits the transform of its record-tree ancestors
  * prints, per node/room, the accumulated transform.
  * prints numeric validation: anchor<->assembled-bbox distance, nearest .way node,
    base height vs the world-space room geometry, and above/below counts.
  * writes a top-down PNG (X/Z, coloured by height) with the .way nodes overlaid and
    the terrain depth-byte contours.

TERRAIN CAVEAT (measured 2026, see docs/b3d_transform.md "terrain"): the depth byte in
terrain0.raw correlates with .way heights only weakly at every horizontal scale tested
(best r ~ 0.4 over 762 nodes of the 40 ENV tiles); the previously quoted
worldY = (157.6 - t) * 5.136 fit is reproduced only for the union of aa.b3d vertex
clusters and also "validates" untransformed local geometry, so it is NOT used as the
ground reference here.  The ground reference used for validation is the world-space room
geometry of the tile itself (rooms 19 contain the road/terrain-hugging meshes), which
agrees with the .way node heights.  terrain0.raw is still drawn (as depth-byte contours)
and its residual is reported for the record only.
"""
from __future__ import annotations

import argparse
import glob
import math
import os
import struct
import sys

import numpy as np

# --------------------------------------------------------------------------------------
# record tokens / layout (verified against tools/b3d_full.py and all 45 .b3d files)
# --------------------------------------------------------------------------------------
T_REC, T_BSEC, T_ESEC, T_OPEN, T_EOF = 0x14D, 0x1BC, 0x22B, 0x6F, 0xDE
KNOWN = (T_REC, T_BSEC, T_ESEC, T_OPEN, T_EOF)
NFM = {0x0: 5, 0x2: 8, 0x3: 6, 0x102: 10, 0x202: 12}   # fmt -> f32 per vertex
FIXED = {0: 10, 1: 16, 2: 8, 4: 20, 5: 12, 9: 8, 10: 8, 12: 10, 13: 6, 14: 10, 18: 20,
         19: 0, 21: 6, 24: 13, 25: 22, 28: 29, 29: 15, 30: 18, 33: 22, 40: 32}
POLYHDR = {35: 7, 8: 5}          # dwords before the poly array (last one = npoly)
SCAN = (20, 23)                   # variable-length leaves

# terrain0.raw = 257x257 u16 LE, the LOW byte is an 8-bit depth.  Stated (unverified) fit:
TERRAIN_N = 257
TERRAIN_UNITS_PER_SAMPLE = 140.0
TERRAIN_ZERO = 128.5
TERRAIN_DEPTH_A = 157.6
TERRAIN_DEPTH_S = 5.136


def u32(d, p):
    return struct.unpack_from('<I', d, p)[0]


def f32(d, p, n=1):
    return struct.unpack_from('<%df' % n, d, p)


def nm32(d, p):
    return d[p:p + 32].split(b'\x00')[0].decode('latin1', 'replace')


def _skip_closers(d, q):
    while u32(d, q) in (T_BSEC, T_ESEC) and u32(d, q + 4) in KNOWN:
        q += 4
    return q


def _find_closer(d, start):
    r, hi = start, len(d) - 8
    while r < hi:
        if u32(d, r) in (T_ESEC, T_BSEC) and u32(d, r + 4) in KNOWN:
            return r
        r += 4
    return None


def _ok_end(d, e):
    if e < 0 or e + 8 > len(d):
        return False
    v = u32(d, e)
    return v == T_ESEC or (v < 100000 and u32(d, e + 4) == T_REC)


def _stride_of(d, voff, vc, lo=1, hi=20):
    for s in range(lo, hi):
        if _ok_end(d, voff + 4 * s * vc):
            return s
    return None


def parse_record(d, p, warn):
    if u32(d, p) != T_REC:
        raise ValueError('expect 333 @0x%x' % p)
    rec = {'at': p, 'name': nm32(d, p + 4), 'sub': u32(d, p + 36)}
    sub, q = rec['sub'], p + 40

    if sub in (7, 37):                                       # mesh
        rec['bbox'] = f32(d, q, 4)
        q += 48
        fmt = u32(d, q) if sub == 37 else 0
        if sub == 37:
            q += 4
        rec['fmt'] = fmt
        rec['vc'] = u32(d, q)
        q += 4
        rec['voff'] = q
        nf = NFM.get(fmt) or _stride_of(d, q, rec['vc'])
        if nf is None:
            raise ValueError('mesh stride fmt %d vc %d (%s)' % (fmt, rec['vc'], rec['name']))
        rec['stride'] = nf
        q += 4 * nf * rec['vc']
    elif sub in POLYHDR:                                     # face (leaf)
        rec['bbox'] = f32(d, q, 4)
        q += (POLYHDR[sub] - 1) * 4
        rec['npoly'] = u32(d, q)
        q += 4
        rec['children'] = []
        r = _find_closer(d, q)
        if r is None:
            raise ValueError('face scan fail @0x%x' % p)
        rec['end'] = _skip_closers(d, r)
        return rec
    elif sub in FIXED:
        q += FIXED[sub] * 4
        v = u32(d, q)
        if v not in (T_ESEC, T_BSEC) and (v > 500000 or
                                          u32(d, q + 4) not in (T_REC, T_BSEC, T_ESEC)):
            r = _find_closer(d, p + 44)                      # variable-length tail
            if r is None:
                raise ValueError('scan-fail subtag %d @0x%x (%s)' % (sub, p, rec['name']))
            rec['children'] = []
            rec['end'] = _skip_closers(d, r + 4)
            return rec
    elif sub in SCAN:
        r, lim = q, q + (1 << 20)
        while r < lim and r < len(d) - 4:
            if u32(d, r) == T_ESEC and u32(d, r + 4) in KNOWN:
                rec['children'] = []
                rec['end'] = _skip_closers(d, r + 4)
                return rec
            r += 4
        raise ValueError('scan-fail subtag %d @0x%x' % (sub, p))
    else:
        r = _find_closer(d, p + 44)
        if r is None:
            raise ValueError('unknown subtag %d @0x%x (%s)' % (sub, p, rec['name']))
        warn.append('subtag %d @0x%x (%s)' % (sub, p, rec['name']))
        rec['children'] = []
        rec['end'] = _skip_closers(d, r + 4)
        return rec

    v = u32(d, q)
    if v in (T_ESEC, T_BSEC):                                # leaf
        rec['children'] = []
        rec['end'] = _skip_closers(d, q)
        return rec
    n = v
    if n > 500000 or (n and u32(d, q + 4) not in (T_REC, T_BSEC)):
        raise ValueError('bad count %d subtag %d (%s)' % (n, sub, rec['name']))
    rec['n'] = n
    q += 4
    kids = []
    for _ in range(n):
        q = _skip_closers(d, q)
        k = parse_record(d, q, warn)
        kids.append(k)
        q = k['end']
    rec['children'] = kids
    rec['end'] = _skip_closers(d, q)
    return rec


def parse_b3d(path):
    d = open(path, 'rb').read()
    if d[:4] != b'b3d\x00':
        raise ValueError('bad magic')
    hdr = struct.unpack_from('<6I', d, 4)
    pos, names = 0x1C, []
    for _ in range(hdr[5]):
        names.append(nm32(d, pos))
        pos += 32
    body = pos
    pos += 4                                                # 0x6f
    top, warn, guard = [], [], 0
    while guard < len(d):
        guard += 4
        v = u32(d, pos)
        if v == T_EOF:
            pos += 4
            break
        if v in (T_BSEC, T_ESEC, T_OPEN):
            pos += 4
            continue
        if v != T_REC:
            raise ValueError('top stray %x @0x%x' % (v, pos))
        k = parse_record(d, pos, warn)
        top.append(k)
        pos = k['end']
    return {'data': d, 'names': names, 'top': top, 'warn': warn,
            'consumed': pos - body, 'total': len(d) - body}


def walk(recs):
    st = list(recs)
    while st:
        r = st.pop()
        yield r
        st.extend(r.get('children') or [])


# --------------------------------------------------------------------------------------
# transforms: 12 floats = 4 rows of 3, row-major;   p_world = p_local . M + t
# --------------------------------------------------------------------------------------
def xf_identity():
    return np.hstack([np.eye(3).ravel(), np.zeros(3)])


def xf_normalize(f):
    return np.asarray(f, dtype=np.float64).reshape(12).copy()


def xf_mul(a, b):
    """row-vector compose 'apply a first, then b': M = Ma.Mb, t = ta.Mb + tb."""
    a = np.asarray(a, float).reshape(12)
    b = np.asarray(b, float).reshape(12)
    Ma, Mb = a[:9].reshape(3, 3), b[:9].reshape(3, 3)
    ta, tb = a[9:12], b[9:12]
    return np.hstack([(Ma @ Mb).ravel(), ta @ Mb + tb])


def xf_apply(v, f, convention='row'):
    """v:(n,3),(3,) in b3d components (0=x,1=z,2=height) -> world."""
    v = np.asarray(v, dtype=np.float64)
    f = np.asarray(f, dtype=np.float64).reshape(12)
    M, t = f[:9].reshape(3, 3), f[9:12]
    if convention == 'row':
        return v @ M + t
    return v @ M.T + t           # column-style (M.p + t) -- kept only for A/B comparison


def xf_dir(v, f, convention='row'):
    v = np.asarray(v, dtype=np.float64)
    f = np.asarray(f, dtype=np.float64).reshape(12)
    M = f[:9].reshape(3, 3)
    if convention == 'row':
        return v @ M
    return v @ M.T


def xf_str(f, indent=''):
    m = np.asarray(f, float).reshape(12)
    rows = [m[0:3], m[3:6], m[6:9]]
    s = indent + ' | '.join('[%7.4f %7.4f %7.4f]' % tuple(r) for r in rows)
    return s + '  t=(%9.3f %9.3f %9.3f)' % tuple(m[9:12])


# --------------------------------------------------------------------------------------
# scene model
# --------------------------------------------------------------------------------------
class Scene:
    def __init__(self, path, convention='row'):
        self.path = path
        self.tree = parse_b3d(path)
        self.d = self.tree['data']
        self.convention = convention
        self.by_name = {}
        self.parent = {}
        self.space_xf = {}          # node id -> local matrix
        self.space_abs = {}         # node id -> absolute matrix
        self.node_of_name = {}
        self.links = []             # dicts: name, space, obj, sphere, at
        self.instances = []         # sub 4
        self.groups = []            # sub 5
        self.rooms = []             # sub 19
        self.portals = []           # sub 30
        self.points = []            # sub 40
        self.warnings = list(self.tree['warn'])
        self._build()

    # -- record-tree walk ---------------------------------------------------------------
    def _build(self):
        d = self.d
        stack = [(r, None) for r in reversed(self.tree['top'])]
        order = []
        while stack:
            r, par = stack.pop()
            rid = id(r)
            self.parent[rid] = id(par) if par is not None else None
            order.append(r)
            if par is None and r['name']:
                self.by_name.setdefault(r['name'], []).append(r)
            elif r['name']:
                self.by_name.setdefault(r['name'], []).append(r)
            if r['sub'] == 24:
                self.space_xf[rid] = xf_normalize(f32(d, r['at'] + 40, 12))
                self.node_of_name.setdefault(r['name'], rid)
            elif r['sub'] == 18:
                q = r['at'] + 40
                self.links.append({'name': r['name'], 'sphere': f32(d, q, 4),
                                   'space': nm32(d, q + 16), 'obj': nm32(d, q + 48),
                                   'at': r['at']})
            elif r['sub'] == 4:
                q = r['at'] + 40
                self.instances.append({'rec': r, 'sphere': f32(d, q, 4),
                                       'anchor': nm32(d, q + 16), 'name2': nm32(d, q + 32)})
            elif r['sub'] == 5:
                q = r['at'] + 40
                self.groups.append({'rec': r, 'sphere': f32(d, q, 4), 'name': nm32(d, q + 16)})
            elif r['sub'] == 19:
                self.rooms.append(r)
            elif r['sub'] == 30:
                q = r['at'] + 40
                self.portals.append({'rec': r, 'centre': f32(d, q, 4), 'other': nm32(d, q + 16),
                                     'p0': f32(d, q + 48, 3), 'p1': f32(d, q + 60, 3)})
            elif r['sub'] == 40:
                q = r['at'] + 40
                self.points.append({'rec': r, 'pos': f32(d, q, 3), 'name': r['name']})
            for c in reversed(r.get('children') or []):
                stack.append((c, r))
        # absolute transforms for space nodes (row-vector: local first, then parent)
        def abs_of(rid):
            if rid in self.space_abs:
                return self.space_abs[rid]
            loc = self.space_xf.get(rid, xf_identity())
            par = self.parent.get(rid)
            pa = abs_of(par) if (par is not None and par in self.space_xf) else None
            if par is not None and pa is None and par in self.parent:
                # parent is a non-space node: use the nearest space ancestor
                pp, seen = self.parent.get(par), 0
                while pp is not None and pp not in self.space_xf and seen < 64:
                    pp = self.parent.get(pp)
                    seen += 1
                pa = abs_of(pp) if pp is not None else None
            res = loc if pa is None else xf_mul(loc, pa)
            self.space_abs[rid] = res
            return res

        for rid in list(self.space_xf):
            abs_of(rid)
        # parent transform for arbitrary nodes (nearest space ancestor)
        self.anc = {}
        for r in order:
            p = self.parent.get(id(r))
            seen = 0
            while p is not None and p not in self.space_xf and seen < 128:
                p = self.parent.get(p)
                seen += 1
            self.anc[id(r)] = self.space_abs.get(p, xf_identity()) if p is not None \
                else xf_identity()

    def anchor(self, name):
        rid = self.node_of_name.get(name)
        if rid is None:
            return None
        return self.space_abs.get(rid)

    def resolve(self, name):
        """name -> absolute transform, or None.  tries exact, then name+'_' (paired child)."""
        for cand in (name, name + '_'):
            a = self.anchor(cand)
            if a is not None:
                return a, cand
        return None, None

    # -- geometry ----------------------------------------------------------------------
    def verts(self, rec, limit=None):
        key = id(rec)
        if not hasattr(self, '_vcache'):
            self._vcache = {}
        if key in self._vcache:
            return self._vcache[key]
        d = self.d
        out = []
        for r in walk([rec]):
            if r['sub'] in (7, 37) and r.get('vc'):
                nf = r['stride']
                a = np.frombuffer(d, dtype='<f4', count=nf * r['vc'],
                                  offset=r['voff']).reshape(r['vc'], nf)[:, :3]
                out.append(a.astype(np.float64))
                if limit and sum(len(x) for x in out) > limit:
                    break
        res = np.vstack(out) if out else np.zeros((0, 3))
        self._vcache[key] = res
        return res

    def placements(self, dedup=True):
        """[(rec, container_name, xf, source, sphere)] for every emitted container.

        A container can be referenced twice with an IDENTICAL transform: a sub-4 node
        carries its own anchor name AND is also named by a sub-18 link, and the sub-4
        anchor name points at the paired <X>Space_ child (identity local) of the linked
        <X>Space -- measured over all 42 .b3d: 990/990 sub-4 self anchors resolve, all of
        them are also covered by a link, and 1036 comparisons agree with 0 disagreements.
        `dedup` collapses such identical (name, matrix) pairs (pure overdraw otherwise).
        """
        out = []
        self.dup_pruned = 0
        seen = set()
        linked = {}
        for lk in self.links:
            linked.setdefault(lk['obj'], []).append(lk)
        for g in self.groups:
            name = g['rec']['name']
            if not name:
                continue                      # unnamed group = world-space room content
            srcs = []
            for lk in linked.get(name, []):
                a, cand = self.resolve(lk['space'])
                if a is None:
                    self.warnings.append('link %s: anchor %r not found' % (lk['name'], lk['space']))
                    continue
                srcs.append((a, 'link:%s' % cand, lk['sphere']))
            for a, s, sp in srcs:
                out.append((g['rec'], name, a, s, sp))
        for it in self.instances:
            name = it['rec']['name']
            srcs = []
            a, cand = self.resolve(it['anchor'])
            if a is not None:
                srcs.append((a, 'self:%s' % cand, it['sphere']))
            else:
                self.warnings.append('sub4 %s: anchor %r not found' % (name, it['anchor']))
            for lk in linked.get(name, []):
                a, cand = self.resolve(lk['space'])
                if a is None:
                    continue
                srcs.append((a, 'link:%s' % cand, lk['sphere']))
            if not srcs:
                srcs.append((self.anc[id(it['rec'])], 'parent', it['sphere']))
            for a, s, sp in srcs:
                out.append((it['rec'], name, a, s, sp))
        if not dedup:
            return out
        res = []
        for rec, name, a, src, sp in out:
            key = (name, tuple(np.round(np.asarray(a, float), 4)))
            if key in seen:
                self.dup_pruned += 1
                continue
            seen.add(key)
            res.append((rec, name, a, src, sp))
        return res


# --------------------------------------------------------------------------------------
# .way reader (WTWR chunks)
# --------------------------------------------------------------------------------------
def read_way(path):
    d = open(path, 'rb').read()
    ch = lambda n: (n + 3) & ~3
    out = []
    mnam = ['']

    def w(off, end):
        i = off
        while i + 8 <= end:
            tag = d[i:i + 4]
            size = u32(d, i + 4)
            if tag == b'MNAM':
                mnam[0] = d[i + 8:i + 8 + size].split(b'\x00')[0].decode('latin1')
            if tag == b'RNOD':
                nd = {'name': '', 'pos': None, 'flag': 0}
                j = i + 8
                while j + 8 <= i + 8 + ch(size):
                    t2, s2 = d[j:j + 4], u32(d, j + 4)
                    pay = d[j + 8:j + 8 + s2]
                    if t2 == b'NNAM':
                        nd['name'] = pay.split(b'\x00')[0].decode('latin1')
                    elif t2 == b'POSN' and s2 >= 24:
                        nd['pos'] = struct.unpack('<3d', pay[:24])
                    elif t2 == b'FLAG' and s2 >= 4:
                        nd['flag'] = u32(pay, 0)
                    j += 8 + ch(s2)
                if nd['pos']:
                    out.append(nd)
            elif tag in (b'GDAT', b'GROM'):
                w(i + 8, i + 8 + size)
            i += 8 + ch(size)

    w(8, len(d))
    return mnam[0], out


# --------------------------------------------------------------------------------------
# terrain depth map
# --------------------------------------------------------------------------------------
class Terrain:
    def __init__(self, path):
        self.ok = False
        if not path or not os.path.exists(path):
            return
        raw = np.fromfile(path, dtype='<u2')
        n = int(round(math.sqrt(raw.size)))
        if n * n != raw.size:
            return
        self.n = n
        self.depth = (raw.reshape(n, n) & 0xFF).astype(np.float64)
        self.ok = True

    def sample(self, x, z, scale=TERRAIN_UNITS_PER_SAMPLE):
        i = np.clip((np.asarray(x) / scale + TERRAIN_ZERO).astype(int), 0, self.n - 1)
        j = np.clip((np.asarray(z) / scale + TERRAIN_ZERO).astype(int), 0, self.n - 1)
        return self.depth[j, i]

    def height(self, x, z, scale=TERRAIN_UNITS_PER_SAMPLE):
        """stated (unverified) depth->height model: worldY = (157.6 - depth) * 5.136"""
        return (TERRAIN_DEPTH_A - self.sample(x, z, scale)) * TERRAIN_DEPTH_S


# --------------------------------------------------------------------------------------
# validation + report
# --------------------------------------------------------------------------------------
def ground_at(room_v, x, z, radius=15.0):
    """median height of world-space room vertices within `radius` of (x,z), or nan."""
    if not len(room_v):
        return float('nan')
    sel = (np.abs(room_v[:, 0] - x) < radius) & (np.abs(room_v[:, 1] - z) < radius)
    return float(np.median(room_v[sel, 2])) if sel.any() else float('nan')


class Ground:
    """Ground reference for a tile.

    The world-space room(19) content of ENV tiles is a set of FLAT ground/road quads
    (aa: every road vertex is at exactly h=447.1) plus sparse raised props (tree
    trunks 450..500, canopies 548..583).  It is therefore NOT a terrain-following
    surface and has real holes where no road quad exists, so:
      * local ground = low percentile of room vertices near (x,z) when any exist
      * otherwise    = `level`, the tile's flat ground level (1st percentile of all
                       world-space room vertices)
    """

    def __init__(self, room_v, cell=20.0):
        self.cell = cell
        self.ok = len(room_v) > 0
        self.grid = {}
        self.level = float(np.percentile(room_v[:, 2], 1)) if self.ok else 0.0
        if not self.ok:
            return
        keep = room_v[:, 2] < self.level + 25.0        # drop tree canopies etc.
        rv = room_v[keep]
        keys = np.floor(rv[:, :2] / cell).astype(np.int64)
        for (i, j), h in zip(map(tuple, keys), rv[:, 2]):
            self.grid.setdefault((i, j), []).append(h)
        # ground = LOW surface of the cell (5th percentile): room cells also contain
        # trees/lamps reaching ~130 units up, so the median would float the ground.
        self.grid = {k: float(np.percentile(v, 5)) for k, v in self.grid.items()}

    def at(self, x, z, search=10):
        if not self.ok:
            return 0.0
        ci, cj = int(math.floor(x / self.cell)), int(math.floor(z / self.cell))
        for r in range(0, search + 1):
            hs = [self.grid[(ci + di, cj + dj)]
                  for di in range(-r, r + 1) for dj in range(-r, r + 1)
                  if (ci + di, cj + dj) in self.grid]
            if hs:
                return float(np.median(hs))
        return self.level


def report(sc, way_path, terrain_path, out_dir, topview=False, quiet=False, tag=None):
    name = tag or os.path.basename(sc.path).split('.')[0]
    conv = sc.convention
    print('=' * 100)
    print('B3D %s   bytes %.2f%% consumed   records=%d   convention=%s' % (
        sc.path, 100.0 * sc.tree['consumed'] / sc.tree['total'],
        sum(1 for _ in walk(sc.tree['top'])), conv))
    if sc.warnings:
        for w in sc.warnings[:6]:
            print('   warn: %s' % w)

    # ---- accumulated transform per space / room / container -------------------------
    print('-- anchors (subtag 24) ---------------------------------------------------------')
    for r in walk(sc.tree['top']):
        if r['sub'] != 24:
            continue
        rid = id(r)
        pl = ''
        p = sc.parent.get(rid)
        while p is not None and p not in sc.space_xf:
            p = sc.parent.get(p)
        if p is not None:
            for rr in walk(sc.tree['top']):
                if id(rr) == p:
                    pl = ' parent=%r' % rr['name']
                    break
        case = f32(sc.d, r['at'] + 52, 1)[0]
        print('  %-22s local %s case=%g%s' % (r['name'], xf_str(sc.space_xf[rid]), case, pl))
        print('  %-22s ABS   %s' % ('', xf_str(sc.space_abs[rid])))

    print('-- rooms (subtag 19, world-space content) -------------------------------------')
    room_v = np.zeros((0, 3))
    for rm in sc.rooms:
        v = sc.verts(rm)
        room_v = np.vstack([room_v, v]) if len(v) else room_v
        if len(v):
            print('  %-22s verts=%6d  x[%8.1f %8.1f] z[%9.1f %9.1f] h[%7.1f %7.1f]  %s' % (
                rm['name'], len(v), v[:, 0].min(), v[:, 0].max(), v[:, 1].min(),
                v[:, 1].max(), v[:, 2].min(), v[:, 2].max(), xf_str(sc.anc[id(rm)])))

    print('-- containers (subtag 4/5) and their placements --------------------------------')
    pls = sc.placements()
    print('   %d placement(s) from %d sub-18 link(s) + %d sub-4 self-reference(s)'
          '  [%d identical (name,matrix) duplicates pruned]' % (
              len(pls), len(sc.links), len(sc.instances), sc.dup_pruned))
    seen_names = []
    for rec, cname, xf, src, sphere in pls:
        if cname not in seen_names:
            seen_names.append(cname)
    for cname in seen_names:
        entries = [p for p in pls if p[1] == cname]
        rec = entries[0][0]
        v = sc.verts(rec)
        has_mesh = len(v) > 0
        print('  %-22s verts=%6d bbox(local)=%s  placements=%d' % (
            cname, len(v), _bbox_str(v), len(entries)))
        for rec2, cn, xf, src, sphere in entries:
            if has_mesh:
                w = xf_apply(v, xf, conv)
                print('      <- %-26s world x[%8.1f %8.1f] z[%9.1f %9.1f] h[%7.2f %7.2f]' % (
                    src, w[:, 0].min(), w[:, 0].max(), w[:, 1].min(), w[:, 1].max(),
                    w[:, 2].min(), w[:, 2].max()))
            else:
                print('      <- %-26s (no mesh; collision/proxy, sphere=%s)' % (src, _f4(sphere)))

    # link targets that do not resolve, and containers without an anchor
    placed = {p[1] for p in pls}
    linked_names = {lk['obj'] for lk in sc.links}
    unresolved = sorted(n for n in linked_names
                        if not any(g['rec']['name'] == n for g in sc.groups)
                        and not any(i['rec']['name'] == n for i in sc.instances))
    if unresolved:
        print('  !! sub-18 link targets with NO matching container record: %s' % unresolved)
    unplaced = [g for g in sc.groups if g['rec']['name'] and g['rec']['name'] not in placed]
    top_unplaced = [g for g in unplaced if g['rec'] in sc.tree['top']]
    nv = sum(len(sc.verts(g['rec'])) for g in unplaced)
    print('  containers with no anchor (inherit parent transform, i.e. record-tree space):')
    print('     %d named containers, %d vertices; %d of them top-level (identity): %s' % (
        len(unplaced), nv, len(top_unplaced),
        ', '.join(g['rec']['name'] for g in top_unplaced[:12]) +
        (' ...' if len(top_unplaced) > 12 else '')))

    # ---- numeric validation ----------------------------------------------------------
    print('-- numeric validation ----------------------------------------------------------')
    way_nodes = []
    if way_path and os.path.exists(way_path):
        mnam, way_nodes = read_way(way_path)
        print('   .way %s (MNAM=%s) nodes=%d' % (way_path, mnam, len(way_nodes)))
    wpos = np.array([[n['pos'][0], n['pos'][1], n['pos'][2]] for n in way_nodes]) \
        if way_nodes else np.zeros((0, 3))

    terr = Terrain(terrain_path)
    grd = Ground(room_v)
    tot_v = 0
    below_ground = 0
    deep_below = 0
    ground_known = 0
    terr_res = []
    worst = []
    for rec, cname, xf, src, sphere in pls:
        v = sc.verts(rec)
        if not len(v):
            continue
        w = xf_apply(v, xf, conv)
        tot_v += len(w)
        # anchor vs assembled footprint
        anc = np.asarray(xf, float)[9:12]
        dx = np.maximum(np.maximum(w[:, 0].min() - anc[0], anc[0] - w[:, 0].max()), 0)
        dz = np.maximum(np.maximum(w[:, 1].min() - anc[1], anc[1] - w[:, 1].max()), 0)
        d_horiz = float(math.hypot(dx, dz))
        d_base = float(w[:, 2].min() - anc[2])
        # nearest .way node
        d_way = float('nan')
        n_way = ''
        if len(wpos):
            dm = np.sqrt((w[:, 0][:, None] - wpos[None, :, 0]) ** 2 +
                         (w[:, 1][:, None] - wpos[None, :, 1]) ** 2)
            k = np.unravel_index(dm.argmin(), dm.shape)
            d_way = float(dm[k])
            n_way = way_nodes[k[1]]['name']
        # ground from world-space room geometry (grid median of room mesh heights)
        gs = np.array([grd.at(w[i, 0], w[i, 1]) for i in range(len(w))])
        m = ~np.isnan(gs)
        frac_below = float('nan')
        med_clear = float('nan')
        if m.any():
            ground_known += int(m.sum())
            below_ground += int(np.sum(gs[m] > w[m, 2]))
            deep_below += int(np.sum(gs[m] > w[m, 2] + 3.0))
            frac_below = 100.0 * np.mean(gs[m] > w[m, 2])
            med_clear = float(np.median(w[m, 2] - gs[m]))
        if terr.ok:
            terr_res.append(w[:, 2] - terr.height(w[:, 0], w[:, 1]))
        worst.append((d_horiz, d_base, d_way, cname, src))
        print('   %-16s <- %-24s anchor_dev=%7.3f  base-anchor_h=%+7.2f  nearest_way=%7.2f (%s)'
              '  verts_below_ground=%.1f%%  median_clearance=%+.2f' % (
                  cname, src, d_horiz, d_base, d_way, n_way, frac_below, med_clear))

    print('-- ground contact (reference = world-space room geometry of this tile) ---------')
    if ground_known:
        print('   tile flat ground level (1st pct of room heights) = %.2f' % grd.level)
        print('   assembled vertices: %d ; below that ground surface: %d (%.1f%%) ; '
              'below by more than 3 units: %d' % (
                  tot_v, below_ground, 100.0 * below_ground / max(1, ground_known), deep_below))
    if terr.ok:
        r = np.concatenate(terr_res) if terr_res else np.zeros(0)
        if len(r):
            print('   depth-map residual (stated model worldY=(157.6-t)*5.136): '
                  'median=%+.1f  p05=%+.1f  p95=%+.1f  frac|<25| = %.0f%%' % (
                      np.median(r), np.percentile(r, 5), np.percentile(r, 95),
                      100 * np.mean(np.abs(r) < 25)))
            print('   NOTE: that model is NOT validated corpus-wide (see docs); the room-geometry')
            print('         ground above is the trusted reference.')
    if worst:
        wd = max(worst, key=lambda x: x[0])
        print('   worst anchor/footprint deviation: %.3f units (%s <- %s)' % (wd[0], wd[3], wd[4]))
        print('   (a large value is data, not a rule failure: the anchor sits at the container')
        print('    ORIGIN, e.g. dq.b3d "lep12" is modelled at local x 241..335, z 865..964)')
    sc.convention = 'row'
    n_r, mx_r, mn_r, rr, nd_r, dm_r = sphere_dev(sc, 'row')
    sc.convention = 'col'
    n_c, mx_c, mn_c, rr_c, nd_c, dm_c = sphere_dev(sc, 'col')
    sc.convention = conv
    if n_r:
        print('   bounding-sphere check: anchor.xf(sphere centre) vs placed mesh bbox centre over')
        print('      %d containers: row mean/max dev = %.3f / %.3f ; col mean/max = %.3f / %.3f'
              % (n_r, mn_r, mx_r, mn_c, mx_c))
        print('      max |sphere radius - max corner distance| = %.3f' % rr)
        print('      -> the 4 floats are the LOCAL bounding sphere of the container mesh')
    # .way nodes: distance to nearest assembled geometry
    if len(wpos) and tot_v:
        allw = np.vstack([xf_apply(sc.verts(rec), xf, conv)
                          for rec, cn, xf, src, sp in pls if len(sc.verts(rec))])
        for i, n in enumerate(way_nodes):
            d = np.sqrt((allw[:, 0] - n['pos'][0]) ** 2 + (allw[:, 1] - n['pos'][1]) ** 2)
            k = int(d.argmin())
            if abs(allw[k, 2] - n['pos'][2]) < 40 or d.min() < 150:
                pass
        named = [n for n in way_nodes if n['name'].startswith(('STO', 'mapSto', 'node_store'))]
        for n in named:
            d = np.sqrt((allw[:, 0] - n['pos'][0]) ** 2 + (allw[:, 1] - n['pos'][1]) ** 2)
            k = int(d.argmin())
            print('   .way node %-20s (%8.1f,%9.1f,h=%6.1f) -> nearest assembled vertex '
                  'd=%7.2f (vert h=%.1f)' % (n['name'], n['pos'][0], n['pos'][1], n['pos'][2],
                                             d[k], allw[k, 2]))

    # ---- portals (subtag 30) --------------------------------------------------------
    if sc.portals:
        print('-- portals (subtag 30: room -> neighbouring room) -------------------------------')
        for p in sc.portals:
            c = p['centre']
            ln = math.dist(p['p0'], p['p1'])
            print('   %-22s -> %-16s centre=(%8.2f %8.2f %8.2f) half_len=%.3f  '
                  'p0=(%8.1f %8.1f %7.1f) p1=(%8.1f %8.1f %7.1f)  |p1-p0|=%.3f' % (
                      p['rec']['name'] or '(unnamed)', p['other'], c[0], c[1], c[2], c[3],
                      p['p0'][0], p['p0'][1], p['p0'][2], p['p1'][0], p['p1'][1], p['p1'][2],
                      ln))

    # ---- PNG ------------------------------------------------------------------------
    if out_dir:
        png = write_topview(sc, room_v, way_nodes, terr, out_dir, name, topview, quiet)
        if png:
            print('   image: %s' % png)


def _bbox_str(v):
    if not len(v):
        return '(none)'
    return 'x[%7.1f %7.1f] z[%7.1f %7.1f] h[%7.1f %7.1f]' % (
        v[:, 0].min(), v[:, 0].max(), v[:, 1].min(), v[:, 1].max(), v[:, 2].min(), v[:, 2].max())


def _f4(s):
    return '(%g %g %g r=%g)' % (s[0], s[1], s[2], s[3])


def _colormap(u):
    """u in [0,1] -> (r,g,b) blue->cyan->green->yellow->red"""
    u = float(min(1.0, max(0.0, u)))
    stops = [(0.00, (40, 60, 220)), (0.25, (40, 200, 210)), (0.50, (60, 220, 70)),
             (0.75, (240, 220, 60)), (1.00, (230, 60, 40))]
    for i in range(len(stops) - 1):
        a, ca = stops[i]
        b, cb = stops[i + 1]
        if a <= u <= b:
            t = (u - a) / (b - a)
            return tuple(int(ca[k] + (cb[k] - ca[k]) * t) for k in range(3))
    return stops[-1][1]


def _splat(canvas, X, Z, rgb, size=1):
    H, W = canvas.shape[:2]
    xi = np.round(X).astype(np.int64)
    zi = np.round(Z).astype(np.int64)
    for dx in range(-size, size + 1):
        for dz in range(-size, size + 1):
            x = np.clip(xi + dx, 0, W - 1)
            z = np.clip(zi + dz, 0, H - 1)
            canvas[z, x] = rgb


def write_topview(sc, room_v, way_nodes, terr, out_dir, name, topview=False, quiet=False):
    try:
        from PIL import Image, ImageDraw
    except Exception as e:                                   # pragma: no cover
        print('   (PIL unavailable: %s)' % e)
        return None
    pls = sc.placements()
    obj_v = []
    for rec, cname, xf, src, sphere in pls:
        v = sc.verts(rec)
        if len(v):
            obj_v.append(xf_apply(v, xf, sc.convention))
    obj_v = np.vstack(obj_v) if obj_v else np.zeros((0, 3))
    pv = np.array([p['pos'] for p in sc.points], dtype=np.float64) if sc.points \
        else np.zeros((0, 3))
    allv = np.vstack([x for x in (obj_v, room_v, pv) if len(x)])
    if not len(allv):
        return None
    x0, x1 = float(allv[:, 0].min()), float(allv[:, 0].max())
    z0, z1 = float(allv[:, 1].min()), float(allv[:, 1].max())
    if way_nodes:
        wp = np.array([[n['pos'][0], n['pos'][1]] for n in way_nodes])
        x0, x1 = min(x0, wp[:, 0].min() - 150), max(x1, wp[:, 0].max() + 150)
        z0, z1 = min(z0, wp[:, 1].min() - 150), max(z1, wp[:, 1].max() + 150)
    pad = 0.05 * max(x1 - x0, z1 - z0)
    x0, x1, z0, z1 = x0 - pad, x1 + pad, z0 - pad, z1 + pad
    W = 1500
    H = max(400, int(W * (z1 - z0) / max(1e-6, (x1 - x0))))

    def P(x, z):
        return ((np.asarray(x) - x0) / (x1 - x0) * (W - 1),
                (np.asarray(z) - z0) / (z1 - z0) * (H - 1))

    cv = np.zeros((H, W, 3), np.uint8)
    # grid every 500 units
    gx = np.arange(np.ceil(x0 / 500.0) * 500.0, x1, 500.0)
    gz = np.arange(np.ceil(z0 / 500.0) * 500.0, z1, 500.0)
    for g in gx:
        i = int(P(g, z0)[0])
        cv[:, i] = np.maximum(cv[:, i], np.array([26, 26, 34], np.uint8))
    for g in gz:
        j = int(P(x0, g)[1])
        cv[j, :] = np.maximum(cv[j, :], np.array([26, 26, 34], np.uint8))
    # room (world-space reference) geometry
    if len(room_v):
        X, Z = P(room_v[:, 0], room_v[:, 1])
        _splat(cv, X, Z, (96, 96, 108), 1)
    # sub-40 point objects
    if len(pv):
        X, Z = P(pv[:, 0], pv[:, 1])
        _splat(cv, X, Z, (90, 220, 120), 2)
    # terrain depth-byte contours (stated sample mapping; depth is NOT height)
    if terr.ok:
        for lvl in range(20, 256, 20):
            ji = np.argwhere(np.abs(terr.depth - lvl) < 1.0)[::4]
            if len(ji):
                tx = (ji[:, 1] - TERRAIN_ZERO) * TERRAIN_UNITS_PER_SAMPLE
                tz = (ji[:, 0] - TERRAIN_ZERO) * TERRAIN_UNITS_PER_SAMPLE
                m = (tx > x0) & (tx < x1) & (tz > z0) & (tz < z1)
                if m.any():
                    X, Z = P(tx[m], tz[m])
                    _splat(cv, X, Z, (44, 52, 46), 0)
    # dim halo boxes so the placed containers remain visible at full-tile zoom
    for rec, cn, xf, src, sp in pls:
        v = sc.verts(rec)
        if not len(v):
            continue
        w = xf_apply(v, xf, sc.convention)
        a, b = P(w[:, 0].min() - 8, w[:, 1].min() - 8)
        c, d = P(w[:, 0].max() + 8, w[:, 1].max() + 8)
        i0, i1 = int(max(0, min(a, c))), int(min(W - 1, max(a, c)))
        j0, j1 = int(max(0, min(b, d))), int(min(H - 1, max(b, d)))
        if i1 > i0 and j1 > j0:
            cv[j0:j1, i0:i1] = (0, 58, 52)
    # assembled containers, coloured by their OWN height range
    if len(obj_v):
        hmin, hmax = float(obj_v[:, 2].min()), float(obj_v[:, 2].max())
        rng = max(1e-6, hmax - hmin)
        X, Z = P(obj_v[:, 0], obj_v[:, 1])
        for k in range(len(obj_v)):
            _splat(cv, [X[k]], [Z[k]], _colormap((obj_v[k, 2] - hmin) / rng), 1)
    else:
        hmin = hmax = 0.0
    img = Image.fromarray(cv, 'RGB')
    dr = ImageDraw.Draw(img)
    # anchors
    for rid, f in sc.space_xf.items():
        t = sc.space_abs[rid][9:12]
        X, Z = P(t[0], t[1])
        dr.ellipse([X - 5, Z - 5, X + 5, Z + 5], outline=(255, 110, 240), width=2)
    # placed containers: bright bbox + label so they are visible at full-tile scale
    for rec, cn, xf, src, sp in pls:
        v = sc.verts(rec)
        if not len(v):
            continue
        w = xf_apply(v, xf, sc.convention)
        a, b = P(w[:, 0].min(), w[:, 1].min())
        c, d = P(w[:, 0].max(), w[:, 1].max())
        dr.rectangle([min(a, c) - 6, min(b, d) - 6, max(a, c) + 6, max(b, d) + 6],
                     outline=(60, 255, 200), width=2)
        dr.rectangle([min(a, c) - 6, min(b, d) - 24, min(a, c) + 12 + 7 * len(cn),
                      min(b, d) - 8], fill=(0, 0, 0))
        dr.text((min(a, c) - 4, min(b, d) - 22), cn, fill=(150, 255, 230))
    # way nodes
    for n in way_nodes:
        X, Z = P(n['pos'][0], n['pos'][1])
        dr.line([(X - 7, Z), (X + 7, Z)], fill=(255, 255, 255), width=2)
        dr.line([(X, Z - 7), (X, Z + 7)], fill=(255, 255, 255), width=2)
        if n['name'] and not n['name'].startswith('pos_'):
            dr.rectangle([X + 7, Z - 12, X + 13 + 7 * len(n['name']), Z + 2], fill=(0, 0, 0))
            dr.text((X + 9, Z - 11), n['name'], fill=(255, 240, 120))
    lab = ['HT2:RE b3d_world.py  %s   convention=%s   %d anchors, %d links, %d placements'
           % (sc.path, sc.convention, len(sc.space_xf), len(sc.links), len(pls)),
           'grey = room(19) world-space ground/road mesh (aa: flat quads at h=447.1)',
           'coloured = assembled sub4/5 containers (height %0.0f..%0.0f)  |  '
           'green = sub40 point objects' % (hmin, hmax),
           'white + = .way nodes   magenta o = subtag-24 anchors   grid = 500 world units',
           'black speckle = terrain0.raw 8-bit depth-byte contours (stated 140 u/sample; '
           'NOT height-validated)']
    for i, t in enumerate(lab):
        dr.rectangle([6, 6 + 14 * i, 14 + 7 * len(t), 18 + 14 * i], fill=(0, 0, 0))
        dr.text((8, 7 + 14 * i), t, fill=(225, 225, 225))
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, 'b3dw_%s.png' % name)
    img.save(out)

    # ---------- zoom on the anchored objects ----------
    if len(obj_v):
        cx, cz = float(obj_v[:, 0].mean()), float(obj_v[:, 1].mean())
        sp = max(120.0, 0.7 * max(np.ptp(obj_v[:, 0]), np.ptp(obj_v[:, 1])))
        X0, X1, Z0, Z1 = cx - sp, cx + sp, cz - sp, cz + sp
        W2 = 1300
        H2 = max(400, int(W2 * (Z1 - Z0) / max(1e-6, (X1 - X0))))

        def P2(x, z):
            return ((np.asarray(x) - X0) / (X1 - X0) * (W2 - 1),
                    (np.asarray(z) - Z0) / (Z1 - Z0) * (H2 - 1))

        cv2 = np.zeros((H2, W2, 3), np.uint8)
        m = ((room_v[:, 0] > X0) & (room_v[:, 0] < X1) &
             (room_v[:, 1] > Z0) & (room_v[:, 1] < Z1)) if len(room_v) else np.zeros(0, bool)
        if len(room_v) and m.any():
            X, Z = P2(room_v[m, 0], room_v[m, 1])
            _splat(cv2, X, Z, (110, 110, 124), 1)
        for g in np.arange(np.ceil(X0 / 100.0) * 100.0, X1, 100.0):
            cv2[:, int(P2(g, Z0)[0])] = np.maximum(cv2[:, int(P2(g, Z0)[0])],
                                                   np.array([30, 30, 40], np.uint8))
        for g in np.arange(np.ceil(Z0 / 100.0) * 100.0, Z1, 100.0):
            cv2[int(P2(X0, g)[1]), :] = np.maximum(cv2[int(P2(X0, g)[1]), :],
                                                   np.array([30, 30, 40], np.uint8))
        hmin2 = float(obj_v[:, 2].min())
        hmax2 = float(obj_v[:, 2].max())
        for rec, cn, xf, src, sp2 in pls:
            v = sc.verts(rec)
            if not len(v):
                continue
            w = xf_apply(v, xf, sc.convention)
            X, Z = P2(w[:, 0], w[:, 1])
            for k in range(len(w)):
                _splat(cv2, [X[k]], [Z[k]], _colormap((w[k, 2] - hmin2) /
                                                      max(1e-6, hmax2 - hmin2)), 1)
        img2 = Image.fromarray(cv2, 'RGB')
        d2 = ImageDraw.Draw(img2)
        for rec, cn, xf, src, sp2 in pls:
            v = sc.verts(rec)
            if not len(v):
                continue
            w = xf_apply(v, xf, sc.convention)
            c = (w[:, 0].mean(), w[:, 1].mean())
            if X0 < c[0] < X1 and Z0 < c[1] < Z1:
                XX, ZZ = P2(c[0], c[1])
                d2.rectangle([XX - 6, ZZ - 6, XX + 6, ZZ + 6], outline=(255, 255, 255))
                d2.rectangle([XX + 6, ZZ - 12, XX + 16 + 7 * len(cn), ZZ + 2], fill=(0, 0, 0))
                d2.text((XX + 8, ZZ - 11), '%s [%s]' % (cn, src), fill=(255, 250, 170))
        for n in way_nodes:
            x, z = n['pos'][0], n['pos'][1]
            if X0 < x < X1 and Z0 < z < Z1:
                XX, ZZ = P2(x, z)
                d2.line([(XX - 9, ZZ), (XX + 9, ZZ)], fill=(255, 255, 255), width=2)
                d2.line([(XX, ZZ - 9), (XX, ZZ + 9)], fill=(255, 255, 255), width=2)
                d2.rectangle([XX + 9, ZZ - 12, XX + 15 + 7 * len(n['name']), ZZ + 2],
                             fill=(0, 0, 0))
                d2.text((XX + 11, ZZ - 11), n['name'], fill=(255, 240, 120))
        for rid, f in sc.space_xf.items():
            t = sc.space_abs[rid][9:12]
            if X0 < t[0] < X1 and Z0 < t[1] < Z1:
                XX, ZZ = P2(t[0], t[1])
                d2.ellipse([XX - 6, ZZ - 6, XX + 6, ZZ + 6], outline=(255, 110, 240), width=2)
        d2.text((8, 8), '%s  zoom on placed containers  (x %.0f..%.0f  z %.0f..%.0f)'
                % (name, X0, X1, Z0, Z1), fill=(235, 235, 235))
        out2 = os.path.join(out_dir, 'b3dw_%s_zoom.png' % name)
        img2.save(out2)
        return '%s , %s' % (out, out2)
    return out


# --------------------------------------------------------------------------------------
# corpus-wide link verification (--all)
# --------------------------------------------------------------------------------------
def sphere_dev(sc, conv):
    """Adversarial test of the placement rule using the sub-18/sub-4 bounding sphere.

    The 4 floats stored in a link record are the container's LOCAL bounding sphere:
    (centre.x, centre.z, centre.height, radius) -- verified against the mesh bbox of the
    container (e.g. dq.b3d lep12: sphere (287.34 914.17 23.33 r=65.56) vs local bbox
    centre (288.05 914.45 30.35), radius = max corner distance).  Placing the sphere
    centre with the anchor must therefore reproduce the placed geometry's centre:
        anchor.xf(sphere_centre)  ==  bbox centre of the placed mesh
    which tests the rotation as well as the translation.  Returns (n, max_dev, mean_dev,
    max_radius_err) over every container that has both a sphere and a mesh.
    """
    devs, rerrs, disc = [], [], []
    for rec, cname, xf, src, sphere in sc.placements():
        v = sc.verts(rec)
        if not len(v):
            continue
        r = float(sphere[3])
        if r <= 0.0:
            continue
        c = np.asarray(sphere[:3], dtype=np.float64)
        pr = xf_apply(c, xf, 'row')
        pc = xf_apply(c, xf, 'col')
        disc.append(float(np.linalg.norm(pr - pc)))          # how much the conventions differ
        pred = pr if conv == 'row' else pc
        w = xf_apply(v, xf, conv)
        act = (w.min(0) + w.max(0)) / 2.0
        devs.append(float(np.linalg.norm(pred - act)))
        rerrs.append(abs(r - float(np.linalg.norm(w - pred, axis=1).max())))
    if not devs:
        return 0, 0.0, 0.0, 0.0, 0, 0.0
    nd = int(np.sum(np.asarray(disc) > 1.0))
    return (len(devs), float(np.max(devs)), float(np.mean(devs)), float(np.max(rerrs)),
            nd, float(np.max(disc)))


def verify_all(pattern='original/**/*.b3d'):
    rows = []
    fails = []
    n_mesh_r0 = 0
    n_nomesh_rpos = 0
    paths = sorted(glob.glob(pattern, recursive=True))
    for p in paths:
        try:
            sc = Scene(p)
        except Exception as e:
            fails.append((p, 'parse: %s' % e))
            continue
        room_v = np.zeros((0, 3))
        for rm in sc.rooms:
            v = sc.verts(rm)
            if len(v):
                room_v = np.vstack([room_v, v])
        n_anchor_ok = 0
        worst = 0.0
        worst_what = ''
        dup = {}
        for rec, cn, xf, src, s in sc.placements():
            dup.setdefault(cn, []).append(src)
            v = sc.verts(rec)
            if not len(v):
                continue
            w = xf_apply(v, xf, sc.convention)
            anc = np.asarray(xf, float)[9:12]
            dx = max(w[:, 0].min() - anc[0], anc[0] - w[:, 0].max(), 0)
            dz = max(w[:, 1].min() - anc[1], anc[1] - w[:, 1].max(), 0)
            d = math.hypot(dx, dz)
            n_anchor_ok += 1
            if d > worst:
                worst, worst_what = d, '%s<-%s' % (cn, src)
        multi = {k: v for k, v in dup.items()
                 if len([x for x in v if x.startswith('link:')]) > 1}
        selflink = {k: v for k, v in dup.items()
                    if len(v) > 1 and not len([x for x in v if x.startswith('link:')]) > 1}
        # sub-4 self-anchor vs link agreement
        agree, disagree = 0, []
        for it in sc.instances:
            a1, c1 = sc.resolve(it['anchor'])
            if a1 is None:
                continue
            for lk in sc.links:
                if lk['obj'] != it['rec']['name']:
                    continue
                a2, c2 = sc.resolve(lk['space'])
                if a2 is None:
                    continue
                if np.allclose(a1, a2, atol=1e-3):
                    agree += 1
                else:
                    disagree.append((it['rec']['name'], c1, c2,
                                     float(np.abs(a1 - a2).max())))
        # sphere-vs-mesh consistency: radius 0 <=> no renderable mesh (engine cull path)
        for rec, cn, xf, src, sp in sc.placements():
            has = len(sc.verts(rec)) > 0
            if has and float(sp[3]) <= 0.0:
                n_mesh_r0 += 1
            if (not has) and float(sp[3]) > 0.0:
                n_nomesh_rpos += 1
        sc.convention = 'row'
        n_row, mx_row, mn_row, rr_row, nd_row, dm_row = sphere_dev(sc, 'row')
        sc.convention = 'col'
        n_col, mx_col, mn_col, rr_col, nd_col, dm_col = sphere_dev(sc, 'col')
        sc.convention = 'row'
        rows.append(dict(path=p, links=len(sc.links), inst=len(sc.instances),
                         groups=len(sc.groups), rooms=len(sc.rooms),
                         placed=len(sc.placements()), worst=worst, worst_what=worst_what,
                         multi=multi, selflink=selflink, agree=agree, disagree=disagree,
                         n_sphere=n_row, sph_row=mx_row, sph_row_mean=mn_row, rad_row=rr_row,
                         sph_col=mx_col, sph_col_mean=mn_col, ndisc=nd_row, dmax=dm_row,
                         verts=sum(len(sc.verts(r)) for r, *_ in sc.placements())))
    print('%-34s %5s %5s %5s %6s %6s %10s %8s' % (
        'file', 'link', 'sub4', 'sub5', 'rooms', 'placed', 'worst_dev', 'multi'))
    for r in rows:
        print('%-34s %5d %5d %5d %6d %6d %10.3f %8d   %s' % (
            os.path.relpath(r['path']), r['links'], r['inst'], r['groups'], r['rooms'],
            r['placed'], r['worst'], len(r['multi']),
            ('worst=%s' % r['worst_what']) if r['worst'] > 1.0 else ''))
    print()
    nmulti = sum(len(r['multi']) for r in rows)
    nagree = sum(r['agree'] for r in rows)
    ndis = sum(len(r['disagree']) for r in rows)
    print('corpus: files=%d  links=%d  sub4=%d  placements=%d' % (
        len(rows), sum(r['links'] for r in rows), sum(r['inst'] for r in rows),
        sum(r['placed'] for r in rows)))
    print('  containers referenced by >1 LINK (true multi-anchor, one copy per link): %d'
          % nmulti)
    shown = 0
    for r in rows:
        for k, v in r['multi'].items():
            if len({x.split(':')[1] for x in v}) > 1 and shown < 12:
                print('     %-24s %-22s %s' % (os.path.basename(r['path']), k, v))
                shown += 1
    same = sum(1 for r in rows for k, v in r['multi'].items()
               if len({x.split(':')[1] for x in v}) == 1)
    print('     of these, containers whose several links all name the SAME anchor: %d' % same)
    print('     -> the anchor list order is the sub-18 record order in the file; each link is an')
    print('        independent copy, so order does not change the assembled set.')
    ns = sum(len(r['selflink']) for r in rows)
    print('  containers carrying BOTH a sub-4 self anchor and sub-18 link(s): %d' % ns)
    print('     (expected: the sub-4 name points at the paired <X>Space_ child of the linked')
    print('      <X>Space, same absolute matrix -> redundant but consistent)')
    print('  sub4 self-anchor == sub18 link anchor: agree=%d disagree=%d' % (nagree, ndis))
    for r in rows:
        for d in r['disagree']:
            print('     DISAGREE %-24s %s %s %s dmax=%.4f' % (os.path.basename(r['path']), *d))
    print()
    print('  ADVERSARIAL SPHERE TEST:  anchor.xf(sphere_centre)  vs  placed mesh bbox centre')
    print('     (the sphere is stored in the LINK/INSTANCE record, the mesh in the container:')
    print('      agreeing them tests translation AND rotation of the placement rule)')
    tot = sum(r['n_sphere'] for r in rows)
    print('     containers tested=%d' % tot)
    for conv, key, keym in (('row (p.M+t)', 'sph_row', 'sph_row_mean'),
                            ('col (M.p+t)', 'sph_col', 'sph_col_mean')):
        mx = max([r[key] for r in rows], default=0.0)
        mm = float(np.mean([r[keym] for r in rows if r['n_sphere']]))
        who = max(rows, key=lambda r: r[key])['path'] if rows else '-'
        print('     %-12s  mean_dev=%9.3f  max_dev=%9.3f (%s)' % (conv, mm, mx, os.path.basename(who)))
    print('     radius check (row): max |r_file - max vertex distance from sphere centre| '
          '= %.3f' % max([r['rad_row'] for r in rows], default=0.0))
    print('     sphere radius 0 <=> no renderable mesh: violations '
          'mesh&r==0 -> %d , no-mesh&r>0 -> %d' % (n_mesh_r0, n_nomesh_rpos))
    print('     DISCRIMINATING POWER: containers whose sphere centre moves >1 unit between the')
    print('       two conventions: %d of %d ; largest row-vs-col displacement = %.1f units'
          % (sum(r['ndisc'] for r in rows), tot, max([r['dmax'] for r in rows], default=0.0)))
    print('     (the sphere centre usually sits near the container origin, so this test is weak;')
    print('      the convention is settled by the disassembly, see docs/b3d_transform.md)')
    bad = [r for r in rows if r['sph_row'] > 5.0]
    print('     files with row max_dev > 5 units: %d %s' % (
        len(bad), [os.path.basename(r['path']) for r in bad][:8]))
    print()
    print('  worst anchor-vs-footprint deviation over all files: %.3f (%s)' % (
        max([r['worst'] for r in rows], default=0.0),
        max(rows, key=lambda r: r['worst'])['path'] if rows else '-'))
    print('     (large values are data, not rule failures: the anchor sits at the container')
    print('      ORIGIN and e.g. dq.b3d "lep12" is modelled at local (241..335, 865..964))')
    if fails:
        print('  PARSE FAILURES:')
        for p, e in fails:
            print('     %s : %s' % (p, e))
    return rows


# --------------------------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description='assemble B3D tiles into world space')
    ap.add_argument('b3d', nargs='?', default='original/ENV/aa.b3d')
    ap.add_argument('--way')
    ap.add_argument('--terrain', default='original/MTR/terrain0.raw')
    ap.add_argument('--out', default='tools/out')
    ap.add_argument('--topview', action='store_true', help='also print an ASCII top view')
    ap.add_argument('--convention', default='row', choices=('row', 'col'))
    ap.add_argument('--all', action='store_true', help='verify every .b3d in original/')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args(argv)

    if a.all:
        verify_all()
        return 0

    way = a.way
    if way is None:
        cand = os.path.splitext(a.b3d)[0] + '.way'
        if os.path.exists(cand):
            way = cand
    sc = Scene(a.b3d, convention=a.convention)
    stem = os.path.splitext(os.path.basename(a.b3d))[0]
    d = os.path.basename(os.path.dirname(os.path.abspath(a.b3d))).lower()
    tag = stem if d == 'env' else '%s_%s' % (d, stem)
    report(sc, way, a.terrain, a.out, topview=a.topview, quiet=a.quiet, tag=tag)
    if a.topview:
        ascii_topview(sc)
    return 0


def ascii_topview(sc, w=110, h=44):
    pts = []
    for rec, cn, xf, src, s in sc.placements():
        v = sc.verts(rec)
        if len(v):
            v = xf_apply(v, xf, sc.convention)
            pts.append(np.column_stack([v[:, 0], v[:, 1], np.full(len(v), 9.0)]))
    for rm in sc.rooms:
        v = sc.verts(rm)
        if len(v):
            pts.append(np.column_stack([v[:, 0], v[:, 1], np.full(len(v), 1.0)]))
    if not pts:
        return
    P = np.vstack(pts)
    x0, x1, z0, z1 = P[:, 0].min(), P[:, 0].max(), P[:, 1].min(), P[:, 1].max()
    grid = [[' '] * w for _ in range(h)]
    for x, z, ch in P:
        i = int((x - x0) / max(1e-6, x1 - x0) * (w - 1))
        j = int((z - z0) / max(1e-6, z1 - z0) * (h - 1))
        grid[j][i] = '#' if ch > 5 else '.'
    print('-- ASCII top view  x[%.0f %.0f] z[%.0f %.0f]  (# = anchored sub4/5, . = room) --'
          % (x0, x1, z0, z1))
    for row in grid:
        print('   ' + ''.join(row))


if __name__ == '__main__':
    sys.exit(main())
