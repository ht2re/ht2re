#!/usr/bin/env python3
"""HT2:RE - colour-key ("transparent pixel") analysis for .txr textures.

Mechanism recovered from king.exe (see docs/txr_alpha.md for the disassembly):

  * Every .res carries a `MATERIALS` table.  A material value string is a
    sequence of keywords; the relevant ones are
        ttx <N>     textured + colour-keyed  (material flags |= 3, so the
                    texture index is N-1 into this .res's TEXTUREFILES list)
        tex <N>     textured, opaque         (material flags |= 1)
        itx <N>     textured + 0x40 flag     (material flags |= 0x41)
        col <C>     material->colorIndex = C-1
  * king.exe @0x1eb815 (keyword "col"):
        call atoi ; dec eax ; mov [ebp+0x28], eax
  * king.exe @0x1db380..0x1db38f: for a material with flag bit 1 set, the
    referenced texture object gets  texture->flags |= 8  (["has colour key"]).
  * king.exe @0x1db529..0x1db582: for every texture with that flag the engine
    calls IDirectDrawSurface7::SetColorKey(DDCKEY_SRCBLT /* = 8 */, &ck) with
        ck.dwColorSpaceLowValue = ck.dwColorSpaceHighValue =
            ((R & 0xF8) << 8) | ((G & 0xFC) << 3) | (B >> 3)
    where (R,G,B) = PALT[material->colorIndex] and PALT is the 256 x RGB triple
    table of the first loaded palette file (COMMON.RES -> common.plm).  The
    palette object built by 0x1e6e90 has a 24-byte header before the triples,
    which is exactly the `add eax,8` (8 entries * 3 bytes) at 0x1db538.

So transparency is *not* stored in the .txr: it is one 16bpp colour per texture,
taken from the .res MATERIALS table plus the shared palette.

CLI:
  python3 tools/txr_key.py [--root original] [--palette original/COMMON/common1.res]
                           [--rules] [--list] [--max N]
"""

from __future__ import annotations

import argparse
import collections
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from res_msk import parse_res, decode_texture, _cstr  # noqa: E402


# --------------------------------------------------------------------------- #
# .res text sections
# --------------------------------------------------------------------------- #

def section_tokens(data: bytes, section: str):
    """Raw NUL-terminated strings of a non-*FILES section (MATERIALS, ...)."""
    at = data.find(section.encode() + b" ")
    if at < 0:
        return []
    line, pos = _cstr(data, at)
    count = int(line.split(" ")[1])
    out = []
    for _ in range(count):
        s, pos = _cstr(data, pos)
        out.append(s)
    return out


def load_palette(res_path: str):
    """256 RGB triples of the first PALT chunk in a .res, or None."""
    data, secs, _ = parse_res(res_path)
    for e in secs.get("PALETTEFILES", []):
        blob = data[e.offset:e.offset + e.size]
        at = blob.find(b"PALT")
        if at < 0:
            continue
        n = struct.unpack_from("<I", blob, at + 4)[0] // 3
        pal = [tuple(blob[at + 8 + 3 * i:at + 11 + 3 * i]) for i in range(n)]
        if pal:
            return pal
    return None


def to565(r, g, b):
    """king.exe @0x1db551..0x1db56b (exact truncating 8 -> 5/6/5)."""
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def tex_key(name: str) -> str:
    """'txr\\tree0.txr 12261928' -> 'tree0.txr' (the .res entry-name form)."""
    s = name.replace("\\", "/").split("/")[-1].strip()
    return s.split(" ")[0]


def parse_materials(lines, tex_names):
    """[(material, texture name|None, has_key, colour index|None, value)]."""
    out = []
    for line in lines:
        s = line.strip()
        if not s:
            continue
        parts = s.split(None, 1)
        name, value = parts[0], (parts[1] if len(parts) > 1 else "")
        toks = value.split()
        tex_idx, key, col = None, False, None
        i = 0
        while i < len(toks):
            t = toks[i]
            if t in ("ttx", "tex", "itx") and i + 1 < len(toks):
                try:
                    tex_idx = int(toks[i + 1]) - 1
                except ValueError:
                    pass
                if t == "ttx":
                    key = True
                i += 2
                continue
            if t == "col" and i + 1 < len(toks):
                try:
                    col = int(toks[i + 1]) - 1
                except ValueError:
                    pass
                i += 2
                continue
            i += 1
        tex = (tex_names[tex_idx] if tex_idx is not None
               and 0 <= tex_idx < len(tex_names) else None)
        out.append((name, tex, key, col, value))
    return out


