#!/usr/bin/env python3
"""economy_check.py -- reproduce src/game/Economy.cpp's dealer and job generation
from the shipped scripts, independently of the C++ code, and report the numbers.

It answers, for the shipped data:
  * how many vehicles a dealer offers and the price range (evehicles.sch)
  * how many cargo jobs exist and how many of them resolved to real .way nodes
    (merchandise.sch + the `town` records of ENV/mod_*.sch + ENV/<tile>.way POSN)
  * the reward range and a few example jobs
  * how many offers the documented start money (45000) buys

Everything is read from the original encrypted data:
    tools/decrypt_sch.py    SDTE cipher
    tools/sexp.py           s-expression reader
    tools/economy_check.py  this file: .way POSN reader + the economy rules

Usage:  python3 tools/economy_check.py [--data original] [-v]
Exit code 0 when every job resolved and the catalogue is non-empty.

The reader is tools/sexp.py, which now handles both quirks this file used to
work around (escapes inside strings, and `()` not being an atom).  One data
defect stays explicit: merchandise.sch writes C_WAGES' sell price as
`1#e+006`; `#` is the exponent marker, so the value is 1000000.
"""
from __future__ import annotations

import argparse
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from decrypt_sch import decrypt                      # noqa: E402
import sexp                                          # noqa: E402
from sexp import Sexp, walk                          # noqa: E402

# ---- the rules of src/game/Economy.{h,cpp} (keep in sync) --------------------
START_MONEY = 45000
CARGO_VALUE_FLOOR = 500
REWARD_VALUE_FRAC = 0.20
REWARD_MIN = 200
REWARD_CAP = 250000
DEALER_ROLE = 4          # parking bay role of the trade stall (names the town)
UNLOAD_ROLE = "unload"   # town node role of the warehouse


# ---- readers -----------------------------------------------------------------
def parse_script(path):
    raw = open(path, 'rb').read()
    if raw[:4] == b'SDTE':
        raw = decrypt(raw)
    return sexp.parse(raw.decode('cp1251'))


def num(node, i, default=0):
    """Lenient number: `#` terminates the mantissa (C_WAGES `1#e+006` -> 1e6)."""
    if node is None or i < 0 or i >= len(node):
        return default
    k = node[i]
    if not isinstance(k, Sexp) or not k.is_atom or k.quoted:
        return default
    token = k.text
    if '#' in token:
        if token.count('#') != 1:
            return default
        pos = token.index('#')
        token = token[:pos] + ('' if token[pos + 1:pos + 2] in ('e', 'E') else 'e') + token[pos + 1:]
    if not token or not set(token) <= set('0123456789+-.eE'):
        return default
    try:
        value = float(token)
        return value if math.isfinite(value) else default
    except ValueError:
        return default


def read_way(path):
    """Minimal WTWR reader: RNOD/NNAM + POSN (+FLAG), i.e. the named nodes."""
    d = open(path, 'rb').read()
    if d[:4] != b'WTWR':
        return {}
    pad = lambda n: (n + 3) & ~3
    out = {}
    u32 = lambda b, o: struct.unpack_from('<I', b, o)[0]

    def rec(off, end):
        i = off
        while i + 8 <= end:
            tag, size = d[i:i + 4], u32(d, i + 4)
            if tag == b'RNOD':
                name, pos, flag = '', None, 0
                j = i + 8
                while j + 8 <= i + 8 + pad(size):
                    t2, s2 = d[j:j + 4], u32(d, j + 4)
                    pay = d[j + 8:j + 8 + s2]
                    if t2 == b'NNAM':
                        name = pay.split(b'\x00')[0].decode('latin1')
                    elif t2 == b'POSN' and s2 >= 24:
                        pos = struct.unpack('<3d', pay[:24])
                    elif t2 == b'FLAG' and s2 >= 4:
                        flag = u32(pay, 0)
                    j += 8 + pad(s2)
                if name and pos:
                    out[name] = {'pos': pos, 'flag': flag, 'tile': d[8:12].decode('latin1')}
            elif tag in (b'GDAT', b'GROM'):
                rec(i + 8, i + 8 + size)
            i += 8 + pad(size)

    rec(8, len(d))
    return out


