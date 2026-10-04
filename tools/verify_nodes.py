#!/usr/bin/env python3
"""verify_nodes.py -- cross-check the names used by ENV/mod_*.sch against the .way files.

For every mod_XX.sch it collects every node-name string used by `node`, `nodeevent`,
`ambush`, `obstacle`, `town`, `parking` and reports:
  * whether the name exists as an RNOD NNAM in ENV/XX.way
  * its decoded POSN (x, z, height) and FLAG
  * the tile b3d room-content bounding box (via tools/b3d_world.py) and whether the node
    lies inside it.
"""
from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse_decrypted, Sexp, walk           # noqa: E402
from b3d_world import read_way, Scene, xf_apply        # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEC = os.path.join(ROOT, 'decrypted')
ENV = os.path.join(ROOT, 'original', 'ENV')

import numpy as np                                      # noqa: E402


def mod_nodes(forms):
    """-> list of (head, name, extra) for every referenced way-node name."""
    refs = []

    def add(h, name, extra=''):
        if name:
            refs.append((h, name, extra))

    for e in walk(forms):
        h = e.head()
        if h == 'node':
            add('node', e.str(1), e.str(2))
        elif h == 'nodeevent':
            add('nodeevent', e.str(3))
        elif h == 'ambush':
            add('ambush', e.str(5))
        elif h == 'obstacle':
            for i in range(5, len(e)):
                if isinstance(e[i], Sexp) and e[i].is_list:
                    for k in e[i]:
                        add('obstacle', k.text)
        elif h in ('town', 'parking'):
            for i in range(len(e)):
                if isinstance(e[i], Sexp) and e[i].is_list:
                    for k in e[i]:
                        if isinstance(k, Sexp) and k.is_list and k.head() == 'node':
                            add(h, k.str(1))
    return refs


def tile_room_bbox(tile):
    p = os.path.join(ENV, tile + '.b3d')
    if not os.path.exists(p):
        return None
    sc = Scene(p)
    v = np.zeros((0, 3))
    for rm in sc.rooms:
        x = sc.verts(rm)
        if len(x):
            v = np.vstack([v, xf_apply(x, sc.anc[id(rm)], sc.convention)])
    return v if len(v) else None


def main():
    tiles = sys.argv[1:]
    if not tiles:
        pre, suf = 'ENV_mod_', '.sch.txt'
        tiles = sorted(n[len(pre):-len(suf)] for n in os.listdir(DEC)
                       if n.startswith(pre) and n.endswith(suf))
    print('%-6s %-8s %-6s %-6s  %-30s %-24s %s' %
          ('tile', 'head', 'n_nod', 'n_used', 'missing names', 'flag hist', 'bbox'))
    for t in tiles:
        ways = os.path.join(ENV, t + '.way')
        if not os.path.exists(ways):
            print('%-6s  (no .way)' % t)
            continue
        mnam, nodes = read_way(ways)
        by = {n['name']: n for n in nodes}
        forms = parse_decrypted(DEC, 'ENV/mod_%s.sch' % t)
        refs = mod_nodes(forms)
        names = []
        for _, n, _ in refs:
            if n not in names:
                names.append(n)
        missing = [n for n in names if n not in by]
        flags = {}
        for n in names:
            if n in by:
                flags[by[n]['flag']] = flags.get(by[n]['flag'], 0) + 1
        bb = tile_room_bbox(t)
        if bb is None:
            bs = '(no b3d)'
        else:
            inside = 0
            outside = []
            for n in names:
                if n not in by:
                    continue
                x, z = by[n]['pos'][0], by[n]['pos'][1]
                if bb[:, 0].min() <= x <= bb[:, 0].max() and bb[:, 1].min() <= z <= bb[:, 1].max():
                    inside += 1
                else:
                    outside.append(n)
            bs = 'x[%.0f %.0f] z[%.0f %.0f] h[%.0f %.0f]  inside=%d/%d%s' % (
                bb[:, 0].min(), bb[:, 0].max(), bb[:, 1].min(), bb[:, 1].max(),
                bb[:, 2].min(), bb[:, 2].max(), inside, len(names),
                ('  OUT:' + ','.join(outside[:4])) if outside else '')
        print('%-6s %-8s %-6d %-6d  %-30s %-24s %s' %
              (t, 'refs', len(nodes), len(names),
               (','.join(missing[:3]) + ('...' if len(missing) > 3 else '')) or '-',
               str(flags) or '-', bs))
        if missing:
            for n in missing:
                print('        MISSING %r' % n)


if __name__ == '__main__':
    main()
