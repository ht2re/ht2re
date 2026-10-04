#!/usr/bin/env python3
"""Aggregate per-edge UV evidence for every vertex format used by the shipped
B3D data.

A mesh record (subtag 37) owns its face records (subtag 8/35) as children, so
for each mesh we can walk the real index lists and look at consecutive vertex
pairs of every polygon.  For a candidate float pair (i,i+1) we measure

  scale = median( |dPos| / |dUV| )      world units per uv unit
  dead  = P(|dPos| > 0.5 and |dUV| < 1e-6)
          (the candidate does not vary along a real edge -> if the material is
           supposed to be textured with this set it would show smeared texels)
  spread = p90/p10 of |dUV|/|dPos|      (1.0 = locally uniform mapping)

Output: per (format, material) aggregate, useful to see which pair each material
family was authored against.

Usage: python3 tools/vfmt_uvsets.py [--min-vc N] [--top N] [b3d ...]
"""
import argparse
import os
import sys
from collections import defaultdict

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import b3d_full as B  # noqa: E402
import b3d_vfmt as V  # noqa: E402

# candidate uv pairs per format, from the king.exe loader (see docs/vertex_formats.md)
PAIRS = {0x0: [(3, 4)], 0x2: [(3, 4)], 0x3: [(3, 4)],
         0x102: [(3, 4), (5, 6)], 0x202: [(3, 4), (5, 6), (7, 8)],
         0x203: [(3, 4), (5, 6)]}


def poly_runs(d, face):
    """yield (material, [vertex indices]) for every polygon of a face record"""
    hdrs = face['polys']
    for k, (w0, mat, off) in enumerate(hdrs):
        cnt = B.u32(d, off + 16)
        ds = off + 20
        if k + 1 < len(hdrs):
            end = hdrs[k + 1][2] + 4
        else:
            end = face.get('end', ds)
            while end - 4 > ds and B.u32(d, end - 4) in (B.T_ESEC, B.T_BSEC):
                end -= 4
        ndw = (end - ds) // 4
        if not cnt or ndw < cnt:
            continue
        s = ndw // cnt if ndw % cnt == 0 else 1
        idx = [B.u32(d, ds + 4 * s * t) for t in range(cnt)]
        yield mat, idx


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='*')
    ap.add_argument('--min-vc', type=int, default=16)
    ap.add_argument('--top', type=int, default=24)
    a = ap.parse_args()
    files = a.files or V.all_b3d()
    # (fmt, material name, pair) -> list of world/uv ratios + dead counts
    stat = defaultdict(lambda: dict(ratios=[], dead=0, n=0, meshes=0))
    fmts = defaultdict(lambda: dict(meshes=0, verts=0, files=set()))
    for f in files:
        d = open(f, 'rb').read()
        try:
            t, ms = V.meshes(f)
        except Exception as e:  # noqa: BLE001
            print('FAIL', f, e, file=sys.stderr)
            continue
        names = t['names']
        for m in ms:
            key = (m['sub'], m['fmt'], m['stride'])
            fmts[key]['meshes'] += 1
            fmts[key]['verts'] += m['vc']
            fmts[key]['files'].add(os.path.basename(f))
            if m['fmt'] not in PAIRS or m['vc'] < a.min_vc:
                continue
            arr = V.verts(d, m)
            kids = [k for k in m['node'].get('children', ()) if k.get('sub') in (8, 35)]
            for face in kids:
                for mat, idx in poly_runs(d, face):
                    nm = names[mat] if mat < len(names) else '#%d' % mat
                    for t2 in range(len(idx)):
                        i, j = idx[t2], idx[(t2 + 1) % len(idx)]
                        if i >= m['vc'] or j >= m['vc']:
                            continue
                        dp = float(np.linalg.norm(arr[i, 0:3] - arr[j, 0:3]))
                        for (iu, iv) in PAIRS[m['fmt']]:
                            du = float(np.hypot(arr[i, iu] - arr[j, iu],
                                                arr[i, iv] - arr[j, iv]))
                            s = stat[(m['fmt'], nm, (iu, iv))]
                            s['n'] += 1
                            if dp > 0.5 and du < 1e-6:
                                s['dead'] += 1
                            elif du > 1e-6 and dp > 1e-6:
                                s['ratios'].append(dp / du)

    print('=== vertex format census (sub, fmt, stride) -> meshes / verts / files')
    for k in sorted(fmts):
        e = fmts[k]
        print('  sub=%-3d fmt=0x%-4x stride=%-3s meshes=%-6d verts=%-7d files=%d' %
              (k[0], k[1], k[2], e['meshes'], e['verts'], len(e['files'])))
    print()
    print('=== per (format, material, candidate pair) edge statistics')
    rows = sorted(stat.items(), key=lambda kv: -kv[1]['n'])[:a.top]
    print('  %-6s %-26s %-9s %8s %8s %8s %8s' %
          ('fmt', 'material', 'pair', 'edges', 'median', 'dead', 'spread'))
    for (fmt, nm, pair), s in rows:
        v = np.array(s['ratios']) if s['ratios'] else np.array([float('nan')])
        spread = (np.percentile(v, 90) / max(1e-9, np.percentile(v, 10))) if len(v) else float('nan')
        print('  0x%-4x %-26s %-9s %8d %8.3f %8.3f %8.1f' %
              (fmt, nm[:26], '(%d,%d)' % pair, s['n'], float(np.median(v)),
               s['dead'] / max(1, s['n']), spread))


if __name__ == '__main__':
    main()
