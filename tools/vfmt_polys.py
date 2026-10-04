#!/usr/bin/env python3
"""Polygon-payload stride evidence for B3D face records (subtag 8/35).

A polygon record is `{ flags, 1.0f, 0x7fff, material, cnt, payload }` and the
payload holds `cnt` interleaved per-corner entries.  Depending on the polygon
the entry stride is 1, 2, 3, 4 or 6 dwords.  Because the record boundary is
found by scanning for the next `{1.0f, 0x7fff}` pair, the dword count measured
between two headers is `cnt*s + 1` for every polygon except the last one of a
face (there it is exactly `cnt*s`).

This tool prints

  * the stride histogram for the corrected rule and for the current
    `src/world/B3D.cpp` rule (`s = dw/cnt` only when `dw % cnt == 0`), and
  * how many polygons each rule decodes with an out-of-range vertex index
    (the corrected rule must be 0 over the whole shipped data set).

See docs/vertex_formats.md §7.

Usage: python3 tools/vfmt_polys.py [b3d ...]
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import b3d_full as B  # noqa: E402
import b3d_vfmt as V  # noqa: E402


def stride_new(dw, cnt):
    if cnt <= 0:
        return 1
    if (dw - 1) % cnt == 0 and (dw - 1) // cnt >= 1:
        return (dw - 1) // cnt
    if dw % cnt == 0 and dw // cnt >= 1:
        return dw // cnt
    return 1


def stride_cur(dw, cnt):
    if cnt and dw % cnt == 0 and dw // cnt >= 1:
        return dw // cnt
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('files', nargs='*')
    a = ap.parse_args()
    files = a.files or V.all_b3d()
    hist_new = collections.Counter()
    hist_cur = collections.Counter()
    stats = dict(total=0, bad_new=0, bad_cur=0)
    bad_mats = collections.Counter()
    strided_mats = collections.Counter()
    for f in files:
        d = open(f, 'rb').read()
        try:
            t, ms = V.meshes(f)
        except Exception as e:  # noqa: BLE001
            print('FAIL', f, e, file=sys.stderr)
            continue
        nm = t['names']
        cur = [None]

        def walk(r):
            if r.get('sub') in (7, 37) and r.get('vc', 0) > 0:
                cur[0] = r
            if r.get('sub') in (8, 35):
                hdrs = r['polys']
                for k, (w0, mat, off) in enumerate(hdrs):
                    cnt = B.u32(d, off + 16)
                    ds = off + 20
                    if k + 1 < len(hdrs):
                        end = hdrs[k + 1][2] + 4
                    else:
                        end = r.get('end', ds)
                        while end - 4 > ds and B.u32(d, end - 4) in (B.T_ESEC, B.T_BSEC):
                            end -= 4
                    dw = (end - ds) // 4
                    mesh = cur[0]
                    if not cnt or mesh is None:
                        continue
                    vc = mesh['vc']
                    name = nm[mat] if mat < len(nm) else '?'
                    s = stride_new(dw, cnt)
                    hist_new[s] += 1
                    if s > 1:
                        strided_mats[(name, mesh['fmt'], s)] += 1
                    if any(B.u32(d, ds + 4 * s * i) >= vc for i in range(cnt)):
                        stats['bad_new'] += 1
                    s2 = stride_cur(dw, cnt)
                    hist_cur[s2] += 1
                    if any(B.u32(d, ds + 4 * s2 * i) >= vc for i in range(cnt)):
                        stats['bad_cur'] += 1
                        bad_mats[(name, mesh['fmt'], dw, cnt)] += 1
                    stats['total'] += 1
            for c in r.get('children', ()):
                walk(c)

        for r in t['top']:
            walk(r)
    total, bad_new, bad_cur = stats['total'], stats['bad_new'], stats['bad_cur']
    print('polygons inspected:', total)
    print('stride histogram, corrected rule :', dict(sorted(hist_new.items())))
    print('stride histogram, current C++ rule:', dict(sorted(hist_cur.items())))
    print('polygons with out-of-range index, corrected rule :', bad_new)
    print('polygons with out-of-range index, current C++ rule:', bad_cur)
    if bad_mats:
        print('\ncurrent-rule failures (material, fmt, dw, cnt):')
        for k, v in bad_mats.most_common(10):
            print('   %-24s fmt=0x%-4x dw=%-3d cnt=%d -> %d' % (k[0][:24], k[1], k[2], k[3], v))
    if strided_mats:
        print('\nmaterials on stride>1 polygons (material, fmt, stride):')
        for k, v in strided_mats.most_common(15):
            print('   %-24s fmt=0x%-4x stride=%d -> %d' % (k[0][:24], k[1], k[2], v))


if __name__ == '__main__':
    main()
