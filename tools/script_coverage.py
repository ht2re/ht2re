#!/usr/bin/env python3
"""script_coverage.py -- what the shipped game scripts contain, and what the
engine actually reads.

The complaint this file answers is "many conditions in the scripts are not
executed".  It enumerates, over all 46 `.sch`/`.CNF` files of `original/`:

  * every distinct **form head** (first element of a list) with its count;
  * for every *grammar* form (see FORM_HEADS) every **field position** with the
    set of token types observed there (string / number / symbol / list);
  * whether the engine consumes that field, and where.

The consumption map mirrors `src/script/GameData.cpp` (the loader) plus the
callers that read GameData from outside: `src/game/Economy.cpp`,
`src/app/App.cpp`, `src/ui/EscMenu.cpp`.  A field marked `DROPPED` is parsed by
nobody - that is the concrete list of script information the engine ignores.

Reproduce:
    python3 tools/script_coverage.py            # markdown table
    python3 tools/script_coverage.py --summary  # one line, for tests
    python3 tools/script_coverage.py --json     # machine readable
"""
from __future__ import annotations

import argparse
import collections
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from decrypt_sch import decrypt                                   # noqa: E402

ROOT = os.path.dirname(HERE)

# ---------------------------------------------------------------------------
# reader (escape aware; tools/sexp.py stops the string at the first quote, which
# splits merchandise.sch's "Телевизор 14\"" and Ms.sch's \n)
# ---------------------------------------------------------------------------


class Node:
    __slots__ = ("kids", "text", "quoted")

    def __init__(self, kids=None, text="", quoted=False):
        self.kids = kids
        self.text = text
        self.quoted = quoted

    @property
    def is_list(self):
        return self.kids is not None

    def head(self):
        if not self.is_list or not self.kids or self.kids[0].is_list:
            return ""
        return self.kids[0].text

    def kind(self):
        if self.is_list:
            return "list"
        if self.quoted:
            return "string"
        try:
            float(self.text)
            return "number"
        except ValueError:
            return "symbol"


def tokenize(text):
    toks, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n\f\v":
            i += 1
        elif c == ";":
            while i < n and text[i] != "\n":
                i += 1
        elif c in "()":
            toks.append(c)
            i += 1
        elif c == '"':
            i += 1
            buf = []
            while i < n and text[i] != '"':
                if text[i] == "\\" and i + 1 < n:
                    i += 1
                    buf.append({"n": "\n", "t": "\t", "r": "\r"}.get(text[i], text[i]))
                else:
                    buf.append(text[i])
                i += 1
            i += 1
            toks.append(("s", "".join(buf)))
        else:
            j = i
            while j < n and text[j] not in " \t\r\n\f\v();":
                j += 1
            toks.append(("a", text[i:j]))
            i = j
    return toks


def parse(text):
    toks, pos = tokenize(text), 0

    def rd():
        nonlocal pos
        t = toks[pos]
        pos += 1
        if t == "(":
            kids = []
            while pos < len(toks) and toks[pos] != ")":
                kids.append(rd())
            if pos >= len(toks):
                raise SyntaxError("unbalanced (")
            pos += 1
            return Node(kids=kids)
        if t == ")":
            raise SyntaxError("unexpected )")
        return Node(text=t[1], quoted=(t[0] == "s"))

    out = []
    while pos < len(toks):
        out.append(rd())
    return out


def walk(node):
    """Yield every list in the tree."""
    if not node.is_list:
        return
    yield node
    for k in node.kids:
        yield from walk(k)