def texture_pixels(blob: bytes):
    """(TexInfo, list of raw 16bpp values) of the base mip level."""
    info = decode_texture(blob)
    w, h = info.width, info.height
    if info.kind == "txr/LOFF":
        raw = blob[info.pixels_offset:info.pixels_offset + 2 * w * h]
        vals = list(struct.unpack_from("<%dH" % (w * h), raw, 0))
    else:
        vals = [to565(*info.rgba[p * 4:p * 4 + 3]) for p in range(w * h)]
    return info, vals


# --------------------------------------------------------------------------- #

def collect(res_files, palette):
    """Per (file, texture) records.

    Returns (records, engine):
      records[(res, tex)] = dict(hist, total, keys={(k,cidx,explicit): mat})
      engine[(res, tex)]  = {key565, ...}
    """
    records, engine = {}, collections.defaultdict(set)
    for p in res_files:
        short = os.path.basename(p)
        data, secs, _ = parse_res(p)
        texs = secs.get("TEXTUREFILES", [])
        names = [e.name for e in texs]
        for e in texs:
            blob = data[e.offset:e.offset + e.size]
            try:
                info, vals = texture_pixels(blob)
            except Exception:                                      # noqa: BLE001
                continue
            records[(short, tex_key(e.name))] = {
                "hist": collections.Counter(vals), "total": len(vals),
                "w": info.width, "h": info.height, "keys": {},
                "kind": info.kind, "raw": e.name}
        for mname, tex, key, col, value in parse_materials(
                section_tokens(data, "MATERIALS"), names):
            if not key or tex is None:
                continue
            base = tex_key(tex)
            rec = records.get((short, base))
            if rec is None:
                continue
            cidx = col if col is not None else 0
            k = to565(*palette[cidx]) if 0 <= cidx < len(palette) else None
            rec["keys"][(k, cidx, col is not None)] = mname
            engine[(short, base)].add(k)
    return records, engine


# --------------------------------------------------------------------------- #