# ---- the shipped tables ------------------------------------------------------
def load_cargo(path):
    cargo = {}
    forms = parse_script(path)
    for top in forms:
        for e in top:
            if not isinstance(e, Sexp) or not e.is_list or len(e) < 5:
                continue
            if not e[0].is_atom or not e[0].text:
                continue
            pb = e[4]
            if not isinstance(pb, Sexp) or not pb.is_list:
                continue
            tags = []
            for k in pb[3:]:
                if isinstance(k, Sexp) and k.is_list:
                    tags = [t.text for t in k]
            cargo[e.str(0)] = {
                'id': e.str(0), 'unit': e.str(1), 'display': e.str(len(e) - 1),
                'weight': num(e, 2), 'volume': num(e, 3),
                'buy': int(num(pb, 0)), 'sell': int(num(pb, 1)),
                'tags': tags,
            }
    return cargo


def load_vehicles(path):
    veh = []
    for e in walk(parse_script(path)):
        if e.head() != 'vehicle' or len(e) < 8:
            continue
        veh.append({
            'id': e.str(1), 'model': e.str(2), 'variant': int(num(e, 3)),
            'class': e.str(4), 'ai': e.str(5),
            'price': int(num(e, 6)), 'field7': int(num(e, 7)),
        })
    return veh


def load_vd_names(path):
    names = {}
    for f in parse_script(path):
        items = [f] + [k for k in f if isinstance(k, Sexp)]
        for p in items:
            if isinstance(p, Sexp) and p.is_list and len(p) >= 3 and p.str(1) == '.':
                names[p.str(0)] = p.str(2)
    return names


def load_counters(path):
    counters = {}
    for f in parse_script(path):
        for e in [f] + [k for k in f if isinstance(k, Sexp)]:
            if isinstance(e, Sexp) and e.is_list and e.head() and e.is_number(1):
                counters[e.head()] = int(num(e, 1))
    return counters


def load_towns(mod_paths):
    towns = []
    for path in mod_paths:
        for e in walk(parse_script(path)):
            if e.head() != 'town':
                continue
            nodes = []
            for grp in e:
                if isinstance(grp, Sexp) and grp.is_list:
                    for k in grp:
                        if isinstance(k, Sexp) and k.is_list and k.head() == 'node':
                            nodes.append((k.str(1), k.str(2)))
            goods, seen = [], set()
            for grp in e:
                if not (isinstance(grp, Sexp) and grp.is_list and len(grp) >= 2):
                    continue
                pairs = [k for k in grp if isinstance(k, Sexp) and k.is_list and len(k) == 2]
                if len(pairs) != len(grp) or not all(k.is_number(1) for k in pairs):
                    continue
                for k in pairs:
                    gid = k.str(0)
                    if not gid or gid.lower() in seen:
                        continue
                    seen.add(gid.lower())
                    goods.append(gid)
            unload = next((n for n, r in nodes if r == UNLOAD_ROLE), '')
            towns.append({'ident': e.str(1), 'display': e.str(2) or e.str(1),
                          'id': int(num(e, 3)), 'nodes': nodes,
                          'unload': unload, 'goods': goods, 'tile': os.path.basename(path)[4:6]})
    return towns


def load_dealers(mod_paths, nodes):
    dealers = []
    for path in mod_paths:
        tile = os.path.basename(path)[4:6]
        for e in walk(parse_script(path)):
            if e.head() != 'parking':
                continue
            bays = []
            for grp in e:
                if isinstance(grp, Sexp) and grp.is_list:
                    for k in grp:
                        if isinstance(k, Sexp) and k.is_list and k.head() == 'node':
                            bays.append((k.str(1), int(num(k, 2)), k.str(3)))
            if int(num(e, 3)) < 0 or not bays:
                continue
            stall = next((b for b in bays if b[1] == DEALER_ROLE), bays[0])
            dealers.append({'id': e.str(1), 'display': stall[2] or e.str(2),
                            'tile': tile, 'node': stall[0],
                            'pos': nodes.get(stall[0], {}).get('pos')})
    return dealers


def find_cargo(cargo, cid):
    """Exact match, then case-insensitive: TOWN_AD's goods list spells the car
    version of tableware `c_TABLEWARE` while merchandise.sch has `C_TABLEWARE`."""
    if cid in cargo:
        return cargo[cid]
    lo = cid.lower()
    for k, v in cargo.items():
        if k.lower() == lo:
            return v
    return None