# ---------------------------------------------------------------------------
# the grammar form heads and what the engine does with each field
# ---------------------------------------------------------------------------
# (head, field index) -> consumer.  '' means DROPPED.  Index 0 is the head.
CONSUMERS = {
    "node": {1: "GameData::Bay.name -> .way RNOD", 2: "GameData::Bay.role/.flags",
             3: "GameData::Bay.town (trade stall)"},
    "town": {1: "GameData::Town.id", 2: "display name", 3: "Town.id",
             4: "Town.kind (2 = petrol station)", 5: "Town.nodes",
             6: "DROPPED (always ())", 7: "Town.goods (item, stock)",
             8: "DROPPED map descriptor"},
    "parking": {1: "GameData::Parking.id", 2: "display", 3: "bazaar id",
                4: "DROPPED (always ())", 5: "Bay list", 6: "Parking.size"},
    "service": {1: "Service.id", 2: "display", 3: "Service.type",
                4: "Service.wayNode", 5: "Service.upgrades", 6: "Service.fuelId"},
    "nodeevent": {1: "NodeEvent.id", 2: "kind (STATEBORDER|UNCONDITIONAL)",
                  3: "wayNode", 4: "direction (ALL|N|S|E|W)", 5: "text"},
    "ambush": {1: "Ambush.id", 2: "class", 3: "tile tag", 4: "'' or tile",
               5: "wayNode", 6: "direction", 7: "slot ids", 8: "paired ambush",
               9: "radius"},
    "obstacle": {1: "Obstacle.id", 2: "kind", 3: "tile", 4: "b3d key",
                 5: "debris A", 6: "debris B", 7: "closed message",
                 8: "open message", 9: "repair nodes"},
    "people": {1: "tile", 2: "gate list"},
    "DAYGW": {1: "Gate.key", 2: "Gate.probability"},
    "DAYONLY": {1: "Gate.key", 2: "Gate.probability"},
    "GOODWEATHER": {1: "Gate.key", 2: "Gate.probability"},
    "UNCONDITIONAL": {1: "Gate.key", 2: "Gate.probability"},
    "playertypes": {1: "counters (live/aitruck/police/mafia/dumb/ghost)"},
    "startposition": {1: "x", 2: "z", 3: "height"},
    "PR_FORCED_ENGINE":  {1: "level 0 (name, price)", 2: "level 1"},
    "PR_OIL_ADDITIVE":   {1: "level 0", 2: "level 1"},
    "PR_ABS":            {1: "level 0", 2: "level 1"},
    "PR_ADJUSTABLE_SUSPENSION": {1: "level 0", 2: "level 1"},
    "PR_BRAKES":         {1: "level 0", 2: "level 1"},
    "PR_ENGINE_BRAKE":   {1: "level 0", 2: "level 1"},
    "PR_REINFORCED_CHASSIS": {1: "level 0", 2: "level 1"},
    "PR_BULETPROOF_GLASS": {1: "level 0", 2: "level 1"},
    "PR_NAVIGATOR":      {1: "level 0", 2: "level 1"},
    "PR_TIRES":          {1: "level 0", 2: "level 1", 3: "level 2"},
    "PR_CARGO_SCANNER":  {1: "level 0", 2: "level 1"},
    "PR_HORN":           {1: "level 0", 2: "level 1"},
    "PR_FIRE_PROTECTION": {1: "level 0", 2: "level 1"},
    "PR_EXTRA_TANK":     {1: "level 0", 2: "level 1"},
    "PR_STEALTH_BODY_PAINT": {1: "level 0", 2: "level 1"},
    "PR_ANTI_RADAR":     {1: "level 0", 2: "level 1"},
    "PR_AIR_FILTER_SNORKEL": {1: "level 0", 2: "level 1"},
    "PR_HEADLIGHT_PROTECTION": {1: "level 0", 2: "level 1"},
    "PR_FUEL":           {1: "level 0", 2: "level 1"},
    "vehicle": {1: "id", 2: "model", 3: "variant", 4: "class", 5: "ai role",
                6: "mass", 7: "price", 8: "trailers", 9: "flag"},
}

# heads that are grammar forms (not data)
FORM_HEADS = set(CONSUMERS) | {
    "forsale", "forsale_used", "forsale_stolen", "forhire", "missions", "fuel",
    "semitrailers", "containers", "parkings", "scenario", "live", "aitruck",
    "police", "mafia", "dumb", "ghost", "world",
}
# GAME.CNF counters: (head, index) -> consumer
for _h in ("forsale", "forsale_used", "forsale_stolen", "forhire", "missions",
           "fuel", "semitrailers", "containers", "parkings", "dumb", "ghost",
           "police", "mafia", "aitruck"):
    CONSUMERS.setdefault(_h, {1: "GAME.CNF world counter"})
