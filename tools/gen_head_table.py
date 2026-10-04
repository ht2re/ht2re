#!/usr/bin/env python3
"""gen_head_table.py -- markdown inventory of every distinct s-expression form head.

    python3 tools/gen_head_table.py > /tmp/heads.md

The head of a list is its first element's text.  In these scripts most heads are real
"forms" (town/node/ambush/...) but in `merchandise.sch` the cargo item names, and in the
`( ... )`-wrapped manifest files the vehicle/model keys, also appear as heads.
"""
from __future__ import annotations

import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse_decrypted, walk                      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEC = os.path.join(ROOT, 'decrypted')
BASE = os.path.join(ROOT, 'original')

# ---- one-line meanings for the structural heads --------------------------------
MEAN = {
    'town': 'town / gas station / fuel depot definition (delivery nodes, goods, map icons)',
    'node': 'a named .way node inside a town/parking, with an optional role tag',
    'nodeevent': 'triggered message when the player reaches a .way node',
    'ambush': 'police/mafia ambush definition (spawn node, direction, route node ids)',
    'obstacle': 'road closure / roadwork: toggles a b3d key node and spawns debris',
    'parking': 'parked-vehicle group: list of .way nodes that carry parked cars',
    'service': 'repair shop / fuel station: available upgrades (+ price id)',
    'people': 'per-tile day/night/weather gate list for animated bystander groups',
    'DAYGW': 'people gate: visible by day AND in good weather, with probability p',
    'DAYONLY': 'people gate: visible only by day, with probability p',
    'UNCONDITIONAL': 'people gate: always visible, with probability p',
    'vehicle': 'vehicle catalogue entry (evehicles.sch / etrashboxes.sch)',
    'playertypes': 'GAME.CNF global counters + the player truck model',
    'live': 'playertypes: an allowed player vehicle model',
    'aitruck': 'playertypes: number of AI trucker rigs to spawn',
    'police': 'playertypes: number of police cars to spawn',
    'mafia': 'playertypes: number of mafia cars to spawn',
    'dumb': 'playertypes: number of "dumb" (simple) traffic cars to spawn',
    'ghost': 'playertypes: number of ghost/non-colliding traffic cars',
    'startposition': 'world spawn point of the player (x z height)',
    'forsale': 'GAME.CNF: trucks offered for sale',
    'forsale_stolen': 'GAME.CNF: stolen trucks offered for sale',
    'forsale_used': 'GAME.CNF: used trucks offered for sale',
    'forhire': 'GAME.CNF: drivers available for hire',
    'missions': 'GAME.CNF: concurrent delivery orders',
    'fuel': 'GAME.CNF: fuel depots / merchandise carrier tag "fuel"',
    'semitrailers': 'GAME.CNF: semitrailers parked in the world',
    'containers': 'GAME.CNF: containers parked in the world',
    'parkings': 'GAME.CNF: parked cars in the world',
    'scenario': 'GAME.CNF (MENV/MTR): the menu scenario name',
    'world': 'GAME.CNF: world name (not present in the shipped ENV file)',
    'trucks': 'merchandise.sch: cargo can be carried by trucks (carrier-class tag; 61 tag occurrences, see spec 5.8)',
    'cars': 'merchandise.sch: cargo can be carried by cars (carrier-class tag)',
    'illegal': 'merchandise.sch: cargo is contraband (police risk)',
    'formafia': 'merchandise.sch: cargo is of interest to the mafia',
    'dummy': 'node role tag: placeholder node (fuel-depot towns)',
    'unload': 'node role tag: the cargo delivery point of a town',
    'REGULAR': 'obstacle kind (only value present in the shipped data)',
    'STATEBORDER': 'nodeevent kind: town/state border crossing',
    'UNCONDITIONAL_EVENT': 'nodeevent kind: always triggers',
    'POLICE': 'ambush class',
    'MAFIA': 'ambush class',
}
for pr in ('PR_FORCED_ENGINE', 'PR_OIL_ADDITIVE', 'PR_ABS', 'PR_ADJUSTABLE_SUSPENSION',
           'PR_BRAKES', 'PR_ENGINE_BRAKE', 'PR_REINFORCED_CHASSIS', 'PR_BULETPROOF_GLASS',
           'PR_NAVIGATOR', 'PR_TIRES', 'PR_CARGO_SCANNER', 'PR_HORN', 'PR_FIRE_PROTECTION',
           'PR_EXTRA_TANK', 'PR_FUEL', 'PR_ANTI_RADAR', 'PR_STEALTH_BODY_PAINT',
           'PR_HEADLIGHT_PROTECTION', 'PR_AIR_FILTER_SNORKEL'):
    MEAN[pr] = 'upgrade item; in mod_*.sch = offered by a service, in restable.sch = price table'


def main():
    files = []
    for sub in ('', 'ENV', 'MENV', 'MTR'):
        d = os.path.join(BASE, sub)
        for n in sorted(os.listdir(d)):
            if n.lower().endswith(('.sch', '.cnf')):
                files.append(os.path.join(sub, n) if sub else n)

    counts = collections.Counter()
    per_file = collections.defaultdict(set)
    for rel in files:
        for e in walk(parse_decrypted(DEC, rel)):
            h = e.head()
            counts[h] += 1
            per_file[h].add(os.path.basename(rel))

    structural, data = [], []
    for h, n in counts.most_common():
        (structural if h in MEAN else data).append((h, n))

    print('distinct heads: **%d** over %d list forms in %d files.\n' %
          (len(counts), sum(counts.values()), len(files)))
    print('### Structural / grammar form heads\n')
    print('| head | count | meaning |')
    print('|------|-------|---------|')
    for h, n in structural:
        print('| `%s` | %d | %s |' % (h, n, MEAN[h]))
    print()
    print('### Data heads (not grammar forms)\n')
    print('These are the first elements of *data* lists.  A walker that calls the first')
    print('element of every list a "form head" reports them too, but none of them is a')
    print('form: they are `merchandise.sch` cargo ids and their per-price-block carrier')
    print('tags, the vehicle/model keys of `vd.sch` and the message keys of `Ms.sch`,')
    print('the numeric ambush-slot ids inside `( id id … )` node lists, the map-icon')
    print('name lists of `town`, and the upgrade-name rows of `restable.sch`.\n')
    print('<details><summary>full list (%d heads)</summary>\n' % len(data))
    print('| head | count | files |')
    print('|------|-------|-------|')
    for h, n in data:
        fl = ','.join(sorted(per_file[h]))
        if len(fl) > 60:
            fl = fl[:57] + '...'
        print('| `%s` | %d | %s |' % (h, n, fl))
    print('\n</details>')


if __name__ == '__main__':
    main()
