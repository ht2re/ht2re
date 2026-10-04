#!/usr/bin/env python3
"""Dump B3D mesh vertex samples and the face-polygon entry dwords that index them.

Usage:
  python3 tools/b3d_vdump.py NAME [--fmt F] [--nv 12] [--np 4] [--file F]
"""
import argparse
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import b3d_full as B  # noqa: E402
import b3d_vfmt as V  # noqa: E402


def face_nodes(r, out, path=''):
    nm = path + '/' + r.get('name', '?')
    if r.get('sub') in (8, 35):
        out.append((nm, r))
    for c in r.get('children', ()):
        face_nodes(c, out, nm)


def dump_polys(d, r, maxpoly=4):
    """print raw dwords of the first polygons of a face record"""
    q = r['polys'][0][2] if r.get('polys') else None
    for i, (mat, x, off) in enumerate(r['polys'][:maxpoly]):
        cnt = B.u32(d, off + 16)
        # data runs from off+20 to the next poly header / closer
        if i + 1 < len(r['polys']):
            end = r['polys'][i + 1][2] + 4
        else:
            end = r.get('end', off)
            # trim closers
            while end - 4 > off and B.u32(d, end - 4) in (B.T_ESEC, B.T_BSEC):
                end -= 4
        dstart = off + 20
        ndw = (end - dstart) // 4
        print('    poly %d mat=%d cnt=%d dataDwords=%d stride=%s' %
              (i, mat, cnt, ndw, (ndw / cnt) if cnt else '?'))
        for k in range(min(ndw, 24)):
            print('       [%2d] 0x%08x  %12.5f' % (k, B.u32(d, dstart + 4 * k),
                                                   struct.unpack_from('<f', d, dstart + 4 * k)[0]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('--fmt', type=lambda s: int(s, 0), default=None)
    ap.add_argument('--nv', type=int, default=12)
    ap.add_argument('--np', type=int, default=4)
    ap.add_argument('--file', default=None)
    a = ap.parse_args()
    files = [a.file] if a.file else V.all_b3d()
    for f in files:
        d = open(f, 'rb').read()
        try:
            t, ms = V.meshes(f)
        except Exception as e:  # noqa: BLE001
            print('FAIL', f, e, file=sys.stderr)
            continue
        for m in ms:
            if a.name.lower() not in m['name'].lower():
                continue
            if a.fmt is not None and m['fmt'] != a.fmt:
                continue
            print('### %s\n    %s  sub=%d fmt=0x%x vc=%d stride=%d voff=0x%x' %
                  (f, m['name'], m['sub'], m['fmt'], m['vc'], m['stride'], m['voff']))
            arr = V.verts(d, m)
            for r in arr[:a.nv]:
                print('    v ' + ' '.join('%10.5f' % x for x in r))
            fn = []
            for r in t['top']:
                face_nodes(r, fn)
            for nm, r in fn:
                if nm.split('/')[-1] == m['name'].split('/')[-1] or nm == m['name']:
                    print('    face %s npoly=%d' % (nm, r.get('npoly', -1)))
                    dump_polys(d, r, a.np)
            print('    all faces in file: %d, names: %s' %
                  (len(fn), [n for n, _ in fn][:8]))


if __name__ == '__main__':
    main()