def reward(cargo, dist_m):
    value = max(cargo['buy'], cargo['sell'], CARGO_VALUE_FLOOR)
    r = round(REWARD_VALUE_FRAC * value * (1.0 + dist_m / 1000.0))
    return max(REWARD_MIN, min(REWARD_CAP, r))


# ---- report ------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', default='original')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--examples', type=int, default=8)
    args = ap.parse_args()
    data = args.data
    env = os.path.join(data, 'ENV')

    mod_paths = sorted(os.path.join(env, f) for f in os.listdir(env)
                       if f.startswith('mod_') and f.endswith('.sch'))
    nodes = {}
    for f in sorted(os.listdir(env)):
        if f.endswith('.way'):
            nodes.update(read_way(os.path.join(env, f)))

    cargo = load_cargo(os.path.join(data, 'merchandise.sch'))
    wages = cargo.get('C_WAGES', {})
    if (len(cargo) != 107 or wages.get('sell') != 1000000 or
            wages.get('buy') != 0 or wages.get('tags') != ['cars', 'formafia']):
        print('FAIL: cargo count / C_WAGES million-unit price / carrier tags', file=sys.stderr)
        return 1
    print('ok: 107 cargo records; C_WAGES sell=1000000, tags=cars,formafia')
    vehicles = load_vehicles(os.path.join(data, 'evehicles.sch'))
    vd = load_vd_names(os.path.join(data, 'vd.sch'))
    counters = load_counters(os.path.join(env, 'GAME.CNF'))
    towns = load_towns(mod_paths)
    dealers = load_dealers(mod_paths, nodes)

    # ---- dealer stock --------------------------------------------------------
    offers = [v for v in vehicles if v['price'] > 0]
    offers.sort(key=lambda v: (v['price'], v['id']))
    skipped = [v for v in vehicles if v['price'] <= 0]
    prices = [v['price'] for v in offers]
    affordable = [v for v in offers if v['price'] <= START_MONEY]

    print('scripts   : %d mod_*.sch, %d vehicles, %d cargo, %d named .way nodes'
          % (len(mod_paths), len(vehicles), len(cargo), len(nodes)))
    print('counters  : ' + ' '.join('%s=%s' % (k, counters[k]) for k in
                                    ('forsale', 'forhire', 'missions', 'fuel') if k in counters))
    print('offers    : %d buyable of %d vehicle defs (%d not for sale, price 0)'
          % (len(offers), len(vehicles), len(skipped)))
    print('prices    : %d..%d  median %d  mean %d  (start money %d buys %d/%d = %.0f%%)'
          % (min(prices), max(prices), prices[len(prices) // 2], sum(prices) // len(prices),
             START_MONEY, len(affordable), len(offers), 100.0 * len(affordable) / len(offers)))
    print('examples  : cheapest %s "%s" %d | dearest %s "%s" %d'
          % (offers[0]['id'], vd.get(offers[0]['id'], '?'), offers[0]['price'],
             offers[-1]['id'], vd.get(offers[-1]['id'], '?'), offers[-1]['price']))
    if args.verbose:
        for v in offers[:6] + offers[-6:]:
            print('   offer %-24s %-16s %-8s v%d %7d  %s'
                  % (v['id'], v['model'], v['class'], v['variant'], v['price'],
                     vd.get(v['id'], '')))

    # ---- dealers -------------------------------------------------------------
    bad_dealers = [d for d in dealers if not d['pos']]
    start = (-3020.0, -4625.0)   # ENV/GAME.CNF startposition -3020 -4625 20
    nearest = min((d for d in dealers if d['pos']),
                  key=lambda d: math.dist(start, (d['pos'][0], d['pos'][1])), default=None)
    print('dealers   : %d bazaar trade stalls (parking bay role %d), %d unresolved'
          % (len(dealers), DEALER_ROLE, len(bad_dealers)))
    for d in dealers:
        if args.verbose:
            p = d['pos']
            print('   dealer %-12s %-12s tile=%s node=%-16s %s'
                  % (d['id'], d['display'], d['tile'], d['node'],
                     '(%.0f, %.0f)' % (p[0], p[1]) if p else 'UNRESOLVED'))
    if nearest:
        print('          nearest the start: %s node=%s (%.0f, %.0f)'
              % (nearest['id'], nearest['node'], nearest['pos'][0], nearest['pos'][1]))

    # ---- jobs ----------------------------------------------------------------
    markets, skipped_towns = [], []
    for t in towns:
        if t['unload'] and t['goods'] and t['unload'] in nodes:
            markets.append(t)
        else:
            skipped_towns.append(t)
    unresolved_cargo = sorted({g for t in markets for g in t['goods'] if not find_cargo(cargo, g)})

    jobs, unresolved_jobs = [], []
    for a in markets:
        for gid in a['goods']:
            c = find_cargo(cargo, gid)
            if not c:
                continue
            for b in markets:
                if b['ident'] == a['ident']:
                    continue
                if a['unload'] not in nodes or b['unload'] not in nodes:
                    unresolved_jobs.append((gid, a['ident'], b['ident']))
                    continue
                pa, pb = nodes[a['unload']]['pos'], nodes[b['unload']]['pos']
                dist = math.dist((pa[0], pa[1]), (pb[0], pb[1]))   # POSN = (x, z, h)
                jobs.append({
                    'cargo': gid, 'name': c['display'], 'from': a['display'],
                    'to': b['display'], 'from_node': a['unload'], 'to_node': b['unload'],
                    'dist': dist, 'reward': reward(c, dist),
                    'value': max(c['buy'], c['sell'], CARGO_VALUE_FLOOR),
                })

    rewards = [j['reward'] for j in jobs]
    print('towns     : %d markets of %d `town` records (%d skipped: petrol stations / '
          'fuel depots / no node)' % (len(markets), len(towns), len(skipped_towns)))
    print('jobs      : %d  (%d pickups x %d destinations), %d unresolved' %
          (len(jobs), sum(len(t['goods']) for t in markets), max(len(markets) - 1, 0),
           len(unresolved_jobs)))
    print('resolved  : %d/%d jobs use a real .way node for pickup and drop-off'
          % (len(jobs), len(jobs) + len(unresolved_jobs)))
    if unresolved_jobs or bad_dealers or unresolved_cargo:
        print('UNRESOLVED: cargo ids %s | town nodes %s | dealer nodes %s'
              % (unresolved_cargo, unresolved_jobs[:5], [d['node'] for d in bad_dealers]))
    print('rewards   : %d..%d  median %d' % (min(rewards), max(rewards), sorted(rewards)[len(rewards) // 2]))

    print('example jobs (nearest pickup to the start position):')
    here = sorted((j for j in jobs
                   if math.dist(start, nodes[j['from_node']]['pos'][:2]) < 250),
                  key=lambda j: j['dist'])
    for j in here[:args.examples]:
        print('   %-16s %-12s -> %-12s %7.0f m  value %-8d reward %-8d  %s -> %s'
              % (j['cargo'], j['from'], j['to'], j['dist'], j['value'], j['reward'],
                 j['from_node'], j['to_node']))
    print('dearest jobs:')
    for j in sorted(jobs, key=lambda j: -j['reward'])[:4]:
        print('   %-16s %-12s -> %-12s %7.0f m  value %-8d reward %-8d%s'
              % (j['cargo'], j['from'], j['to'], j['dist'], j['value'], j['reward'],
                 '  (cap)' if j['reward'] == REWARD_CAP else ''))

    # ---- expectations the C++ module must match ------------------------------
    expect = {
        'offers': len(offers),
        'price_min': min(prices),
        'price_max': max(prices),
        'jobs': len(jobs),
        'jobs_unresolved': len(unresolved_jobs),
        'dealers': len(dealers),
        'dealers_unresolved': len(bad_dealers),
        'towns': len(markets),
        'cargo': len(cargo),
        'player': 'ZilR',
    }
    print('EXPECT    : ' + ' '.join('%s=%s' % kv for kv in expect.items()))

    ok = bool(offers) and bool(jobs) and not unresolved_jobs and not bad_dealers
    ok = ok and not unresolved_cargo and len(markets) == 11 and len(cargo) == 107
    print('RESULT    : %s' % ('ok' if ok else 'FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