CONSUMERS["scenario"] = {1: "world/race scenario name"}
CONSUMERS["live"] = {1: "flag", 2: "player vehicle model", 3: "variant"}


def load_files(data):
    pats = [os.path.join(data, "*.sch"), os.path.join(data, "*.cnf"),
            os.path.join(data, "ENV", "*.sch"), os.path.join(data, "ENV", "*.CNF"),
            os.path.join(data, "MENV", "*.CNF"), os.path.join(data, "MTR", "*.CNF")]
    files = sorted({p for pat in pats for p in glob.glob(pat)})
    out = []
    for p in files:
        raw = open(p, "rb").read()
        if raw[:4] == b"SDTE":
            raw = decrypt(raw)
        out.append((os.path.relpath(p, data), parse(raw.decode("cp1251"))))
    return out


def survey(data):
    heads = collections.Counter()
    head_files = collections.defaultdict(set)
    fields = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
    forms = 0
    for rel, top in load_files(data):
        for f in top:
            for node in walk(f):
                h = node.head()
                if not h:
                    continue
                heads[h] += 1
                head_files[h].add(rel)
                if h in FORM_HEADS:
                    forms += 1
                    # index 0 is the head itself, not a field
                    for i, k in enumerate(node.kids):
                        if i == 0:
                            continue
                        fields[h][i][k.kind()] += 1
    return heads, head_files, fields, forms


# heads whose fields 2..n are a *repeated* child list rather than fixed slots:
# every `people` gate and every `playertypes` counter is read by the engine.
REPEATED = {"people": "the gate list (every child)", "playertypes": "the counter list (every child)"}


def rows(fields):
    """Yield (head, index, shapes, consumer, resolved) rows for the table."""
    for h in sorted(fields, key=lambda k: (-sum(sum(c.values()) for c in fields[k].values()), k)):
        for i in sorted(fields[h]):
            if i == 0:
                continue
            shapes = fields[h][i]
            shape = ",".join(f"{t}x{n}" for t, n in sorted(shapes.items()))
            consumer = CONSUMERS.get(h, {}).get(i, "")
            if not consumer and i >= 2 and h in REPEATED:
                consumer = REPEATED[h]
            yield h, i, shape, consumer


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default=os.path.join(ROOT, "original"))
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--md", action="store_true", help="markdown coverage table")
    a = ap.parse_args()

    heads, head_files, fields, forms = survey(a.data)
    dropped = [(h, i, s) for h, i, s, c in rows(fields) if not c or c.startswith("DROPPED")]
    if a.summary:
        print(f"coverage: files={len(load_files(a.data))} heads={len(heads)} "
              f"forms={forms} fields={sum(len(v) for v in fields.values())} "
              f"dropped_fields={len(dropped)}")
        return 0
    if a.json:
        print(json.dumps({
            "heads": dict(heads),
            "forms": forms,
            "fields": {h: {str(i): dict(c) for i, c in v.items()} for h, v in fields.items()},
            "dropped": dropped,
        }, indent=1, ensure_ascii=False))
        return 0
    if a.md:
        print("| form | field | shapes (count) | consumed by |")
        print("|------|-------|----------------|-------------|")
        for h, i, shape, consumer in rows(fields):
            if not consumer:
                consumer = "**DROPPED** (nothing reads it)"
            elif consumer.startswith("DROPPED"):
                consumer = "**DROPPED** -- " + consumer.split(" ", 1)[1]
            print(f"| `{h}` | {i} | {shape} | {consumer} |")
        return 0
    print(f"{len(heads)} distinct heads over {forms} grammar forms")
    for h, n in heads.most_common(20):
        print(f"  {h:28s} {n}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
