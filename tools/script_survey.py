#!/usr/bin/env python3
"""script_survey.py -- inventory every form head across the shipped scripts.

    python3 tools/script_survey.py                 # head -> count table
    python3 tools/script_survey.py --head node     # dump every `node` form with file:line
    python3 tools/script_survey.py --tree FILE     # pretty-print one file
    python3 tools/script_survey.py --heads node,ambush,obstacle

Reads the pre-decrypted copies in decrypted/ (filename = path with '/' -> '_').
"""
from __future__ import annotations

import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse_decrypted, Sexp, walk  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEC = os.path.join(ROOT, 'decrypted')
BASE = os.path.join(ROOT, 'original')


def script_list():
    out = []
    for sub, names in (('', None), ('ENV', None), ('MENV', None), ('MTR', None)):
        d = os.path.join(BASE, sub)
        if not os.path.isdir(d):
            continue
        for n in sorted(os.listdir(d)):
            if n.lower().endswith(('.sch', '.cnf')):
                out.append(os.path.join(sub, n) if sub else n)
    return out


def load(rel):
    return parse_decrypted(DEC, rel)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--head', help='dump all forms with this head')
    ap.add_argument('--heads', help='comma list of heads to dump')
    ap.add_argument('--tree', help='pretty print one script')
    a = ap.parse_args()

    files = script_list()

    if a.tree:
        forms = load(a.tree)
        for f in forms:
            _pp(f)
        return 0

    counts = collections.Counter()
    where = collections.defaultdict(list)
    per_file = {}
    for rel in files:
        try:
            forms = load(rel)
        except Exception as e:                       # pragma: no cover
            print('!! %s: %s' % (rel, e))
            continue
        c = collections.Counter()
        for e in walk(forms):
            c[e.head()] += 1
        per_file[rel] = c
        for k, v in c.items():
            counts[k] += v
            where[k].append((rel, v))

    if a.head:
        heads = [a.head]
    elif a.heads:
        heads = a.heads.split(',')
    else:
        heads = None

    if heads:
        for h in heads:
            print('=' * 100)
            print('HEAD %s  total=%d  files=%s' % (h, counts.get(h, 0), where.get(h)))
            print('=' * 100)
            for rel in files:
                forms = load(rel)
                for e in walk(forms):
                    if e.head() == h:
                        print('--- %s' % rel)
                        _pp(e)
        return 0

    print('scripts: %d' % len(files))
    print('%-24s %6s   %s' % ('head', 'count', 'files (n)'))
    for h, n in counts.most_common():
        print('%-24s %6d   %s' % (h, n, ' '.join('%s(%d)' % (f.split('/')[-1], c)
                                                 for f, c in where[h])))
    print()
    print('%-40s %s' % ('file', 'heads'))
    for rel in files:
        print('%-40s %s' % (rel, dict(per_file[rel].most_common())))
    return 0


def _pp(e, depth=0, maxdepth=99):
    ind = '  ' * depth
    if e.is_atom:
        print(ind + (('"%s"' % e.text) if e.quoted else e.text))
        return
    if depth >= maxdepth:
        print(ind + '( ... %d elems )' % len(e))
        return
    print(ind + '(')
    for k in e:
        _pp(k, depth + 1, maxdepth)
    print(ind + ')')


if __name__ == '__main__':
    sys.exit(main())