def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="original")
    ap.add_argument("--palette", default="original/COMMON/common1.res")
    ap.add_argument("--rules", action="store_true")
    ap.add_argument("--max", type=int, default=25)
    args = ap.parse_args(argv)

    palette = load_palette(args.palette)
    print(f"palette {args.palette}: {len(palette)} entries, "
          f"PALT[0]={palette[0]} -> {to565(*palette[0]):#06x}, "
          f"PALT[255]={palette[255]} -> {to565(*palette[255]):#06x}")

    res_files = sorted(glob.glob(os.path.join(args.root, "ENV", "*.res")))
    res_files += sorted(glob.glob(os.path.join(args.root, "MENV", "*.res")))
    res_files += sorted(glob.glob(os.path.join(args.root, "COMMON", "*.res")))
    for extra in ("COMMON.RES", "LOAD.RES"):
        p = os.path.join(args.root, extra)
        if os.path.exists(p):
            res_files.append(p)

    records, engine = collect(res_files, palette)
    print(f"{len(records)} (file, texture) pairs from {len(res_files)} .res")

    withcol, nocol, opaque = [], [], []
    for key, rec in records.items():
        tot = rec["total"]
        top, topn = rec["hist"].most_common(1)[0]
        if rec["keys"]:
            for (k, cidx, explicit), mname in rec["keys"].items():
                share = rec["hist"].get(k, 0) * 100.0 / tot
                row = (key[0], key[1], mname, cidx, k, share, top,
                       topn * 100.0 / tot)
                (withcol if explicit else nocol).append(row)
        else:
            opaque.append((key[0], key[1], top, topn * 100.0 / tot,
                           0x31C2 in rec["hist"], 0 in rec["hist"],
                           rec["hist"].get(0, 0) * 100.0 / tot,
                           rec["hist"].get(0x31C2, 0) * 100.0 / tot))

    print(f"\n## engine rule: {len(withcol)} 'ttx N col C' pairs, "
          f"{len(nocol)} 'ttx N' (default colour index 0 -> "
          f"{to565(*palette[0]):#06x}) pairs, "
          f"{len(opaque)} textures not referenced by any ttx material")
    ok = sum(1 for r in withcol if r[6] == r[4])
    print(f"### ttx+col: key colour IS the most frequent colour: {ok}/{len(withcol)}"
          f"   key share >=10%: {sum(1 for r in withcol if r[5] >= 10)}")
    print("\n-- ttx+col where the key does NOT occur (<0.5% of pixels) --")
    for r in sorted(withcol, key=lambda r: r[5]):
        if r[5] < 0.5:
            print("  %-10s %-28s %-22s col=%3d key=%#06x %6.2f%%  "
                  "top=%#06x %5.2f%%" % r)
    print("\n-- ttx+col with the largest key share --")
    for r in sorted(withcol, key=lambda r: -r[5])[:args.max]:
        print("  %-10s %-28s %-22s col=%3d key=%#06x %6.2f%%  "
              "top=%#06x %5.2f%%" % r)

    d = collections.Counter()
    for r in nocol:
        d["black is top colour"] += (r[6] == 0)
        d["black >= 25%"] += (r[5] >= 25)
        d["black >= 5%"] += (r[5] >= 5)
        d["black < 1%"] += (r[5] < 1)
    print(f"\n-- ttx without col ({len(nocol)} pairs): {dict(d)}")
    print("-- worst matches (default key hides nothing) --")
    for r in sorted(nocol, key=lambda r: r[5])[:args.max]:
        print("  %-10s %-28s %-22s col=%3d key=%#06x %6.2f%%  "
              "top=%#06x %5.2f%%" % r)
    print("-- best matches (default black key) --")
    for r in sorted(nocol, key=lambda r: -r[5])[:args.max]:
        print("  %-10s %-28s %-22s col=%3d key=%#06x %6.2f%%  "
              "top=%#06x %5.2f%%" % r)

    kinds = collections.Counter(r["kind"] for r in records.values())
    print(f"\n## texture formats: {dict(kinds)}")

    if args.rules:
        # the world/ENV data: ENV/*.res + COMMON.RES only (LOAD.RES/MENV are
        # a separate 8bpp palette-index-0 mechanism / menu data).
        scope = {k: r for k, r in records.items()
                 if r["kind"] == "txr/LOFF"
                 and not k[0].startswith(("dr", "menu"))}
        eng = {k for k in engine if k in scope}
        print(f"\n## naive rules over {len(scope)} LOFF (16bpp) textures "
              f"from ENV/*.res + COMMON.RES ({len(eng)} engine-keyed)")
        for thresh in (5, 10, 20, 25, 40, 50):
            hit = {k for k, r in scope.items()
                   if r["hist"].most_common(1)[0][1] * 100.0 / r["total"]
                   >= thresh}
            print(f"  dominant >= {thresh:2d}%      : keys {len(hit):5d}; "
                  f"misses {len(eng - hit):4d}; false positives {len(hit - eng):5d}")
        for nm in ("black", "0x31C2", "black|0x31C2", "black|0x31C2>=5%"):
            if nm == "black":
                hit = {k for k, r in scope.items() if 0 in r["hist"]}
            elif nm == "0x31C2":
                hit = {k for k, r in scope.items() if 0x31C2 in r["hist"]}
            elif nm == "black|0x31C2":
                hit = {k for k, r in scope.items()
                       if 0 in r["hist"] or 0x31C2 in r["hist"]}
            else:
                hit = {k for k, r in scope.items()
                       if r["hist"].get(0, 0) * 100.0 / r["total"] >= 5
                       or r["hist"].get(0x31C2, 0) * 100.0 / r["total"] >= 5}
            print(f"  {nm:16s}     : keys {len(hit):5d}; "
                  f"misses {len(eng - hit):4d}; false positives {len(hit - eng):5d}")
        op = [r for r in opaque if r[0] not in ("LOAD.RES",)
              and not r[0].startswith(("dr", "menu"))]
        bad = sorted(op, key=lambda r: -max(r[6], r[7]))[:args.max]
        print("\n  'black|0x31C2' false positives: opaque textures whose pixels "
              "would be hidden")
        for res, tex, top, tpct, h31, hb, bpct, gpct in bad:
            if max(bpct, gpct) < 5:
                break
            print(f"    {res:12s} {tex:30s} black={bpct:5.2f}% "
                  f"0x31C2={gpct:5.2f}% top={top:#06x} {tpct:5.2f}%")


if __name__ == "__main__":
    sys.exit(main())
