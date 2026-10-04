#!/usr/bin/env python3
"""collision_probe.py -- reproduce the C++ world-collision rule in Python.

Reads the shipped tiles (original/ENV/*.b3d) with tools/b3d_full.py, assembles
them into world space exactly like ht2::B3D::buildFrom()/buildScene() do (the
same list the renderer draws and the list Collision::build() consumes), applies
the blocking rule of src/game/Collision.cpp and reports blocked-area fractions
at known places.

    python3 tools/collision_probe.py                 # default report
    python3 tools/collision_probe.py --materials     # triangle census per material
    python3 tools/collision_probe.py --png out.png   # block grid as an image
    python3 tools/collision_probe.py --only aa       # one tile (debug)
    python3 tools/collision_probe.py --refresh       # ignore the tile cache

Coordinate order is the B3D one used everywhere in this project:
    component 0 = world x, 1 = world z, 2 = world height ("y").
The rule/grid constants mirror src/game/Collision.cpp; keep them in sync.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import math
import os
import re
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import b3d_full as B

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'tools', 'out')

# ---------------------------------------------------------------------------
# rule constants -- mirror src/game/Collision.cpp
# ---------------------------------------------------------------------------
CELL = 1.0                 # grid cell size (world units)
DECAL_MIN_EXTENT = 0.5     # submeshes flatter than this are decals/markings
HORIZONTAL_UP = 0.8        # |up| above this = ground/roof/deck -> not a wall
ABSURD_H = 1000.0          # sentinel vertices (|height| beyond this)
ABSURD_XZ = 20000.0
MAX_DEPTH = 8              # sub-18 link nesting guard (ht2::B3D::buildFrom)
CACHE_VERSION = 'v2'       # bump when the assembler changes

# Materials that are drivable surfaces (roads/aprons/yards/water) or decals.
# A submesh whose material matches is never solid -- unless it also matches
# SOLID_MAT (e.g. mat_beton_stolb: concrete *pillar*, not concrete ground).
GROUND_MAT = re.compile(
    r'road|grass|beton|concrete|gravel|zebra|spl|ground|ter_|water|cross|mark|'
    r'asphalt|sand|dirt|pavement|sidewalk|kerb|curb|trotoar|trotuar|stoika|'
    r'stoyanka|parking|runway|taxiway|apron|swamp|bshadow|river|tonnel|'
    r'zont|bottom_bridge|bridge_0',
    re.I)
SOLID_MAT = re.compile(
    r'stolb|pillar|column|kolonn|wall|fence|confence|ograjdenie|rama|'
    r'beton_bridge|side_beton', re.I)

RULE_KEY = '%s|%s|%s|%s|%s|%s' % (CELL, DECAL_MIN_EXTENT, HORIZONTAL_UP,
                                  ABSURD_H, GROUND_MAT.pattern, SOLID_MAT.pattern)
RULE_HASH = hashlib.sha1(RULE_KEY.encode()).hexdigest()[:10]


def is_ground_material(mat: str) -> bool:
    return bool(GROUND_MAT.search(mat)) and not SOLID_MAT.search(mat)


# ---------------------------------------------------------------------------
# B3D assembly (mirrors ht2::B3D::buildFrom walker)
# ---------------------------------------------------------------------------
def u32(d, p):
    return struct.unpack_from('<I', d, p)[0]


def f32(d, p, n=1):
    return list(struct.unpack_from('<%df' % n, d, p))


def nm32(d, p):
    return d[p:p + 32].split(b'\x00')[0].decode('latin1', 'replace')


def walk(recs):
    st = list(recs)
    while st:
        r = st.pop()
        yield r
        st.extend(r.get('children') or [])


def xf_mul(a, b):
    """Column-vector compose: apply a first, then b (matches ht2::xfMul)."""
    Ma, ta = a
    Mb, tb = b
    return (Ma @ Mb, Ma @ tb + ta)


def xf_apply(f, v):
    M, t = f
    return v @ M.T + t


IDENT = (np.eye(3), np.zeros(3))


def poly_indices(d, q, last):
    """Decode one poly record at header offset q -> (w0, matId, [vertex idx]).

    Identical to ht2::B3D::parsePolys: the payload length is found by scanning
    for the next {1.0f, 0x7fff} header (or a 555/444 closer) and the per-entry
    stride is (dw-1)/cnt when that divides, else dw/cnt.
    """
    w0 = u32(d, q)
    mat = u32(d, q + 12)
    cnt = u32(d, q + 16)
    ds = q + 20
    j = ds
    hi = min(len(d) - 8, ds + (1 << 20))
    nq = None
    while j < hi:
        v = u32(d, j)
        if v in (B.T_ESEC, B.T_BSEC) and u32(d, j + 4) in B.KNOWN:
            break
        if not last and v == 0x3f800000 and u32(d, j + 4) == 0x00007fff:
            nq = j - 4
            break
        j += 4
    end = nq if nq is not None else j
    dw = (end - ds) // 4
    s = 1
    if cnt > 0 and dw > cnt and (dw - 1) % cnt == 0:
        s = (dw - 1) // cnt
    elif cnt > 0 and dw >= cnt and dw % cnt == 0:
        s = dw // cnt
    return w0, mat, [u32(d, ds + 4 * s * k) for k in range(cnt)]


def triangulate(idx, strip):
    n = len(idx)
    if n < 3:
        return
    if not strip:
        for t in range(2, n):
            yield idx[0], idx[t - 1], idx[t]
    else:
        for t in range(2, n):
            if t % 2 == 0:
                yield idx[t - 2], idx[t - 1], idx[t]
            else:
                yield idx[t - 1], idx[t - 2], idx[t]


class Tile:
    """One ENV tile flattened to world-space triangles (mirrors buildScene)."""

    def __init__(self, path, skip_linked=True):
        self.path = path
        self.tree = B.parse_b3d(path)
        self.d = open(path, 'rb').read()
        self.names = self.tree['names']
        self.warnings = list(self.tree['warn'])
        self.tris = []          # (matName, v0, v1, v2, submeshId) world space
        self.subext = []        # per submesh: vertical extent (height max-min)
        self.sublist = []       # (nodeName, material, triCount, cx, cz)
        self.hit_nodes = 0
        self._anchors()
        self._links()
        self._node_index()
        self._flatten(skip_linked)

    # -- anchors (subtag 24) / links (subtag 18) ---------------------------
    def _anchors(self):
        self.anchors = {}
        for r in walk(self.tree['top']):
            if r['sub'] == 24 and r['name']:
                raw = f32(self.d, r['at'] + 40, 12)
                # king.exe uses row-vector p.M + t; we use the column
                # convention (p' = M.p) -> transpose once, like ht2::B3D.
                M = np.asarray(raw[:9], float).reshape(3, 3).T
                self.anchors[r['name']] = (M, np.asarray(raw[9:12], float))

    def _links(self):
        self.link_targets = set()
        self.links = []
        for r in walk(self.tree['top']):
            if r['sub'] == 18:
                a = nm32(self.d, r['at'] + 40 + 16)
                o = nm32(self.d, r['at'] + 40 + 48)
                if a and o:
                    self.links.append((a, o))
                    self.link_targets.add(o)

    def _node_index(self):
        self.node_index = {}
        for r in walk(self.tree['top']):
            if r['name'] and r['name'] not in self.node_index:
                self.node_index[r['name']] = r

    # -- the buildFrom walker ---------------------------------------------
    def _flatten(self, skip_linked):
        self.emitted_pairs = set()

        def emit(mesh, face, xf):
            nf = mesh.get('stride') or B.NFM.get(mesh.get('fmt', 0))
            voff, vc = mesh['voff'], mesh['vc']
            verts = np.frombuffer(self.d, dtype='<f4', count=nf * vc,
                                  offset=voff).reshape(vc, nf)[:, :3].astype(np.float64)
            wv = xf_apply(xf, verts)
            # one SubMesh per (face, material) -- same split as ht2::B3D::emit
            groups = {}
            for i, (w0, mat, off) in enumerate(face['polys']):
                _, _, idx = poly_indices(self.d, off, i + 1 == len(face['polys']))
                groups.setdefault(mat, []).append((w0, idx))
            nodename = face['name'] or mesh['name']
            for mat, items in groups.items():
                name = self.names[mat] if mat < len(self.names) else ''
                sub = len(self.subext)
                hmin, hmax = 1e30, -1e30
                mnx = mnz = 1e30; mxx = mxz = -1e30
                ntri = 0
                for w0, idx in items:
                    for a, b, c in triangulate(idx, (w0 & 0x80) != 0):
                        if a >= vc or b >= vc or c >= vc:
                            continue
                        v0, v1, v2 = wv[a], wv[b], wv[c]
                        hmin = min(hmin, v0[2], v1[2], v2[2])
                        hmax = max(hmax, v0[2], v1[2], v2[2])
                        mnx = min(mnx, v0[0], v1[0], v2[0]); mxx = max(mxx, v0[0], v1[0], v2[0])
                        mnz = min(mnz, v0[1], v1[1], v2[1]); mxz = max(mxz, v0[1], v1[1], v2[1])
                        ntri += 1
                        self.tris.append((name, v0, v1, v2, sub))
                self.subext.append(hmax - hmin if hmax > hmin else 0.0)
                self.sublist.append((nodename, name, ntri,
                                     0.5 * (mnx + mxx) if ntri else 0.0,
                                     0.5 * (mnz + mxz) if ntri else 0.0))

        def go(n, cur_mesh, xf, depth):
            if depth > MAX_DEPTH:
                return
            if n['name'] and skip_linked and depth == 0 and n['name'] in self.link_targets:
                return
            if n['name'].startswith('hit_') and depth == 0:
                self.hit_nodes += 1
                return
            if n['sub'] in (7, 37):
                cur_mesh = n
            if n['sub'] in (35, 8) and cur_mesh is not None:
                emit(cur_mesh, n, xf)
            if n['sub'] == 18:
                frm = nm32(self.d, n['at'] + 40 + 16)
                to = nm32(self.d, n['at'] + 40 + 48)
                tgt = self.node_index.get(to)
                if tgt is not None and tgt is not n:
                    key = (to, frm)
                    if frm:
                        if key in self.emitted_pairs:
                            return
                        self.emitted_pairs.add(key)
                    comb = xf
                    if frm and frm in self.anchors:
                        comb = xf_mul(xf, self.anchors[frm])
                    go(tgt, None, comb, depth + 1)
                return
            # NB: ht2::B3D::buildFrom passes `depth` unchanged here; only a
            # sub-18 link bumps it, so depth counts link nesting, not tree depth.
            for k in n.get('children') or []:
                go(k, cur_mesh, xf, depth)

        for n in self.tree['top']:
            go(n, None, IDENT, 0)


# ---------------------------------------------------------------------------
# world flattening (with cache)
# ---------------------------------------------------------------------------
def tile_paths(only=None):
    files = sorted(glob.glob(os.path.join(ROOT, 'original/ENV/*.b3d')))
    if only:
        files = [f for f in files if os.path.basename(f).startswith(only)]
    return files


def flatten_world(only=None, quiet=False, refresh=False):
    """-> (tris (N,3,3), matname (N,), subid (N,), subext (S,), files)."""
    os.makedirs(OUT, exist_ok=True)
    tag = only or 'all'
    cache = os.path.join(OUT, 'collision_tris_%s_%s.npz' % (tag, CACHE_VERSION))
    if os.path.exists(cache) and not refresh:
        z = np.load(cache, allow_pickle=True)
        return (z['tris'], z['matname'], z['subid'], z['subext'],
                list(z['files']))
    files = tile_paths(only)
    names, subs, tris = [], [], []
    subext = []
    for p in files:
        t = Tile(p)
        base = len(subext)
        subext.extend(t.subext)
        for name, v0, v1, v2, sub in t.tris:
            names.append(name)
            subs.append(sub + base)
            tris.append((v0, v1, v2))
        if not quiet:
            print('  %-10s %7d triangles  %4d submeshes' %
                  (os.path.basename(p), len(t.tris), len(t.subext)), file=sys.stderr)
    arr = np.asarray(tris, float).reshape(-1, 3, 3)
    nm = np.asarray(names, dtype=object)
    np.savez_compressed(cache, tris=arr, matname=nm,
                        subid=np.asarray(subs, np.int32),
                        subext=np.asarray(subext, np.float32),
                        files=np.asarray(files))
    return arr, nm, np.asarray(subs, np.int32), np.asarray(subext, np.float32), files


def up_components(tris):
    e1 = tris[:, 1] - tris[:, 0]
    e2 = tris[:, 2] - tris[:, 0]
    n = np.cross(e1, e2)
    l = np.linalg.norm(n, axis=1)
    up = np.where(l > 1e-9, np.abs(n[:, 2]) / np.maximum(l, 1e-30), 1.0)
    return up


def classify(tris, matname, subid, subext):
    """Vectorised Collision::build rule -> bool mask of blocking triangles.

    Mirrors src/game/Collision.cpp: material filter first (whole SubMesh), then
    the flat-decal submesh filter, then per-triangle sentinel + orientation.
    """
    nc = np.char.lower(matname.astype(str))
    ground = np.array([is_ground_material(m) for m in nc], bool)
    flat = subext[subid] < DECAL_MIN_EXTENT
    big = (np.abs(tris[:, :, 0]).max(1) > ABSURD_XZ) | \
          (np.abs(tris[:, :, 1]).max(1) > ABSURD_XZ) | \
          (np.abs(tris[:, :, 2]).max(1) > ABSURD_H)
    # ht2::Collision computes these in float32; do the same so triangles that
    # sit exactly on the 0.8 threshold classify identically.
    up = up_components(tris.astype(np.float32))
    return (~ground) & (~flat) & (~big) & (up <= HORIZONTAL_UP)


# ---------------------------------------------------------------------------
# grid (mirrors Collision::build/blocked)
# ---------------------------------------------------------------------------
def tri_rect_overlap(p0, p1, p2, x0, x1, z0, z1):
    """SAT: triangle vs axis-aligned rectangle in 2D."""
    pts = (p0, p1, p2)
    for (a, b) in ((p0, p1), (p1, p2), (p2, p0)):
        nx = -(b[1] - a[1])
        nz = (b[0] - a[0])
        tv = [nx * p[0] + nz * p[1] for p in pts]
        rv = [nx * xx + nz * zz for xx in (x0, x1) for zz in (z0, z1)]
        if max(tv) < min(rv) or max(rv) < min(tv):
            return False
    if max(p[0] for p in pts) < x0 or min(p[0] for p in pts) > x1:
        return False
    if max(p[1] for p in pts) < z0 or min(p[1] for p in pts) > z1:
        return False
    return True


class Grid:
    def __init__(self, cell=CELL):
        self.cell = cell
        self.blocked = None
        self.x0 = self.z0 = 0.0
        self.nx = self.nz = 0
        self.tris = None

    def build(self, tris, pad_cells=4):
        if len(tris) == 0:
            self.blocked = np.zeros((1, 1), bool)
            return
        lo = tris.reshape(-1, 3).min(0)
        hi = tris.reshape(-1, 3).max(0)
        pad = pad_cells * self.cell
        self.x0 = math.floor((lo[0] - pad) / self.cell) * self.cell
        self.z0 = math.floor((lo[1] - pad) / self.cell) * self.cell
        self.nx = int(math.ceil((hi[0] + pad - self.x0) / self.cell)) + 1
        self.nz = int(math.ceil((hi[1] + pad - self.z0) / self.cell)) + 1
        g = np.zeros((self.nx, self.nz), bool)
        cell = self.cell
        tri2 = tris[:, :, :2]
        for t in tri2:
            xs = t[:, 0]
            zs = t[:, 1]
            i0 = max(0, int(math.floor((xs.min() - self.x0) / cell)))
            i1 = min(self.nx - 1, int(math.floor((xs.max() - self.x0) / cell)))
            j0 = max(0, int(math.floor((zs.min() - self.z0) / cell)))
            j1 = min(self.nz - 1, int(math.floor((zs.max() - self.z0) / cell)))
            p0, p1, p2 = t[0], t[1], t[2]
            if (p1[0] - p0[0]) * (p2[1] - p0[1]) - (p1[1] - p0[1]) * (p2[0] - p0[0]) < 0:
                p1, p2 = p2, p1
            for i in range(i0, i1 + 1):
                r0 = self.x0 + i * cell
                r1 = r0 + cell
                for j in range(j0, j1 + 1):
                    if g[i, j]:
                        continue
                    s0 = self.z0 + j * cell
                    s1 = s0 + cell
                    if tri_rect_overlap(p0, p1, p2, r0, r1, s0, s1):
                        g[i, j] = True
        self.blocked = g
        self.tris = tris

    def cell_of(self, x, z):
        return (int(math.floor((x - self.x0) / self.cell)),
                int(math.floor((z - self.z0) / self.cell)))

    def blocked_circle(self, x, z, r):
        i0 = max(0, int(math.floor((x - r - self.x0) / self.cell)))
        i1 = min(self.nx - 1, int(math.floor((x + r - self.x0) / self.cell)))
        j0 = max(0, int(math.floor((z - r - self.z0) / self.cell)))
        j1 = min(self.nz - 1, int(math.floor((z + r - self.z0) / self.cell)))
        if i1 < i0 or j1 < j0:
            return False
        for i in range(i0, i1 + 1):
            for j in range(j0, j1 + 1):
                if not self.blocked[i, j]:
                    continue
                cx = self.x0 + i * self.cell
                cz = self.z0 + j * self.cell
                dx = max(cx - x, 0.0, x - (cx + self.cell))
                dz = max(cz - z, 0.0, z - (cz + self.cell))
                if dx * dx + dz * dz <= r * r:
                    return True
        return False

    def fraction_blocked(self, x, z, r, step=0.5):
        n = hit = 0
        k = int(math.ceil(r / step))
        for a in range(-k, k + 1):
            for b in range(-k, k + 1):
                px = x + a * step
                pz = z + b * step
                if (px - x) ** 2 + (pz - z) ** 2 > r * r:
                    continue
                n += 1
                i, j = self.cell_of(px, pz)
                if 0 <= i < self.nx and 0 <= j < self.nz and self.blocked[i, j]:
                    hit += 1
        return (hit / n if n else 0.0), n

    # -- solid connected components (fill building/yard interiors) --------
    def components(self, min_cells=1):
        """Rooms of free space / sheets of solid; returns (labels, sizes)."""
        try:
            from scipy import ndimage
        except Exception:
            return None, None
        lab, n = ndimage.label(self.blocked)
        if n == 0:
            return lab, np.zeros(0, int)
        sizes = np.bincount(lab.ravel())
        return lab, sizes

    def save_png(self, path, scale=4, points=()):
        from PIL import Image
        img = np.zeros((self.nx, self.nz, 3), 'u1')
        img[..., 0] = np.where(self.blocked, 60, 0)
        img[..., 1] = np.where(self.blocked, 60, 0)
        img[..., 2] = np.where(self.blocked, 70, 0)
        img[self.blocked] = (230, 230, 230)
        im = Image.fromarray(img.transpose(1, 0, 2))   # x right, z down
        im = im.resize((max(1, self.nz // scale), max(1, self.nx // scale)),
                       Image.NEAREST)
        im = im.transpose(Image.TRANSPOSE)
        if points:
            from PIL import ImageDraw
            dr = ImageDraw.Draw(im)
            for (px, pz, col) in points:
                cx = int((pz - self.z0) / self.cell / scale)
                cy = int((px - self.x0) / self.cell / scale)
                dr.ellipse([cx - 4, cy - 4, cx + 4, cy + 4], outline=col, width=2)
        im.save(path)
        print('wrote %s' % path, file=sys.stderr)


# ---------------------------------------------------------------------------
# known places
# ---------------------------------------------------------------------------
def read_way_nodes(path):
    """Minimal ENV/*.way RNOD reader (name + world position)."""
    d = open(path, 'rb').read()
    ch = lambda n: (n + 3) & ~3
    out = []

    def walk_chunks(off, end):
        i = off
        while i + 8 <= end:
            tag = d[i:i + 4]
            size = u32(d, i + 4)
            if tag == b'RNOD':
                nd = {'name': '', 'pos': None}
                j = i + 8
                while j + 8 <= i + 8 + ch(size):
                    t2, s2 = d[j:j + 4], u32(d, j + 4)
                    pay = d[j + 8:j + 8 + s2]
                    if t2 == b'NNAM':
                        nd['name'] = pay.split(b'\x00')[0].decode('latin1')
                    elif t2 == b'POSN' and s2 >= 24:
                        nd['pos'] = struct.unpack('<3d', pay[:24])
                    j += 8 + ch(s2)
                if nd['pos']:
                    out.append(nd)
            elif tag in (b'GDAT', b'GROM'):
                walk_chunks(i + 8, i + 8 + size)
            i += 8 + ch(size)

    walk_chunks(8, len(d))
    return out


def way_nodes():
    nodes = []
    for p in sorted(glob.glob(os.path.join(ROOT, 'original/ENV/*.way'))):
        try:
            for nd in read_way_nodes(p):
                nodes.append((os.path.basename(p), nd['name'],
                              nd['pos'][0], nd['pos'][1], nd['pos'][2]))
        except Exception as e:                       # pragma: no cover
            print('way: %s failed (%s)' % (p, e), file=sys.stderr)
    return nodes


def compare_cpp(path, g):
    """Diff this Python grid against a grid dumped by tools/collision_check.cpp."""
    raw = open(path, 'rb').read()
    nx, nz, x0, z0, cell = struct.unpack_from('<iifff', raw, 0)
    # C++ packs bit (idx & 7) as `1 << (idx & 7)` (LSB first)
    bits = np.unpackbits(np.frombuffer(raw, dtype='u1', offset=20),
                         bitorder='little')[:nx * nz]
    cpp = bits.reshape(nx, nz).astype(bool)
    di = int(round((x0 - g.x0) / g.cell))
    dj = int(round((z0 - g.z0) / g.cell))
    py = np.zeros_like(cpp)
    # cpp[i, j] corresponds to python cell (i + di, j + dj)
    si0, sj0 = max(0, -di), max(0, -dj)
    ti0, tj0 = max(0, di), max(0, dj)
    h = min(nx - si0, g.nx - ti0)
    w = min(nz - sj0, g.nz - tj0)
    py[si0:si0 + h, sj0:sj0 + w] = g.blocked[ti0:ti0 + h, tj0:tj0 + w]
    diff = cpp ^ py
    n = int(diff.sum())
    print('compare %s: cpp %dx%d @ %.2f origin (%.1f,%.1f) vs python %dx%d @ %.2f '
          'origin (%.1f,%.1f)' % (path, nx, nz, cell, x0, z0, g.nx, g.nz,
                                  g.cell, g.x0, g.z0))
    print('  cpp solid=%d  python solid=%d  mismatching cells=%d (%.4f%%)'
          % (int(cpp.sum()), int(py.sum()), n, 100.0 * n / cpp.size))
    if n:
        idx = np.argwhere(diff)
        rng = np.random.default_rng(0)
        for (i, j) in idx[rng.choice(len(idx), size=min(10, len(idx)), replace=False)]:
            print('    (%.1f, %.1f) cpp=%d python=%d'
                  % (x0 + (i + .5) * cell, z0 + (j + .5) * cell, cpp[i, j], py[i, j]))
    return n


POINTS = [
    ('airport apron', -3063.0, -4658.0, 'free (mat_road / mat_beton apron)'),
    ('highway', -2330.0, -3760.0, 'free'),
    ('player spawn', -3020.0, -4625.0, 'free (ENV/GAME.CNF start)'),
    ('warehouse STOS bay', 4750.5, -5675.0, 'drive-in repair bay: floor free'),
    ('warehouse STOS wall', 4748.1, -5675.0, 'on the interior wall: blocked'),
    ('repair shop af bay', -2400.0, 2758.5, 'parking node inside a small shop'),
]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--materials', action='store_true', help='triangle census')
    ap.add_argument('--radius', type=float, default=12.0)
    ap.add_argument('--cell', type=float, default=CELL, help='grid cell size (m)')
    ap.add_argument('--only', default=None, help='tile prefix')
    ap.add_argument('--png', default=None, help='write the block grid as PNG')
    ap.add_argument('--refresh', action='store_true', help='ignore the tile cache')
    ap.add_argument('--quiet', action='store_true')
    ap.add_argument('--ways', action='store_true', help='check every .way node')
    ap.add_argument('--compare-cpp', default=None,
                    help='diff against a grid dumped by tools/collision_check.cpp')
    ap.add_argument('--point', action='append', default=[],
                    help='extra probe point x,z[,label]')
    args = ap.parse_args(argv)

    print('loading ENV tiles ...', file=sys.stderr)
    tris, matname, subid, subext, files = flatten_world(args.only, args.quiet,
                                                        args.refresh)
    solid = classify(tris, matname, subid, subext)
    print('tiles=%d triangles=%d submeshes=%d materials=%d blocking=%d (%.1f%%)'
          % (len(files), len(tris), len(subext), len(set(matname.tolist())),
             int(solid.sum()), 100.0 * solid.sum() / max(1, len(tris))),
          file=sys.stderr)

    if args.materials:
        per = {}
        for m, s in zip(matname.tolist(), solid.tolist()):
            e = per.setdefault(m, [0, 0])
            e[0] += 1
            e[1] += int(s)
        print('%-30s %9s %9s' % ('material', 'tris', 'blocking'))
        for m, (t, b) in sorted(per.items(), key=lambda kv: -kv[1][0]):
            print('%-30s %9d %9d %s' % (m, t, b, 'GROUND' if is_ground_material(m) else ''))
        return 0

    g = Grid(args.cell)
    g.build(tris[solid])
    nb = int(g.blocked.sum())
    print('grid %dx%d cell=%.2f  blocked cells=%d (%.2f%% of %.1f km2, %.1f MB at 1 bit)'
          % (g.nx, g.nz, args.cell, nb, 100.0 * nb / g.blocked.size,
             g.nx * g.nz * args.cell * args.cell / 1e6, g.blocked.size / 8e6),
          file=sys.stderr)

    if args.png:
        pts = [(x, z, (255, 0, 0)) for _, x, z, _ in POINTS]
        g.save_png(args.png, points=pts)

    if args.compare_cpp:
        compare_cpp(args.compare_cpp, g)
        return 0

    pts = list(POINTS)
    for p in args.point:
        parts = p.split(',')
        x, z = float(parts[0]), float(parts[1])
        label = parts[2] if len(parts) > 2 else 'point'
        pts.append((label, x, z, ''))

    print()
    print('%-22s %10s %10s   %8s %8s   %s' % ('place', 'x', 'z',
                                              'disc r=3', 'disc r=%.0f' % args.radius,
                                              'blocked(r=0)'))
    for name, x, z, _exp in pts:
        f3, _ = g.fraction_blocked(x, z, 3.0)
        fr, n = g.fraction_blocked(x, z, args.radius)
        print('%-22s %10.1f %10.1f   %7.1f%% %7.1f%%   %-5s  (%d samples)'
              % (name, x, z, 100.0 * f3, 100.0 * fr,
                 g.blocked_circle(x, z, 0.0), n))

    if args.ways:
        nodes = way_nodes()
        print()
        print('-- .way nodes (truck must be able to stand there) --')
        for r in (0.0, 1.0, 1.5, 2.0):
            bad = [n for n in nodes if g.blocked_circle(n[2], n[3], r)]
            print('radius %.1f m: %d / %d nodes blocked' % (r, len(bad), len(nodes)))
        bad0 = [n for n in nodes if g.blocked_circle(n[2], n[3], 0.0)]
        kinds = {}
        for n in bad0:
            k = 'debris' if n[1].startswith('debris') else \
                'car_repair' if 'car_repair' in n[1] else \
                'sto' if n[1].startswith('sto_') else \
                'border/other'
            kinds.setdefault(k, []).append(n)
        print('blocked(r=0) by kind: %s'
              % ', '.join('%s=%d' % (k, len(v)) for k, v in sorted(kinds.items())))
        for n in bad0[:12]:
            print('   %-10s %-34s (%8.1f, %8.1f, h=%.1f)'
                  % (n[0], n[1], n[2], n[3], n[4]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
