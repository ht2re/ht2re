#!/usr/bin/env python3
"""Vertex-format census + per-slot statistics for B3D meshes (subtag 7/37).

Usage:
  python3 tools/b3d_vfmt.py census  [files...]     # fmt -> mesh/vertex counts
  python3 tools/b3d_vfmt.py slots   [files...]     # per-float-slot statistics per fmt
  python3 tools/b3d_vfmt.py dump FMT [n] [files]   # sample vertices of one fmt
  python3 tools/b3d_vfmt.py mesh NAME              # dump one mesh by name match

Files default to every original/**/*.b3d.
"""
import glob
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import b3d_full as B  # noqa: E402


def all_b3d():
    return sorted(glob.glob('original/**/*.b3d', recursive=True))


def walk(r, out, path=''):
    nm = path + '/' + r.get('name', '?')
    if r.get('sub') in (7, 37):
        out.append(dict(file=None, name=nm, sub=r['sub'], fmt=r.get('fmt', 0),
                        vc=r['vc'], stride=r.get('stride'), voff=r['voff'], node=r))
    for c in r.get('children', ()):
        walk(c, out, nm)


def meshes(path):
    t = B.parse_b3d(path)
    out = []
    for r in t['top']:
        walk(r, out)
    d = open(path, 'rb').read()
    for m in out:
        m['file'] = path
        m['names'] = t['names']
        if not m.get('stride'):
            m['stride'] = B.NFM.get(m['fmt']) or B._stride_of(d, m['voff'], m['vc'])
    return t, out


def verts(d, m):
    n = m['stride'] * m['vc']
    a = np.frombuffer(d, dtype='<f4', count=n, offset=m['voff'])
    return a.reshape(m['vc'], m['stride']).astype(np.float64)


def collect(files):
    for f in files:
        d = open(f, 'rb').read()
        try:
            t, ms = meshes(f)
        except Exception as e:  # noqa: BLE001
            print('%-28s FAIL %s' % (f, e), file=sys.stderr)
            continue
        for m in ms:
            if not m.get('stride'):
                m['stride'] = B.NFM.get(m['fmt']) or B._stride_of(d, m['voff'], m['vc'])
            m['data'] = verts(d, m)
        yield f, t, ms


def cmd_census(files):
    tot = {}
    for f, t, ms in collect(files):
        for m in ms:
            k = (m['sub'], m['fmt'], m['stride'])
            e = tot.setdefault(k, dict(meshes=0, verts=0, files=set(), ex=[]))
            e['meshes'] += 1
            e['verts'] += m['vc']
            e['files'].add(os.path.basename(f))
            if len(e['ex']) < 3:
                e['ex'].append('%s:%s(%dv)' % (os.path.basename(f), m['name'].strip('/'), m['vc']))
    print('%-4s %-7s %-6s %8s %10s  %s' % ('sub', 'fmt', 'stride', 'meshes', 'verts', 'examples'))
    for (sub, fmt, st) in sorted(tot):
        e = tot[(sub, fmt, st)]
        print('%-4d 0x%-5x %-6s %8d %10d  %s (%d files)' %
              (sub, fmt, st, e['meshes'], e['verts'], ', '.join(e['ex']), len(e['files'])))


def slot_stats(a):
    """a: (N,S) float64 -> per-slot dict of statistics."""
    if a.shape[0] == 0:
        return []
    n, s = a.shape
    res = []
    for i in range(s):
        v = a[:, i]
        unit = np.abs(v) <= 1.0001
        res.append(dict(mean=np.mean(v), sd=np.std(v), amin=np.min(v), amax=np.max(v),
                        frac_abs_le1=np.mean(unit),
                        frac01=np.mean((v >= -0.001) & (v <= 1.001)),
                        fracint=np.mean(np.abs(v - np.round(v)) < 1e-4)))
    return res


def unit_triple_frac(a, i):
    """fraction of rows where floats i..i+2 form a unit-length (or zero) vector."""
    n = a.shape[0]
    if i + 2 >= a.shape[1]:
        return 0.0
    v = a[:, i:i + 3]
    l = np.linalg.norm(v, axis=1)
    return float(np.mean((np.abs(l - 1.0) < 1e-3) | (l < 1e-6)))


def cmd_slots(files):
    buckets = {}
    for f, t, ms in collect(files):
        for m in ms:
            buckets.setdefault((m['sub'], m['fmt'], m['stride']), []).append(m['data'])
    for k in sorted(buckets):
        if not buckets[k]:
            continue
        a = np.vstack(buckets[k])
        print('=== sub %d fmt 0x%x stride %d  meshes=%d verts=%d' %
              (k[0], k[1], k[2], len(buckets[k]), a.shape[0]))
        st = slot_stats(a)
        print('  %-4s %10s %10s %10s %10s %8s %8s %8s' %
              ('slot', 'mean', 'sd', 'min', 'max', '|x|<=1', 'in01', 'int'))
        for i, s in enumerate(st):
            print('  %-4d %10.4f %10.4f %10.4f %10.4f %8.3f %8.3f %8.3f' %
                  (i, s['mean'], s['sd'], s['amin'], s['amax'],
                   s['frac_abs_le1'], s['frac01'], s['fracint']))
        print('  unit-length triple at offset: ' +
              ' '.join('%d:%.3f' % (i, unit_triple_frac(a, i)) for i in range(a.shape[1] - 2)))
        # consecutive-float unit pair candidates (uv-ish): 0..1 range pairs
        print('  pair frac in [0,1]^2: ' + ' '.join(
            '%d:%.3f' % (i, float(np.mean((a[:, i] >= -0.001) & (a[:, i] <= 1.001) &
                                          (a[:, i + 1] >= -0.001) & (a[:, i + 1] <= 1.001))))
            for i in range(a.shape[1] - 1)))
        print()


def cmd_dump(fmt, count, files):
    sfmt = int(fmt, 0)
    shown = 0
    for f, t, ms in collect(files):
        for m in ms:
            if m['fmt'] != sfmt:
                continue
            print('--- %s  %s  vc=%d stride=%d' % (f, m['name'], m['vc'], m['stride']))
            for r in m['data'][:count]:
                print('   ' + ' '.join('%9.4f' % x for x in r))
            shown += 1
            if shown >= 6:
                return


def cmd_mesh(name):
    for f in all_b3d():
        d = open(f, 'rb').read()
        try:
            t, ms = meshes(f)
        except Exception:  # noqa: BLE001
            continue
        for m in ms:
            if name.lower() in m['name'].lower():
                print('%s %s sub=%d fmt=0x%x vc=%d stride=%d' %
                      (f, m['name'], m['sub'], m['fmt'], m['vc'], m['stride']))
                for r in verts(d, m)[:20]:
                    print('   ' + ' '.join('%9.4f' % x for x in r))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'census'
    rest = sys.argv[2:]
    fl = [a for a in rest if a.endswith('.b3d') or a.endswith('.B3D')] or all_b3d()
    if cmd == 'census':
        cmd_census(fl)
    elif cmd == 'slots':
        cmd_slots(fl)
    elif cmd == 'dump':
        cmd_dump(rest[0], int(rest[1]) if len(rest) > 1 and rest[1].isdigit() else 8, fl)
    elif cmd == 'mesh':
        cmd_mesh(rest[0])
    else:
        print(__doc__)
